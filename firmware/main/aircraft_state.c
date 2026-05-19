/*
 * aircraft_state.c — per-aircraft state aggregation.
 *
 * Open-addressing table indexed by ICAO % capacity, linear probing for
 * collisions, LRU eviction when the table is full. Mutex-protected so
 * dsp_task (writer via aircraft_state_ingest) and ble_gatt_task
 * (reader via aircraft_state_snapshot) can run on different cores.
 */

#include "aircraft_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mode-s.h"  /* struct mode_s_msg + MODE_S_UNIT_FEET */

static aircraft_t        s_table[AIRCRAFT_TABLE_CAPACITY];
static SemaphoreHandle_t s_lock;

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
 * Must be called under s_lock. */
static aircraft_t *lookup_or_claim(uint32_t icao24, int64_t now_us)
{
    const uint32_t base  = icao24 % AIRCRAFT_TABLE_CAPACITY;
    aircraft_t    *empty = NULL;
    aircraft_t    *lru   = &s_table[base];

    for (uint32_t step = 0; step < AIRCRAFT_TABLE_CAPACITY; ++step) {
        aircraft_t *s = &s_table[(base + step) % AIRCRAFT_TABLE_CAPACITY];
        if (s->icao24 == icao24) return s;
        if (!empty && s->icao24 == 0) empty = s;
        if (s->last_seen_us < lru->last_seen_us) lru = s;
    }

    aircraft_t *chosen = empty ? empty : lru;
    memset(chosen, 0, sizeof(*chosen));
    chosen->icao24       = icao24;
    chosen->last_seen_us = now_us;
    return chosen;
}

void aircraft_state_ingest(const struct mode_s_msg *mm, int64_t now_us)
{
    if (mm == NULL || !mm->crcok || mm->errorbit >= 0) return;
    /* Only DF11 / DF17 / DF18 / DF20 / DF21 carry useful per-aircraft data. */
    const int df = mm->msgtype;
    if (df != 11 && df != 17 && df != 18 && df != 20 && df != 21) return;

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
            /* Aircraft identification — callsign. */
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
        } else if (mm->metype >= 9 && mm->metype <= 18) {
            /* Airborne position — altitude only (position arrives via CPR
             * path, see aircraft_state_update_position). */
            a->altitude_ft = (mm->unit == MODE_S_UNIT_METERS)
                                 ? (int)(mm->altitude * 3.28084 + 0.5)
                                 : mm->altitude;
            a->have_altitude = true;
        } else if (mm->metype == 19) {
            /* Airborne velocity (sub-types 1-4). */
            a->heading_deg     = mm->heading;
            a->ground_speed_kt = mm->velocity;
            a->vert_rate_fpm   = (mm->vert_rate_sign == 0)
                                     ?  mm->vert_rate
                                     : -mm->vert_rate;
            a->have_velocity   = true;
        }
    }

    /* DF20 / DF21 (Mode-S long surveillance replies) also report altitude. */
    if (df == 20 || df == 21) {
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
