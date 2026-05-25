/*
 * icao_country.h — ICAO 24-bit address → country lookup.
 *
 * ICAO Annex 10 Vol III (Doc 7910) assigns 24-bit address blocks to
 * contracting states. This module embeds a generated table derived from
 * tar1090's flags.js range list. Lookups not covered by that source
 * return NULL (caller renders empty / "Unknown").
 *
 * Static C arrays in flash, no NVS / SD / net. Refresh with
 * firmware/scripts/gen_icao_country.py.
 *
 * Lookup is O(n) over ~200 generated ranges and returns the smallest
 * matching span so territory sub-ranges override larger parent blocks.
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
