/* Pure filename metadata for AIRBAND recordings; no filesystem dependency. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t session;
    uint32_t channel_khz;
    uint32_t carrier_hz;
    uint32_t volume;
    bool interrupted;
} pk_airband_recording_name_t;

/* Accepts final files emitted by airband_record.c.  In-progress .part files
 * are deliberately not part of this interface. */
bool pk_airband_recording_name_parse(const char *name,
                                     pk_airband_recording_name_t *out);

/* qsort-compatible ordering: newest session first, then newest volume. */
int pk_airband_recording_newest_first(const void *lhs, const void *rhs);
