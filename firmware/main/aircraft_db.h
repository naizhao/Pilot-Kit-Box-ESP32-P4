/*
 * aircraft_db.h — ICAO 24-bit address → aircraft type / model / registration.
 *
 * The 24-bit ICAO transponder address uniquely identifies a registered
 * aircraft worldwide. It is NOT broadcast inside the ADS-B payload, but
 * is the cleartext header of every Mode-S frame. This module maps that
 * address to:
 *   - ICAO type designator (e.g. "B738", "A320", "EC35")
 *   - canonical human-readable model name (e.g. "BOEING 737-800")
 *   - ICAO Doc 8643 technical descriptor (e.g. "L2J" = Land 2-Jet)
 *   - aircraft registration tail (e.g. "B-5797", "N12345", "9V-MBH")
 *
 * Data source: tar1090-db (github.com/wiedehopf/tar1090-db) — mictronics'
 * aggregated dump of ~600 k observed aircraft. Embedded in the firmware
 * as `aircraft_db.bin` via EMBED_FILES, generated offline by
 * `firmware/scripts/gen_aircraft_db.py`. See that script's docstring for
 * the file format and refresh procedure.
 *
 * What we deliberately DON'T expose: the per-aircraft `flags` field
 * (military / VIP / PIA / LADD bits). Identifying military aircraft from
 * a civilian receiver is legally fraught in some jurisdictions (specifically
 * PRC — military aircraft surveillance is treated as espionage under
 * domestic law). The firmware never embeds or surfaces those bits.
 *
 * Lookup is O(log n) binary search over an ICAO24-sorted record array
 * mapped directly out of flash. No heap allocation; safe to call from
 * any task. ~20 µs worst case at ~570 k entries.
 */
#pragma once

#include <stdint.h>

/*
 * Validate the embedded blob and cache header pointers. Call once at
 * boot from app_main. On bad magic / version the module logs an error
 * and leaves itself disabled — subsequent lookups return NULL.
 */
void pk_aircraft_db_init(void);

/*
 * Resolve ICAO24 → ICAO type designator (e.g. "B738"). Returns a
 * pointer to a NUL-terminated string in flash (do not free), or NULL
 * if the aircraft isn't in the database OR is in the database with
 * only a registration (no type code). Pointer is stable for the
 * lifetime of the program.
 */
const char *pk_aircraft_type_code(uint32_t icao24);

/*
 * Resolve ICAO24 → canonical model name for the aircraft's type (e.g.
 * "BOEING 737-800"). Same NULL / lifetime semantics as the code variant.
 * One model name per type — chosen as the most common model string
 * observed for that type across all aircraft in the dataset.
 */
const char *pk_aircraft_type_model(uint32_t icao24);

/*
 * Resolve ICAO24 → ICAO Doc 8643 technical descriptor (e.g. "L2J" =
 * Land 2-Jet, "H2T" = Helicopter 2-Turboprop). Three-char string.
 * NULL if the aircraft has no type code OR its type isn't in the
 * descriptor table (rare — 96% of types have a desc).
 */
const char *pk_aircraft_type_desc(uint32_t icao24);

/*
 * Resolve ICAO24 → aircraft registration tail (e.g. "B-5797", "N12345").
 * NULL if the aircraft is in the database without a registration field
 * OR isn't in the database at all. Note that an aircraft can have a
 * registration but no type code, in which case pk_aircraft_type_code()
 * returns NULL while this returns the tail.
 */
const char *pk_aircraft_registration(uint32_t icao24);
