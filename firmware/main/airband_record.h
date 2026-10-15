#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_RECORD_BLOCK_SAMPLES 64u
#define PK_AIRBAND_RECORD_QUEUE_BLOCKS 512u
/* 16 kHz / 64 samples = 250 capture blocks/s.  Tolerate isolated DMA
 * timeouts, but treat 25 consecutive empty blocks (100 ms of audio) as a
 * real source discontinuity. */
#define PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS 25u
typedef struct {
    bool started, enabled, active, stopping, sd_missing, interrupted, error;
    uint32_t mount_generation, segments, dropped_blocks, dropped_samples;
    uint32_t write_errors, sync_errors, close_errors, recovery_errors, recovered;
    /* Existing .part.wav entries retained unchanged by preparation. Content
     * and size do not imply an I/O error or block new recording. This is a
     * cumulative inspection count; automatic preparation never recovers. */
    uint32_t recovery_skipped;
    uint32_t queued_blocks, queue_high_water, source_gaps;
    uint32_t max_write_us, max_sync_us;
    uint64_t bytes;
    char file[112];
} pk_airband_record_status_t;

bool pk_airband_record_init(void);
/* Enabling requests writer-side preparation without filesystem I/O here.
 * Capture is accepted only once that mounted card has prepared successfully. */
bool pk_airband_record_set_enabled(bool enabled);
/* Capture task only: at most 64 samples; larger calls interrupt the segment.
 * Bounded copy, no filesystem or blocking mutex. Invalid RF, a sustained PCM
 * source gap and a closed squelch end the segment; speaker mute is not an
 * input. */
bool pk_airband_record_submit(const int16_t *pcm, size_t frames, bool rf_valid,
                              bool squelch_open, uint32_t channel_khz,
                              uint32_t carrier_hz);
void pk_airband_record_get(pk_airband_record_status_t *out);
const char *pk_airband_record_label(const pk_airband_record_status_t *status);
