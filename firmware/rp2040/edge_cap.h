/* edge_cap.h — PIO 双沿捕获 + DMA 块队列取数（block-queue 重设计）。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "edge_cap_queue.h"

#define EDGE_CAP_SM_CLK_HZ 125000000u     /* RP2040 默认 sys clk，div=1 */
#define EDGE_CAP_TICK_HZ   (EDGE_CAP_SM_CLK_HZ / 2u)   /* 2 周期/迭代 */
/* 缓冲总容量（u32 条目数）= 块队列几何：8 块 × 256 = 2048 条（8KB）。
 * 条目不再按条发布，而按块（256 条/块）显式交接，几何见 edge_cap_queue.h；
 * 保留本宏供 selftest_gen 的 flush 脉冲预算（SELFTEST_FLUSH_EDGES）按
 * 总深换算，语义 = "推满整个块队列"，与容量一致。 */
#define EDGE_CAP_RING_ITEMS (EDGE_CAP_Q_N_BLOCKS * EDGE_CAP_Q_BLOCK_ITEMS)

/* PIO 推送的是递减计数器原值；换算成真实间隔 tick：
 * 设 D = PRELOAD − raw（两次沿捕获间的递减次数），对程序逐拍计数得
 * cycles = 2D+7（edge 块 4 + 高相位 2H + 变低转换 2 + 低相位 2L + 被命中的
 * 那拍 jmp pin edge 1），即真实间隔 = D+3.5 tick。tick 只能取整：+3 是该值
 * 的 floor，每个间隔带 ≤1 tick（16ns）的同号偏差；该偏差逐间隔累积、不会
 * 跨帧抵消，必须逐沿修正——这就是 +3 在每个沿上各自应用的原因。
 * 饱和（raw≈0，X 停在 0）得到 ≈PRELOAD+3 的超大值 = 空闲标记，与 >5µs gap 语义一致。 */
#define EDGE_CAP_PRELOAD 0xFFFFFFE0u
static inline uint32_t edgecap_raw_to_ticks(uint32_t raw)
{
    return (EDGE_CAP_PRELOAD - raw) + 3u;
}

/* 块队列发布合同（gpt-5.6-sol 两轮审计收敛的最终模型）：
 *
 *   · 单 DMA 通道，N=8 块 × 256 条；IRQ 驱动重武装，**显式所有权**：
 *     每个块任一时刻只属于一方——FREE →（DMA 在飞写）→ FULL（IRQ 发布）
 *     →（消费者取空）→ FREE。块绝不同时被 DMA 写和被消费者读。旧共享环
 *     的"整环发布让下一圈盖掉未读数据"（审计 C1）与随之而来的
 *     claim/window_ok 写穿重检算术一并废除：所有权由构造排除写穿，
 *     重检不再存在，也勿加回。
 *
 *   · 生产者 = DMA IRQ（core0）：完成 → push_full 发布（release）→
 *     guard 放行则立即重武装下一 FREE 块（write_addr 重写 +
 *     TRANS_COUNT_TRIG 触发）；环满（容量 N−1，保留槽贴着消费游标）则
 *     **DMA 停机**：置 lost 标志、记 overrun。停机到消费侧重启之间的
 *     边沿数据丢失（overrun 语义，真实过载路径，诚实计数）。
 *
 *   · 消费者 = edge_cap_drain（core1 独占）：按 FIFO 序整块 peek →
 *     逐条换算 → free（release 发布 FREE）。cap 按块取整：剩余容量
 *     不足一块（256 条）时不弹块，本调用按块粒度返回。
 *     停机重启：drain 在释放过 ≥1 块后检查 lost 标志，用 arm_slot 的
 *     guard 重新武装（guard 失败 = 环仍满，保持停机等下一拍）。
 *     重启 guard 必须走 arm_slot——它复用满环判定，防止把保留槽填满
 *     发布出 fill_done == consume 的 8 块 FULL 态（host 测试 11 的
 *     canary 反例）。
 *
 *   · 跨核配对：fill_idx/fill_done 唯一写者 = IRQ（core0），consume_idx
 *     唯一写者 = drain（core1），发布/回收全走 release/acquire（同
 *     adsb1090 帧环的 SPSC 口径）。DMA 的 SRAM 写对两核一致可见，IRQ
 *     在传输完成后才触发，块数据无需额外屏障。所有权逐条论证见
 *     edge_cap_queue.h 头注释。
 */

void   edge_cap_start(void);
size_t edge_cap_drain(uint32_t *out, size_t cap);
uint32_t edge_cap_overruns(void);
/* 诊断：等待消费的**FULL 块数**（0..7，不折算条目数；含消费者正在
 * 转换中的那块）。按块计——满块发布粒度下它以 256 条/块为台阶跳动。 */
uint32_t edge_cap_pending(void);
