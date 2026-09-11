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
#include "cjtag.h"          /* CC13 首刷：cJTAG 位脉冲（'F' 烧录 / 'J' 诊断 / 'Z' 让路） */
#include "cc13_bsl.h"       /* CC13 升级：ROM 串行 bootloader（'U' 烧录 / 'L' 诊断） */

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

static void health_fill(uint32_t c[11])
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
    /* 字段 10（协议 v1.3 新增）：USB_VBUS_SENSE **分压中点**的 mV，不是 VBUS。
     * 换算成 VBUS 要乘板型分压比，而 RP2040 不分板型——见 board_pins.h。 */
    c[10] = (uint32_t)threshold_ctl_read_vbus_node_mv();
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
    if (cjtag_cdc_active() || cc13_bsl_active()) return;
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

    /* BSL 代刷模式：同样进入后所有字节都是镜像数据，不再按命令解析。 */
    if (cc13_bsl_cdc_busy()) {
        if (c != PICO_ERROR_TIMEOUT)
            cc13_bsl_cdc_byte((uint8_t)c);
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
        uint32_t h[11]; health_fill(h);
        printf("ant1090=%s antgnss=%s\n",
               p4_link_ant_1090() ? "EXT-J6" : "ONBOARD-IFA",
               p4_link_ant_gnss() ? "ONBOARD-PATCH" : "EXT-J2");
        printf("stats pre=%u f56=%u f112=%u noise=%u ovr=%u "
               "ringdrop=%u tx=%u rx=%u linked=%d\n",
               h[0], h[1], h[2], h[4], h[5], h[7], h[6], h[8],
               (int)p4_link_linked());
    } else if (c == 'H') {
        /* rssi_raw 标注为不可信：实测它读回的是门限那一路的电压，不是
         * AD8313 的检波输出（机理见 threshold_ctl.h 文件头）。不加这个
         * 标注，它就是一个看起来完全正常、实际会把人带偏的数字。 */
        printf("tl_level=%dmV rssi_raw=%d(!不可信,见 threshold_ctl.h)\n",
               threshold_ctl_read_level_mv(),
               threshold_ctl_read_rssi_raw());
        /* 连读判据：被驱动的节点连读立刻稳定；悬空的高阻节点会被采样电容
         * 反复充电而漂。2026-09-11 发现 rssi_raw 全程跟着门限走，这一串是
         * 用来区分"没人驱动"和"读错通道"的。 */
        uint16_t b[8];
        threshold_ctl_adc_burst(PIN_ADC_RSSI, b, 8);
        printf("  rssi 连读:");
        for (int i = 0; i < 8; i++) printf(" %u", (unsigned)b[i]);
        threshold_ctl_adc_burst(PIN_ADC_LEVEL, b, 8);
        printf("\n  level 连读:");
        for (int i = 0; i < 8; i++) printf(" %u", (unsigned)b[i]);
        printf("\n");
    } else if (c == 'F') {
        /* CC1312R 代刷模式（cJTAG 位脉冲）：暂停 1090 解码、独占
         * SUBG_TMSC/TCKC/RESET → 读 IDCODE → 流式收镜像 → 擦/写/校验
         * → RESET → 恢复 SPI master。
         * 完整操作流见 docs/firmware_update.md CC1312R 段。 */
        if (cjtag_cdc_enter()) {
            /* 打印 cjtag_cdc_enter() 本次读到的值，不要再调一次
             * cjtag_read_idcode()——那会把已经建好的会话打回 TLR。 */
            printf("FLASH-MODE READY (ICEPick IDCODE=0x%08lX)\n",
                   (unsigned long)cjtag_cdc_idcode());
            printf("Send 4-byte LE length, then the image.\n");
        } else {
            printf("FLASH-MODE FAIL (IDCODE=0x%08lX, 期望 0x_BB4102F) "
                   "—— 跑 'J' 看激活参数矩阵\n",
                   (unsigned long)cjtag_cdc_idcode());
        }
    } else if (c == 'U') {
        /* 经 ROM 串行 bootloader 代刷 CC1312R —— 这是正式升级通道。
         * 前提是目标已有一版镜像且 CCFG 开了 bootloader+backdoor
         * （firmware/cc1312r/ccfg.c，构建时由 check_ccfg.py 卡死）。 */
        cc13_bsl_cdc_start();
    } else if (c == 'Z') {
        /* 释放 cJTAG 三根线，给外接仿真器让路（见 cjtag_release_bus）。 */
        cjtag_release_bus();
    } else if (c == 'L') {
        /* CC1312R 的 ROM 串行 bootloader 探测（SSI0，不走 cJTAG）。
         * 只读不写 flash，见 cc13_bsl.h 讲为什么这条路存在。 */
        cc13_bsl_diag();
    } else if (c == 'J') {
        /* cJTAG 链路诊断：跑一遍激活参数矩阵，打印每个变体读回的原始 DR。
         * 只读，不碰 flash。 */
        cjtag_diag();
    } else if (c == 'Q' && cjtag_cdc_active()) {
        cjtag_cdc_quit();
        printf("FLASH-DONE\n");
    } else if (c == 'A' || c == 'a' || c == 'N' || c == 'n') {
        /* 天线选择的真值表只在 rf_safety.c 一处（P4 的 CONFIG_REQ 走的也是
         * 同一对函数）。这里曾各自 gpio_put 一遍，两处抄同一张表，改一处
         * 漏一处只是时间问题。
         *
         * ⚠ 这几个键是**台架调试入口**，改的是易失状态：P4 下一次下发
         * CONFIG_REQ（用户改设置、或 RP 重启后握手）会把它覆盖回设置页里
         * 存的值。要永久改，改设置页。 */
        switch (c) {
        case 'A': rf_safety_set_ant_1090(RF_ANT_1090_EXTERNAL);
                  printf("1090 ant -> EXTERNAL (J6)  [临时，P4 下发会覆盖]\n"); break;
        case 'a': rf_safety_set_ant_1090(RF_ANT_1090_ONBOARD);
                  printf("1090 ant -> ONBOARD IFA    [临时，P4 下发会覆盖]\n"); break;
        case 'N': rf_safety_set_ant_gnss(RF_ANT_GNSS_EXTERNAL);
                  printf("GNSS ant -> J2 外接        [临时，P4 下发会覆盖]\n"); break;
        default:  rf_safety_set_ant_gnss(RF_ANT_GNSS_ONBOARD);
                  printf("GNSS ant -> J8 板载 patch  [临时，P4 下发会覆盖]\n"); break;
        }
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
        uint32_t h[11];
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
