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

void   edge_cap_start(void);
size_t edge_cap_drain(uint32_t *out, size_t cap);
uint32_t edge_cap_overruns(void);
