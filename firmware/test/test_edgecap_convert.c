/*
 * test_edgecap_convert.c — PIO 递减计数器原值 → 真实间隔 tick 的换算 +
 * edgecap_claim()/edgecap_window_ok() 消费裁决算术（gpt-5.6-sol re-audit
 * Fix 1 回归）的单测。
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
 * claim/window_ok 覆盖（gpt-5.6-sol Fix 1；IRQ 本体无法在 host 运行——
 * 中断侧合同见 edge_cap.h，handler 只做掩码/清理/原子累加，drain 不再
 * 采样任何 DMA 硬件进度）：正常前进、零滞后、exact-lap 不误报写穿、
 * 写穿重同步（含**重同步目标恰为 0**）、回卷后 u32 模差无下溢、回卷 +
 * 写穿叠加；窗口重校验的裕度边界（裕度 = 整环 − 已复制窗口：越窗量恰
 * 等于裕度仍有效、+1 即作废）、n = 整环与 n = 0 两个端点、窗口回卷。
 */
#include "edge_cap.h"
#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

#define TICK_HZ 62500000u          /* SM 125MHz / 2（每迭代 2 周期） */
#define RING EDGE_CAP_RING_ITEMS

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

    /* ── edgecap_claim()（gpt-5.6-sol Fix 1 回归）──────────────────────
     * 环大小用真实值 EDGE_CAP_RING_ITEMS；算术对任意 ring_items 成立。 */

    /* 7. 正常前进（滞后 < 一环）→ 原样给出 avail，无 overrun。 */
    {
        edgecap_claim_t a = edgecap_claim(1000u, 0u, RING);
        CHECK(a.avail == 1000u && a.resync == 0u, "normal %u/%u\n",
              a.avail, a.resync);
    }

    /* 8. 零滞后：producer == read → avail=0（空转不报错）。 */
    {
        edgecap_claim_t a = edgecap_claim(4096u, 4096u, RING);
        CHECK(a.avail == 0u && a.resync == 0u, "zero-lag %u/%u\n",
              a.avail, a.resync);
    }

    /* 9. exact-lap（滞后恰好一整环）：avail == RING_ITEMS，**不得**误报
     *    overrun（lag > ring_items 为假）。这是合同允许的最大滞后。 */
    {
        edgecap_claim_t a = edgecap_claim(RING, 0u, RING);
        CHECK(a.avail == RING, "exact-lap avail=%u\n", a.avail);
        CHECK(a.resync == 0u, "exact-lap resync=%u\n", a.resync);
    }

    /* 10. 写穿：滞后超过一整环 → 置 resync 标志、重同步到
     *    producer_pos − ring_items（保留最新一环），avail 封顶。 */
    {
        edgecap_claim_t a = edgecap_claim(3000u, 0u, RING);
        CHECK(a.avail == RING, "overrun avail=%u\n", a.avail);
        CHECK(a.resync == 1u, "overrun resync flag=%u\n", a.resync);
        CHECK(a.resync_to == 3000u - RING, "overrun resync=%u\n",
              a.resync_to);
    }

    /* 11. 重同步目标恰为 0：producer_pos=2048、read=0xFFFFFFFF（回卷前
     *    1 项）→ 模差 2049 > 2048，重同步目标 = 2048−2048 = **0**。
     *    旧"非 0 即写穿"哨兵会把这个合法目标漏判成"无写穿"；判定只看
     *    resync 标志。 */
    {
        edgecap_claim_t a = edgecap_claim(RING, 0xFFFFFFFFu, RING);
        CHECK(a.resync == 1u, "zero-target flag=%u\n", a.resync);
        CHECK(a.resync_to == 0u, "zero-target=%u\n", a.resync_to);
        CHECK(a.avail == RING, "zero-target avail=%u\n", a.avail);
    }

    /* 12. 回卷无下溢：producer_pos 回卷到 100，read_pos 停在回卷前的
     *    0xFFFFFF00 → u32 模差必须还原真实差 356，而不是无符号下溢的
     *    "巨可用量"。 */
    {
        edgecap_claim_t a = edgecap_claim(100u, 0xFFFFFF00u, RING);
        CHECK(a.avail == 356u, "wrap avail=%u\n", a.avail);
        CHECK(a.resync == 0u, "wrap resync=%u\n", a.resync);
    }

    /* 13. 回卷 + 写穿叠加：read 在 0xFFFFF800、producer_pos 回卷到 1000
     *    → 真实差 3072 > 2048 → 重同步，且 resync 目标本身也允许落在
     *    回卷后的低地址段（1000−2048 模 2^32 = 0xFFFFFB88）。 */
    {
        edgecap_claim_t a = edgecap_claim(1000u, 0xFFFFF800u, RING);
        CHECK(a.avail == RING, "wrap+ovr avail=%u\n", a.avail);
        CHECK(a.resync == 1u, "wrap+ovr flag=%u\n", a.resync);
        CHECK(a.resync_to == 1000u - RING, "wrap+ovr resync=%u\n",
              a.resync_to);
    }

    /* ── edgecap_window_ok()（gpt-5.6-sol C2 回归）─────────────────────
     * 裕度 = 整环 − 已复制窗口：复制期间生产者不得跨过窗口起点 + 一环。 */

    /* 14. 裕度边界（有效）：s=0、n=256 → 越窗量 P−(s+n) 恰好 = RING−n
     *    （即 P = s+RING）仍有效——窗口首槽的第二轮写入尚未被记账完成。 */
    {
        CHECK(edgecap_window_ok(RING - 256u, 0u, 256u, RING) != 0u,
              "margin-ok\n");
        CHECK(edgecap_window_ok(RING, 0u, 256u, RING) != 0u,
              "margin-eq\n");
    }

    /* 15. 裕度越界（作废）：越窗量 = RING−n + 1（即 P = s+RING+1）——
     *    生产者已记账越过窗口首槽的重写点，本批必须作废。 */
    {
        CHECK(edgecap_window_ok(RING + 1u, 0u, 256u, RING) == 0u,
              "margin-violate\n");
    }

    /* 16. n = 整环端点：全环窗口只在 P == s+RING（等号）时有效；P 再进
     *    1 即作废。 */
    {
        CHECK(edgecap_window_ok(RING, 0u, RING, RING) != 0u, "full-ring\n");
        CHECK(edgecap_window_ok(RING + 1u, 0u, RING, RING) == 0u,
              "full-ring+1\n");
    }

    /* 17. n = 0 端点：空批与 claim 的滞后合同一致（P ≤ s+RING 有效）。 */
    {
        CHECK(edgecap_window_ok(RING, 0u, 0u, RING) != 0u, "zero-n\n");
        CHECK(edgecap_window_ok(RING + 1u, 0u, 0u, RING) == 0u,
              "zero-n+1\n");
    }

    /* 18. 窗口回卷：s=0xFFFFFF00、n=256 → 窗口终点跨过 2^32 回到 0；
     *    P=1792（= s+RING 模 2^32）恰在等号上有效，P=1793 作废。 */
    {
        CHECK(edgecap_window_ok(1792u, 0xFFFFFF00u, 256u, RING) != 0u,
              "window-wrap\n");
        CHECK(edgecap_window_ok(1793u, 0xFFFFFF00u, 256u, RING) == 0u,
              "window-wrap+1\n");
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
