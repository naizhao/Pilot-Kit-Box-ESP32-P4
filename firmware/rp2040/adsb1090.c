/*
 * adsb1090.c — 扩展板 RP2040 主程序。
 * core1：edge_cap DMA 扫描 → modes_edge 解码 → 帧环（绝不阻塞）。
 * core0：帧环 → p4_link 发送、RX 轮询、1 Hz HEALTH、CDC 命令、看护 core1。
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "board_pins.h"
#include "rf_safety.h"
#include "edge_cap.h"
#include "modes_edge.h"
#include "p4_link.h"
#include "selftest_gen.h"
#include "threshold_ctl.h"

#define FRAME_RING_LEN 64u

typedef struct { modes_edge_frame_t f; } slot_t;
static slot_t           s_ring[FRAME_RING_LEN];
static volatile uint32_t s_ring_head, s_ring_tail;   /* core1 写 head / core0 写 tail */
static volatile uint32_t s_ring_drops;
static volatile uint32_t s_core1_beat;

static modes_edge_t s_edge;

static void on_frame(const modes_edge_frame_t *f, void *user)
{
    (void)user;
    uint32_t next = (s_ring_head + 1) % FRAME_RING_LEN;
    if (next == s_ring_tail) { s_ring_drops++; return; }   /* 背压：丢帧不阻塞 */
    s_ring[s_ring_head].f = *f;
    s_ring_head = next;
}

static void core1_entry(void)
{
    static uint32_t buf[256];
    while (true) {
        size_t n = edge_cap_drain(buf, 256);
        if (n) modes_edge_feed(&s_edge, buf, n);
        s_core1_beat++;
        tight_loop_contents();
    }
}

static void health_fill(uint32_t c[10])
{
    uint32_t tx = 0, rx = 0, gaps = 0;
    p4_link_get_stats(&tx, &rx, &gaps);
    c[0] = s_edge.preamble_hits;  c[1] = s_edge.frames_56;
    c[2] = s_edge.frames_112;     c[3] = 0;                  /* resyncs 预留 */
    c[4] = s_edge.dropped_noise;
    c[5] = edge_cap_overruns() + s_edge.edge_overruns;
    c[6] = tx;                    c[7] = s_ring_drops;
    c[8] = rx;                    c[9] = gaps;
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
        while (s_ring_tail != s_ring_head) {
            p4_link_send_modes(&s_ring[s_ring_tail].f, 0xFF);
            s_ring_tail = (s_ring_tail + 1) % FRAME_RING_LEN;
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

            if (s_core1_beat == last_beat) {
                if (++stuck_s >= 5) {
                    printf("ERROR: core1 decoder stalled %ds\n", stuck_s);
                    p4_link_send_error(2);          /* code=2: decoder stall */
                }
            } else { last_beat = s_core1_beat; stuck_s = 0; }
        }
    }
}
