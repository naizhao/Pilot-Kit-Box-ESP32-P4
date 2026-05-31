#pragma once
#include <stdint.h>
#include "aircraft_state.h"

typedef enum {
    PK_OWN_SRC_NONE = 0,
    PK_OWN_SRC_BOUND_ADSB,   /* user manually bound an ADS-B aircraft */
    PK_OWN_SRC_GPS,          /* fallback: GPS fix */
} pk_own_src_t;

/* Resolve the effective own-ship.
   Priority: manual ADS-B binding ALWAYS wins; else GPS fix; else none.
   Fills *out and (if non-NULL) *src. Returns true if a usable own-ship exists. */
bool pk_own_ship_resolve(int64_t now_us, int64_t max_age_us,
                         aircraft_t *out, pk_own_src_t *src);
