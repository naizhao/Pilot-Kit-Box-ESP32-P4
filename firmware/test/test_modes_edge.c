/*
 * test_modes_edge.c — 上升沿间隔 → Mode-S 帧解码的 host 单测。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_modes_edge \
 *      firmware/test/test_modes_edge.c firmware/rp2040/modes_edge.c \
 *   && /tmp/test_modes_edge
 *
 * 向量构造与实现共用同一份时序合同：上升沿绝对时刻一律用 quarter-µs
 * （qus）整数 —— preamble 沿在 0/1.0/3.5/4.5µs（= 0/4/14/18 qus，三段
 * 间隔 [1.0, 2.5, 1.0]µs）；数据块自 +8µs 起、每比特 1.0µs（外部锚点
 * mode-s.c:708-715），脉冲在比特 +0µs → bit1、+0.5µs → bit0
 * （= 32 + 4k (+2) qus）。
 * qus→tick 取四舍五入：62.5MHz 下 1 qus = 15.625 tick，真实捕获本就把
 * 间隔量化到最近 tick，这样任一 qus 值经 tick 往返不失真；若用截断，
 * 每个 2/4 qus 间隔固定丢 1 qus，112 比特内累积漂移远超 ±1 qus 容差。
 */
#include "modes_edge.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

#define TICK_HZ 62500000u          /* SM 125MHz / 2（每迭代 2 周期） */
#define US(x)   ((uint32_t)((double)(x) * TICK_HZ / 1000000.0))
#define LONG_GAP US(100.0)         /* >5µs 的帧间长隔（用 100µs）*/

static int g_frames;
static modes_edge_frame_t g_last;
static void cb(const modes_edge_frame_t *f, void *user)
{ (void)user; g_frames++; g_last = *f; }

/* bits → 上升沿绝对时刻（quarter-µs），转相邻间隔 tick（四舍五入），
 * 末尾追加长隔关 burst。 */
static size_t build_deltas(const uint8_t *frame, int msgbits,
                           int jitter_qus, uint32_t *out, size_t cap)
{
    uint32_t rise[128]; size_t nr = 0;
    rise[nr++] = 0; rise[nr++] = 4;                    /* preamble: 0,1.0,3.5,4.5µs */
    rise[nr++] = 14; rise[nr++] = 18;
    for (int k = 0; k < msgbits; k++) {
        int bit = (frame[k / 8] >> (7 - (k % 8))) & 1;
        rise[nr++] = (uint32_t)(32 + 4 * k + (bit ? 0 : 2));   /* 8µs + 1µs·k (+0.5µs) */
    }
    if (jitter_qus)                                    /* 确定性抖动：±交替 */
        for (size_t i = 4; i < nr; i++)
            rise[i] += (uint32_t)(((int)i % 2) ? jitter_qus : -jitter_qus);
    size_t n = 0;
    for (size_t i = 1; i < nr && n < cap; i++)
        out[n++] = (uint32_t)(((uint64_t)(rise[i] - rise[i-1]) * TICK_HZ
                               + 2000000u) / 4000000u);
    if (n < cap) out[n++] = LONG_GAP;                  /* 关 burst 终止符 */
    return n;
}

static const uint8_t FRAME112[14] = {
    0x8D, 0x4C, 0xA1, 0xBD, 0x58, 0xBF, 0x34, 0x62,
    0x59, 0x2C, 0x69, 0x8A, 0xD5, 0x3B };
static const uint8_t FRAME56[7] = { 0x5D, 0x4C, 0xA1, 0xBD, 0x00, 0x00, 0x00 };

int main(void)
{
    uint32_t d[256];

    /* 1. 112-bit 理想帧。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_deltas(FRAME112, 112, 0, d, 256);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(g_last.nbits == 112, "nbits=%u\n", g_last.nbits);
        CHECK(memcmp(g_last.frame, FRAME112, 14) == 0, "frame bytes\n");
        CHECK(g_last.start_tick == 0, "start_tick=%llu\n", (unsigned long long)g_last.start_tick);
        CHECK(m.preamble_hits == 1 && m.frames_112 == 1, "stats\n");
    }

    /* 2. ±0.25µs 抖动（1 quarter-µs，F5 的 1ms RC 门限下沿抖动的量级）仍可解。 */
    {
        g_frames = 0;
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        size_t n = build_deltas(FRAME112, 112, 1, d, 256);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
    }

    /* 3. 56-bit 帧（DF11）。 */
    {
        g_frames = 0;
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        size_t n = build_deltas(FRAME56, 56, 0, d, 256);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(g_last.nbits == 56, "nbits=%u\n", g_last.nbits);
        CHECK(memcmp(g_last.frame, FRAME56, 7) == 0, "frame bytes\n");
    }

    /* 4. 两连帧（中间 100µs 间隔）都解出；间隔小于阈值不断 burst 也不影响。 */
    {
        g_frames = 0;
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        size_t n1 = build_deltas(FRAME112, 112, 0, d, 128);
        size_t n2 = build_deltas(FRAME56, 56, 0, d + n1 + 1, 128);
        d[n1] = US(100);                              /* 100µs 帧间隔 */
        modes_edge_feed(&m, d, n1 + 1 + n2);
        CHECK(g_frames == 2, "frames=%d\n", g_frames);
    }

    /* 5. 噪声 burst（间隔乱序）不产帧、计 dropped_noise，且之后的好帧仍可解。 */
    {
        g_frames = 0;
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        uint32_t noise[8] = { US(1), US(9), US(0.4), US(3), US(1), US(2.9),
                              US(1), LONG_GAP };
        modes_edge_feed(&m, noise, 8);
        size_t n = build_deltas(FRAME112, 112, 0, d, 256);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(m.dropped_noise >= 1, "noise=%u\n", m.dropped_noise);
    }

    /* 6. 超长空闲（饱和值）只是关 burst，不产帧。 */
    {
        g_frames = 0;
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        uint32_t idle[3] = { 0xFFFFFFFFu, LONG_GAP, LONG_GAP };
        modes_edge_feed(&m, idle, 3);
        CHECK(g_frames == 0 && m.frames_112 == 0, "idle produced frames\n");
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
