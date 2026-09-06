/*
 * test_edgecap_convert.c — PIO 递减计数器原值 → 真实间隔 tick 的换算 +
 * edgecap_avail() 生产/消费裁决算术（audit round 4 Fix 1 回归）的单测。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_edgecap_convert \
 *      firmware/test/test_edgecap_convert.c \
 *   && /tmp/test_edgecap_convert
 *
 * 只包含 edge_cap.h：它必须保持无 pico 依赖（本文件能独立编译即证明）。
 * 合同（edge_cap.pio 逐周期推导）：上升沿检测后 edge 块固定 4 拍
 * （mov isr/push/set x/mov x,~x）无递减；低/高相位每迭代 2 拍、1 次递减，
 * 高→低转换 2 拍无递减。相邻上升沿检测之间 cycles = 2×(PRELOAD − raw) + 6，
 * 1 tick = 2 SM 周期 → ticks = (PRELOAD − raw) + 3。
 *
 * edgecap_avail() 覆盖（DMA IRQ 单写者合同的消费端算术，IRQ 本体无法在
 * host 运行——中断侧合同见 edge_cap.h，handler 只做掩码/清理/原子累加）：
 * 正常前进、exact-lap 不误报写穿、写穿重同步、producer_pos 回卷后 u32
 * 模差无下溢。
 */
#include "edge_cap.h"
#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

#define TICK_HZ 62500000u          /* SM 125MHz / 2（每迭代 2 周期） */

int main(void)
{
    /* 1. raw 贴着 PRELOAD → 极小间隔：0/1 次递减。 */
    CHECK(edgecap_raw_to_ticks(EDGE_CAP_PRELOAD) == 3u, "raw=PRELOAD\n");
    CHECK(edgecap_raw_to_ticks(EDGE_CAP_PRELOAD - 1u) == 4u, "raw=PRELOAD-1\n");

    /* 2. 斜率精确 1 tick/递减（间隔可分辨率 = 1 tick）。 */
    CHECK(edgecap_raw_to_ticks(EDGE_CAP_PRELOAD - 2u) ==
          edgecap_raw_to_ticks(EDGE_CAP_PRELOAD - 1u) + 1u, "slope\n");

    /* 3. 真实 1µs 比特周期（mode-s.c:708-715）：62.5MHz 下 = 62.5 tick
     *    = 125 SM 周期；沿检测只能落在奇数周期 → raw = PRELOAD−59
     *    （125 = 2×59+7 拍）换算 62 tick，qus 往返 round(62×4/62.5) = 4 ✓；
     *    相邻量化 raw = PRELOAD−60 → 63 tick 同样落到 4 qus。 */
    {
        uint32_t t62 = edgecap_raw_to_ticks(EDGE_CAP_PRELOAD - 59u);
        uint32_t t63 = edgecap_raw_to_ticks(EDGE_CAP_PRELOAD - 60u);
        CHECK(t62 == 62u, "1us ticks=%u\n", t62);
        CHECK(t63 == 63u, "1us next ticks=%u\n", t63);
        for (uint32_t t = t62; t <= t63; t++) {
            uint32_t qus = (uint32_t)(((uint64_t)t * 4000000u + TICK_HZ / 2u) / TICK_HZ);
            CHECK(qus == 4u, "t=%u qus=%u\n", t, qus);   /* 1.0µs = 4 qus ±1 容差内 */
        }
    }

    /* 4. 6µs 空闲（>5µs burst 阈值）：375 次递减 → raw = PRELOAD−375
     *    → 378 tick > 312 tick（5µs @62.5MHz 的 burst_gap_ticks=312）。 */
    {
        uint32_t t = edgecap_raw_to_ticks(EDGE_CAP_PRELOAD - 375u);
        CHECK(t == 378u, "6us idle ticks=%u\n", t);
        CHECK(t > 312u, "6us idle below threshold\n");
    }

    /* 5. 帧内最长沿间隔 4.0µs（preamble 末沿 4.5µs → 首数据脉冲 8.5µs）
     *    必须留在阈值之下：247 次递减 → 250 tick = 4.0µs < 312。 */
    {
        uint32_t t = edgecap_raw_to_ticks(EDGE_CAP_PRELOAD - 247u);
        CHECK(t == 250u, "4us in-frame ticks=%u\n", t);
        CHECK(t <= 312u, "4us in-frame above threshold\n");
    }

    /* 6. 饱和（X 停在 0，raw=0）= 空闲标记：PRELOAD+3，不回绕，
     *    远大于任何合法间隔（含 100µs 帧间隔 = 6.25e6 tick）。 */
    CHECK(edgecap_raw_to_ticks(0u) == 0xFFFFFFE3u, "saturated value\n");
    CHECK(edgecap_raw_to_ticks(0u) > 6250000u, "saturated vs 100us\n");

    /* ── edgecap_avail()（audit round 4 Fix 1 回归）────────────────────
     * 环大小用真实值 EDGE_CAP_RING_ITEMS；算术对任意 ring_items 成立。 */

    /* 7. 正常前进：producer 领先未满一环 → 原样给出 avail，无 overrun。 */
    {
        edgecap_avail_t a = edgecap_avail(1000u, 0u, EDGE_CAP_RING_ITEMS);
        CHECK(a.avail == 1000u && a.overrun_resync_to == 0u, "normal %u/%u\n",
              a.avail, a.overrun_resync_to);
    }

    /* 8. 零滞后：producer == read → avail=0（旧行为：空转不报错）。 */
    {
        edgecap_avail_t a = edgecap_avail(4096u, 4096u, EDGE_CAP_RING_ITEMS);
        CHECK(a.avail == 0u && a.overrun_resync_to == 0u, "zero-lag %u/%u\n",
              a.avail, a.overrun_resync_to);
    }

    /* 9. exact-lap（旧 TRANS_COUNT 方案的盲区、race 的触发邻域）：
     *    producer 恰好领先一整环 → avail == RING_ITEMS，**不得**误报
     *    overrun（total > ring_items 为假）。 */
    {
        edgecap_avail_t a = edgecap_avail(EDGE_CAP_RING_ITEMS, 0u,
                                          EDGE_CAP_RING_ITEMS);
        CHECK(a.avail == EDGE_CAP_RING_ITEMS, "exact-lap avail=%u\n", a.avail);
        CHECK(a.overrun_resync_to == 0u, "exact-lap resync=%u\n",
              a.overrun_resync_to);
    }

    /* 10. 写穿：producer 领先超过一整环 → 重同步到 producer−ring_items
     *     （保留最新一环），avail 封顶 ring_items。 */
    {
        edgecap_avail_t a = edgecap_avail(3000u, 0u, EDGE_CAP_RING_ITEMS);
        CHECK(a.avail == EDGE_CAP_RING_ITEMS, "overrun avail=%u\n", a.avail);
        CHECK(a.overrun_resync_to == 3000u - EDGE_CAP_RING_ITEMS,
              "overrun resync=%u\n", a.overrun_resync_to);
    }

    /* 11. 回卷无下溢（单写者合同的数学保证）：producer_pos 已按 2^32
     *     回卷到 100，read_pos 停在回卷前的 0xFFFFFF00 → u32 模差必须
     *     还原真实差 356，而不是无符号下溢的"巨可用量"。 */
    {
        edgecap_avail_t a = edgecap_avail(100u, 0xFFFFFF00u,
                                          EDGE_CAP_RING_ITEMS);
        CHECK(a.avail == 356u, "wrap avail=%u\n", a.avail);
        CHECK(a.overrun_resync_to == 0u, "wrap resync=%u\n",
              a.overrun_resync_to);
    }

    /* 12. 回卷 + 写穿叠加：read 在 0xFFFFF800、producer 回卷到 1000 →
     *     真实差 3072 > 2048 → 重同步，且 resync 目标本身也允许落在
     *     回卷后的低地址段。 */
    {
        edgecap_avail_t a = edgecap_avail(1000u, 0xFFFFF800u,
                                          EDGE_CAP_RING_ITEMS);
        CHECK(a.avail == EDGE_CAP_RING_ITEMS, "wrap+ovr avail=%u\n", a.avail);
        CHECK(a.overrun_resync_to == 1000u - EDGE_CAP_RING_ITEMS,
              "wrap+ovr resync=%u\n", a.overrun_resync_to);
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
