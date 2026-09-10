#include "own_ship.h"
#include <string.h>
#include "gps.h"
#include "pk_own_sampler.h"   /* pk_own_sampler_get_phase() —— 空地状态来源 */
#include "ui_state.h"

/*
 * 空地状态取飞行相位状态机的**明确结论**。
 *
 * GPS 这一路本身不携带空地位，老实现于是恒 have_air_ground=false；而 GDL90
 * 线上没有"未知"编码，未知发出去就是 surface。相位机（pk_flight_phase）已经
 * 在用 60 s 位移窗口 + 振动地板 + 绑定机 ADS-B 位交叉判定这件事，它给出
 * AIRBORNE / 地面族时是有据的结论，给出 UNKNOWN 时是真的不知道——那时就
 * 保持"不知道"，由 pk_own_gdl90_should_emit() 决定整帧不发。
 */
static void air_ground_from_phase(aircraft_t *out){
    const pk_flight_phase_t ph = pk_own_sampler_get_phase();
    if(ph == PK_PHASE_UNKNOWN){
        out->have_air_ground = false;
        out->on_ground       = false;
        return;
    }
    out->have_air_ground = true;
    out->on_ground       = pk_flight_phase_is_ground_family(ph);
}

bool pk_own_ship_resolve_ex(int64_t now_us, int64_t max_age_us,
                            aircraft_t *out, pk_own_src_t *src,
                            pk_own_alt_t *alt){
    pk_own_alt_t a;
    memset(&a, 0, sizeof(a));

    /* 1. Manual binding wins outright. */
    uint32_t icao = pk_ui_get_own_icao();
    if(icao != 0 && aircraft_state_get_own(icao, now_us, max_age_us, out)){
        /* 绑定机自报的高度就是气压高度（Mode-C/DF17），与目标同基准。 */
        a.have_press_alt      = out->have_altitude;
        a.press_alt_ft        = out->have_altitude ? out->altitude_ft : 0;
        /* TC20-22 的 HAE 原样带出，不与 MSL 混。ADS-B 不播 MSL 正高。 */
        a.have_gnss_ellipsoid = out->have_gnss_altitude;
        a.gnss_ellipsoid_ft   = out->have_gnss_altitude ? out->gnss_altitude_ft : 0;
        /* 绑定机若没播过空地位（只发过身份帧/DF11），用相位机兜底——否则
         * 0x0A 会因为"空地未知"被整帧压掉，而我们其实知道自己在哪。 */
        if(!out->have_air_ground) air_ground_from_phase(out);
        if(src) *src = PK_OWN_SRC_BOUND_ADSB;
        if(alt) *alt = a;
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
        /* aircraft_t.altitude_ft 的定义是**气压高度**。GGA 给的是 GNSS 正高
         * (MSL)，两者差着当地气压偏差——把 MSL 写进这里，它会被 ble_gatt 当
         * 压力高度编进 GDL90，被 EFB 拿去和别的飞机的压力高度比。MSL 单独
         * 走 pk_own_alt_t，谁要用谁明写。 */
        out->have_altitude      = false;
        out->altitude_ft        = 0;
        /* 椭球高是 ADS-B TC20-22 的量，GPS 兜底没有——不得拿 MSL 顶替。 */
        out->have_gnss_altitude = false;
        out->gnss_altitude_ft   = 0;
        /* 地速/航迹跟着 GPS 的字段级有效位走。老实现无条件置 true，于是
         * RMC 的空字段经 atof 变成的那个 0 上了线：EFB 上本机是一架笃定停在
         * 原地、机头朝北的飞机。 */
        out->have_ground_speed = g.have_ground_speed;
        out->ground_speed_kt   = g.have_ground_speed ? g.ground_speed_kt : 0;
        out->have_heading      = g.have_track;
        out->heading_deg       = g.have_track ? g.track_deg : 0;
        /* 复合位：两者同时有效才算有一支可用的速度矢量。 */
        out->have_velocity     = out->have_ground_speed && out->have_heading;
        /* GPS 这一路没有垂速来源。标成"不可用"而不是 0 fpm——后者是个
         * 合法读数（平飞），读的人分不出"平飞"和"没有数据"。 */
        out->have_vertical_rate = false;
        out->vert_rate_fpm   = 0;
        out->last_seen_us    = g.updated_us;
        air_ground_from_phase(out);
        a.have_gnss_msl = g.have_altitude;
        a.gnss_msl_ft   = g.have_altitude ? g.altitude_ft : 0;
        if(src) *src = PK_OWN_SRC_GPS;
        if(alt) *alt = a;
        return true;
    }

    /* 3. Nothing. */
    if(src) *src = PK_OWN_SRC_NONE;
    if(alt) *alt = a;
    return false;
}

bool pk_own_ship_resolve(int64_t now_us, int64_t max_age_us,
                         aircraft_t *out, pk_own_src_t *src){
    return pk_own_ship_resolve_ex(now_us, max_age_us, out, src, NULL);
}

bool pk_own_gdl90_should_emit(bool own_valid, const aircraft_t *own){
    if(!own_valid || own == NULL) return false;
    /* 见 own_ship.h：线上没有"空地未知"的编码，发了就是宣称在地面。 */
    return own->have_air_ground;
}

bool pk_own_heading_resolve(bool own_valid, pk_own_src_t own_src,
                            const aircraft_t *own,
                            bool imu_valid, float imu_yaw_deg,
                            float *out_deg, pk_hdg_src_t *out_src){
    pk_hdg_src_t src = PK_HDG_SRC_NONE;
    float deg = 0.0f;
    /* 判据是 have_heading（航迹本身），不是复合的 have_velocity：地面目标
     * 会出现"有航迹没地速"，那时航迹依然可用。 */
    if(own_valid && own->have_heading && own_src == PK_OWN_SRC_BOUND_ADSB){
        deg = (float)own->heading_deg;  src = PK_HDG_SRC_ADSB;   /* 1. 绑定飞机航迹 */
    } else if(imu_valid){
        deg = imu_yaw_deg;              src = PK_HDG_SRC_IMU;    /* 2. IMU 磁航向 */
    } else if(own_valid && own->have_heading && own->have_ground_speed &&
              own_src == PK_OWN_SRC_GPS && own->ground_speed_kt >= 2){
        deg = (float)own->heading_deg;  src = PK_HDG_SRC_GPS;    /* 3. GPS track 兜底 */
    }
    if(out_src) *out_src = src;
    if(src == PK_HDG_SRC_NONE) return false;
    *out_deg = deg;
    return true;
}
