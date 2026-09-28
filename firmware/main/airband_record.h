#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_RECORD_BLOCK_SAMPLES 64u
#define PK_AIRBAND_RECORD_QUEUE_BLOCKS 64u
typedef struct {
    bool started, enabled, active, stopping, sd_missing, interrupted, error;
    uint32_t mount_generation, segments, dropped_blocks, dropped_samples;
    uint32_t write_errors, sync_errors, close_errors, recovery_errors, recovered;
    uint32_t queued_blocks;
    uint64_t bytes;
    char file[112];
} pk_airband_record_status_t;

bool pk_airband_record_init(void);
bool pk_airband_record_set_enabled(bool enabled);
/* Capture task only: at most 64 samples; larger calls interrupt the segment.
 * Bounded copy, no filesystem or blocking mutex. Invalid
 * RF/PCM and a closed squelch end the segment; speaker mute is not an input. */
bool pk_airband_record_submit(const int16_t *pcm, size_t frames, bool rf_valid,
                              bool squelch_open, uint32_t channel_khz,
                              uint32_t carrier_hz);
void pk_airband_record_get(pk_airband_record_status_t *out);
const char *pk_airband_record_label(const pk_airband_record_status_t *status);
