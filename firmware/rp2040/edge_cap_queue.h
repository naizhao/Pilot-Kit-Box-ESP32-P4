/* edge_cap_queue.h — edge 捕获的块队列所有权协议（纯单元，无 pico 依赖）。
 *
 * gpt-5.6-sol 两轮审计收敛的最终发布模型：单 DMA 通道 × N 块 × 每块
 * BLOCK_ITEMS 个 u32 间隔原值，IRQ 驱动重武装，**显式所有权**——每个块
 * 任一时刻只处于一种状态：
 *
 *     FREE →（DMA 在飞写）→ FULL（IRQ 发布，可消费）→（消费者取空）→ FREE
 *
 * 一个块绝不同时被 DMA 写和被消费者读（审计 C1 的"整环发布让下一圈盖掉
 * 未读数据"由构造排除）。游标（全部 mod N）：
 *
 *   · fill_idx（atomic_uint）：生产者游标——下一个 FREE 槽位。DMA 在飞块
 *     恒为 (fill_idx − 1) mod N（arm_slot/push 成功都把它推进一位）。
 *   · fill_done（atomic_uint）：生产者游标——已发布 FULL 的 one-past 位置。
 *     唯一写者是 DMA IRQ，release 发布；消费者 acquire 读。FULL 弧段 =
 *     [consume_idx, fill_done)。
 *   · consume_idx（atomic_uint）：消费者游标——下一个待消费的 FULL 块。
 *     消费者 peek（pop_full 不动游标）→ 整块转换 → free 才推进并 release
 *     发布 FREE。**消费者正在转换的块 = consume_idx 本身**。
 *
 * 满环判定（生产者 guard）：拒绝武装槽位 s 当且仅当 s == (consume_idx − 1)
 * mod N——环上恒保留 1 槽（容量 N−1=7），保留槽永远贴着消费游标后一格滚动。
 * 这是对任务书草案 "(fill_done+1) mod N != consume_idx" 的修正：后者在整环
 * 恰好 8 块 FULL 时游标与"空"重合（mod N 歧义），消费侧重启路径会据
 * pending==0 误判而把保留槽武装出去——恰好复现 C1。容量 N−1 使满/空在
 * 游标上可区分（满 ⇔ fill_idx == consume−1，空 ⇔ fill_done == consume）。
 * 同一判定**在重启路径复用**（arm_slot 的 bool 返回）：满环停机后，只有
 * 消费者释放过块、(fill_idx+1) != consume_idx 成立时才允许重新武装——
 * 否则把保留槽填满并发布会造出 fill_done == consume 的 8 块 FULL 态，
 * pop 的空判定失真（host 测试以 canary 捕获过该反例，见测试 11）。
 *
 * **可靠性论证（soundness，逐条对应实现）**：
 *   1. DMA 只在"武装之后、push 发布之前"写块。武装只发生在 arm_slot
 *      （init 首块 / 消费侧重启，此时 DMA 停、IRQ 静默）和 IRQ push 成功
 *      分支（guard 已拒掉 s == consume−1），故在飞块 ∈ FREE 槽。
 *   2. 块只在 DMA 完成后（IRQ）经 fill_done 的 release 发布为 FULL；消费
 *      者 pop 以 acquire 读 fill_done，看见即数据完整，绝不读到半块。
 *   3. 消费者持有的块 = consume_idx（peek 后、free 前）。生产者武装前沿
 *      被 guard 钉在 consume−2 以内（拒绝 consume−1），前沿要够到
 *      consume 必须先武装 consume−1——被 guard 永久拒绝，直到 free 使
 *      consume 前进。⇒ DMA 与消费者对同一块的操作在时间轴上绝不重叠。
 *   4. 释放（free 的 release 推进 consume_idx）先于生产者 guard 的
 *      acquire 重读，重读通过才可能重新武装该槽——FREE 的回收对生产者
 *      可见后才会再被写。
 *   5. 单 DMA 通道 ⇒ 同时至多一个在飞块；push 严格按武装顺序发生 ⇒
 *      块序 = 槽序 = FIFO，不存在"跨过未读块绕写"的自由度（旧共享环
 *      设计恰败在此，审计 C1）。
 *   6. 跨核前提：生产者 = DMA IRQ（core0），消费者 = drain（core1），
 *      各游标单写者（fill_idx/fill_done ← IRQ；consume_idx ← 消费者），
 *      发布/消费配对全走 release/acquire（同 adsb1090 帧环的 SPSC 口径）。
 *      DMA 的 SRAM 写对两核一致可见，IRQ 在传输完成后才触发，块数据无需
 *      额外屏障。
 *
 * host 单测（test_edgecap_convert.c 后半）以 canary 写覆盖断言第 3 条
 * （生产者绕圈撞上消费者持有块 → 拒绝、原块内容完好）——C1 类性质由
 * 构造成立并回归锁定。
 */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define EDGE_CAP_Q_N_BLOCKS    8u       /* 块数（含 1 个保留槽）*/
#define EDGE_CAP_Q_BLOCK_ITEMS 256u     /* 每块 u32 间隔原值数 = 1KB/块 */
#define EDGE_CAP_Q_CAPACITY    (EDGE_CAP_Q_N_BLOCKS - 1u)   /* 可同时 FULL 的块数 = 7 */

typedef struct {
    atomic_uint fill_idx;     /* 生产者：下一个 FREE 槽位（武装目标）*/
    atomic_uint fill_done;    /* 生产者：已发布 FULL 的 one-past（release）*/
    atomic_uint consume_idx;  /* 消费者：下一个待消费 FULL 块（release）*/
    uint32_t refused;         /* 生产者私有诊断：满环拒发次数（host 测断言用；
                               * 目标侧权威计数在 edge_cap.c 的 s_overruns）*/
} edgecap_q_t;

/* 全游标归零。调用必须先于任何 arm/push/pop（审计 C1-init：状态清零与
 * IRQ 安装都先于 dma_channel_configure，首块最后武装）。 */
void edgecap_q_init(edgecap_q_t *q);

/* 取下一个 FREE 槽位下标并把 fill_idx 前进一位。**仅允许在 DMA 停止时
 * 调用**（init 首块武装 / overrun 后消费侧重启）：此刻 IRQ 静默，fill_idx
 * 无并发写者。返回 true → *slot = 应写入 dma_channel_set_write_addr 的块
 * 下标；返回 false → 环仍满（保留槽尚未被消费者释放，(fill_idx+1) mod N
 * == consume_idx），**不得武装**，保持停机、lost 标志保持置位，等下一次
 * 释放后再试。无副作用（guard 先行，失败时 fill_idx 不动）。 */
bool edgecap_q_arm_slot(edgecap_q_t *q, uint32_t *slot);

/* 生产者（IRQ 上下文）：把在飞块（fill_idx 的前驱）发布为 FULL。
 * 返回 true → *next = 应立即武装的 FREE 槽位（fill_idx 已同步前进）；
 * 返回 false → 环满，DMA 必须停：fill_idx 停在被拒槽位 = 消费者稍后
 * 重启的目标；本次完成块**已发布**（发布先于 guard，数据不丢）。 */
bool edgecap_q_push_full(edgecap_q_t *q, uint32_t *next);

/* 消费者：有 FULL 块可消费则置 *idx 并返回 true。peek 语义——不动任何
 * 游标，消费者整块取空后必须以 edgecap_q_free(idx) 释放。 */
bool edgecap_q_pop_full(edgecap_q_t *q, uint32_t *idx);

/* 消费者：释放 idx（必须是最近一次 peek 且已整块取空的块；单消费者严格
 * 顺序）。release 推进 consume_idx，向生产者 guard 发布该槽 FREE。 */
void edgecap_q_free(edgecap_q_t *q, uint32_t idx);

/* 诊断：已发布未释放的块数（0..N−1；含消费者正在转换的那块——数的是
 * FULL 块，不折算条目）。 */
uint32_t edgecap_q_pending(const edgecap_q_t *q);
