/*
 * test_edgecap_convert.c — PIO 递减计数器原值 → 真实间隔 tick 的换算 +
 * edge_cap_queue 块队列所有权协议（block-queue 重设计）的单测。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_edgecap_convert \
 *      firmware/test/test_edgecap_convert.c \
 *      firmware/rp2040/edge_cap_queue.c \
 *   && /tmp/test_edgecap_convert
 *
 * 只包含 edge_cap.h / edge_cap_queue.h：两者必须保持无 pico 依赖（本文件
 * 能独立编译即证明）。换算合同（edge_cap.pio 逐周期推导）：上升沿检测后
 * edge 块固定 4 拍（mov isr/push/set x/mov x,~x）无递减；低/高相位每迭代
 * 2 拍、1 次递减，高→低转换 2 拍无递减。相邻上升沿检测之间 cycles =
 * 2×(PRELOAD − raw) + 6，1 tick = 2 SM 周期 → ticks = (PRELOAD − raw) + 3。
 *
 * 块队列覆盖（block-queue 重设计：IRQ 生产者 / drain 消费者的显式所有权
 * 协议，合同见 edge_cap_queue.h；旧的 claim/window_ok 消费裁决算术随
 * 共享环一起废除——显式所有权下"写穿重检"不再存在）：空队列边界、满容量
 * N−1 顺序 fill→drain 覆盖全部槽位、生产者绕圈撞上消费者持有块被拒且原块
 * 完好（C1 类性质，由构造成立）、消费者释放重新解锁生产者、多轮交错 FIFO
 * 序、填满后取一放一。台架严格模拟 target 驱动策略：DMA 只在运行时填；
 * 满环拒绝后只有消费者释放过块才允许重启（安全重启前提，见
 * edge_cap_queue.h 容量 N−1 论证）。
 */
#include "edge_cap.h"
#include "edge_cap_queue.h"
#include <stdatomic.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

#define TICK_HZ 62500000u          /* SM 125MHz / 2（每迭代 2 周期） */
#define QN   EDGE_CAP_Q_N_BLOCKS
#define QCAP EDGE_CAP_Q_CAPACITY

/* ── 块队列测试台架：按 target 的生产/消费时序模拟 ──────────────────── */
static edgecap_q_t q;
static uint32_t blocks[QN][EDGE_CAP_Q_BLOCK_ITEMS];   /* canary 数据 */
static uint32_t seq;                 /* 已完成填充的块数（整块 = 生产序号）*/

static bool     dma_running;
static uint32_t dma_armed;           /* 在飞块下标 */
static uint32_t freed_since_stop;    /* 消费者自上次停止以来释放的块数 */

static void kick(void)               /* init 首块武装 / 消费侧重启 */
{
    uint32_t slot;
    CHECK(edgecap_q_arm_slot(&q, &slot), "kick refused (guard)\n");
    dma_armed = slot;
    dma_running = true;
}

static void restart_if_possible(void)   /* target 的 re-kick 前提：释放过块 */
{
    if (!dma_running && freed_since_stop) {
        kick();
        freed_since_stop = 0;
    }
}

/* IRQ：在飞块完成 → 发布 + 重武装或停。 */
static void irq_complete(void)
{
    uint32_t next;
    if (edgecap_q_push_full(&q, &next)) {
        dma_armed = next;
        return;
    }
    dma_running = false;             /* 满环：DMA 停（target 记 overrun/lost）*/
    freed_since_stop = 0;            /* 停机后须重新释放才可重启 */
}

/* DMA + IRQ：填满在飞块并完成。须 dma_running。 */
static void produce_step(void)
{
    for (uint32_t i = 0; i < EDGE_CAP_Q_BLOCK_ITEMS; i++)
        blocks[dma_armed][i] = seq;
    seq++;
    irq_complete();
}

/* drain 的一个块：peek → 全块校验（序号递增 = FIFO 且未被重写）→ free。
 * 内容不符时不 free（保留现场），由调用方 peek_head/断言收尾。 */
static bool consume_one(uint32_t *expect)
{
    uint32_t idx;
    if (!edgecap_q_pop_full(&q, &idx))
        return false;
    for (uint32_t i = 0; i < EDGE_CAP_Q_BLOCK_ITEMS; i++) {
        if (blocks[idx][i] != *expect)
            return true;             /* 内容损坏：交给调用方断言 */
    }
    edgecap_q_free(&q, idx);
    freed_since_stop++;
    (*expect)++;
    return true;
}

/* 队头块首字（不释放）；空队列返回哨兵。 */
static uint32_t peek_head(void)
{
    uint32_t idx = QN;
    if (!edgecap_q_pop_full(&q, &idx))
        return 0xFFFFFFFFu;
    return blocks[idx][0];
}

static void q_reset(void)
{
    edgecap_q_init(&q);
    for (uint32_t k = 0; k < QN; k++)
        for (uint32_t i = 0; i < EDGE_CAP_Q_BLOCK_ITEMS; i++)
            blocks[k][i] = 0xDEADBEEFu;
    seq = 0;
    dma_running = false;
    freed_since_stop = 0;
}

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

    /* ── edge_cap_queue：块队列所有权协议（block-queue 重设计）────────── */

    /* 7. 空队列边界：init 后 pop=false 且不动出参、pending=0；arm_slot
     *    从 0 起单调前进（首块武装序 = 槽序），空环 guard 恒放行。 */
    {
        q_reset();
        uint32_t idx = 123u;
        CHECK(!edgecap_q_pop_full(&q, &idx), "empty pop\n");
        CHECK(idx == 123u, "empty pop must not touch idx\n");
        CHECK(edgecap_q_pending(&q) == 0u, "empty pending\n");
        CHECK(edgecap_q_arm_slot(&q, &idx) && idx == 0u, "first arm=0\n");
        CHECK(edgecap_q_arm_slot(&q, &idx) && idx == 1u, "second arm=1\n");
        CHECK(edgecap_q_arm_slot(&q, &idx) && idx == 2u, "third arm=2\n");
    }

    /* 8. fill→drain 严格顺序，满容量 N−1/批，三圈绕环覆盖全部 8 个槽位
     *    （canary 全字校验：序号递增 = FIFO 且无任何槽位被提前重写）。 */
    {
        q_reset();
        kick();
        uint32_t expect = 0u;
        for (uint32_t round = 0; round < 3u * QN; round++) {
            while (dma_running)
                produce_step();
            CHECK(edgecap_q_pending(&q) == QCAP,
                  "round %u pending=%u want %u\n", round,
                  edgecap_q_pending(&q), QCAP);
            while (consume_one(&expect))
                ;
            restart_if_possible();
            CHECK(dma_running, "round %u restart failed\n", round);
        }
        CHECK(expect == seq, "FIFO consumed %u of %u\n", expect, seq);
        CHECK(seq == 3u * QN * QCAP, "produced %u\n", seq);
    }

    /* 9. 生产者绕圈撞上消费者持有块 → 拒绝（C1 类性质，由构造成立）：
     *    消费者不动，第 QCAP 次完成发布后 guard 拒绝武装保留槽；完成块
     *    已发布（pending 含它）；被持有块 0 之后 pop 出来内容原样
     *    （canary 证明从未被重写）。未释放时重启被策略拒绝。 */
    {
        q_reset();
        kick();
        while (dma_running)
            produce_step();
        CHECK(seq == QCAP, "fills=%u want %u\n", seq, QCAP);
        CHECK(!dma_running, "DMA must be stopped at capacity\n");
        CHECK(q.refused == 1u, "refused=%u want 1\n", q.refused);
        CHECK(edgecap_q_pending(&q) == QCAP, "pending after refuse\n");

        restart_if_possible();
        CHECK(!dma_running, "no free yet: must stay stopped\n");
        {
            uint32_t s = 123u;
            CHECK(!edgecap_q_arm_slot(&q, &s),
                  "arm guard must refuse while full\n");
            CHECK(s == 123u, "failed arm must not touch slot\n");
            CHECK(atomic_load(&q.fill_idx) == 7u,
                  "failed arm must not advance fill_idx\n");
        }

        CHECK(peek_head() == 0u, "held block head seq\n");
        uint32_t expect = 0u;
        CHECK(consume_one(&expect), "held block 0 must pop\n");
        CHECK(expect == 1u, "held block was rewritten (C1!)\n");
        CHECK(edgecap_q_pending(&q) == QCAP - 1u, "pending after free\n");
    }

    /* 10. 消费者释放重新解锁生产者：未释放保持停；释放一块后恰好放行
     *     一次生产（一换一），FIFO 序继续，随后再次拒停。 */
    {
        q_reset();
        kick();
        while (dma_running)
            produce_step();
        uint32_t expect = 0u;
        CHECK(consume_one(&expect) && expect == 1u, "free block 0\n");
        restart_if_possible();
        CHECK(dma_running, "restart armed\n");
        CHECK(dma_armed == 7u, "restart slot=%u want reserved 7\n", dma_armed);
        produce_step();
        CHECK(edgecap_q_pending(&q) == QCAP, "pending back to full\n");
        CHECK(peek_head() == 1u, "FIFO continues at seq 1\n");
        CHECK(!dma_running, "one free == one produce, stopped again\n");
    }

    /* 11. 多轮确定性伪随机交错下 FIFO 序保持（生产偏快 → 反复逼到满环
     *     拒绝 + 安全重启路径；canary 全程校验）。 */
    {
        q_reset();
        kick();
        uint32_t expect = 0u;
        uint32_t rng = 0x1234567u;
        for (uint32_t round = 0; round < 1024u; round++) {
            rng = rng * 1664525u + 1013904223u;
            if ((rng >> 24) < 200u && dma_running)
                produce_step();
            else
                (void)consume_one(&expect);
            restart_if_possible();
        }
        while (consume_one(&expect))
            ;
        CHECK(expect == seq, "interleaved FIFO consumed %u of %u\n",
              expect, seq);
        CHECK(edgecap_q_pending(&q) == 0u, "fully drained\n");
    }

    /* 12. 填满后取一放一：pending 恒满、队头连续、拒绝/重启节奏稳定。 */
    {
        q_reset();
        kick();
        while (dma_running)
            produce_step();
        uint32_t expect = 0u;
        for (uint32_t i = 0; i < 5u * QN; i++) {
            CHECK(edgecap_q_pending(&q) == QCAP, "i=%u pending\n", i);
            CHECK(peek_head() == expect, "i=%u head want %u\n", i, expect);
            CHECK(consume_one(&expect), "i=%u consume\n", i);
            restart_if_possible();
            CHECK(dma_running, "i=%u restart\n", i);
            produce_step();
            CHECK(edgecap_q_pending(&q) == QCAP, "i=%u refilled\n", i);
        }
        while (consume_one(&expect))
            ;
        CHECK(expect == seq, "take-one-put-one FIFO %u/%u\n", expect, seq);
    }

    /* 13. 拒绝时完成块已发布的边界：填到第 QCAP 块，push 返回停（拒绝
     *     的只是武装保留槽），但该块已发布、pending == QCAP、块 0 完好。 */
    {
        q_reset();
        kick();
        for (uint32_t i = 0; i < QCAP - 1u; i++) {
            CHECK(dma_running, "pre-fill %u stopped early\n", i);
            produce_step();
        }
        CHECK(dma_running, "still running at %u fills\n", QCAP - 1u);
        produce_step();                  /* 第 QCAP 块：发布 + 拒停 */
        CHECK(!dma_running, "stopped at capacity\n");
        CHECK(edgecap_q_pending(&q) == QCAP, "completed block published\n");
        uint32_t expect = 0u;
        CHECK(consume_one(&expect) && expect == 1u, "block 0 intact\n");
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
