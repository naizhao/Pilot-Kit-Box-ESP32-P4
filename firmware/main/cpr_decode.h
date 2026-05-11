/*
 * cpr_decode.h — ADS-B Compact Position Reporting (CPR) global decoder.
 *
 * The naizhao/esp32-rtl-sdr fork's mode-s.c parses each individual ADS-B
 * airborne-position frame down to its raw 17-bit CPR latitude and CPR
 * longitude fields plus the odd/even flag, but the geodetic lat/lon
 * coordinates only fall out once an odd frame and an even frame from the
 * same aircraft are combined (RTCA DO-260B Appendix A.1.7.2). This
 * module owns that pairing layer:
 *
 *   1. A small open-addressing table keyed by 24-bit ICAO address holds
 *      each aircraft's most recent even-frame and odd-frame CPR samples.
 *   2. Whenever cpr_decode_position() receives a new sample, it pairs
 *      with the freshest opposite-parity sample (if any, and within
 *      CPR_PAIR_MAX_AGE_US) and computes the geodetic position.
 *   3. The decoded position is cached on the per-aircraft entry so
 *      subsequent calls can be answered even when only one parity is
 *      currently fresh — useful for the surface-position metype 5-8
 *      sub-protocol we don't decode in Phase 2 but plan for in Phase 3.
 *
 * The decoder is intentionally allocation-free and fixed-capacity; on
 * an ESP32-P4 the entire table fits in <8 KiB of RAM.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CPR_TABLE_CAPACITY     64           /* aircraft slots */
#define CPR_PAIR_MAX_AGE_US    10000000ULL  /* 10 s window per DO-260B */

typedef struct {
    bool   valid;       /* True if (lat, lon) is currently populated. */
    double lat;
    double lon;
} cpr_position_t;

/*
 * Reset the entire table. Call once on boot.
 */
void cpr_init(void);

/*
 * Submit a freshly-received airborne-position frame's CPR fields. The
 * function updates the per-aircraft state and, if a global decode is
 * possible, fills *out_pos with valid=true and the decoded lat/lon.
 *
 *   icao24  : 24-bit ICAO address
 *   fflag   : 0 for even frame, 1 for odd frame
 *   lat_cpr : 17-bit raw latitude field (0..131071)
 *   lon_cpr : 17-bit raw longitude field (0..131071)
 *   now_us  : current monotonic time in microseconds (esp_timer_get_time())
 *   out_pos : output; valid=false if no pairing yet.
 *
 * Returns true if *out_pos was just freshly decoded, false otherwise.
 */
bool cpr_decode_position(uint32_t icao24,
                         int      fflag,
                         int      lat_cpr,
                         int      lon_cpr,
                         int64_t  now_us,
                         cpr_position_t *out_pos);
