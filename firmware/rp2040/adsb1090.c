/*
 * adsb1090.c — 扩展板 RP2040 主程序。
 * core1：edge_cap DMA 扫描 → modes_edge 解码 → 帧环（绝不阻塞）。
 * core0：帧环 → p4_link 发送、RX 轮询、1 Hz HEALTH、CDC 命令、看护 core1。
 */
#include <stdio.h>
#include <stdatomic.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"

#include "rf_safety.h"
#include "edge_cap.h"
#include "modes_edge.h"
#include "p4_link.h"
#include "selftest_gen.h"
#include "threshold_ctl.h"
#include "spi_master.h"      /* WP-E：CC1312R SPI master（core0 轮询） */
#include "board_pins.h"      /* 运行期天线选择（U16/U17 软通断） */
#include "rp_core0_scheduler.h"
#include "cjtag.h"           /* CC13 代刷：cJTAG 位脉冲（CDC 'F' 命令） */

#define FRAME_RING_LEN 64u

typedef struct { modes_edge_frame_t f; } slot_t;
static slot_t s_ring[FRAME_RING_LEN];
/* SPSC 环（audit round 3：volatile → C11 原子）。core1 生产者写槽后以
 * release 发布 head；core0 消费者以 acquire 读 head 后才读槽，tail 反向
 * 同理。 dropped 计数与 beat 只作统计/看护，relaxed 足够。 */
static atomic_uint s_ring_head, s_ring_tail;
static atomic_uint s_ring_drops;
static atomic_uint s_core1_beat;

/* ── CC1312R SPI master（WP-E）───────────────────────────────────────
 * core0 轮询驱动（审计 round-WP-E-1 P1-3：此前 spim_hw_init/spim_poll
 * 无调用点、UF2 不含 SUBG 路径——现在正式接线）。UAT 帧经回调转
 * p4_link_send_uat() 上送 P4。事务间隔节流（协议 R14）在 spim_poll
 * 内部：drain 流水时每事务阻塞 ≈1 ms——1090 帧环积压由 s_ring_drops
 * 计数暴露，台架期再调（SSI 中断/DMA 化是二期项）。 */
static spi_master_t s_spim;

static void on_uat_frame(const uint8_t *frame, size_t len,
                         uint8_t rssi, uint32_t ts_us, void *user)
{
    (void)user;
    if (len != 552) return;             /* UAT 事实卡 §7 定长合同 */
    p4_link_send_uat(frame, rssi, ts_us);
}

static modes_edge_t s_edge;
static uint32_t s_last_beat;
static int s_stuck_s;
static absolute_time_t s_next_hz;

/* 'P' 设门限的非阻塞状态机：收到 'P' 后逐轮收数字（每轮至多 1 字符），
 * 非数字字符或 300ms 无输入即提交。避免旧实现 getchar_timeout_us(200000)
 * ×4 在 poll_control 内阻塞 ~1s，停摆 edge_cap_service/p4_link/spim。 */
static bool s_tl_pending;
static int  s_tl_val, s_tl_n;
static absolute_time_t s_tl_deadline;

static void tl_commit(void)
{
    if (s_tl_n > 0 && s_tl_val <= 1000) {
        threshold_ctl_set_permille(s_tl_val);
        printf("tl -> %d permille (level=%dmV)\n", s_tl_val,
               threshold_ctl_read_level_mv());
    } else {
        printf("tl set: 'P' + 0..1000\n");
    }
    s_tl_pending = false;
}

static void on_frame(const modes_edge_frame_t *f, void *user)
{
    (void)user;
    uint32_t head = atomic_load_explicit(&s_ring_head, memory_order_relaxed);
    uint32_t next = (head + 1) % FRAME_RING_LEN;
    if (next == atomic_load_explicit(&s_ring_tail, memory_order_acquire)) {
        atomic_fetch_add_explicit(&s_ring_drops, 1, memory_order_relaxed);
        return;                                /* 背压：丢帧不阻塞 */
    }
    s_ring[head].f = *f;
    atomic_store_explicit(&s_ring_head, next, memory_order_release);
}

static void core1_entry(void)
{
    static uint32_t buf[256];
    uint32_t last_overruns = 0;        /* edge_cap_start 清零先于本核启动 */
    while (true) {
        /* 每次至多取一块（edge_cap.h 合同）：断点只能表达在块边界，
         * 逐块取、块空让出。disc 块先 reset 再喂（断点两侧 delta 不拼接）；
         * 时间基保留（round-2 P1-b）：断点后帧的 rp_ts_us 单调、仅被丢失
         * 段时长轻微提前偏置（丢失固有，modes_edge.h 有记），协议
         * rp_ts_us 无 0 哨兵（模 2^32 单调，PROTOCOL §2 勘误）；P4 侧
         * 现忽略 meta，无下游影响。 */
        for (;;) {
            bool disc;
            size_t n = edge_cap_drain(buf, 256, &disc);
            if (!n)
                break;
            /* 丢沿如实上报（re-audit round-2 Fix 1）：RXSTALL 单沿丢失不
             * 打 disc、时间基无自愈——disc（结构性断点）或 edge_cap_overruns()
             * 增加（RXSTALL 计入其中）都是丢沿实据，sticky 置退化标志供
             * 1 Hz 诊断读取，不假装恢复。 */
            uint32_t ovr = edge_cap_overruns();
            if (disc || ovr != last_overruns)
                modes_edge_mark_degraded(&s_edge);
            last_overruns = ovr;
            if (disc)
                modes_edge_reset(&s_edge);
            modes_edge_feed(&s_edge, buf, n);
        }
        atomic_fetch_add_explicit(&s_core1_beat, 1, memory_order_release);
        tight_loop_contents();
    }
}

static void health_fill(uint32_t c[10])
{
    uint32_t tx = 0, rx = 0, gaps = 0;
    p4_link_get_stats(&tx, &rx, &gaps);
    /* s_edge 统计字段为 C11 _Atomic（modes_edge.h，audit round 5 Fix 4）：
     * core1 独占写、core0 在此只读（单写者，与 P4 侧 dsp stats 注释同一
     * 口径），relaxed load 防编译器跨调用缓存旧值；多字段快照可能跨字段
     * 撕裂，但各计数单调，对 1 Hz 诊断无碍。 */
    c[0] = atomic_load_explicit(&s_edge.preamble_hits, memory_order_relaxed);
    c[1] = atomic_load_explicit(&s_edge.frames_56, memory_order_relaxed);
    c[2] = atomic_load_explicit(&s_edge.frames_112, memory_order_relaxed);
    c[3] = modes_edge_time_degraded(&s_edge) ? 1 : 0;
    /* 字段 3（PROTOCOL §4，原恒 0 的 resyncs 预留槽位复用）：sticky 丢沿
     * 退化标志——1 = 断点后时间可信度降级（rp_ts_us 带轻微提前偏置，
     * 单调性不受影响），重启前不清除。P4 侧 v1.0 可忽略该位（diag 页
     * 呈现为后续项）。 */
    c[4] = atomic_load_explicit(&s_edge.dropped_noise, memory_order_relaxed);
    c[5] = edge_cap_overruns() +
           atomic_load_explicit(&s_edge.edge_overruns, memory_order_relaxed);
    c[6] = tx;
    c[7] = atomic_load_explicit(&s_ring_drops, memory_order_relaxed);
    c[8] = rx;                                 c[9] = gaps;
}

static bool core0_send_one_modes(void *user)
{
    (void)user;
    uint32_t tail = atomic_load_explicit(&s_ring_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&s_ring_head, memory_order_acquire);
    if (tail == head)
        return false;

    p4_link_send_modes(&s_ring[tail].f, 0xFF);
    atomic_store_explicit(&s_ring_tail, (tail + 1) % FRAME_RING_LEN,
                          memory_order_release);
    return true;
}

static void core0_poll_p4_rx(void *user)
{
    (void)user;
    p4_link_poll_rx();
}

static void core0_poll_spim(void *user)
{
    (void)user;
    /* 代刷期间独占 GPIO16/17/18（含 CC1312R RESET_N = GPIO18），必须暂停
     * SPI master：否则 spim 收不到 HELLO 会周期进 RECOVERY 并拉低 GPIO18
     * 复位 CC1312R，打断 cJTAG 会话/把 flash 留在半编程态（见 cjtag.h 的
     * 互斥合同）。 */
    if (cjtag_cdc_active()) return;
    spim_poll(&s_spim, time_us_32());
}

static void core0_poll_control(void *user)
{
    (void)user;
    edge_cap_service();                 /* core0：DMA 重武装/停摆恢复 */

    int c = getchar_timeout_us(0);

    /* 'P' 门限输入进行中：本轮的字符只当数字处理，不当作命令。 */
    if (s_tl_pending) {
        if (c >= '0' && c <= '9') {
            s_tl_val = s_tl_val * 10 + (c - '0');
            s_tl_deadline = make_timeout_time_ms(300);
            if (++s_tl_n >= 4)
                tl_commit();
        } else if (c != PICO_ERROR_TIMEOUT) {
            tl_commit();                /* 任意非数字字符结束 */
        } else if (absolute_time_diff_us(get_absolute_time(),
                                         s_tl_deadline) < 0) {
            tl_commit();                /* 300ms 无输入结束 */
        }
        return;
    }

    /* CC1312R 代刷模式（方案 A：先 4 字节小端长度，再是镜像）：进入后
     * 所有字节都是镜像数据，交给 cjtag 引擎流式接收，不再按命令解析
     * （镜像里的 'T'/'A'/'Q' 等字节不能当命令）。 */
    if (cjtag_cdc_active()) {
        if (c != PICO_ERROR_TIMEOUT)
            cjtag_cdc_data((uint8_t)c);
        return;
    }

    if (c == 'T') {
        /* 闭环自检（审计 Fix 2）：DF17 要走通 PIO→DMA→解码全链路，
         * frames_112 增长才算收到；P4 侧 CRC 门在 P4 控制台另行
         * 验证（Task 15）。 */
        uint32_t f0 = atomic_load_explicit(&s_edge.frames_112,
                                           memory_order_relaxed);
        bool sent = selftest_run();
        uint32_t f1 = f0;
        for (int i = 0; sent && i < 500 && f1 == f0; i++) {
            sleep_ms(1);
            f1 = atomic_load_explicit(&s_edge.frames_112,
                                      memory_order_relaxed);
        }
        if (!sent)
            printf("selftest: FAILED\n");
        else if (f1 != f0)
            printf("selftest: ROUND-TRIP OK (%u frames)\n",
                   (unsigned)(f1 - f0));
        else
            printf("selftest: sent but NOT received within 500ms "
                   "-- check wire/decode\n");
    } else if (c == 'S') {
        uint32_t h[10]; health_fill(h);
        printf("stats pre=%u f56=%u f112=%u noise=%u ovr=%u "
               "ringdrop=%u tx=%u rx=%u linked=%d\n",
               h[0], h[1], h[2], h[4], h[5], h[7], h[6], h[8],
               (int)p4_link_linked());
    } else if (c == 'H') {
        printf("tl_level=%dmV rssi_raw=%d\n",
               threshold_ctl_read_level_mv(),
               threshold_ctl_read_rssi_raw());
    } else if (c == 'F') {
        /* CC1312R 代刷模式（cJTAG 位脉冲）：暂停 1090 解码、独占
         * SUBG_TMSC/TCKC/RESET → 读 IDCODE → 流式收镜像 → 擦/写/校验
         * → RESET → 恢复 SPI master。
         * 完整操作流见 docs/firmware_update.md CC1312R 段。 */
        if (cjtag_cdc_enter()) {
            printf("FLASH-MODE READY (CC1312R IDCODE=0x%08X)\n",
                   cjtag_read_idcode());
            printf("Send 4-byte LE length, then the image.\n");
        } else {
            printf("FLASH-MODE FAIL (no JTAG response — check CC1312R "
                   "power/clock)\n");
        }
    } else if (c == 'Q' && cjtag_cdc_active()) {
        cjtag_cdc_quit();
        printf("FLASH-DONE\n");
    } else if (c == 'A') {
        /* 1090 天线切外接 J6（U16 SPDT：A=0/B=1） */
        gpio_put(PIN_ANT_SEL_1090_A, 0);
        gpio_put(PIN_ANT_SEL_1090_B, 1);
        printf("1090 ant -> EXTERNAL (J6)\n");
    } else if (c == 'a') {
        /* 1090 天线切板载 IFA（U16：A=1/B=0，boot 默认） */
        gpio_put(PIN_ANT_SEL_1090_A, 1);
        gpio_put(PIN_ANT_SEL_1090_B, 0);
        printf("1090 ant -> ONBOARD IFA\n");
    } else if (c == 'N') {
        /* GNSS 天线切 J2（U17：A=0/B=1，boot 默认） */
        gpio_put(PIN_GNSS_SEL_A, 0);
        gpio_put(PIN_GNSS_SEL_B, 1);
        printf("GNSS ant -> J2\n");
    } else if (c == 'n') {
        /* GNSS 天线切 J8 内置 patch 位（U17：A=1/B=0） */
        gpio_put(PIN_GNSS_SEL_A, 1);
        gpio_put(PIN_GNSS_SEL_B, 0);
        printf("GNSS ant -> J8\n");
    } else if (c == 'P') {
        /* 运行期设门限：'P' 后跟 1–4 位十进制 permille（0..1000），
         * 非数字字符或 300ms 无输入结束。闭环/协议下发是后续项
         * （PLAN.md §6.5）。非阻塞状态机见文件上方 tl_commit()。 */
        s_tl_pending = true;
        s_tl_val = 0;
        s_tl_n = 0;
        s_tl_deadline = make_timeout_time_ms(300);
    } else if (c == 'B') {
        /* 软入口回 BOOTSEL：现场重刷不用再拆机短接 SW2/SW1（J1 扣上
         * 时插拔 USB 不产生复位，硬进 BOOTSEL 很麻烦）。 */
        printf("-> BOOTSEL\n");
        sleep_ms(50);
        reset_usb_boot(0, 0);
    }
}

static void core0_poll_periodic(void *user)
{
    (void)user;
    if (absolute_time_diff_us(get_absolute_time(), s_next_hz) < 0) {
        s_next_hz = make_timeout_time_ms(1000);
        uint32_t h[10];
        health_fill(h);
        p4_link_tick_health(h);

        uint32_t beat = atomic_load_explicit(&s_core1_beat,
                                             memory_order_acquire);
        if (beat == s_last_beat) {
            if (++s_stuck_s >= 5) {
                printf("ERROR: core1 decoder stalled %ds\n", s_stuck_s);
                p4_link_send_error(2);      /* code=2: decoder stall */
            }
        } else {
            s_last_beat = beat;
            s_stuck_s = 0;
        }
    }
}

int main(void)
{
    stdio_init_all();
    printf("pkb-adsb1090 rp2040  build " __DATE__ " " __TIME__ "\n");

    rf_safety_apply_boot_state();     /* F2：bias 关断 + 合法互补选择态 */
    threshold_ctl_init();             /* F5：20kHz 50% 起步 */
    printf("init: bias=OFF  tl_level=%dmV  rssi_raw=%d\n",
           threshold_ctl_read_level_mv(), threshold_ctl_read_rssi_raw());

    edge_cap_start();
    p4_link_init();
    modes_edge_init(&s_edge, EDGE_CAP_TICK_HZ, on_frame, NULL);
    multicore_launch_core1(core1_entry);

    /* WP-E：SUBG 链路（CC1312R）——握手由 spim_poll 首个 HELLO 发起
     * （RESET 释放后 ≥100 ms 由 spim_hw_reset_pulse 保证，§6.1）。 */
    spim_hw_init();
    spi_master_init(&s_spim, on_uat_frame, NULL);

    s_last_beat = 0;
    s_stuck_s = 0;
    s_next_hz = make_timeout_time_ms(1000);

    rp_core0_ops_t ops = {
        .send_one_modes = core0_send_one_modes,
        .poll_p4_rx = core0_poll_p4_rx,
        .poll_spim = core0_poll_spim,
        .poll_control = core0_poll_control,
        .poll_periodic = core0_poll_periodic,
    };

    while (true) {
        rp_core0_schedule_once(&ops);
        tight_loop_contents();
    }
}
