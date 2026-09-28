#include "airband_audio.h"

static uint16_t gate_advance(uint16_t level, bool audible);

void pk_airband_audio_route_reset(pk_airband_audio_route_t *s)
{ if (s) *s = (pk_airband_audio_route_t){0}; }

size_t pk_airband_audio_route16(pk_airband_audio_route_t *s,
                                int16_t *program, size_t frames,
                                bool open, bool muted,
                                int16_t *out, size_t capacity)
{
    if (!s || !program || !out || frames == 0 || frames > capacity / 3) return 0;
    size_t n = 0;
    for (size_t i = 0; i < frames; i++) {
        /* Three gate steps per 16k sample preserve the 48k ramp duration. */
        for (unsigned k = 0; k < 3; k++)
            s->program_gate.level_q15 = gate_advance(s->program_gate.level_q15, open);
        const int16_t current = (int16_t)((int32_t)program[i] * s->program_gate.level_q15 /
                                          (int32_t)PK_AIRBAND_AUDIO_GATE_FULL_Q15);
        program[i] = current;
        const int32_t delta = (int32_t)current - s->previous16;
        for (int k = 1; k <= 3; k++) {
            s->speaker_gate.level_q15 = gate_advance(s->speaker_gate.level_q15, !muted);
            const int32_t interpolated = s->previous16 + delta * k / 3;
            const int16_t sample = (int16_t)(interpolated * s->speaker_gate.level_q15 /
                                              (int32_t)PK_AIRBAND_AUDIO_GATE_FULL_Q15);
            out[n*2] = sample;
            out[n*2+1] = sample;
            n++;
        }
        s->previous16 = current;
    }
    return n;
}

static int16_t decode_s24le_to_s16(const uint8_t *p)
{
    /* 24→16 是丢掉最低 8 位，因此结果正好是原样取 bits 23..8；把这 16 位
     * 直接解释成 int16_t 已包含符号，不需要先做对最终结果无影响的 24 位
     * 符号扩展。 */
    return (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
}

static int16_t apply_program_gain(int16_t sample)
{
    const int32_t amplified = (int32_t)sample * PK_AIRBAND_AUDIO_PROGRAM_GAIN;
    if (amplified > INT16_MAX) return INT16_MAX;
    if (amplified < INT16_MIN) return INT16_MIN;
    return (int16_t)amplified;
}

static uint16_t gate_advance(uint16_t level, bool audible)
{
    const uint16_t target = audible ? PK_AIRBAND_AUDIO_GATE_FULL_Q15 : 0u;
    if (level < target) {
        const uint32_t next = (uint32_t)level + PK_AIRBAND_AUDIO_GATE_STEP_Q15;
        return next < target ? (uint16_t)next : target;
    }
    if (level > target) {
        return level > PK_AIRBAND_AUDIO_GATE_STEP_Q15
                   ? (uint16_t)(level - PK_AIRBAND_AUDIO_GATE_STEP_Q15)
                   : 0u;
    }
    return level;
}

void pk_airband_audio_gate_reset(pk_airband_audio_gate_t *gate, bool open)
{
    if (gate == NULL) return;
    gate->level_q15 = open ? PK_AIRBAND_AUDIO_GATE_FULL_Q15 : 0u;
}

void pk_airband_audio_gate_stream_break(pk_airband_audio_gate_t *gate)
{
    pk_airband_audio_gate_reset(gate, false);
}

size_t pk_airband_audio_render(const uint8_t *src, size_t src_len,
                               int16_t *dst, size_t dst_frames,
                               bool audible, pk_airband_audio_gate_t *gate)
{
    if (src == NULL || dst == NULL || gate == NULL ||
        src_len == 0 || src_len % PK_AIRBAND_AUDIO_PACKED_FRAME_BYTES != 0) {
        return 0;
    }
    const size_t frames = src_len / PK_AIRBAND_AUDIO_PACKED_FRAME_BYTES;
    if (dst_frames < frames) return 0;

    uint16_t level = gate->level_q15;
    for (size_t i = 0; i < frames; i++) {
        level = gate_advance(level, audible);
        const int32_t sample16 = apply_program_gain(
            decode_s24le_to_s16(src + i * PK_AIRBAND_AUDIO_PACKED_FRAME_BYTES));
        const int16_t out = (int16_t)((sample16 * (int32_t)level) /
                                      (int32_t)PK_AIRBAND_AUDIO_GATE_FULL_Q15);
        dst[i * 2] = out;
        dst[i * 2 + 1] = out;
    }
    gate->level_q15 = level;
    return frames;
}
