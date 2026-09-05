#include <string.h>
#include "modes_edge.h"

/* 判据全部用 quarter-µs（qus）整数；换算 1 qus = tick_hz/4e6。
 * 外部锚点：firmware/components/esp32-rtl-sdr/main/mode-s.c:708-715、
 * docs/configuration-zh_CN.md:259「每比特 1 µs，每帧 120 µs」。
 * Mode S 下行：preamble = 0/1.0/3.5/4.5µs 四个 0.5µs 脉冲；数据每比特
 * 1.0µs，前半（+0.0µs）有脉冲 = bit1，后半（+0.5µs）= bit0。 */
#define QUS_PREAM_D1   4     /* 1.0µs  */
#define QUS_PREAM_D2   10    /* 2.5µs  */
#define QUS_PREAM_D3   4     /* 1.0µs  */
#define QUS_TOL        1     /* ±0.25µs */
#define QUS_DATA_OFF   32    /* preamble 首沿 +8.0µs */
#define QUS_BIT        4     /* 1.0µs/比特 */
#define QUS_B1_AT      0     /* bit1 脉冲在比特 +0.0µs（前半） */
#define QUS_B0_AT      2     /* bit0 脉冲在比特 +0.5µs（后半） */
#define QUS_BURST_GAP  20    /* >5µs 无沿 = burst 结束；帧内最长沿间隔
                              * 4.0µs（preamble 末沿 4.5µs → 首数据脉冲
                              * 8.5µs）留有余量 */

static uint32_t ticks_to_qus(uint32_t ticks, uint32_t tick_hz)
{
    /* 四舍五入：沿在被捕获时已量化到最近 tick，反变换同样取最近 qus。
     * 逐段截断在 62.5MHz（1 qus = 15.625 tick）下会让每个 2/4 qus 间隔
     * 固定缩 1 qus，112 比特累积漂移远超 ±1 qus 容差。uint64 防溢出。 */
    return (uint32_t)(((uint64_t)ticks * 4000000u + tick_hz / 2u) / tick_hz);
}

static void burst_reset(modes_edge_t *m)
{
    m->burst_n = 0;
}

static void burst_emit(modes_edge_t *m)
{
    if (m->burst_n < 4) { if (m->burst_n) m->bursts++; burst_reset(m); return; }

    /* burst[i] = 流入 burst 内第 i+1 个沿的间隔（首沿 e0 自己的流入间隔
     * 是关上一个 burst 的长隔，>5µs、不存缓冲）。因此累加得到的
     * t[i] = 第 i+1 个沿相对 e0 的时刻（qus）：t[0..2] 对应 preamble 的
     * 后三个沿（三段间隔 [1.0, 2.5, 1.0]µs），首个数据脉冲在 t[3]。 */
    uint32_t t[MODES_EDGE_MAX_EDGES];
    uint32_t acc = 0;
    for (int i = 0; i < m->burst_n; i++) {
        acc += ticks_to_qus(m->burst[i], m->tick_hz);
        t[i] = acc;
    }

    /* preamble：t0≈4, t1-t0≈10, t2-t1≈4 (±1)，即 e0 之后三段间隔。 */
    uint32_t d1 = t[0], d2 = t[1] - t[0], d3 = t[2] - t[1];
    if (d1 < QUS_PREAM_D1 - QUS_TOL || d1 > QUS_PREAM_D1 + QUS_TOL ||
        d2 < QUS_PREAM_D2 - QUS_TOL || d2 > QUS_PREAM_D2 + QUS_TOL ||
        d3 < QUS_PREAM_D3 - QUS_TOL || d3 > QUS_PREAM_D3 + QUS_TOL) {
        m->bursts++; m->dropped_noise += (uint32_t)m->burst_n;
        burst_reset(m);
        return;
    }
    m->preamble_hits++;

    /* 组帧：逐比特判位；比特窗口无脉冲或有歧义 → 帧作废。
     * bit1 窗 = base±1 qus；bit0 窗 = base+2±1 qus（= base+1..base+3）。
     * 两窗在 base+1 处相接：恰在 +1 的等距沿按确定性 tie-break 判 bit1
     * （if/else-if 顺序保证）。base+3 同为与下一比特 bit1 窗（base+4−1）
     * 的公共边界，顺序消费下先判本比特 bit0，无歧义。窗前的沿按噪声跳过。
     * t[] 本就相对 e0，数据块自 e0 +8µs 起，故 rel 直接与 base 比较，
     * 沿下标从 3（首数据脉冲）起。 */
    uint8_t frame[14] = {0};
    int got = 0, ei = 3;
    const uint32_t data0 = QUS_DATA_OFF;
    for (int k = 0; k < 112; k++) {
        uint32_t base = data0 + (uint32_t)(QUS_BIT * k);
        int bit = -1;
        while (ei < m->burst_n) {
            uint32_t rel = t[ei];
            if (rel + QUS_TOL < base) { ei++; continue; }     /* 窗前噪声沿 */
            if (rel >= base - QUS_TOL && rel <= base + QUS_TOL)          bit = 1;
            else if (rel >= base + QUS_B0_AT - QUS_TOL &&
                     rel <= base + QUS_B0_AT + QUS_TOL)                  bit = 0;
            if (bit >= 0) ei++;                               /* 本沿已被消费 */
            break;                                            /* 其余情形：本比特无脉冲 */
        }
        if (bit < 0) break;
        if (bit) frame[k / 8] |= (uint8_t)(0x80u >> (k % 8));
        got = k + 1;
        if (got == 56 && (frame[0] >> 3) <= 15) break;        /* 短帧收满 */
    }

    int want = ((frame[0] >> 3) > 15) ? 112 : 56;
    if (got < want) {                           /* burst 提前结束：半帧 */
        m->dropped_noise++;
        m->bursts++;
        burst_reset(m);
        return;
    }

    modes_edge_frame_t f;
    memcpy(f.frame, frame, sizeof(f.frame));
    f.nbits = (uint32_t)want;
    f.start_tick = m->burst_start_tick;
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
    m->burst_gap_ticks = (uint32_t)((uint64_t)QUS_BURST_GAP * tick_hz / 4000000u);
}

void modes_edge_feed(modes_edge_t *m, const uint32_t *deltas, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t d = deltas[i];
        m->abs_tick += d;
        if (d > m->burst_gap_ticks) {             /* 长隔：关 burst */
            if (m->burst_n >= 4) burst_emit(m);
            else burst_reset(m);
            m->burst_start_tick = m->abs_tick;    /* 下一沿开新 burst */
            continue;
        }
        if (m->burst_n == 0)
            m->burst_start_tick = m->abs_tick - d; /* 首沿绝对时刻 */
        if (m->burst_n < MODES_EDGE_MAX_EDGES) {
            m->burst[m->burst_n++] = d;
        } else {
            m->edge_overruns++;
            burst_emit(m);                        /* 缓冲满：按噪声帧处理 */
        }
    }
}
