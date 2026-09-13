/* threshold_sweep.h — 门限扫描的选点判据（纯函数，host 可测）。
 *
 * 扫描本身是个跑在 core0 上的状态机（adsb1090.c），但"哪个点最好"这个
 * 判断是纯逻辑，单独放这里以便 host 单测——它有一条容易想当然的分支。
 *
 * ── 为什么不能只看解出的帧数 ────────────────────────────────────
 * 帧数是最终目标，但它的计数率太低、且完全受制于天上此刻有没有飞机。
 * 2026-09-13 傍晚实测：外置天线在同一门限上连测三轮拿到 33 / 0 / 5 帧，
 * 而同期 preamble 命中是 198 / 28 / 91 —— 高一个数量级，方差小得多。
 * 流量低的时候整张表可能**全是 0 帧**，此时按帧数选点等于随机取第一个。
 *
 * 所以判据是两级的：
 *   1. 有任何一点解出过帧 → 按帧数选（平手时用 pre 打破）
 *   2. 整张表帧数全 0     → 退回 pre 选（它至少还能分辨"有没有 Mode-S
 *                            形状的能量进来"，这正是低流量下唯一的信息）
 *
 * pre 的门槛远低于解出一帧：只要求 4 个脉冲的间距 1.0/2.5/1.0µs、脉宽
 * 0.5µs±0.25（modes_edge.c:89-101），不查 CRC。所以它是"信号进没进来"
 * 的探针，而不是"能不能用"的判据 —— 两者都全 0 时不该假装有答案，
 * 见返回值约定。
 */
#pragma once

#include <stdint.h>

/* 从扫描结果里挑建议点，返回下标；n<=0 或所有指标全 0 时返回 -1
 * （"没找到"必须能和"选中第 0 个"区分开，否则调用方会把一张空表
 * 报成"建议 200 permille"）。 */
static inline int threshold_sweep_pick(const uint32_t *frames,
                                       const uint32_t *pre, int n)
{
    if (!frames || !pre || n <= 0)
        return -1;

    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!frames[i])
            continue;
        if (best < 0 || frames[i] > frames[best] ||
            (frames[i] == frames[best] && pre[i] > pre[best]))
            best = i;
    }
    if (best >= 0)
        return best;

    for (int i = 0; i < n; i++) {
        if (!pre[i])
            continue;
        if (best < 0 || pre[i] > pre[best])
            best = i;
    }
    return best;                    /* 仍是 -1 表示整张表毫无信号 */
}
