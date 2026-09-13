/*
 * test_edge_consume.c — edge_cap → modes_edge 的断点决策 host 单测。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_edge_consume \
 *      firmware/test/test_edge_consume.c firmware/rp2040/modes_edge.c \
 *   && /tmp/test_edge_consume
 *
 * 盯的是一条没有直接症状的慢性失效：RXSTALL 丢沿后如果不 modes_edge_reset，
 * 后续边沿的奇偶极性整体反相，解码器要一直错到下一次 >5µs 长隔才自然重锚。
 * 噪声密集时长隔极稀少 —— 表现为「链路活着、门限怎么调都收不到帧」。
 *
 * test_modes_edge.c 用例 6 喂的是单帧，丢沿后 0 帧被当作正确行为；用例 15
 * 证明了 reset 能救回来，但没有人验证生产代码在 RXSTALL 路径上**调没调**。
 * 本文件补的就是这一段：端到端跑一遍消费循环，断言丢沿之后的下一帧仍然
 * 解得出，同时锁定「没丢沿时不得无谓 reset」的反向约束。
 *
 * 构造器与 test_modes_edge.c 同源（真实波形的全部边沿间隔），刻意复制而不
 * 抽公共头：测试文件保持自解释、可单独 cc 一条命令跑通，是这套测试既有的
 * 约定。
 */
#include "edge_consume.h"
#include "modes_edge.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

#define TICK_HZ 62500000u          /* SM 125MHz / 2（每迭代 2 周期） */
#define US(x)   ((uint32_t)((double)(x) * TICK_HZ / 1000000.0))
#define LONG_GAP US(100.0)         /* >5µs 的帧间长隔 */
#define QUS(x)  ((uint32_t)((x) * 4.0))

static int g_frames;
static modes_edge_frame_t g_last;
static void cb(const modes_edge_frame_t *f, void *user)
{ (void)user; g_frames++; g_last = *f; }

/* bits → 全边沿相邻间隔（tick）；脉冲宽 0.5µs，相邻脉冲首尾相接要融合。
 * with_gap=0 时**不**追加长隔终止符 —— 本文件要构造「中间没有静默」的
 * 连续流，长隔会让奇偶自然重锚，把要测的失效掩盖掉。 */
static size_t build_edges(const uint8_t *frame, int msgbits, int with_gap,
                          uint32_t *out, size_t cap)
{
    uint32_t istart[300], iend[300]; size_t ni = 0;
    uint32_t rise[300]; size_t nr = 0;
    rise[nr++] = 0; rise[nr++] = QUS(1.0);
    rise[nr++] = QUS(3.5); rise[nr++] = QUS(4.5);
    for (int k = 0; k < msgbits; k++) {
        int bit = (frame[k / 8] >> (7 - (k % 8))) & 1;
        rise[nr++] = (uint32_t)((8.0 + 1.0 * k + (bit ? 0.0 : 0.5)) * 4.0);
    }
    for (size_t i = 0; i < nr; i++) {
        uint32_t s = rise[i], e = rise[i] + QUS(0.5);
        if (ni && s <= iend[ni - 1]) {
            if (e > iend[ni - 1]) iend[ni - 1] = e;
        } else {
            istart[ni] = s; iend[ni] = e; ni++;
        }
    }
    uint32_t tog[300]; size_t nt = 0;
    for (size_t i = 0; i < ni; i++) { tog[nt++] = istart[i]; tog[nt++] = iend[i]; }
    size_t n = 0;
    for (size_t i = 1; i < nt && n < cap; i++)
        out[n++] = (uint32_t)((uint64_t)(tog[i] - tog[i-1]) * TICK_HZ / 4000000u);
    if (with_gap && n < cap) out[n++] = LONG_GAP;
    return n;
}

static const uint8_t FRAME112[14] = {
    0x8D, 0x4C, 0xA1, 0xBD, 0x58, 0xBF, 0x34, 0x62,
    0x59, 0x2C, 0x69, 0x8A, 0xD5, 0x3B
};

/* adsb1090.c core1 消费循环的等价体：决策 → 上报 → reset → 喂。
 * 生产代码改动后这里必须跟着改，否则测的就不是生产行为了。 */
static void consume(modes_edge_t *m, const uint32_t *d, size_t n,
                    bool disc, uint32_t ovr, uint32_t *last_ovr)
{
    const edge_consume_act_t a = edge_consume_decide(disc, ovr, *last_ovr);
    if (a.degrade) modes_edge_mark_degraded(m);
    *last_ovr = ovr;
    if (a.reset) modes_edge_reset(m);
    modes_edge_feed(m, d, n);
}

int main(void)
{
    uint32_t d[512], e[512];

    /* 1. 决策表。丢沿数量不可知 ⇒ overruns 一动就必须重锚。 */
    {
        edge_consume_act_t a;
        a = edge_consume_decide(false, 0, 0);
        CHECK(!a.reset && !a.degrade, "流连续时不该有任何动作\n");
        a = edge_consume_decide(true, 0, 0);
        CHECK(a.reset && a.degrade, "disc 必须 reset + degrade\n");
        a = edge_consume_decide(false, 1, 0);
        CHECK(a.reset, "RXSTALL 丢沿必须 reset 以重锚奇偶\n");
        CHECK(a.degrade, "RXSTALL 丢沿必须如实上报退化\n");
        /* 计数器回绕：!= 而不是 > ，否则回绕那一次会被漏判 */
        a = edge_consume_decide(false, 0, 0xFFFFFFFFu);
        CHECK(a.reset, "overruns 回绕也是丢沿，必须 reset\n");
    }

    /* 2. 核心回归：RXSTALL 之后的下一帧必须还能解出。
     *
     * 第一块喂奇数个边沿的残片（模拟丢沿后剩下的半截 burst），随后 RXSTALL
     * 使 overruns 递增，第二块是一整帧。两块之间**没有长隔**——真实的噪声
     * 环境正是这样，所以不能指望自然重锚。
     *
     * 修复前：第二块被拼进同一个 burst，帧的 preamble 落在奇数（下降）沿
     * 位上，滑窗扫不到 → g_frames == 0。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        uint32_t last = 0;
        g_frames = 0;

        size_t nfrag = build_edges(FRAME112, 112, 0, d, 512);
        (void)nfrag;
        consume(&m, d, 39, false, 0, &last);          /* 奇数截断的残片 */

        size_t n = build_edges(FRAME112, 112, 1, e, 512);
        consume(&m, e, n, false, 1, &last);           /* RXSTALL：ovr 0→1 */

        CHECK(g_frames == 1,
              "RXSTALL 后的下一帧丢失，frames=%d（奇偶未重锚）\n", g_frames);
        CHECK(g_frames == 0 || memcmp(g_last.frame, FRAME112, 14) == 0,
              "重锚后解出的帧内容不对\n");
    }

    /* 3. 反向约束：没有丢沿就不许 reset，否则跨块的帧会被砍掉。
     *
     * edge_cap 按 256 条一块发布，一帧的边沿完全可能跨两块。这里把一帧从
     * 中间切开分两次喂，overruns 全程不变 —— 必须照常解出。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        uint32_t last = 7;                            /* 非零起点，验相等判断 */
        g_frames = 0;

        size_t n = build_edges(FRAME112, 112, 1, d, 512);
        size_t cut = n / 2;
        consume(&m, d, cut, false, 7, &last);
        consume(&m, d + cut, n - cut, false, 7, &last);

        CHECK(g_frames == 1, "跨块帧被无谓 reset 砍掉了，frames=%d\n", g_frames);
        CHECK(g_frames == 0 || memcmp(g_last.frame, FRAME112, 14) == 0,
              "跨块帧内容不对\n");
    }

    /* 4. disc 与 RXSTALL 同时发生：只 reset 一次，帧照常解出。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        uint32_t last = 0;
        g_frames = 0;

        size_t nf = build_edges(FRAME112, 112, 0, d, 512);
        (void)nf;
        consume(&m, d, 39, false, 0, &last);
        size_t n = build_edges(FRAME112, 112, 1, e, 512);
        consume(&m, e, n, true, 3, &last);            /* disc + ovr 双触发 */

        CHECK(g_frames == 1, "disc+RXSTALL 同发时丢帧，frames=%d\n", g_frames);
    }

    /* 5. 退化标志是 sticky：reset 不清除（丢失时长不可回补）。 */
    {
        modes_edge_t m; modes_edge_init(&m, TICK_HZ, cb, NULL);
        uint32_t last = 0;
        g_frames = 0;
        size_t n = build_edges(FRAME112, 112, 1, d, 512);
        consume(&m, d, n, false, 1, &last);           /* 触发一次退化 */
        CHECK(modes_edge_time_degraded(&m), "退化标志应已置位\n");
        consume(&m, d, n, false, 1, &last);           /* 之后一切正常 */
        CHECK(modes_edge_time_degraded(&m),
              "退化是 sticky，不该被后续正常块清掉\n");
    }

    printf(g_fail ? "test_edge_consume: %d FAILED\n" : "test_edge_consume: OK\n",
           g_fail);
    return g_fail ? 1 : 0;
}
