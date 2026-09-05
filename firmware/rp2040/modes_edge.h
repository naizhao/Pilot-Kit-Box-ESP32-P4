/*
 * modes_edge.h — TLV3501 上升沿间隔 → 56/112-bit Mode-S 帧。
 *
 * 纯 C、无 pico 依赖：host 单测与上板共用同一份实现。
 * 职责边界（PLAN.md §4）：这里只做 preamble 同步 + PPM 采样 + 组帧，
 * 不做 CRC、不维护任何目标状态——裁决与融合都在 P4。
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MODES_EDGE_MAX_EDGES 160   /* 4 preamble + 112 数据 + 余量 */

typedef struct {
    uint8_t  frame[14];
    uint32_t nbits;                /* 56 | 112 */
    uint64_t start_tick;           /* preamble 首沿的绝对 tick（caller 换算 µs）*/
} modes_edge_frame_t;

typedef void (*modes_edge_frame_fn)(const modes_edge_frame_t *f, void *user);

typedef struct {
    uint32_t tick_hz;              /* 传入 deltas 的 tick 频率 */
    modes_edge_frame_fn cb;
    void *user;
    /* burst 累积状态 */
    uint64_t abs_tick;             /* 上一沿绝对 tick */
    uint64_t burst_start_tick;
    uint32_t burst[MODES_EDGE_MAX_EDGES];
    int      burst_n;
    uint32_t burst_gap_ticks;      /* > 此值 = burst 结束（5µs）*/
    /* 统计 */
    uint32_t preamble_hits, frames_56, frames_112;
    uint32_t dropped_noise, bursts, edge_overruns;
} modes_edge_t;

void modes_edge_init(modes_edge_t *m, uint32_t tick_hz,
                     modes_edge_frame_fn cb, void *user);
void modes_edge_feed(modes_edge_t *m, const uint32_t *deltas, size_t n);
