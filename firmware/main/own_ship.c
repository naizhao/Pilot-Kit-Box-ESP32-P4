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

bool pk_own_heading_resolve(bool own_valid, pk_own_src_t own_src,
                            const aircraft_t *own,
                            bool imu_valid, float imu_yaw_deg,
                            float *out_deg, pk_hdg_src_t *out_src){
    pk_hdg_src_t src = PK_HDG_SRC_NONE;
    float deg = 0.0f;
    if(own_valid && own->have_velocity && own_src == PK_OWN_SRC_BOUND_ADSB){
        deg = (float)own->heading_deg;  src = PK_HDG_SRC_ADSB;   /* 1. 绑定飞机航迹 */
    } else if(imu_valid){
        deg = imu_yaw_deg;              src = PK_HDG_SRC_IMU;    /* 2. IMU 磁航向 */
    } else if(own_valid && own->have_velocity &&
              own_src == PK_OWN_SRC_GPS && own->ground_speed_kt >= 2){
        deg = (float)own->heading_deg;  src = PK_HDG_SRC_GPS;    /* 3. GPS track 兜底 */
    }
    if(out_src) *out_src = src;
    if(src == PK_HDG_SRC_NONE) return false;
    *out_deg = deg;
    return true;
}
