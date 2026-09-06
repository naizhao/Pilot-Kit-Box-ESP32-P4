/*
 * modes_edge.h — 双沿间隔流 → 56/112-bit Mode-S 帧重构。
 *
 * 纯 C、无 pico 依赖：host 单测与上板共用同一份实现。
 * 职责边界：这里只做 preamble 同步 + PPM 位重建（含 R11 融合串拆分）+
 * 组帧，不做 CRC、不维护任何目标状态——裁决与融合都在 P4。
 *
 * 时序合同（外部锚点：esp32-rtl-sdr/main/mode-s.c:708-715 —— 0.5µs 脉冲 @
 * 0/1.0/3.5/4.5µs；数据 1µs/位、脉冲在位首=1 / 位中=0；quarter-µs 整数域，容差 ±1 qus）：
 *   - feed 的是**全部边沿**（上升+下降交替）的相邻间隔，burst 以上升沿开启
 *     （空闲低电平 → 首个跳变必为上升），burst 内边沿严格交替；
 *   - 间隔 >5µs 关闭 burst（帧间静默）；
 *   - 融合串规则：只有 0→1 会融合。脉冲起点在位首（4k，bit1）则宽度只能是
 *     单脉冲（1→0 不融合）；起点在位中（4k+2，bit0）且宽度 >2 qus 时为融合串，
 *     串内位按 0,1,0,1,… 交替、覆盖连续 chip——由波形唯一确定，无歧义；
 *   - 每个 chip 恰好被覆盖一次，出现空洞/重叠/越界 → 整帧丢弃；
 *   - 帧长按 DF=bits[0..4]（>15 → 112 else 56）；不做 CRC（P4 裁决）。
 *
 * 重入约束：cb 在 burst 清理前被调用；cb 内不得调用 modes_edge_feed
 * （会追加入即将清空的缓冲/嵌套 burst_emit）。
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define MODES_EDGE_MAX_EDGES 256   /* 双沿：4 preamble + 2×112 数据 + 余量 */

typedef struct {
    uint8_t  frame[14];
    uint32_t nbits;                /* 56 | 112 */
    uint64_t start_tick;           /* preamble 首上升沿绝对 tick（burst 首沿
                                    * 是帧前噪声时，经候选滑窗跳过噪声） */
} modes_edge_frame_t;

typedef void (*modes_edge_frame_fn)(const modes_edge_frame_t *f, void *user);

typedef struct {
    uint32_t tick_hz;              /* 传入 deltas 的 tick 频率 */
    modes_edge_frame_fn cb;
    void *user;
    /* burst 累积状态 */
    uint64_t abs_tick;             /* 上一边沿绝对 tick */
    uint64_t burst_start_tick;
    uint32_t burst[MODES_EDGE_MAX_EDGES];
    int      burst_n;
    uint32_t burst_gap_ticks;      /* > 此值 = burst 结束（5µs）*/
    /* 统计（audit round 5 Fix 4：C11 原子——core1 解码任务独占写、
     * core0 的 1 Hz health 只读；更新点全部 relaxed 原子加，读方
     * relaxed load。逐字段独立采样，不做跨字段一致性承诺——各计数
     * 单调，与 P4 侧 pk_dsp/adsb_link stats 同一口径）。结构布局不变。 */
    atomic_uint preamble_hits, frames_56, frames_112;
    /* dropped_noise 按 burst 记账（该 burst 最终一无所获才 +1，每 burst
     * 至多一次）；dropped_decode 按候选记账：间距合格的 preamble 候选
     * 数据解码失败（两中心同电平）时 +1，随后滑到下一候选继续（audit
     * round 4）——同一 burst 两者可同时非零。 */
    atomic_uint dropped_noise, dropped_decode, bursts, edge_overruns;
} modes_edge_t;

void modes_edge_init(modes_edge_t *m, uint32_t tick_hz,
                     modes_edge_frame_fn cb, void *user);
void modes_edge_feed(modes_edge_t *m, const uint32_t *deltas, size_t n);
/* 断点重置：在 discontinuity（丢沿/重启，见 edge_cap.h drain 的
 * discontinuity 出参）后、喂入断点标记批次之前调用。丢弃开着的半截
 * burst（burst_n/abs_tick/burst_start_tick 归零重计），不 emit、不回调；
 * 统计字段保留（boot-lifetime 口径）。不 reset 的后果：断点前后的 delta
 * 被拼进同一 burst——接缝奇偶错乱时后续真帧整体丢失（test_modes_edge
 * 用例 15 对照锁定），abs_tick 也把永久偏移带进 start_tick。 */
void modes_edge_reset(modes_edge_t *m);
