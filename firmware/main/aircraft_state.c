/*
 * aircraft_state.c — per-aircraft state aggregation.
 *
 * Open-addressing table indexed by ICAO % capacity, linear probing for
 * collisions, LRU eviction when the table is full. Mutex-protected so
 * dsp_task (writer via aircraft_state_ingest) and ble_gatt_task
 * (reader via aircraft_state_snapshot) can run on different cores.
 */

#include "aircraft_state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mode-s.h"   /* struct mode_s_msg + MODE_S_UNIT_FEET */
#include "ui_state.h" /* pk_ui_get_own_icao() — pin the bound own-ship
                       * slot against LRU eviction (see lookup_or_claim) */

static const char *TAG = "aircraft";

/* s_table lives in PSRAM (EXT_RAM_BSS). aircraft_t grew enough with the
 * squawk / wake / on_ground additions that keeping all 64 slots in
 * internal DRAM started squeezing ESP-Hosted's boot-time timer-task
 * allocation off the heap. Mutex-guarded reads from PSRAM are cheap
 * enough for the dsp_task ingest path (~30-50 calls/s). */
static EXT_RAM_BSS_ATTR aircraft_t s_table[AIRCRAFT_TABLE_CAPACITY];
static SemaphoreHandle_t           s_lock;

static void take_lock(void)    { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void release_lock(void) { xSemaphoreGive(s_lock); }

void aircraft_state_init(void)
{
    memset(s_table, 0, sizeof(s_table));
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/* Locate an existing slot or claim a free one. LRU eviction on full table.
 * Must be called under s_lock.
 *
 * Own-ship pinning: the slot whose icao24 matches pk_ui_get_own_icao()
 * is excluded from LRU candidacy. After a packet-loss gap the PFD
 * relies on the slot's preserved altitude / velocity / position to
 * keep ALT/VS/GS visible the instant the bound aircraft's first
 * post-gap message arrives — without pinning, that slot can get
 * evicted under a busy sky (≥ 64 unique ICAOs) and reappear empty,
 * which makes the PFD look like it "lost" the binding even though
 * pk_ui_get_own_icao() still points at the correct ICAO. */
static aircraft_t *lookup_or_claim(uint32_t icao24, int64_t now_us)
{
    const uint32_t base     = icao24 % AIRCRAFT_TABLE_CAPACITY;
    const uint32_t own_icao = pk_ui_get_own_icao();
    aircraft_t    *empty    = NULL;
    aircraft_t    *lru      = NULL;

    for (uint32_t step = 0; step < AIRCRAFT_TABLE_CAPACITY; ++step) {
        aircraft_t *s = &s_table[(base + step) % AIRCRAFT_TABLE_CAPACITY];
        if (s->icao24 == icao24) return s;
        if (!empty && s->icao24 == 0) empty = s;
        /* Skip the bound own-ship slot when picking an eviction victim. */
        if (own_icao != 0 && s->icao24 == own_icao) continue;
        if (lru == NULL || s->last_seen_us < lru->last_seen_us) lru = s;
    }

    aircraft_t *chosen = empty ? empty : lru;
    if (chosen == NULL) {
        /* Pathological: every slot is pinned. Can only happen if the
         * own_icao matches the entire table, which lookup_or_claim's
         * "at most one slot per icao24" invariant prevents. Defensive
         * fallback: just reuse the home slot so we don't crash. */
        ESP_LOGW(TAG, "lookup_or_claim: no eviction candidate for %06lX "
                      "(own_icao=%06lX, table full of pinned?) — using "
                      "home slot",
                 (unsigned long)icao24, (unsigned long)own_icao);
        chosen = &s_table[base];
    }
    memset(chosen, 0, sizeof(*chosen));
    chosen->icao24       = icao24;
    chosen->last_seen_us = now_us;
    return chosen;
}

/* Decode the ADS-B aircraft category (DF17 metype 1-4) into the
 * compact pk_wake_t enum. Caller passes the message's metype (1..4 →
 * Category Sets D/C/B/A — note the ordering is reversed) and mesub
 * (0..7). Returns PK_WAKE_NONE for combinations we don't categorise. */
static pk_wake_t decode_wake_category(int metype, int mesub)
{
    /* DO-260B Table 2-67: metype 4 = Set A (most common — airborne
     * powered), metype 3 = Set B (gliders, LTAs, UAV, ...), metype 2
     * = Set C (surface vehicles), metype 1 = Set D (reserved). */
    if (metype == 4) {
        switch (mesub) {
        case 1: return PK_WAKE_LIGHT;
        case 2: return PK_WAKE_SMALL;
        case 3: return PK_WAKE_LARGE;
        case 4: return PK_WAKE_HIGH_VORTEX;
        case 5: return PK_WAKE_HEAVY;
        case 6: return PK_WAKE_HIGH_PERF;
        case 7: return PK_WAKE_ROTOR;
        default: return PK_WAKE_NONE;
        }
    }
    if (metype == 3) {
        switch (mesub) {
        case 1: return PK_WAKE_GLIDER;
        case 2: return PK_WAKE_LTA;
        case 3: return PK_WAKE_PARACHUTE;
        case 4: return PK_WAKE_ULTRALIGHT;
        case 6: return PK_WAKE_UAV;
        case 7: return PK_WAKE_SPACE;
        default: return PK_WAKE_NONE;
        }
    }
    if (metype == 2) {
        switch (mesub) {
        case 1: return PK_WAKE_SURFACE_EMERG;
        case 3: return PK_WAKE_SURFACE_SERVICE;
        case 4: case 5: case 6: case 7:
            return PK_WAKE_SURFACE_OBSTACLE;
        default: return PK_WAKE_NONE;
        }
    }
    return PK_WAKE_NONE;
}

char pk_wake_letter(pk_wake_t w)
{
    switch (w) {
    case PK_WAKE_LIGHT:           return 'L';
    case PK_WAKE_SMALL:           return 'S';
    case PK_WAKE_LARGE:           return 'M';   /* M = Medium (FAA convention) */
    case PK_WAKE_HIGH_VORTEX:     return 'V';
    case PK_WAKE_HEAVY:           return 'H';
    case PK_WAKE_HIGH_PERF:       return 'F';   /* F = Fast / Fighter */
    case PK_WAKE_ROTOR:           return 'R';
    case PK_WAKE_GLIDER:          return 'G';
    case PK_WAKE_LTA:             return 'B';   /* B = Balloon */
    case PK_WAKE_PARACHUTE:       return 'P';
    case PK_WAKE_ULTRALIGHT:      return 'U';
    case PK_WAKE_UAV:             return 'D';   /* D = Drone */
    case PK_WAKE_SPACE:           return 'X';
    case PK_WAKE_SURFACE_EMERG:   return 'E';
    case PK_WAKE_SURFACE_SERVICE: return 'T';   /* T = Tug / service */
    case PK_WAKE_SURFACE_OBSTACLE:return 'O';
    case PK_WAKE_NONE:
    default:                      return ' ';
    }
}

const char *pk_wake_name(pk_wake_t w)
{
    switch (w) {
    case PK_WAKE_LIGHT:            return "Light";
    case PK_WAKE_SMALL:            return "Small";
    case PK_WAKE_LARGE:            return "Medium";
    case PK_WAKE_HIGH_VORTEX:      return "B757-class";
    case PK_WAKE_HEAVY:            return "Heavy";
    case PK_WAKE_HIGH_PERF:        return "High-perf";
    case PK_WAKE_ROTOR:            return "Rotorcraft";
    case PK_WAKE_GLIDER:           return "Glider";
    case PK_WAKE_LTA:              return "Balloon/LTA";
    case PK_WAKE_PARACHUTE:        return "Parachute";
    case PK_WAKE_ULTRALIGHT:       return "Ultralight";
    case PK_WAKE_UAV:              return "Drone/UAV";
    case PK_WAKE_SPACE:            return "Spacecraft";
    case PK_WAKE_SURFACE_EMERG:    return "Emergency vehicle";
    case PK_WAKE_SURFACE_SERVICE:  return "Service vehicle";
    case PK_WAKE_SURFACE_OBSTACLE: return "Surface object";
    case PK_WAKE_NONE:
    default:                       return "";
    }
}

void aircraft_state_ingest(const struct mode_s_msg *mm, int64_t now_us)
{
    if (mm == NULL || !mm->crcok || mm->errorbit >= 0) return;
    /* Only DF5 / DF11 / DF17 / DF18 / DF20 / DF21 carry useful per-aircraft
     * data. DF5 (surveillance identity reply) carries Squawk; DF20
     * (Comm-B altitude) carries altitude; DF21 (Comm-B identity) carries
     * Squawk. Identity-bearing DFs need a separate ingest hook below. */
    const int df = mm->msgtype;
    if (df != 5 && df != 11 && df != 17 && df != 18 && df != 20 && df != 21) return;

    const uint32_t icao24 = ((uint32_t)mm->aa1 << 16)
                          | ((uint32_t)mm->aa2 << 8)
                          | (uint32_t)mm->aa3;
    if (icao24 == 0) return;

    take_lock();
    aircraft_t *a = lookup_or_claim(icao24, now_us);
    a->last_seen_us = now_us;

    /* DF17 / DF18 carry the Extended Squitter sub-types. */
    if (df == 17 || df == 18) {
        if (mm->metype >= 1 && mm->metype <= 4) {
            /* Aircraft identification — callsign + wake category.
             * metype 1..4 maps to Category Sets D/C/B/A; mesub gives
             * the in-set position (Light/Small/Large/Heavy/etc.). */
            memcpy(a->callsign, mm->flight, AIRCRAFT_CALLSIGN_LEN - 1);
            a->callsign[AIRCRAFT_CALLSIGN_LEN - 1] = '\0';
            /* Strip dump1090 trailing underscores for nicer display. */
            for (int i = AIRCRAFT_CALLSIGN_LEN - 2; i >= 0; --i) {
                if (a->callsign[i] == '_' || a->callsign[i] == ' ') {
                    a->callsign[i] = '\0';
                } else {
                    break;
                }
            }
            a->have_callsign = (a->callsign[0] != '\0');
            pk_wake_t w = decode_wake_category(mm->metype, mm->mesub);
            if (w != PK_WAKE_NONE) a->wake = w;
        } else if (mm->metype >= 5 && mm->metype <= 8) {
            /* Surface position — aircraft is on the ground (taxi /
             * runway / apron). Don't try to decode the CPR here; the
             * surface CPR encoding is different from airborne and not
             * yet wired through cpr_decode.c. We just flag the state. */
            a->on_ground = true;
        } else if (mm->metype >= 9 && mm->metype <= 18) {
            /* Airborne position — altitude only (position arrives via CPR
             * path, see aircraft_state_update_position).
             *
             * decode_ac12_field returns 0 as a sentinel when the Q-bit
             * is clear (Gillham 100ft encoding — not implemented in the
             * vendored decoder). Treat altitude==0 as "could not decode"
             * and keep the previous good value, otherwise an airborne
             * plane reporting alt=FL350 will flicker to 0 every time
             * a Gillham-encoded frame slips in. */
            if (mm->altitude != 0) {
                a->altitude_ft = (mm->unit == MODE_S_UNIT_METERS)
                                     ? (int)(mm->altitude * 3.28084 + 0.5)
                                     : mm->altitude;
                a->have_altitude = true;
            }
            /* Airborne position implies the aircraft is no longer on
             * the ground — clear the on_ground flag so a freshly-
             * departed aircraft doesn't keep showing the GND badge
             * after climb-out. */
            a->on_ground = false;
        } else if (mm->metype == 19) {
            /* Airborne velocity (sub-types 1-4).
             *
             * mm->vert_rate is the 9-bit encoded value per RTCA DO-260B
             * Table 2-69: real fpm = (encoded - 1) * 64; encoded == 0
             * means "vertical rate information not available". The
             * vendored decoder (mode-s.c:508) leaves the value un-
             * scaled, so we apply the conversion here. Without it,
             * vert_rate_fpm was off by ~×64 (caller saw raw -32 / +2
             * instead of -1984 / +64 fpm) which also propagated into
             * gdl90.c's GDL90 emit (divides by 64 again → almost
             * always 0) and pfd.c's own-ship VS readout. */
            /* Update turn-rate estimate BEFORE overwriting heading_deg —
             * we need the previous and current values to compute the
             * delta. Sample-to-sample delta is noisy (heading_deg is
             * integer-valued, dt jitters around 1 s), so smooth with an
             * EMA. Skip the update if dt is unrealistic (< 100 ms means
             * frame duplication; > 5 s means stale gap, restart fresh). */
            if (a->prev_velocity_us != 0) {
                int64_t dt_us = now_us - a->prev_velocity_us;
                if (dt_us > 100000 && dt_us < 5000000) {
                    int delta = mm->heading - a->prev_heading_deg;
                    while (delta >  180) delta -= 360;
                    while (delta < -180) delta += 360;
                    float new_rate_dps =
                        (float)delta * 1000000.0f / (float)dt_us;
                    if (a->have_turn_rate) {
                        a->turn_rate_dps =
                            0.5f * a->turn_rate_dps + 0.5f * new_rate_dps;
                    } else {
                        a->turn_rate_dps = new_rate_dps;
                        a->have_turn_rate = true;
                    }
                }
            }
            a->prev_heading_deg  = mm->heading;
            a->prev_velocity_us  = now_us;

            a->heading_deg     = mm->heading;
            a->ground_speed_kt = mm->velocity;
            if (mm->vert_rate == 0) {
                a->vert_rate_fpm = 0;   /* "not available" — leave at 0 */
            } else {
                int v_fpm = (mm->vert_rate - 1) * 64;
                a->vert_rate_fpm = (mm->vert_rate_sign == 0) ? v_fpm : -v_fpm;
            }
            a->have_velocity   = true;
        }
    }

    /* DF5 (Surveillance Identity Reply) and DF21 (Comm-B Identity
     * Reply) both decode the Squawk (4-octal Mode-A code) into
     * mm->identity. Other DFs leave it as stack residue, so only
     * ingest from these two. The decoder always populates the
     * identity field unconditionally (mode-s.c:412-428 is outside
     * any if-block), but for DF other than 5/21 the bit positions
     * map to altitude / other things — interpreting those as a
     * Squawk would be garbage. */
    if (df == 5 || df == 21) {
        a->squawk      = mm->identity;
        a->have_squawk = true;
    }

    /* DF20 (Comm-B altitude reply) carries an AC13 altitude field.
     * DF21 (Comm-B identity reply) occupies the same bit positions
     * with the Squawk identity code instead — there is NO altitude in
     * a DF21 frame, and the vendored mode_s_decode() reflects that by
     * skipping decode_ac13_field() for msgtype 21 (mode-s.c:458-463
     * only handles DF0/4/16/20). Crucially `struct mode_s_msg mm;` in
     * mode_s_detect (mode-s.c:804) is uninitialised, so for a DF21
     * frame `mm->altitude` is whatever the previous decoder call left
     * on the stack — almost always a neighbour aircraft's recently-
     * decoded altitude. Ingesting that scribbles random "altitudes"
     * (often 5000 / 16700 / 33000 ft, depending on what's overhead)
     * over the real aircraft's known altitude, which is exactly the
     * "altitude jumps between 5000, 19900, 33000 for CSZ993X" symptom.
     *
     * Same sentinel-0 guard as the DF17 path: decode_ac13_field
     * returns 0 when the M-bit is set (meters mode — not implemented
     * in the vendored decoder) or when Q=0 / M=0 (Gillham 100ft — also
     * not implemented). */
    if (df == 20 && mm->altitude != 0) {
        a->altitude_ft = (mm->unit == MODE_S_UNIT_METERS)
                             ? (int)(mm->altitude * 3.28084 + 0.5)
                             : mm->altitude;
        a->have_altitude = true;
    }
    release_lock();
}

void aircraft_state_update_position(uint32_t icao24,
                                    double lat, double lon,
                                    int64_t now_us)
{
    if (icao24 == 0) return;
    take_lock();
    aircraft_t *a   = lookup_or_claim(icao24, now_us);
    a->lat          = lat;
    a->lon          = lon;
    a->position_us  = now_us;
    a->last_seen_us = now_us;
    a->have_position = true;
    release_lock();
}

/* qsort comparator: ascending by ICAO24. Stable row order is what the
 * list view + index-based cursor rely on; without it the hash-table
 * scan order shuffles whenever an aircraft enters or leaves the table
 * and the selection cursor lands on a different aircraft. */
static int cmp_aircraft_by_icao(const void *a, const void *b)
{
    uint32_t la = ((const aircraft_t *)a)->icao24;
    uint32_t lb = ((const aircraft_t *)b)->icao24;
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}

size_t aircraft_state_snapshot(aircraft_t *out, size_t cap, int64_t now_us,
                               int64_t max_age_us)
{
    if (out == NULL || cap == 0) return 0;
    size_t n = 0;
    take_lock();
    for (size_t i = 0; i < AIRCRAFT_TABLE_CAPACITY && n < cap; ++i) {
        const aircraft_t *s = &s_table[i];
        if (s->icao24 == 0) continue;
        if ((int64_t)(now_us - s->last_seen_us) > max_age_us) continue;
        out[n++] = *s;
    }
    release_lock();
    if (n > 1) qsort(out, n, sizeof(*out), cmp_aircraft_by_icao);
    return n;
}

bool aircraft_state_get_own(uint32_t icao24, int64_t now_us,
                            int64_t max_age_us, aircraft_t *out)
{
    if (icao24 == 0 || out == NULL) return false;
    bool fresh = false;
    take_lock();
    for (size_t i = 0; i < AIRCRAFT_TABLE_CAPACITY; ++i) {
        const aircraft_t *s = &s_table[i];
        if (s->icao24 != icao24) continue;
        if ((int64_t)(now_us - s->last_seen_us) <= max_age_us) {
            *out  = *s;
            fresh = true;
        }
        break;
    }
    release_lock();
    return fresh;
}

bool pk_aircraft_derive_bank(uint32_t icao24, int64_t now_us,
                             int64_t max_age_us, float *out_bank_deg)
{
    if (icao24 == 0 || out_bank_deg == NULL) return false;

    aircraft_t a;
    if (!aircraft_state_get_own(icao24, now_us, max_age_us, &a)) return false;
    if (!a.have_turn_rate || !a.have_velocity) return false;
    /* Coordinated-turn assumption breaks down at low GS — heading
     * changes are dominated by wind / yaw / skidding rather than a
     * banked turn. 60 kt is a typical airliner taxi / final-approach
     * floor; below it the derivation isn't meaningful. */
    if (a.ground_speed_kt < 60) return false;
    /* Also gate on freshness of the velocity sample: if the latest
     * DF17 metype 19 is more than ~10 s old, the turn-rate EMA is
     * stale (the aircraft may have already rolled out of the turn). */
    if ((int64_t)(now_us - a.prev_velocity_us) > 10LL * 1000000LL) {
        return false;
    }

    /* bank = atan(V × ω / g)   — coordinated-turn formula
     *   V in m/s              = kt × 0.514444
     *   ω in rad/s            = deg/s × π/180
     *   g = 9.81 m/s²
     * Sign of ω carries through to the bank sign (right turn → + ω →
     * + bank), matching the PFD attitude-indicator convention.
     *
     * Bank rate-of-change isn't smoothed beyond the EMA on turn-rate
     * itself — the PFD draws once every ~33 ms so any high-freq
     * residue averages out visually. Clamp to ±60° to keep wild noise
     * (e.g. heading wrap glitches we missed) from rotating the
     * horizon line all the way around. */
    const float KT_TO_MPS = 0.514444f;
    const float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
    const float G_MPSS = 9.81f;
    float v_mps     = (float)a.ground_speed_kt * KT_TO_MPS;
    float omega_rps = a.turn_rate_dps * DEG_TO_RAD;
    float bank_rad  = atanf(v_mps * omega_rps / G_MPSS);
    float bank_deg  = bank_rad * (180.0f / 3.14159265358979323846f);
    if (bank_deg >  60.0f) bank_deg =  60.0f;
    if (bank_deg < -60.0f) bank_deg = -60.0f;
    *out_bank_deg = bank_deg;
    return true;
}
