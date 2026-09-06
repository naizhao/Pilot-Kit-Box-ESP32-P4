/*
 * test_modes_edge.c — 双沿间隔流 → Mode-S 帧重构的 host 单测（R11）。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_modes_edge \
 *      firmware/test/test_modes_edge.c firmware/rp2040/modes_edge.c \
 *   && /tmp/test_modes_edge
 *
 *   ASan/UBSan 变体（audit round 4 Fix 2 的回归用例 12 以此为探针：
 *   burst_n 打满 MAX_EDGES 时 t[burst_n] 是栈越界写，-fsanitize=address
 *   必须零报告）：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/rp2040 -o /tmp/test_modes_edge_asan \
 *      firmware/test/test_modes_edge.c firmware/rp2040/modes_edge.c \
 *   && /tmp/test_modes_edge_asan
 *
 * 构造器按真实波形生成**全部边沿**（上升+下降，0.5µs 脉宽）的相邻间隔，
 * 与 edgecap PIO 的双沿捕获同构。判据：mode-s.c:708-715（0.5µs 脉冲 @
 * 0/1.0/3.5/4.5µs；1µs/位、脉冲在位首=1/位中=0）。
 *
 * 抖动包络（R11 决策记录）：半位中心采样对边沿位置抖动的安全预算是
 * ±0.5 qus；±1 qus 的抖动会把部分采样点推到边沿上，此时解码器必须
 * **安全丢弃**（两半位同电平 → INVALID），绝不输出错位帧——抖动用例
 * 断言的就是这条安全性质，而不是"必须解出"。
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
#define QUS(x)  ((uint32_t)((x) * 4.0))   /* quarter-µs → tick */

static int g_frames;
static modes_edge_frame_t g_last;
static void cb(const modes_edge_frame_t *f, void *user)
{ (void)user; g_frames++; g_last = *f; }

/* bits → 全边沿相邻间隔（tick）。脉冲宽 0.5µs（2 qus）；jitter_qus 为
 * ±交替抖动幅值（作用于每个物理边沿）。末尾追加长隔关闭 burst。
 * 关键：相邻脉冲首尾相接（bit0→bit1）必须**合并成单个长高电平**——
 * 物理上融合处不存在边沿；否则会注入零间隔假边沿、破坏电平重建。 */
static size_t build_edges(const uint8_t *frame, int msgbits,
                          int jitter_qus, uint32_t *out, size_t cap)
{
    uint32_t istart[300], iend[300]; size_t ni = 0;
    uint32_t rise[300]; size_t nr = 0;
    rise[nr++] = 0; rise[nr++] = QUS(1.0);          /* preamble 0/1.0/3.5/4.5µs */
    rise[nr++] = QUS(3.5); rise[nr++] = QUS(4.5);
    for (int k = 0; k < msgbits; k++) {
        int bit = (frame[k / 8] >> (7 - (k % 8))) & 1;
        rise[nr++] = (uint32_t)((8.0 + 1.0 * k + (bit ? 0.0 : 0.5)) * 4.0);
    }
    /* 脉冲区间（0.5µs 宽）+ 相邻合并（融合） */
    for (size_t i = 0; i < nr; i++) {
        uint32_t s = rise[i], e = rise[i] + QUS(0.5);
        if (ni && s <= iend[ni - 1]) {
            if (e > iend[ni - 1]) iend[ni - 1] = e;         /* 融合 */
        } else {
            istart[ni] = s; iend[ni] = e; ni++;
        }
    }
    /* 边界 → 边沿间隔（含 ±交替抖动，作用于物理边沿本身） */
    uint32_t tog[300]; size_t nt = 0;
    for (size_t i = 0; i < ni; i++) {
        tog[nt++] = istart[i]; tog[nt++] = iend[i];
    }
    if (jitter_qus)
        for (size_t i = 0; i < nt; i++)
            tog[i] += (uint32_t)(((int)i % 2) ? jitter_qus : -jitter_qus);
    size_t n = 0;
    for (size_t i = 1; i < nt && n < cap; i++)
        out[n++] = (uint32_t)((uint64_t)(tog[i] - tog[i-1]) * TICK_HZ / 4000000u);
    if (n < cap) out[n++] = LONG_GAP;             /* 关 burst 终止符 */
    return n;
}

static const uint8_t FRAME112[14] = {
    0x8D, 0x4C, 0xA1, 0xBD, 0x58, 0xBF, 0x34, 0x62,
    0x59, 0x2C, 0x69, 0x8A, 0xD5, 0x3B
};
static const uint8_t FRAME56_ALT[7] = {         /* 01010101… DF=10，全融合串 */
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55
};
static const uint8_t FRAME56_HEAD0[7] = {       /* DF11=01011：帧首 bit0，帧首融合 */
    0x5D, 0x4C, 0xA1, 0xBD, 0x00, 0x00, 0x00
};

int main(void)
{
    uint32_t d[512];

    /* 1. 理想 112-bit 帧（含 0→1 融合与全部四种相邻组合）。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_edges(FRAME112, 112, 0, d, 512);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(g_last.nbits == 112, "nbits=%u\n", g_last.nbits);
        CHECK(memcmp(g_last.frame, FRAME112, 14) == 0, "frame bytes\n");
        CHECK(m.preamble_hits == 1 && m.frames_112 == 1, "stats\n");
        /* 钉住该向量确实覆盖四种相邻组合与至少一个 0→1 融合对 */
        int have00 = 0, have01 = 0, have10 = 0, have11 = 0;
        for (int k = 0; k + 1 < 112; k++) {
            int a = (FRAME112[k / 8] >> (7 - (k % 8))) & 1;
            int b = (FRAME112[(k+1) / 8] >> (7 - ((k+1) % 8))) & 1;
            if (a && !b) have10 = 1; if (!a && b) have01 = 1;
            if (!a && !b) have00 = 1; if (a && b) have11 = 1;
        }
        CHECK(have00 && have01 && have10 && have11, "coverage %d%d%d%d\n",
              have00, have01, have10, have11);
    }

    /* 2. 全融合串 56-bit（0x55…，DF=10；帧首即为 bit0 → 帧首融合）。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_edges(FRAME56_ALT, 56, 0, d, 512);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(g_last.nbits == 56, "nbits=%u\n", g_last.nbits);
        CHECK(memcmp(g_last.frame, FRAME56_ALT, 7) == 0, "alt bytes\n");
    }

    /* 3. DF11 帧首 bit0（帧首融合 + 帧首 10 组合）。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_edges(FRAME56_HEAD0, 56, 0, d, 512);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(memcmp(g_last.frame, FRAME56_HEAD0, 7) == 0, "head0 bytes\n");
    }

    /* 4. 脉宽展宽 ±1 qus（上升沿准、下降沿抖）：仍是确定性正确。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_edges(FRAME112, 112, 0, d, 512);
        for (size_t i = 1; i + 1 < n; i += 2)      /* 每个下降沿 ±1 tick */
            d[i] += (uint32_t)(((int)i % 2) ? 8 : -8);   /* ±0.125µs */
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(memcmp(g_last.frame, FRAME112, 14) == 0, "broadened bytes\n");
    }

    /* 5. 数据边沿 ±1 qus 抖动：超出安全包络（±0.5 qus）→ 必须**安全丢弃**
     * （两半位同电平 → INVALID），绝不输出错位帧。具体帧数取决于量化
     * 碰撞位置——断言安全性质而非具体帧数。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_edges(FRAME112, 112, 1, d, 512);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 0 || memcmp(g_last.frame, FRAME112, 14) == 0,
              "unsafe frame emitted\n");
    }

    /* 6. 丢沿致奇偶错乱：删去一个中间 delta（模拟 FIFO 丢沿）→ 后续全部
     * 边沿电平翻转 → 必须安全丢弃，不产帧。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_edges(FRAME112, 112, 0, d, 512);
        for (size_t i = 40; i + 1 < n; i++) d[i] = d[i + 1];   /* 抽掉一个沿 */
        modes_edge_feed(&m, d, n - 1);
        CHECK(g_frames == 0, "garbled parity produced %d frames\n", g_frames);
        CHECK(m.dropped_noise >= 1, "noise=%u\n", m.dropped_noise);
    }

    /* 7. 两连帧（间隔 100µs）都解出。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n1 = build_edges(FRAME112, 112, 0, d, 256);
        size_t n2 = build_edges(FRAME56_HEAD0, 56, 0, d + n1, 256);
        modes_edge_feed(&m, d, n1 + n2);
        CHECK(g_frames == 2, "frames=%d\n", g_frames);
        CHECK(g_last.nbits == 56, "last nbits=%u\n", g_last.nbits);
    }

    /* 8. 噪声 burst 不产帧、计数；随后好帧仍可解。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        uint32_t noise[10] = { US(1), US(0.4), US(0.6), US(0.3), US(0.5),
                               US(0.4), US(0.6), US(0.3), US(0.5), LONG_GAP };
        modes_edge_feed(&m, noise, 10);
        size_t n = build_edges(FRAME112, 112, 0, d, 512);
        modes_edge_feed(&m, d, n);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(m.dropped_noise >= 1, "noise=%u\n", m.dropped_noise);
    }

    /* 9. 空闲饱和值只是关 burst，不产帧。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        uint32_t idle[3] = { 0xFFFFFFFFu, LONG_GAP, LONG_GAP };
        modes_edge_feed(&m, idle, 3);
        CHECK(g_frames == 0 && m.frames_112 == 0, "idle produced frames\n");
    }

    /* 10. 噪声前缀紧贴帧头（同一 burst）：burst 首沿是 0.5µs 噪声脉冲，
     * 1.5µs 后跟合法 112-bit 帧。preamble 锚定必须滑到后续上升沿候选，
     * 而不是按 burst 首沿判废丢掉整帧——审计实测：修复前 frames=0
     * （dropped_noise 吞帧）。旧噪声用例（case 8）噪声与帧隔 100µs、
     * 分属两个 burst，从未覆盖这条。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        size_t n = build_edges(FRAME112, 112, 0, d, 512);
        uint32_t all[512];
        all[0] = US(0.5);                     /* 噪声脉冲宽 */
        all[1] = US(1.5);                     /* 噪声脉冲尾 → 帧 preamble 首沿 */
        memcpy(all + 2, d, (n - 1) * sizeof(uint32_t));  /* 去掉终止符 */
        size_t total = 2 + (n - 1);
        all[total++] = LONG_GAP;
        modes_edge_feed(&m, all, total);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(memcmp(g_last.frame, FRAME112, 14) == 0, "frame bytes\n");
        /* start_tick 必须落在 preamble 首沿（跳过噪声前缀），不是 burst
         * 首沿（=0）。US() 宏对小延时向下取整，期望值按注入的实际
         * 间隔之和算。 */
        CHECK(g_last.start_tick == (uint64_t)(all[0] + all[1]), "start_tick=%llu\n",
              (unsigned long long)g_last.start_tick);
        CHECK(m.preamble_hits == 1 && m.frames_112 == 1, "stats\n");
    }

    /* 11. 纯噪声 burst（无 preamble 候选可滑）仍按噪声记账、不产帧：
     * 滑窗不得把 dropped_noise 的口径改坏。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        uint32_t noise[10] = { US(1), US(0.4), US(0.6), US(0.3), US(0.5),
                               US(0.4), US(0.6), US(0.3), US(0.5), LONG_GAP };
        modes_edge_feed(&m, noise, 10);
        CHECK(g_frames == 0, "noise produced %d frames\n", g_frames);
        CHECK(m.dropped_noise >= 1, "noise=%u\n", m.dropped_noise);
    }

    /* 12. 噪声洪流打满容量（audit round 4 Fix 2 回归，ASan 探针）：257 个
     *     连续 0.5µs 短间隔使 burst_n 恰达 MAX_EDGES(256)，第 257 个间隔
     *     走 feed 的溢出路径（edge_overruns++ → burst_emit），emit 内
     *     nedges = 257、t[burst_n=256] 必须落在 +1 槽内——旧代码
     *     t[MAX_EDGES] 是栈越界写，ASan 变体下当场爆。行为合同：记账、
     *     不产帧、不崩。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        uint32_t flood[258];
        for (int i = 0; i < 257; i++) flood[i] = US(0.5);
        flood[257] = LONG_GAP;                /* 关 burst（此时已空转） */
        modes_edge_feed(&m, flood, 258);
        CHECK(g_frames == 0, "flood produced %d frames\n", g_frames);
        CHECK(m.edge_overruns == 1, "edge_overruns=%u\n", m.edge_overruns);
        CHECK(m.dropped_noise == MODES_EDGE_MAX_EDGES, "noise=%u\n",
              m.dropped_noise);
        CHECK(m.bursts == 1 && m.burst_n == 0, "bursts=%u n=%d\n",
              m.bursts, m.burst_n);
    }

    /* 13. 假前导后跟真帧（audit round 4 Fix 3 回归）：噪声凑出间距合格
     *     的 [4,10,4] 三连脉冲（同 burst 内），首个候选数据解码必败
     *     （两中心同电平）；滑窗必须弃暗礁、继续锁到真帧——修复前整帧
     *     被 false preamble 吞掉（frames=0）。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        g_frames = 0;
        /* 假前导：脉冲 @ 0/1.0/3.5/4.5µs（0.5µs 宽）→ 边沿 0,2,4,6,
         * 14,16,18,20 qus；其后数据区两采样点（33/35 qus）无任何边沿
         * → 候选 0 判负。真帧首沿 @9.5µs（38 qus）：与假前导末沿间隔
         * 4.5µs < 5µs，同 burst；38-18=20 qus 使候选 2/4/6 间距全废。 */
        uint32_t all[512];
        int i = 0;
        all[i++] = US(0.5); all[i++] = US(0.5); all[i++] = US(0.5);
        all[i++] = US(2.0);
        all[i++] = US(0.5); all[i++] = US(0.5); all[i++] = US(0.5);
        all[i++] = US(4.5);                   /* 假前导末沿 → 真帧首沿 */
        size_t n = build_edges(FRAME112, 112, 0, d, 512);
        memcpy(all + i, d, (n - 1) * sizeof(uint32_t));  /* 去掉终止符 */
        i += (int)(n - 1);
        all[i++] = LONG_GAP;
        modes_edge_feed(&m, all, (size_t)i);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(memcmp(g_last.frame, FRAME112, 14) == 0, "frame bytes\n");
        CHECK(m.preamble_hits == 2, "preamble_hits=%u\n", m.preamble_hits);
        CHECK(m.dropped_decode == 1, "dropped_decode=%u\n",
              m.dropped_decode);
        CHECK(m.dropped_noise == 0, "noise=%u\n", m.dropped_noise);
        CHECK(m.frames_112 == 1, "f112=%u\n", m.frames_112);
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
