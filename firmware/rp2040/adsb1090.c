/*
 * adsb1090.c — 扩展板 RP2040 主程序。
 * core1：edge_cap DMA 扫描 → modes_edge 解码 → 帧环（绝不阻塞）。
 * core0：帧环 → p4_link 发送、RX 轮询、1 Hz HEALTH、CDC 命令、看护 core1。
 */
#include <stdio.h>
#include <stdatomic.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "rf_safety.h"
#include "edge_cap.h"
#include "modes_edge.h"
#include "p4_link.h"
#include "selftest_gen.h"
#include "threshold_ctl.h"

#define FRAME_RING_LEN 64u

typedef struct { modes_edge_frame_t f; } slot_t;
static slot_t s_ring[FRAME_RING_LEN];
/* SPSC 环（audit round 3：volatile → C11 原子）。core1 生产者写槽后以
 * release 发布 head；core0 消费者以 acquire 读 head 后才读槽，tail 反向
 * 同理。 dropped 计数与 beat 只作统计/看护，relaxed 足够。 */
static atomic_uint s_ring_head, s_ring_tail;
static atomic_uint s_ring_drops;
static atomic_uint s_core1_beat;

static modes_edge_t s_edge;

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
    bool disc;
    while (true) {
        size_t n = edge_cap_drain(buf, 256, &disc);
        if (n) {
            /* 丢沿/重启断点：先丢掉既有半截 burst 再喂（abs_tick 基线
             * 归零重计），断点两侧的 delta 不拼接。 */
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
    c[3] = 0;                                  /* resyncs 预留 */
    c[4] = atomic_load_explicit(&s_edge.dropped_noise, memory_order_relaxed);
    c[5] = edge_cap_overruns() +
           atomic_load_explicit(&s_edge.edge_overruns, memory_order_relaxed);
    c[6] = tx;
    c[7] = atomic_load_explicit(&s_ring_drops, memory_order_relaxed);
    c[8] = rx;                                 c[9] = gaps;
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

    uint32_t last_beat = 0;
    int stuck_s = 0;
    absolute_time_t next_hz = make_timeout_time_ms(1000);

    while (true) {
        for (;;) {
            uint32_t tail = atomic_load_explicit(&s_ring_tail,
                                                 memory_order_relaxed);
            uint32_t head = atomic_load_explicit(&s_ring_head,
                                                 memory_order_acquire);
            if (tail == head) break;
            p4_link_send_modes(&s_ring[tail].f, 0xFF);
            atomic_store_explicit(&s_ring_tail, (tail + 1) % FRAME_RING_LEN,
                                  memory_order_release);
        }
        p4_link_poll_rx();

        int c = getchar_timeout_us(0);
        if (c == 'T') {
            printf("selftest: %s\n", selftest_run() ? "sent" : "FAILED");
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
        }

        if (absolute_time_diff_us(get_absolute_time(), next_hz) < 0) {
            next_hz = make_timeout_time_ms(1000);
            uint32_t h[10]; health_fill(h);
            p4_link_tick_health(h);

            uint32_t beat = atomic_load_explicit(&s_core1_beat,
                                                 memory_order_acquire);
            if (beat == last_beat) {
                if (++stuck_s >= 5) {
                    printf("ERROR: core1 decoder stalled %ds\n", stuck_s);
                    p4_link_send_error(2);          /* code=2: decoder stall */
                }
            } else { last_beat = beat; stuck_s = 0; }
        }
    }
}
