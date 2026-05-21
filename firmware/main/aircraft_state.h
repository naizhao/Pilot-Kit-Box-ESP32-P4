/*
 * aircraft_state.h — per-aircraft state aggregation across Mode-S frames.
 *
 * The dump1090-derived decoder in mode-s.c hands us one Mode-S message
 * at a time. To produce a GDL90 Traffic Report (msg ID 20) the BLE
 * transport must combine fields that arrive in *different* frames —
 * callsign comes from DF17 metype 1..4, altitude + raw CPR from
 * metype 9..18, velocity + heading + vrate from metype 19. This
 * module owns that fusion layer.
 *
 * Internally it holds a small open-addressing table keyed by 24-bit
 * ICAO address (similar shape to cpr_decode.c's table, kept separate
 * to avoid coupling the BLE transport to CPR math). Each slot tracks
 * the freshest known callsign / altitude / position / velocity plus a
 * monotonic last-seen timestamp; the 1 Hz GDL90 emitter walks the
 * table once per second and notifies BLE subscribers about every
 * aircraft seen in the trailing AIRCRAFT_STALE_AGE_US window.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mode_s_msg;  /* forward decl from mode-s.h */

#define AIRCRAFT_TABLE_CAPACITY   64
#define AIRCRAFT_STALE_AGE_US     (60ULL * 1000000ULL)  /* 60 s */
#define AIRCRAFT_CALLSIGN_LEN     9                     /* 8 chars + NUL */

/* Wake-vortex / aircraft-category enum compatible with ADS-B
 * Category Sets A/B/C (DO-260B section 2.2.3.2.5.2). The mapping
 * from raw (metype, mesub) is performed by aircraft_state_ingest.
 * Single-letter rendering for the list view comes from
 * pk_wake_letter(); full names from pk_wake_name(). */
typedef enum {
    PK_WAKE_NONE = 0,        /* unknown / not yet reported */
    PK_WAKE_LIGHT,           /* A1 */
    PK_WAKE_SMALL,           /* A2 */
    PK_WAKE_LARGE,           /* A3 */
    PK_WAKE_HIGH_VORTEX,     /* A4 — B757 */
    PK_WAKE_HEAVY,           /* A5 — B777/A330/... */
    PK_WAKE_HIGH_PERF,       /* A6 — high-performance / fighter */
    PK_WAKE_ROTOR,           /* A7 — helicopter */
    PK_WAKE_GLIDER,          /* B1 */
    PK_WAKE_LTA,             /* B2 — balloon / blimp */
    PK_WAKE_PARACHUTE,       /* B3 */
    PK_WAKE_ULTRALIGHT,      /* B4 */
    PK_WAKE_UAV,             /* B6 — drone */
    PK_WAKE_SPACE,           /* B7 — spacecraft */
    PK_WAKE_SURFACE_EMERG,   /* C1 — emergency vehicle */
    PK_WAKE_SURFACE_SERVICE, /* C3 — service vehicle */
    PK_WAKE_SURFACE_OBSTACLE,/* C4..C7 */
} pk_wake_t;

typedef struct {
    uint32_t icao24;          /* 0 → empty slot */
    int64_t  last_seen_us;

    bool     have_callsign;
    char     callsign[AIRCRAFT_CALLSIGN_LEN];

    bool     have_altitude;
    int      altitude_ft;     /* converted from meters if needed */

    bool     have_position;
    double   lat;
    double   lon;
    int64_t  position_us;     /* timestamp of the last decoded fix */

    bool     have_velocity;
    int      heading_deg;     /* 0..359 */
    int      ground_speed_kt;
    int      vert_rate_fpm;   /* signed; positive = climb */

    bool     have_squawk;     /* set when a DF5/DF21 identity reply was
                                 decoded for this aircraft */
    int      squawk;          /* 4-digit octal Mode-A code 0000..7777 */

    pk_wake_t wake;           /* PK_WAKE_NONE until DF17 metype 1-4 seen */

    bool     on_ground;       /* true if last position-bearing frame was
                                 DF17 metype 5-8 (surface position) */

    /* Turn-rate tracking, used to DERIVE bank angle from coordinated-
     * turn geometry (bank = atan(V × ω / g)). ADS-B doesn't broadcast
     * bank or pitch, so the only way to surface the aircraft's actual
     * attitude over Mode-S is to estimate it from the velocity vector's
     * evolution between successive DF17 metype 19 frames.
     *
     *   - prev_heading_deg / prev_velocity_us hold the previous sample;
     *   - turn_rate_dps is an exponentially-smoothed
     *     delta-heading / delta-time in degrees per second
     *     (signed; + = right turn).
     * have_turn_rate flips on once we've consumed at least two velocity
     * samples within a sane temporal window (≈100 ms to 5 s apart). */
    bool     have_turn_rate;
    float    turn_rate_dps;
    int      prev_heading_deg;
    int64_t  prev_velocity_us;
} aircraft_t;

/* Single-letter abbreviation for the list view (one column). Returns
 * ' ' (space) for PK_WAKE_NONE so the column renders blank rather than
 * showing a misleading code. */
char pk_wake_letter(pk_wake_t w);

/* Human-readable name for the detail pane. Returns "" for PK_WAKE_NONE. */
const char *pk_wake_name(pk_wake_t w);

/*
 * Derive an estimated bank angle (degrees, signed: + = right bank)
 * from the bound aircraft's smoothed turn rate and ground speed using
 * the coordinated-turn formula  bank = atan(V × ω / g)  where V is
 * approximated by GS in m/s and ω is the yaw rate in rad/s.
 *
 * Returns true and fills *out_bank_deg only when:
 *   - the aircraft has at least 2 fresh velocity samples
 *     (have_turn_rate is set);
 *   - the latest velocity report is within max_age_us of now_us;
 *   - ground speed is high enough (≥ 60 kt) for the coordinated-turn
 *     assumption to be sensible — at lower speeds the heading is
 *     dominated by skidding / wind and the derivation devolves to
 *     noise.
 *
 * Returns false (and leaves *out_bank_deg untouched) otherwise. Caller
 * is expected to fall back to IMU roll or render the attitude
 * indicator un-banked in that case.
 *
 * Note this is the AIRCRAFT'S bank under the coordinated-turn
 * assumption — useful when own-ship is bound to a transponder; the
 * kit's own IMU stays authoritative when there's no ADS-B reference.
 */
bool pk_aircraft_derive_bank(uint32_t icao24, int64_t now_us,
                             int64_t max_age_us, float *out_bank_deg);

/* Reset table. Call once on boot. */
void aircraft_state_init(void);

/*
 * Ingest one CRC-valid Mode-S frame. Caller passes the parsed
 * `mode_s_msg` (from mode_s.c) plus a monotonic timestamp in
 * microseconds (esp_timer_get_time()). The function updates the
 * relevant fields of the aircraft's slot, allocating a new slot or
 * evicting the LRU one on collision.
 *
 * Position decoding (CPR) is *not* duplicated here — that lives in
 * cpr_decode.c. The position field is populated by passing the freshly
 * decoded cpr_position_t through aircraft_state_update_position().
 */
void aircraft_state_ingest(const struct mode_s_msg *mm, int64_t now_us);

/*
 * Override an aircraft's position. Used by dsp_task.c after running
 * cpr_decode_position() so the BLE traffic report carries lat/lon.
 */
void aircraft_state_update_position(uint32_t icao24,
                                    double lat, double lon,
                                    int64_t now_us);

/*
 * Copy a snapshot of recently-seen aircraft into *out (capacity cap).
 * Returns the number of entries written. Aircraft whose last_seen_us
 * is older than max_age_us are skipped. Pass AIRCRAFT_STALE_AGE_US
 * to get the "fresh contacts" window that the BLE/GDL90 emitter uses;
 * pass a larger value (e.g. 30 minutes) for diagnostic history dumps.
 * Snapshot is taken under an internal mutex; safe to call from any task.
 */
size_t aircraft_state_snapshot(aircraft_t *out, size_t cap, int64_t now_us,
                               int64_t max_age_us);

/*
 * Copy the slot for `icao24` into *out, but only if its last_seen_us
 * is within max_age_us of now_us. Returns true on fresh hit, false if
 * not present or stale. Takes the same internal mutex as
 * aircraft_state_snapshot/_ingest; safe to call from any task. Used
 * by the PFD to source own-ship altitude / VS / GS from the live ADS-B
 * receive pipeline.
 */
bool aircraft_state_get_own(uint32_t icao24, int64_t now_us,
                            int64_t max_age_us, aircraft_t *out);
