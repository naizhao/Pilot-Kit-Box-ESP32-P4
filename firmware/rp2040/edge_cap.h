/* edge_cap.h — PIO 上升沿捕获 + DMA 环取数。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define EDGE_CAP_SM_CLK_HZ 125000000u     /* RP2040 默认 sys clk，div=1 */
#define EDGE_CAP_TICK_HZ   (EDGE_CAP_SM_CLK_HZ / 2u)   /* 2 周期/迭代 */
#define EDGE_CAP_RING_ITEMS 2048u         /* u32 ×2048 = 8KB，双半区 */

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

/* 单调生产位置合同（audit round 4，单写者重设计）：
 *   · producer_pos 的唯一写者是 DMA 完成中断：每认领一次通道完成事件
 *     +ring_items（两通道交替写同一个环，一次完成 = 恰好一整环；交接
 *     瞬间两位置位按 2 计）。producer_pos 单调不减、按 2^32 回卷；
 *   · read_pos 只前进（消费侧独占写，单读者）。
 * 在这两条合同下，(uint32_t)(producer_pos − read_pos) 的模差永不"下溢"：
 * 只要滞后 < 2^31（环只有 ring_items ≤ 2048 项，恒成立），模差就等于
 * 真实差值——即使 producer_pos 已回卷越过 0 也一样。IRQ 侧只做
 * 掩码/清理/原子累加（不采样 TRANS_COUNT），与消费端的读取以
 * release/acquire 配对（edge_cap.c）。
 *
 * 本 helper 是 drain 的全部裁决算术（纯函数，host 单测直接覆盖：
 * test_edgecap_convert.c）。
 */
typedef struct {
    uint32_t avail;              /* 本次可安全读取的样本数（≤ ring_items）*/
    uint32_t overrun_resync_to;  /* 非 0 = 检出写穿，消费位应重同步到该值
                                  * （丢旧，保留最新 ring_items 个样本）*/
} edgecap_avail_t;

static inline edgecap_avail_t edgecap_avail(uint32_t producer_pos,
                                            uint32_t read_pos,
                                            uint32_t ring_items)
{
    edgecap_avail_t r;
    uint32_t total = producer_pos - read_pos;    /* u32 模差：合同内无下溢 */
    if (total > ring_items) {                    /* 被写穿：跳到头重同步 */
        r.overrun_resync_to = producer_pos - ring_items;
        r.avail = ring_items;
    } else {
        r.overrun_resync_to = 0;
        r.avail = total;
    }
    return r;
}

void   edge_cap_start(void);
size_t edge_cap_drain(uint32_t *out, size_t cap);
uint32_t edge_cap_overruns(void);
