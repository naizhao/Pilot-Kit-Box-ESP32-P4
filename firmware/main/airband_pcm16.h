/* Continuous 48k packed signed-24 stereo -> 16k signed-16 mono program. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_PCM16_RATE 16000u
#define PK_AIRBAND_PCM16_TAPS 63u
typedef struct {
    uint32_t input_frames, output_frames;
    uint32_t raw_peak, pre_peak, post_peak;
    uint64_t raw_sum_sq, pre_sum_sq, post_sum_sq;
    uint32_t raw_clips, pre_clips, post_clips;
} pk_airband_pcm16_stats_t;
typedef struct {
    int32_t history[PK_AIRBAND_PCM16_TAPS];
    unsigned cursor, phase;
    pk_airband_pcm16_stats_t stats;
} pk_airband_pcm16_t;
/* Reset FIR/phase on retune, invalid RF or discontinuity. */
void pk_airband_pcm16_reset(pk_airband_pcm16_t *s);
/* Clear a diagnostic window (normally 1s) without disturbing FIR continuity. */
void pk_airband_pcm16_stats_reset(pk_airband_pcm16_t *s);
/* Atomic reject for malformed input or insufficient output capacity. Gain is
 * applied only after the FIR. Output is independent of speaker volume/mute. */
size_t pk_airband_pcm16_process(pk_airband_pcm16_t *s, const uint8_t *packed,
                                size_t bytes, int16_t *mono, size_t capacity,
                                unsigned program_gain);
