/* edge_cap.h — PIO 上升沿捕获 + DMA 环取数。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define EDGE_CAP_SM_CLK_HZ 125000000u     /* RP2040 默认 sys clk，div=1 */
#define EDGE_CAP_TICK_HZ   (EDGE_CAP_SM_CLK_HZ / 2u)   /* 2 周期/迭代 */
#define EDGE_CAP_RING_ITEMS 2048u         /* u32 ×2048 = 8KB，双半区 */

void   edge_cap_start(void);
size_t edge_cap_drain(uint32_t *out, size_t cap);
uint32_t edge_cap_overruns(void);
