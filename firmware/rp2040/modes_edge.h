/*
 * modes_edge.h — TLV3501 上升沿间隔 → 56/112-bit Mode-S 帧。
 *
 * 纯 C、无 pico 依赖：host 单测与上板共用同一份实现。
 * 职责边界（PLAN.md §4）：这里只做 preamble 同步 + PPM 采样 + 组帧，
 * 不做 CRC、不维护任何目标状态——裁决与融合都在 P4。
 *
 * 时序合同（外部锚点：firmware/components/esp32-rtl-sdr/main/mode-s.c:708-715、
 * docs/configuration-zh_CN.md:259「每比特 1 µs，每帧 120 µs」）：
 *   preamble 上升沿在 0 / 1.0 / 3.5 / 4.5µs（三段间隔 [4,10,4]±1 qus）；
 *   数据自 preamble 首沿 +8µs（32 qus）起，每比特 1.0µs（QUS_BIT=4），
 *   比特 k 的上升沿在 32+4k qus（bit1，前半）或 32+4k+2 qus（bit0，后半），
 *   判位窗口 ±1 qus。两窗在 32+4k+1 处相接：恰落等距点（32+4k+1）的沿
 *   判 bit1 —— 确定性 tie-break，见 modes_edge.c 常量旁注释。
 *   帧内最长沿间隔 4.0µs（preamble 末沿 4.5µs → 首数据脉冲 8.5µs）
 *   < 5µs burst 阈值，判据不变。
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
/* cb 在 burst 清理前被调用；cb 内不得调用 modes_edge_feed（会追加入即将清空的缓冲/嵌套 burst_emit）*/
void modes_edge_feed(modes_edge_t *m, const uint32_t *deltas, size_t n);
