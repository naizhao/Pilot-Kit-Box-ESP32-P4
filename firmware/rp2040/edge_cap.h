/* edge_cap.h — PIO 双沿捕获 + DMA 环取数。 */
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

/* 单调生产位置合同（gpt-5.6-sol re-audit 重设计：IRQ 整环记账 + 纯边界消费）：
 *
 *   · producer_pos（edge_cap.c 内 atomic_uint）的唯一写者是 DMA_IRQ_0：
 *     每认领一个通道完成事件 +EDGE_CAP_RING_ITEMS（ping-pong 双通道交替写
 *     同一个 8KB 环，一次完成 = 恰好一整环入环；交接瞬间双通道位置位，
 *     popcount 计 2 环、不重不漏）。位置单调不减、只按 2^32 回卷、永不回退。
 *     CPU 侧**不存在任何对 DMA 硬件进度（TRANS_COUNT）的采样**：整环边界
 *     之外的"活进度"是跨硬件/中断窗口的二手读数——完成事件先落到硬件、
 *     后被 IRQ 记账，边界重检看不见这段错位（gpt-5.6-sol C1 反例），已整体
 *     废除，勿加回。
 *
 *   · ⚠⚠ **发布粒度 = 整环 2048 沿（接受的取舍，务必知悉）**：不满一整环
 *     的尾批边沿——最坏情形是一个 240 沿的孤立突发——在后续流量把块填满
 *     之前**不可见**（trailing-burst latency）。真实空中连续流量下环以
 *     ~2048 沿/毫秒级滚动，无感；台架单发自检由 selftest_gen 位流尾部的
 *     flush 脉冲串把环补满（SELFTEST_FLUSH_EDGES）。诊断窗口见
 *     edge_cap_pending()。
 *
 *   · read_pos（s_read）只前进，core1 的 drain 独占写。
 *
 * 模差合同：滞后 < 2^31 时 (uint32_t)(producer_pos − read_pos) 等于真实
 * 差值（回卷越过 0 同样成立）；环只有 2048 项，正常运营恒在合同内。
 *
 * drain 的两段裁决（gpt-5.6-sol C1+C2），纯函数在下方、host 单测覆盖
 * （test_edgecap_convert.c）：
 *
 *   1. edgecap_claim()：acquire 读边界、对 read_pos 取模差。差 > 一整环 =
 *      写穿：resync 标志置位（判定只看标志——重同步目标可为 0），目标
 *      = 边界 − 环（丢旧、保留最新一环），可用量封顶一整环。
 *
 *   2. edgecap_window_ok()：复制完成后重读边界（acquire）。绝对位置 p 的
 *      槽位被重写，当且仅当生产者推进到 p + RING 之后；窗口 [s, s+n) 内
 *      最先被重写的是 s ⇒ 本批有效 ⇔ 生产者边界 ≤ s + RING，即越窗量
 *      producer_pos − (s+n) ≤ 环 − n（**裕度 = 整环 − 已复制窗口**）。
 *      越界 = 复制期间生产者跨过窗口起点 + 一整环 ⇒ 本批作废：overruns
 *      计数、read_pos 重同步到当前边界 − 环、返回 0；下一拍拿到的是完整
 *      新环。成立时（含等号）窗口内任何槽位都未被重写，本批可提交。
 *      平台前提（同环上其它 SPSC 结构的既有假设）：DMA IRQ 服务时延 ≪
 *      整环填充时间（淹没流量下 2048 沿 ≥ 1ms，handler 仅 µs）——IRQ
 *      未记账的已写前沿远小于一环，裕度把它整个覆盖。
 */
typedef struct {
    uint32_t avail;              /* 本次可安全读取的样本数（≤ ring_items，
                                  * 写穿时封顶并携带重同步目标）*/
    uint32_t resync;             /* 非 0 = 写穿，read_pos 应重同步到
                                  * resync_to（判定只看本标志，不看目标值）*/
    uint32_t resync_to;          /* 重同步目标（丢旧，保留最新一环；可为 0）*/
} edgecap_claim_t;

static inline edgecap_claim_t edgecap_claim(uint32_t producer_pos,
                                            uint32_t read_pos,
                                            uint32_t ring_items)
{
    edgecap_claim_t r = {0u, 0u, 0u};
    uint32_t lag = producer_pos - read_pos;      /* u32 模差：合同内无下溢 */
    if (lag > ring_items) {                      /* 写穿：跳到头重同步 */
        r.resync = 1u;
        r.resync_to = producer_pos - ring_items;
        r.avail = ring_items;
    } else {
        r.avail = lag;
    }
    return r;
}

/* 复制后重校验：返回非 0 = 窗口 [window_start, window_start + n) 全程未被
 * 生产者重写，本批可提交；返回 0 = 本批作废，read_pos 应重同步到
 * producer_pos_now − ring_items。要求 n ≤ ring_items。等价形式：
 * producer_pos_now − window_start ≤ ring_items。 */
static inline uint32_t edgecap_window_ok(uint32_t producer_pos_now,
                                         uint32_t window_start, uint32_t n,
                                         uint32_t ring_items)
{
    return (uint32_t)(producer_pos_now - (window_start + n)) <=
           (ring_items - n);
}

void   edge_cap_start(void);
size_t edge_cap_drain(uint32_t *out, size_t cap);
uint32_t edge_cap_overruns(void);
/* 诊断：producer_pos − s_read（u32 模差）——已入环未被消费的样本数。
 * 整环粒度发布下它按 2048 整阶跳动；长期停在 <2048 的非零值 = 尾批
 * 边沿未满块（见上方发布粒度取舍）。 */
uint32_t edge_cap_pending(void);
