/*
 * adsb_link.h — P4↔RP2040 链路 v1 的共享编解码器接口。
 *
 * 规范：firmware/PROTOCOL_P4_RP2040_UART.md（v1.3；v1.0 冻结于 2026-09-05，
 * v1.1 加 UAT_UPLINK、v1.2 加 CONFIG_REQ/ACK、v1.3 加 SWEEP_ROW 与
 * HEALTH_STATS 的实际生效门限/天线，均向前兼容，见规范变更清单）。本文件被
 * 两侧固件编译同一份实现（P4: components/adsb_link_codec；RP2040:
 * rp2040/CMakeLists.txt 直接引用同一 .c），协议永不漂移。纯 C99，无平台依赖。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADSB_LINK_MAGIC0       0x50   /* 'P' */
#define ADSB_LINK_MAGIC1       0x4B   /* 'K' */
#define ADSB_LINK_VER_MAJOR    1
#define ADSB_LINK_VER_MINOR    3      /* v1.3（v1.2=2、v1.1=1、v1.0=0；接收方只看 major）*/
#define ADSB_LINK_HDR_LEN      8
#define ADSB_LINK_MAX_PAYLOAD  576    /* v1.0 为 464；v1.1 扩容，规范 §6 */
#define ADSB_LINK_MAX_FRAME    (ADSB_LINK_HDR_LEN + ADSB_LINK_MAX_PAYLOAD + 2)

/* msg_type（v1，规范 §2；0x11 为 v1.1 新增） */
#define ADSB_LINK_MSG_HELLO         0x01
#define ADSB_LINK_MSG_CAPABILITIES  0x02
#define ADSB_LINK_MSG_HEALTH_STATS  0x03
#define ADSB_LINK_MSG_MODES_RAW     0x10
#define ADSB_LINK_MSG_UAT_UPLINK    0x11   /* v1.1，规范 §6 */
#define ADSB_LINK_MSG_SWEEP_ROW     0x12   /* v1.3：门限扫描逐点结果，见下 */
#define ADSB_LINK_MSG_CONFIG_REQ    0x20   /* v1.2 起有定义，见下 */
#define ADSB_LINK_MSG_CONFIG_ACK    0x21
#define ADSB_LINK_MSG_ERROR         0x7F

/* ── CONFIG_REQ / CONFIG_ACK（v1.2，规范 §7）───────────────────────────
 * v1.0/v1.1 里这两个 type 是「预留不实现」，RP 侧收到一律回 ERROR code=1。
 * v1.2 起定义为一组 key/value：
 *   CONFIG_REQ (P4→RP): { u8 n; {u8 key; u8 val;} item[n] }
 *   CONFIG_ACK (RP→P4): { u8 applied; u8 rejected; }
 *
 * 键值编号刻意让**全 0 的 payload 等于上电默认**（与 rf_safety.c 的开机
 * 安全向量一致）。这样"配置没送到"和"配置是默认值"落到同一个状态，不会
 * 出现一半新一半旧的天线组合——RF 通路上半套配置是会让人查半天的。
 *
 * 未知 key 计入 rejected 但不影响其它项（minor 前向兼容，与 §3.5 未知
 * msg_type 必须递交同一条原则）。 */
#define ADSB_LINK_CFG_MAX_ITEMS   16
#define ADSB_LINK_CFG_ANT_1090    0x01   /* 0=板载 IFA（默认）/ 1=外接 J6 */
#define ADSB_LINK_CFG_ANT_GNSS    0x02   /* 0=外接 J2（默认）/ 1=板载 patch */
#define ADSB_LINK_CFG_THRESHOLD   0x03   /* 0=用固件默认 / 1..100 = 10..1000‰ */
#define ADSB_LINK_CFG_SWEEP       0x04   /* 0=停止（默认）/ 1=启动门限扫描 */

typedef struct { uint8_t key, val; } adsb_link_cfg_item_t;

/* ── 门限的 8 位编码（CFG_THRESHOLD）────────────────────────────────
 * 门限是 0..1000 permille，而 cfg 的 val 只有 8 位。编码成「每 10‰ 一档」：
 *   val = 0        → **用固件默认**（不是 0‰！）
 *   val = 1..100   → 10..1000‰
 *   val > 100      → 无效
 *
 * val=0 之所以必须是"用默认"而不是"0‰"，是为了守住本节开头那条不变量：
 * 全 0 的 payload 等于上电默认。否则"配置没送到"和"配置是 0‰"会落到同一
 * 个字节上——而 0‰ 恰好是个灾难性取值（判决电平贴地，比较器被噪声淹没、
 * 一帧都解不出），这两件事绝不能长得一样。
 *
 * 10‰ 的档距够用：扫描步进是 100‰，手动微调也只到 10‰ 量级。 */
static inline uint8_t adsb_link_threshold_to_val(int permille)
{
    /* <10 夹到 10 而不是落成 0：想设 5‰ 和想用默认是两件事。 */
    if (permille < 10) permille = 10;
    if (permille > 1000) permille = 1000;
    return (uint8_t)((permille + 5) / 10);      /* 四舍五入到最近档 */
}

/* 返回 permille；0 = "用固件默认"（由调用方决定那是多少），-1 = 无效。
 * 三态刻意不合并：把无效值悄悄当成默认，等于让对面用一个它没要求的门限
 * 跑下去，而门限错了的表现就是"收不到任何包"，最难查。 */
static inline int adsb_link_threshold_from_val(uint8_t val)
{
    if (val == 0)   return 0;
    if (val > 100)  return -1;
    return (int)val * 10;
}

/* ── SWEEP_ROW（v1.3，RP→P4）─────────────────────────────────────────
 * 扫描逐点上报，不等整轮结束——一轮 90s，攒到最后才发的话 P4 既画不了
 * 进度，也没法中途让人叫停。
 *
 * payload（小端，与 MODES_RAW 的 rp_ts_us 同序）：
 *   u8 idx; u8 total; u8 permille_val; u16 level_mv;
 *   u32 frames; u32 pre; u32 noise; u32 ovr;
 * permille_val 用与 CFG_THRESHOLD 相同的编码，两边不各存一套标度。 */
#define ADSB_LINK_SWEEP_ROW_LEN   21

size_t adsb_link_sweep_row_encode(uint8_t *out, size_t cap,
                                  uint8_t idx, uint8_t total,
                                  uint8_t permille_val, uint16_t level_mv,
                                  uint32_t frames, uint32_t pre,
                                  uint32_t noise, uint32_t ovr);

/* 长度不符一律整帧拒绝并且**不写任何输出**（与 uat_uplink_decode 同款
 * 单元契约）。任一 out 指针可为 NULL 表示不关心该字段。 */
bool adsb_link_sweep_row_decode(const uint8_t *payload, size_t len,
                                uint8_t *idx, uint8_t *total,
                                uint8_t *permille_val, uint16_t *level_mv,
                                uint32_t *frames, uint32_t *pre,
                                uint32_t *noise, uint32_t *ovr);

/* ── HEALTH_STATS payload（u32le × N，RP→P4，1 Hz）────────────────────
 * 字段下标与两侧实现共用，避免 P4 用字面量偏移、RP 用另一套。v1.3 起 N=13：
 *   0 preamble_hits  1 frames_56  2 frames_112  3 time_degraded
 *   4 dropped_noise  5 edge_overruns  6 tx_frames  7 tx_drops
 *   8 rx_frames      9 rx_seq_gaps
 *   10 vbus_mid_mV（v1.3 早期加入，见规范 §4）
 *   11 门限**实际生效值**（permille，RP 侧 threshold_ctl_get_permille()）
 *   12 1090 天线**实际生效值**（0=板载 IFA / 1=外接 J6，RP 侧 rf_safety）
 * 11/12 记的是实际生效值而不是 P4 的下发值：扫描或 CDC 切换期间两者会不同，
 * 而计数正是产生在那个条件下——落盘日志必须带上它（见 rf1090_log.h）。
 * 接收方用 payload_len 判断字段是否存在，缺字段时回退到本地值。 */
#define ADSB_LINK_HEALTH_FIELDS          13
#define ADSB_LINK_HEALTH_IDX_THRESHOLD   11
#define ADSB_LINK_HEALTH_IDX_ANT_1090    12

/* 组 CONFIG_REQ payload。返回 payload 长度；cap 不足或 n 超限返回 0
 * （与 adsb_link_encode 同款约定：失败返回 0 且不写输出）。 */
size_t adsb_link_config_encode(uint8_t *out, size_t cap,
                               const adsb_link_cfg_item_t *items, uint8_t n);

/* 拆 CONFIG_REQ payload 到调用方数组。长度与声明的条数对不上、或 cap 装不下
 * 就返回 false 且**不写任何输出**（与 uat_uplink_decode 同款单元契约）。
 * 这里拷贝而不是像 UAT 那样回指 payload 内部：item 是结构体，回指要把
 * uint8_t* 转成结构体指针，白惹一个严格别名问题，而这里总共最多 32 字节。*/
bool adsb_link_config_decode(const uint8_t *payload, size_t len,
                             adsb_link_cfg_item_t *out, uint8_t cap,
                             uint8_t *n);

/* MODES_RAW payload */
#define ADSB_LINK_MODES_LONG        0x01   /* flags bit0：112-bit 帧 */

/* UAT_UPLINK payload（v1.1 规范 §6.1）：552 B 交织帧原样 + 5 B 元数据，
 * 固定 557 B、无长度/flags 字段（上行单帧长，事实卡 §1/§6.1）。帧内容
 * 语义（解交织/RS）不属 codec——两侧共用的是这里的字节布局合同。 */
#define ADSB_LINK_UAT_FRAME_BYTES   552
#define ADSB_LINK_UAT_META_BYTES    5      /* rssi(1) + rp_ts_us(4) */
#define ADSB_LINK_UAT_PAYLOAD_LEN \
    (ADSB_LINK_UAT_META_BYTES + ADSB_LINK_UAT_FRAME_BYTES)   /* 557 */

uint16_t adsb_link_crc16(const uint8_t *data, size_t len);   /* CCITT-FALSE */

/* 返回帧总长；cap 不足或 payload_len 超限返回 0。 */
size_t adsb_link_encode(uint8_t *out, size_t cap, uint8_t msg_type,
                        uint8_t seq, const uint8_t *payload,
                        size_t payload_len);

/* 组 UAT_UPLINK payload 到 out（cap ≥ ADSB_LINK_UAT_PAYLOAD_LEN）。
 * 返回 payload 长度 557；cap 不足返回 0（与 adsb_link_encode 同款约定）。
 * rssi 单位 0.5 dB、0xFF=无值；rp_ts_us 模 2^32 单调 µs（规范 §6.1）。 */
size_t adsb_link_uat_uplink_encode(uint8_t *out, size_t cap, uint8_t rssi,
                                   uint32_t rp_ts_us,
                                   const uint8_t frame[ADSB_LINK_UAT_FRAME_BYTES]);

/* 拆 UAT_UPLINK payload。len ≠ 557 返回 false 且不写任何输出（失败不
 * 改写输出，与 uat_decode 纯单元契约同款）；*frame 指向 payload 内部
 * 偏移 5，随调用方缓冲存活。 */
bool adsb_link_uat_uplink_decode(const uint8_t *payload, size_t len,
                                 uint8_t *rssi, uint32_t *rp_ts_us,
                                 const uint8_t **frame);

typedef struct {
    uint8_t  type;
    uint8_t  seq;
    uint16_t payload_len;
    uint8_t  payload[ADSB_LINK_MAX_PAYLOAD];
} adsb_link_msg_t;

typedef void (*adsb_link_on_msg_fn)(void *user, const adsb_link_msg_t *msg);

typedef struct adsb_link_dec {
    uint8_t  buf[ADSB_LINK_MAX_FRAME];
    uint16_t fill;
    uint8_t  last_seq;
    uint8_t  have_seq;
    /* 计数器（规范 §3；诊断页/HEALTH_STATS 直接读这里） */
    uint32_t msgs;              /* 成功递交的消息数 */
    uint32_t crc_errors;
    uint32_t resyncs;           /* 头部扫描前进次数 */
    uint32_t len_errors;
    uint32_t version_mismatch;
    uint32_t seq_gaps;
    adsb_link_on_msg_fn cb;
    void    *user;
} adsb_link_dec_t;

void adsb_link_dec_init(adsb_link_dec_t *d, adsb_link_on_msg_fn cb,
                        void *user);
void adsb_link_dec_feed(adsb_link_dec_t *d, const uint8_t *bytes, size_t n);
