/*
 * icao_country.h — ICAO 24-bit address → country lookup.
 *
 * ICAO Annex 10 Vol III (Doc 7910) assigns contiguous 24-bit address
 * blocks to each contracting state. The blocks are static and well
 * documented; this module embeds a curated subset covering all major
 * aviation nations (China + the user's regional traffic + the global
 * top-30 sources). Lookups not covered return NULL (caller renders
 * empty / "Unknown").
 *
 * Not exhaustive — see the expansion note at the top of icao_country.c.
 * Static C arrays in flash, no NVS / SD / net.
 *
 * Lookup is O(log n) binary search over sorted-by-low-bound ranges.
 */
#pragma once

#include <stdint.h>

typedef struct {
    const char *iso2;   /* ISO 3166-1 alpha-2 (2 chars + NUL), e.g. "CN" */
    const char *name;   /* Human-readable country name, e.g. "China"     */
} pk_country_t;

/*
 * Resolve an ICAO 24-bit address to its country of registration.
 * Returns NULL when the address is outside any known assigned block.
 * Safe to call from any task — pure data lookup, no locking.
 */
const pk_country_t *pk_country_from_icao24(uint32_t icao24);
