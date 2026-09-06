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

/* 单调生产位置合同（audit round 5：整环发布 + 细粒度活进度）：
 *   · producer_pos 的唯一写者是 DMA 完成中断：每认领一次通道完成事件
 *     +ring_items（两通道交替写同一个环，一次完成 = 恰好一整环；交接
 *     瞬间两位置位按 2 计）。producer_pos 是**整环粒度**的边界值，
 *     单调不减、按 2^32 回卷；小于一整环的突发只体现在活跃通道的
 *     TRANS_COUNT 里，因此 drain 必须再取环内细粒度进度（见下），
 *     否则 <2048 边沿的突发（如 ~240 边沿的台架自检）永远不可见；
 *   · in_block_progress = ring_items − 活跃通道剩余 TRANS_COUNT
 *     （remaining==0 视作满块：交接窗/完成块未认领时该整环其实已
 *     完整入环）。完成事件是通道切换的唯一时机，而完成事件的 IRQ
 *     侧效果就是 producer_pos 的 release 累加——drain 以"读 boundary
 *     → 读 TRANS_COUNT → 重读 boundary 不变"的重检环闭合撕裂采样；
 *   · read_pos 只前进（消费侧独占写，单读者）。
 * 模差合同：滞后 < 2^31（环只有 ring_items ≤ 2048 项，恒成立）时
 * (uint32_t)(live − read_pos) 就等于真实差值——即使已回卷越过 0。
 * live 的加法必须先在 u64 域完成再取模：boundary 贴 2^32 时
 * boundary + ring_items 会溢出 u32、丢掉刚补上的细粒度进度（u64
 * 加法无溢出；2^32 × 16ns ≈ 68 s 的时间尺度也在 u64 之内）。
 * 活进度只增不减（DMA 只前进），avail = live − read_pos 在合同内
 * 不下溢；若消费端长滞（>一整环）则走写穿重同步。IRQ 侧只做
 * 掩码/清理/原子累加，与消费端的读取以 release/acquire 配对
 * （edge_cap.c）。
 *
 * 写穿判定经 resync 标志给出（audit round 5 Fix 1）：重同步目标
 * 本身可以是 0（如 boundary=2048、read=0xFFFFFFFF 时目标恰为 0），
 * 旧的"非 0 即写穿"哨兵把这一合法目标漏判成"无写穿"——判定只看
 * 标志，目标值随结构体带回，由 drain 内部执行重同步。
 *
 * 本 helper 是 drain 的全部裁决算术（纯函数，host 单测直接覆盖：
 * test_edgecap_convert.c）。
 */
typedef struct {
    uint32_t avail;              /* 本次可安全读取的样本数（≤ ring_items）*/
    uint32_t resync;             /* 非 0 = 检出写穿，overrun_resync_to 有效
                                  * （判定只看本标志，不看目标值）*/
    uint32_t overrun_resync_to;  /* resync 时消费位应重同步到该值（丢旧，
                                  * 保留最新 ring_items 个样本）；可为 0 */
} edgecap_avail_t;

static inline edgecap_avail_t edgecap_avail(uint32_t producer_boundary,
                                            uint32_t in_block_progress,
                                            uint32_t read_pos,
                                            uint32_t ring_items)
{
    edgecap_avail_t r = {0u, 0u, 0u};
    /* u64 域加法防 boundary+progress 溢出 u32，再按 2^32 取模衔接模差 */
    uint32_t live = (uint32_t)((uint64_t)producer_boundary + in_block_progress);
    uint32_t total = live - read_pos;            /* u32 模差：合同内无下溢 */
    if (total > ring_items) {                    /* 被写穿：跳到头重同步 */
        r.resync = 1u;
        r.overrun_resync_to = live - ring_items;
        r.avail = ring_items;
    } else {
        r.avail = total;
    }
    return r;
}

void   edge_cap_start(void);
size_t edge_cap_drain(uint32_t *out, size_t cap);
uint32_t edge_cap_overruns(void);
