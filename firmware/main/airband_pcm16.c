#include "airband_pcm16.h"
#include <string.h>

/* 63-tap Blackman-windowed sinc, fc=6250Hz at 48k, Q15 unity DC sum.
 * H(f): 1k=-0.003dB, 5k=-0.327dB, 7.5k=-28.7dB, 12k=-74.7dB.
 * Design: sinc(2*pi*fc*(n-31)/48000) * Blackman(n), normalized to 32768.
 * FIR operates on signed24 before any program amplification/saturation. */
static const int16_t taps[PK_AIRBAND_PCM16_TAPS] = {
    0,0,-1,-3,-1,7,16,16,-1,-33,-59,-48,17,108,160,103,
    -74,-280,-357,-177,231,641,722,257,-638,-1463,-1525,-318,
    2123,5117,7580,8528,7580,5117,2123,-318,-1525,-1463,-638,
    257,722,641,231,-177,-357,-280,-74,103,160,108,17,-48,
    -59,-33,-1,16,16,7,-1,-3,-1,0,0
};

void pk_airband_pcm16_reset(pk_airband_pcm16_t *s) { if (s) memset(s, 0, sizeof(*s)); }
void pk_airband_pcm16_stats_reset(pk_airband_pcm16_t *s) { if (s) s->stats = (pk_airband_pcm16_stats_t){0}; }

static uint32_t magnitude(int32_t v) { return v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v; }
static void measure(int32_t v, uint32_t *peak, uint64_t *sum)
{
    const uint32_t abs = magnitude(v);
    if (abs > *peak) *peak = abs;
    *sum += (uint64_t)((int64_t)v * v);
}

size_t pk_airband_pcm16_process(pk_airband_pcm16_t *s, const uint8_t *p, size_t bytes,
                                int16_t *mono, size_t cap, unsigned gain)
{
    if (!s || !p || !mono || bytes == 0 || bytes % 6 || gain > 16 ||
        s->phase > 2 || s->cursor >= PK_AIRBAND_PCM16_TAPS ||
        cap < (bytes / 6 + s->phase) / 3) return 0;
    size_t n = 0;
    for (size_t i = 0; i < bytes; i += 6) {
        uint32_t bits = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) | ((uint32_t)p[i+2] << 16);
        const int32_t v = (int32_t)(bits ^ 0x800000u) - 0x800000;
        s->history[s->cursor] = v;
        if (++s->cursor == PK_AIRBAND_PCM16_TAPS) s->cursor = 0;
        s->stats.input_frames++;
        measure(v, &s->stats.raw_peak, &s->stats.raw_sum_sq);
        if (v == 8388607 || v == -8388608) s->stats.raw_clips++;
        if (++s->phase != 3) continue;
        s->phase = 0;
        int64_t sum = 0;
        unsigned pos = s->cursor;
        for (unsigned j = 0; j < PK_AIRBAND_PCM16_TAPS; j++) {
            pos = pos == 0 ? PK_AIRBAND_PCM16_TAPS - 1 : pos - 1;
            sum += (int64_t)s->history[pos] * taps[j];
        }
        /* Q15 filter + signed24 -> signed16, round symmetrically. */
        const int32_t pre = (int32_t)((sum + (sum >= 0 ? 4194304 : -4194304)) / 8388608);
        measure(pre, &s->stats.pre_peak, &s->stats.pre_sum_sq);
        if (pre > 32767 || pre < -32768) s->stats.pre_clips++;
        int32_t post = pre * (int32_t)gain;
        if (post > 32767) { post = 32767; s->stats.post_clips++; }
        else if (post < -32768) { post = -32768; s->stats.post_clips++; }
        measure(post, &s->stats.post_peak, &s->stats.post_sum_sq);
        s->stats.output_frames++;
        mono[n++] = (int16_t)post;
    }
    return n;
}
