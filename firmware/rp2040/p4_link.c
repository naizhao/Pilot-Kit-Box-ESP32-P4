/*
 * p4_link.c — RP2040 侧 P4 链路（UART0 @921600，协议 v1）。
 *
 * 发送：uart_write_blocking —— 仅在 core0 sender 语境（p4_link_send_modes
 * 由 Task 14 的 sender 循环独占调用；tick_health/poll_rx 亦在核轮询语境，
 * 不会在解码 ISR 中直发）。接收：poll_rx 逐字节喂共享 codec，收到任何
 * 合法帧即置 linked。HEALTH_STATS payload 直接取 counters10 的内存字节：
 * RP2040（Cortex-M0+）为小端，与协议 §4 的 u32le 布局一致。
 */
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "adsb_link.h"
#include "board_pins.h"
#include "edge_cap.h"          /* EDGE_CAP_TICK_HZ：tick→µs */
#include "p4_link.h"

#define P4_UART uart0           /* SDK 2.1.1 实例名（旧名 UART_ID_UART0 已不存在）*/
#define P4_BAUD 921600

static adsb_link_dec_t s_dec;
static uint8_t  s_seq;
static bool     s_linked;
static uint32_t s_tx, s_rx, s_encode_fail;
static const char BUILD_TAG[16] = "rp2040-mvp";

static void tx_frame(uint8_t type, const uint8_t *pl, size_t n)
{
    /* 586 B 帧缓冲静态化（WP-E-2 P1 栈审计：UAT 链最深
     * tx_frame 曾 +616 B；单调用者 = core0 发送语境）。 */
    static uint8_t buf[ADSB_LINK_MAX_FRAME];
    size_t len = adsb_link_encode(buf, sizeof buf, type, s_seq++, pl, n);
    if (len == 0) { s_encode_fail++; return; }
    uart_write_blocking(P4_UART, buf, len);   /* core0 sender 语境，允许阻塞 */
    s_tx++;
}

static void on_rx_msg(void *user, const adsb_link_msg_t *m)
{
    (void)user;
    s_linked = true;
    s_rx++;
    if (m->type == ADSB_LINK_MSG_CONFIG_REQ) {
        uint8_t pl[2] = { 1, 0 };             /* ERROR code=1: v1 不支持配置 */
        tx_frame(ADSB_LINK_MSG_ERROR, pl, sizeof pl);
    }
}

void p4_link_init(void)
{
    uart_init(P4_UART, P4_BAUD);
    gpio_set_function(PIN_P4_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_P4_UART_RX, GPIO_FUNC_UART);
    adsb_link_dec_init(&s_dec, on_rx_msg, NULL);
}

bool p4_link_send_modes(const modes_edge_frame_t *f, uint8_t rssi)
{
    size_t nb = (f->nbits == 112) ? 14 : 7;
    uint8_t pl[6 + 14];
    pl[0] = (f->nbits == 112) ? ADSB_LINK_MODES_LONG : 0;
    pl[1] = rssi;                              /* 定标前恒 0xFF（无值）*/
    /* rp_ts_us 回绕合同（PROTOCOL §2 勘误 2026-09-05）：此截断即模 2^32
     * 单调语义（约 71.6 min 回绕）——消费方用 (u32)(ts_now − ts_prev)
     * 无符号差值解释时间差，不把回绕当回跳；0 是合法回绕值（RP2040 MVP
     * 始终提供有效值，无 0=无值 哨兵）。换算走 edgecap_tick_to_us：
     * tick×2/125 无 u64 溢出路径（旧 ×1000000ull 公式 ~82h 即溢出）。 */
    uint32_t ts = edgecap_tick_to_us(f->start_tick);
    pl[2] = (uint8_t)ts; pl[3] = (uint8_t)(ts >> 8);
    pl[4] = (uint8_t)(ts >> 16); pl[5] = (uint8_t)(ts >> 24);
    memcpy(pl + 6, f->frame, nb);
    if (6 + nb > ADSB_LINK_MAX_PAYLOAD) { s_encode_fail++; return false; }
    tx_frame(ADSB_LINK_MSG_MODES_RAW, pl, 6 + nb);
    return true;
}

/* UAT_UPLINK（协议 v1.1 §6，type 0x11）：CC1312R 的 978 上行帧经
 * SPI（RX_DESCRIPTOR/分片）由 spi_master 重组完成后经此转发 P4。
 * 557 B payload = rssi + ts_us + 552 B 交织帧原样（单位不改写，
 * 时钟域=CC1312R 描述符时钟——见 uat_ingest.h 的域警告）；
 * 解交织/RS/消息层解码在 P4 侧（uat_ingest 前门）。 */
bool p4_link_send_uat(const uint8_t frame552[552], uint8_t rssi,
                      uint32_t ts_us)
{
    /* 557 B 载荷缓冲静态化（审计 WP-E-2 P1 栈越界：SPI digest →
     * 本回调链曾达 3448 B > core0 栈 2048 B）。单调用者：core0。 */
    static uint8_t pl[ADSB_LINK_UAT_PAYLOAD_LEN];
    if (adsb_link_uat_uplink_encode(pl, sizeof(pl), rssi, ts_us,
                                    frame552) != ADSB_LINK_UAT_PAYLOAD_LEN) {
        s_encode_fail++;
        return false;
    }
    tx_frame(ADSB_LINK_MSG_UAT_UPLINK, pl, sizeof(pl));
    return true;
}

void p4_link_poll_rx(void)
{
    uint8_t b;
    while (uart_is_readable(P4_UART)) {
        b = (uint8_t)uart_getc(P4_UART);
        adsb_link_dec_feed(&s_dec, &b, 1);
    }
}

void p4_link_tick_health(const uint32_t counters10[10])
{
    if (!s_linked) {
        uint8_t pl[17] = { 0 };
        memcpy(pl + 1, BUILD_TAG, sizeof BUILD_TAG);
        tx_frame(ADSB_LINK_MSG_HELLO, pl, sizeof pl);
    }
    if (counters10)
        tx_frame(ADSB_LINK_MSG_HEALTH_STATS, (const uint8_t *)counters10, 40);
}

bool p4_link_linked(void)                  { return s_linked; }

void p4_link_send_error(uint8_t code)
{
    uint8_t pl[2] = { code, 0 };
    tx_frame(ADSB_LINK_MSG_ERROR, pl, sizeof pl);
}

void p4_link_get_stats(uint32_t *tx, uint32_t *rx, uint32_t *gaps)
{
    if (tx)   *tx   = s_tx;
    if (rx)   *rx   = s_rx;
    if (gaps) *gaps = s_dec.seq_gaps;
}
