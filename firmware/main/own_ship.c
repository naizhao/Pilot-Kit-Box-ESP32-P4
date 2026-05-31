#include "own_ship.h"
#include <string.h>
#include "gps.h"
#include "ui_state.h"

bool pk_own_ship_resolve(int64_t now_us, int64_t max_age_us,
                         aircraft_t *out, pk_own_src_t *src){
    /* 1. Manual binding wins outright. */
    uint32_t icao = pk_ui_get_own_icao();
    if(icao != 0 && aircraft_state_get_own(icao, now_us, max_age_us, out)){
        if(src) *src = PK_OWN_SRC_BOUND_ADSB;
        return true;
    }
    /* 2. Fallback to GPS fix. */
    pk_gps_state_t g;
    if(pk_gps_get(&g)){
        memset(out, 0, sizeof(*out));
        out->icao24          = 0;
        out->have_position   = true;
        out->lat             = g.lat;
        out->lon             = g.lon;
        out->have_altitude   = g.have_altitude;
        out->altitude_ft     = g.altitude_ft;
        out->have_velocity   = true;
        out->heading_deg     = g.track_deg;
        out->ground_speed_kt = g.ground_speed_kt;
        out->vert_rate_fpm   = 0;          /* no baro/VS yet */
        out->last_seen_us    = g.updated_us;
        if(src) *src = PK_OWN_SRC_GPS;
        return true;
    }
    /* 3. Nothing. */
    if(src) *src = PK_OWN_SRC_NONE;
    return false;
}
