/* edge_glitch_filter.h — 在 Mode-S 解码前合并比较器窄高毛刺。
 *
 * edge_cap.pio 从 low_wait 冷启动，输出严格交替的 [low, high] 间隔。
 * 实板原始抓取反复出现 3 tick = 48ns 的 high；合法 Mode-S 高脉冲标称
 * 0.5us（约 31/32 tick），解码器容差下界 0.25us。这里仅删除 <12 tick
 *（<192ns）的 high，并把 low+glitch+next-low 合并，既保持总时间，也避免
 * 毛刺把 modes_edge 的候选扫描拖垮。原始诊断必须在本滤波器之前抓取。
 *
 * 纯 header 实现，供 RP2040 core1 与 host 单测共用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_GLITCH_MIN_HIGH_TICKS 12u
#define EDGE_GLITCH_IDLE_TICKS     313u /* modes_edge: >312 tick 结束 burst */

typedef struct {
    uint32_t pending_low;
    uint32_t rejected;
    bool expect_low;
    bool have_low;
} edge_glitch_filter_t;

static inline uint32_t edge_glitch_sat_add(uint32_t a, uint32_t b)
{
    return UINT32_MAX - a < b ? UINT32_MAX : a + b;
}

static inline void edge_glitch_filter_init(edge_glitch_filter_t *f)
{
    f->pending_low = 0;
    f->rejected = 0;
    f->expect_low = true;
    f->have_low = false;
}

/* PIO 完整重启/结构性断点后重新从 low 相位锚定；保留累计拒绝数。 */
static inline void edge_glitch_filter_resync(edge_glitch_filter_t *f)
{
    f->pending_low = 0;
    f->expect_low = true;
    f->have_low = false;
}

/* out 容量至少为 n；输出永远不多于输入。容量不足时不消费、不改状态。 */
static inline size_t edge_glitch_filter_feed(edge_glitch_filter_t *f,
                                             const uint32_t *in, size_t n,
                                             uint32_t *out, size_t cap)
{
    if (!f || !in || !out || cap < n)
        return 0;

    size_t on = 0;
    for (size_t i = 0; i < n; i++) {
        const uint32_t d = in[i];
        /* 合法帧内最大间隔约 4us（250 tick）；>5us 只可能是 idle low。
         * 丢奇数个沿后可借此恢复 [low,high] 相位，避免永久反相。 */
        if (d >= EDGE_GLITCH_IDLE_TICKS) {
            f->pending_low = d;
            f->have_low = true;
            f->expect_low = false;
            continue;
        }
        if (f->expect_low) {
            f->pending_low = f->have_low
                ? edge_glitch_sat_add(f->pending_low, d) : d;
            f->have_low = true;
            f->expect_low = false;
            continue;
        }

        if (d < EDGE_GLITCH_MIN_HIGH_TICKS) {
            f->pending_low = edge_glitch_sat_add(f->pending_low, d);
            f->rejected++;
            f->expect_low = true;
            continue;
        }

        out[on++] = f->pending_low;
        out[on++] = d;
        f->pending_low = 0;
        f->have_low = false;
        f->expect_low = true;
    }
    return on;
}
