#include <string.h>
#include "modes_edge.h"

/* 判据全部用 quarter-µs（qus）整数；换算 1 qus = tick_hz/4e6（四舍五入）。
 * 时序锚点：mode-s.c:708-715 —— preamble 0.5µs 脉冲 @ 0/1.0/3.5/4.5µs；
 * 数据 1µs/位、0.5µs 脉冲在位首（bit1）/ 位中（bit0）。 */
#define QUS_PREAM_D1   4     /* 1.0µs  */
#define QUS_PREAM_D2   10    /* 2.5µs  */
#define QUS_PREAM_D3   4     /* 1.0µs  */
#define QUS_TOL        1     /* ±0.25µs（preamble 间隔/宽度容差）*/
#define QUS_HALF       2     /* 0.5µs 脉冲宽度（标称）*/
#define QUS_DATA_OFF   32    /* preamble 首沿 +8.0µs */
#define QUS_BIT        4     /* 1.0µs/位 */
#define QUS_BURST_GAP  20    /* >5µs 无边沿 = burst 结束 */

static uint32_t ticks_to_qus(uint32_t ticks, uint32_t tick_hz)
{
    return (uint32_t)(((uint64_t)ticks * 4000000u + tick_hz / 2) / tick_hz);
}

static void burst_reset(modes_edge_t *m)
{
    m->burst_n = 0;
}

/*
 * 半位中心电平采样：位 k 的两个半位中心在 32+4k+1（bit1 半位）与 32+4k+3
 * （bit0 半位）qus 处。电平由边沿表重建（边沿 0 = 上升沿，之后严格交替），
 * 区间语义为 [t_i, t_i+1)。x 恰落在边沿上时读到翻转后的电平——这是
 * ±0.25µs 抖动下的"安全失败"边界（最坏判 INVALID 丢帧，不会判错位）。
 *
 * cursor 单调前进（查询位置单调递增），整帧 O(n)。
 */
static int level_at(const uint32_t *t, int n, int *cursor, uint32_t x)
{
    while (*cursor < n && t[*cursor] <= x) (*cursor)++;
    if (*cursor >= n)                       /* 最后一个边沿之后：终态电平 */
        return (n % 2 == 0) ? 0 : 1;        /* 偶数边沿=止于下降沿 → 低 */
    /* 已通过 cursor 条边沿：最后通过的是下标 cursor-1。
     * 奇数条 → 最后是上升沿 → 高；偶数条 → 最后是下降沿 → 低。 */
    return (*cursor % 2 == 1) ? 1 : 0;
}

static void burst_emit(modes_edge_t *m)
{
    if (m->burst_n < 8) {                    /* preamble 至少 8 个边沿 */
        if (m->burst_n) { m->bursts++; m->dropped_noise += (uint32_t)m->burst_n; }
        burst_reset(m);
        return;
    }

    /* 边沿绝对时刻（qus）。t[0] = 0：burst 首沿（上升沿）为时间原点；
     * t[i] = Σ burst[0..i-1]（burst[j] 是边沿 j→j+1 的间隔）。
     * 奇偶：偶下标=上升，奇下标=下降。
     * 数组取 MAX_EDGES+1（audit round 4 Fix 2）：burst_n 可达 MAX_EDGES
     * （feed 的溢出路径在 burst_n==MAX 时才触发 emit），而最后一条间隔的
     * 到达沿必须进表（见下）——t[burst_n] 是**语义必需的第 257 个槽**
     * （t[burst_n] = 最后一条间隔的到达沿），不是冗余填充。 */
    uint32_t t[MODES_EDGE_MAX_EDGES + 1];
    uint32_t acc = 0;
    t[0] = 0;
    for (int i = 1; i < m->burst_n; i++) {
        acc += ticks_to_qus(m->burst[i - 1], m->tick_hz);
        t[i] = acc;
    }
    /* burst 存的是 E-1 条间隔（E = 边沿数）；最后一条间隔的到达沿也要进表，
     * 否则帧尾下降沿丢失、末位两半位中心同电平被误判时序损坏（R11 实测）。 */
    int nedges = m->burst_n + 1;
    t[m->burst_n] = acc + ticks_to_qus(m->burst[m->burst_n - 1], m->tick_hz);

    /* preamble + 数据解码（audit round 4 Fix 3）：候选锚点沿上升沿序列
     * 滑动（burst 首沿可能是帧前噪声脉冲，按首沿锚定失败会丢整帧——
     * 实测 0.5µs 噪声 + 1.5µs 间隔 + 合法帧 → frames=0）。间距+脉宽合格
     * 的候选若随后数据解码失败（两中心同电平），**继续滑到下一个候选**
     * ——噪声凑出的假前导（如 [4,10,4] 间距三连脉冲）不再吞掉同 burst
     * 里紧跟的真帧。候选有限 → 终止性显然。解码失败的候选单独记
     * dropped_decode（不进 dropped_noise，后者仍按 burst 记账、口径
     * 不变）；preamble_hits 按候选计（间距+脉宽合格即命中，含最终解码
     * 失败者——它确实检到了 preamble 形状的能量）。 */
    modes_edge_frame_t f;
    int frame_ok = 0, pre = -1, want = 0, tried = 0;
    for (int r = 0; r + 7 < nedges; r += 2) {
        uint32_t d1 = t[r + 2] - t[r], d2 = t[r + 4] - t[r + 2],
                 d3 = t[r + 6] - t[r + 4];
        if (d1 < QUS_PREAM_D1 - QUS_TOL || d1 > QUS_PREAM_D1 + QUS_TOL ||
            d2 < QUS_PREAM_D2 - QUS_TOL || d2 > QUS_PREAM_D2 + QUS_TOL ||
            d3 < QUS_PREAM_D3 - QUS_TOL || d3 > QUS_PREAM_D3 + QUS_TOL)
            continue;
        int widths_ok = 1;
        for (int k = 0; k < 4; k++) {
            uint32_t w = t[r + 2 * k + 1] - t[r + 2 * k];
            if (w < QUS_HALF - QUS_TOL || w > QUS_HALF + QUS_TOL) {
                widths_ok = 0;
                break;
            }
        }
        if (!widths_ok) continue;
        m->preamble_hits++;
        tried++;

        /* 数据：逐位在两个半位中心采样电平，采样时轴以本候选的上升沿
         * 为原点。R11 融合（bit0→bit1 连续高电平）在此模型下自然正确：
         * 融合把两个半位都垫成高。两个中心同电平 → 本候选下时序已被
         * 破坏（丢沿/抖动越界/假前导）→ 放弃本候选，滑向下一个。 */
        uint8_t frame[14] = {0};
        int covered = 0, df = -1;
        want = 0;
        int cur = r + 8;                  /* 电平游标：数据区从候选后第 8 沿起 */
        for (int k = 0; k < 112; k++) {
            uint32_t c1 = t[r] + QUS_DATA_OFF + QUS_BIT * k + 1;
            uint32_t c0 = c1 + QUS_HALF;
            int lv1 = level_at(t, nedges, &cur, c1);
            int lv0 = level_at(t, nedges, &cur, c0);
            if (lv1 == lv0) break;        /* 两中心同电平：候选判负 */
            if (lv1) frame[k / 8] |= (uint8_t)(0x80u >> (k % 8));
            covered = k + 1;
            if (covered == 5) {
                df = frame[0] >> 3;
                want = (df > 15) ? 112 : 56;
            }
            if (want && covered >= want) break;
        }
        if (want && covered >= want) {
            memcpy(f.frame, frame, sizeof(f.frame));
            f.nbits = (uint32_t)want;
            pre = r;
            frame_ok = 1;
            break;
        }
        m->dropped_decode++;              /* 本候选判负，试下一个 */
    }
    if (!frame_ok) {
        /* 保留既有 dropped_noise 双口径（audit round 4 不改账，仅把候选级
         * 失败分账到 dropped_decode）：从未出现合格候选 = 纯噪声 burst，
         * 按边沿数记（旧路径 1）；有候选但全部解码失败，按 burst 记 1
         * （旧路径 2）。 */
        m->bursts++;
        m->dropped_noise += tried ? 1u : (uint32_t)m->burst_n;
        burst_reset(m);
        return;
    }

    /* start_tick 是 preamble 首沿（可能不是 burst 首沿——候选滑窗跳过
     * 了帧前噪声），按候选前的间隔精确回加。 */
    uint64_t start = m->burst_start_tick;
    for (int i = 0; i < pre; i++) start += m->burst[i];
    f.start_tick = start;
    if (want == 56) m->frames_56++; else m->frames_112++;
    if (m->cb) m->cb(&f, m->user);

    m->bursts++;
    burst_reset(m);
}

void modes_edge_init(modes_edge_t *m, uint32_t tick_hz,
                     modes_edge_frame_fn cb, void *user)
{
    memset(m, 0, sizeof(*m));
    m->tick_hz = tick_hz;
    m->cb = cb;
    m->user = user;
    m->burst_gap_ticks = (uint32_t)(((uint64_t)QUS_BURST_GAP * tick_hz + 2000000u) / 4000000u);
}

void modes_edge_feed(modes_edge_t *m, const uint32_t *deltas, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t d = deltas[i];
        m->abs_tick += d;
        if (d > m->burst_gap_ticks) {         /* 长隔：关 burst */
            if (m->burst_n) burst_emit(m);
            m->burst_start_tick = m->abs_tick; /* 下一沿开新 burst */
            continue;
        }
        if (m->burst_n == 0)
            m->burst_start_tick = m->abs_tick - d; /* 首沿绝对时刻 */
        if (m->burst_n < MODES_EDGE_MAX_EDGES) {
            m->burst[m->burst_n++] = d;
        } else {
            m->edge_overruns++;
            burst_emit(m);                    /* 缓冲满：按噪声帧处理 */
        }
    }
}
