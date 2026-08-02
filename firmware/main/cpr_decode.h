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
 *      currently fresh.
 *
 * Surface-position metype 5-8 does NOT use this global (odd+even
 * pairing) path — see cpr_decode_surface_local() below. Ground targets
 * drop their squitter rate to ~5 s once stationary (RTCA DO-260B
 * §2.2.3.2.4.5.2), which routinely exceeds CPR_PAIR_MAX_AGE_US and
 * would starve global pairing on a quiet apron. Local (single-frame,
 * relative-to-a-known-reference) decoding per DO-260B Appendix A.1.7.3
 * needs only one frame, at the cost of requiring a reference position
 * already known to be within the CPR zone's half-width.
 *
 * The decoder is intentionally allocation-free and fixed-capacity; on
 * an ESP32-P4 the entire table fits in <8 KiB of RAM.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CPR_TABLE_CAPACITY     64           /* aircraft slots */
#define CPR_PAIR_MAX_AGE_US    10000000ULL  /* 10 s window per DO-260B */

/* Local (surface) decode is only trusted within this radius of the
 * reference position — DO-260B ties this to half the surface even-frame
 * latitude zone width: 90/60/2 = 0.75 deg ~= 45 NM. See cpr_decode.c's
 * cpr_decode_surface_local() for why the algorithm can't silently
 * produce a result outside this radius even on a bogus reference. */
#define CPR_SURFACE_VALID_RADIUS_NM 45.0

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
 * Submit a freshly-received position frame's CPR fields for GLOBAL
 * (odd+even pairing) decoding — airborne position, metype 9-18. The
 * function updates the per-aircraft state and, if a global decode is
 * possible, fills *out_pos with valid=true and the decoded lat/lon.
 *
 *   icao24     : 24-bit ICAO address
 *   fflag      : 0 for even frame, 1 for odd frame
 *   is_surface : true if this sample came from a surface-position frame
 *                (metype 5-8). Surface frames should normally be routed
 *                through cpr_decode_surface_local() instead of this
 *                function; the flag exists so that IF a caller ever does
 *                feed a surface sample in here (e.g. a future change),
 *                it can never be paired against an airborne sample for
 *                the same ICAO — airborne and surface CPR use different
 *                Dlat/Dlon scales (360 deg vs 90 deg), so pairing one of
 *                each silently decodes a wrong position with no error
 *                signalled.
 *   lat_cpr    : 17-bit raw latitude field (0..131071)
 *   lon_cpr    : 17-bit raw longitude field (0..131071)
 *   now_us     : current monotonic time in microseconds (esp_timer_get_time())
 *   out_pos    : output; valid=false if no pairing yet.
 *
 * Returns true if *out_pos was just freshly decoded, false otherwise.
 */
bool cpr_decode_position(uint32_t icao24,
                         int      fflag,
                         bool     is_surface,
                         int      lat_cpr,
                         int      lon_cpr,
                         int64_t  now_us,
                         cpr_position_t *out_pos);

/*
 * Decode a single surface-position (metype 5-8) frame using LOCAL
 * (single-frame, reference-relative) CPR decoding per DO-260B Appendix
 * A.1.7.3. Unlike cpr_decode_position() this needs no opposite-parity
 * partner frame — just a reference position already known to be within
 * CPR_SURFACE_VALID_RADIUS_NM of the aircraft's true position.
 *
 *   fflag   : 0 for even frame, 1 for odd frame
 *   lat_cpr : 17-bit raw latitude field (0..131071)
 *   lon_cpr : 17-bit raw longitude field (0..131071)
 *   ref_lat : reference latitude, decimal degrees. Caller resolves this
 *             (own-ship GPS fix preferred, else the aircraft's own last
 *             known position, airborne or surface) — this module does
 *             not know about GPS or the aircraft_state table.
 *   ref_lon : reference longitude, decimal degrees.
 *   out_pos : output. Always written; valid=false on rejection.
 *
 * Returns true and fills *out_pos when the decode succeeds AND the
 * decoded position lies within CPR_SURFACE_VALID_RADIUS_NM of the
 * reference position. Returns false (out_pos->valid = false) otherwise
 * — callers must NOT use out_pos in that case; this function never
 * hands back a position it isn't confident about.
 */
bool cpr_decode_surface_local(int     fflag,
                              int     lat_cpr,
                              int     lon_cpr,
                              double  ref_lat,
                              double  ref_lon,
                              cpr_position_t *out_pos);
