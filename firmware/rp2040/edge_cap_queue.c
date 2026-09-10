/* edge_cap_queue.c — 块队列所有权协议实现。合同与可靠性论证见
 * edge_cap_queue.h（该头注释是本文件语义的规范原文）。 */
#include "edge_cap_queue.h"

void edgecap_q_init(edgecap_q_t *q)
{
    atomic_store_explicit(&q->fill_idx, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->fill_done, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->consume_idx, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->disc_bitmap, 0u, memory_order_relaxed);
    q->refused = 0u;
    q->in_flight = false;
}

bool edgecap_q_arm_slot(edgecap_q_t *q, uint32_t *slot)
{
    /* 仅在 DMA 停止且完成 IRQ 已处理时调用：in_flight=false 证明旧
     * push 已结束，fill_idx 无并发写者；consume_idx 用 acquire——与
     * 消费者 free 的 release 配对，释放先于重启武装可见。 */
    if (q->in_flight)
        return false;
    uint32_t next = atomic_load_explicit(&q->fill_idx,
                                         memory_order_relaxed);
    uint32_t consume = atomic_load_explicit(&q->consume_idx,
                                            memory_order_acquire);
    if ((next + 1u) % EDGE_CAP_Q_N_BLOCKS == consume)
        return false;                    /* 环仍满：保留槽未释放，勿武装 */
    q->in_flight = true;
    atomic_store_explicit(&q->fill_idx, (next + 1u) % EDGE_CAP_Q_N_BLOCKS,
                          memory_order_relaxed);
    *slot = next;
    return true;
}

bool edgecap_q_push_full(edgecap_q_t *q, uint32_t *next)
{
    /* 在飞块 = fill_idx 的前驱（arm_slot / 上次 push 成功都把 fill_idx
     * 推进过一位）。单 DMA 通道 ⇒ 本函数只在 IRQ 里、按武装顺序到达。 */
    uint32_t prev = atomic_load_explicit(&q->fill_idx,
                                         memory_order_relaxed);
    uint32_t armed = (prev + EDGE_CAP_Q_N_BLOCKS - 1u) % EDGE_CAP_Q_N_BLOCKS;
    uint32_t done = (armed + 1u) % EDGE_CAP_Q_N_BLOCKS;   /* == prev mod N */

    /* 先发布 FULL（release → 消费者 acquire 读 fill_done 后才能看见块），
     * guard 拒绝的只是"下一次武装"——完成块本身不丢。 */
    atomic_store_explicit(&q->fill_done, done, memory_order_release);

    uint32_t next_slot = done;
    uint32_t consume = atomic_load_explicit(&q->consume_idx,
                                            memory_order_acquire);
    if ((next_slot + 1u) % EDGE_CAP_Q_N_BLOCKS == consume) {
        /* 满环（容量 N−1）：next_slot 是贴着消费游标的保留槽，消费者
         * 持有块 = consume_idx 在它前面一格，前沿到此为止。fill_idx
         * 停在 next_slot = 消费者释放后的重启武装目标。 */
        q->refused++;
        q->in_flight = false;
        return false;
    }
    atomic_store_explicit(&q->fill_idx, (next_slot + 1u) % EDGE_CAP_Q_N_BLOCKS,
                          memory_order_relaxed);
    *next = next_slot;
    return true;
}

bool edgecap_q_pop_full(edgecap_q_t *q, uint32_t *idx)
{
    /* consume_idx 只有消费者写，relaxed 读自身游标；fill_done 必须
     * acquire——与 IRQ 发布 FULL 的 release 配对，看见即整块完整。 */
    uint32_t consume = atomic_load_explicit(&q->consume_idx,
                                            memory_order_relaxed);
    uint32_t done = atomic_load_explicit(&q->fill_done,
                                         memory_order_acquire);
    if (consume == done)
        return false;                    /* 空：无 FULL 块 */
    *idx = consume;                      /* peek：游标不动，消费者持有该块 */
    return true;
}

void edgecap_q_free(edgecap_q_t *q, uint32_t idx)
{
    /* 单消费者严格顺序：idx 即最近 peek 的块。release 推进 = 向生产者
     * guard 发布该槽 FREE（guard acquire 重读通过后才可能重新武装）。 */
    atomic_store_explicit(&q->consume_idx,
                          (idx + 1u) % EDGE_CAP_Q_N_BLOCKS,
                          memory_order_release);
}

void edgecap_q_mark_disc(edgecap_q_t *q, uint32_t slot)
{
    atomic_fetch_or_explicit(&q->disc_bitmap, 1u << slot,
                             memory_order_relaxed);
}

bool edgecap_q_take_disc(edgecap_q_t *q, uint32_t idx)
{
    uint32_t bit = 1u << idx;
    uint32_t old = atomic_fetch_and_explicit(&q->disc_bitmap, ~bit,
                                             memory_order_relaxed);
    return (old & bit) != 0u;
}

uint32_t edgecap_q_pending(const edgecap_q_t *q)
{
    uint32_t consume = atomic_load_explicit(&q->consume_idx,
                                            memory_order_relaxed);
    uint32_t done = atomic_load_explicit(&q->fill_done,
                                         memory_order_acquire);
    return (done + EDGE_CAP_Q_N_BLOCKS - consume) % EDGE_CAP_Q_N_BLOCKS;
}
