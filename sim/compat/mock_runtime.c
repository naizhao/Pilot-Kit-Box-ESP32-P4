/*
 * mock_runtime.c — 见 mock_runtime.h。
 *
 * 提供 pfd_hsi_traffic.c 所需的四个数据源桩：IMU 采样、本机解算、气压、
 * 目标表快照。数值是合成的，但**结构与真实接口完全一致**，所以被测的
 * 渲染代码走的是与固件相同的分支。
 */

#include "mock_runtime.h"

#include <math.h>
#include <string.h>

#include "aircraft_state.h"
#include "baro.h"
#include "imu_task.h"
#include "own_ship.h"

/* 本机基准位置：北京首都机场以北一点，纬度值只影响经度方向的缩放，
 * 取多少都行，但取真实值可顺带验证磁偏角查表没走进死区。 */
#define OWN_LAT   40.0
#define OWN_LON  116.6

static float s_yaw_deg   = 0.0f;
static int   s_own_alt   = 23000;
static bool  s_own_ok    = true;
static bool  s_traffic_ok = true;

void pk_mock_update(float yaw_deg, int own_alt_ft)
{
    s_yaw_deg = yaw_deg;
    s_own_alt = own_alt_ft;
}

void pk_mock_set_enabled(bool own_valid, bool traffic_valid)
{
    s_own_ok     = own_valid;
    s_traffic_ok = traffic_valid;
}

/* ── IMU ──────────────────────────────────────────────────────────── */
bool pk_imu_sample_get(pk_imu_sample_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->valid    = s_own_ok;
    out->yaw_deg  = s_yaw_deg;
    out->accuracy = 3;
    return s_own_ok;
}

/* ── 本机 ─────────────────────────────────────────────────────────── */
bool pk_own_ship_resolve(int64_t now_us, int64_t max_age_us,
                         aircraft_t *out, pk_own_src_t *src)
{
    (void)now_us; (void)max_age_us;
    if (!s_own_ok) return false;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->icao24        = 0x780ABC;
        out->have_position = true;
        out->lat           = OWN_LAT;
        out->lon           = OWN_LON;
        out->have_altitude = true;
        out->altitude_ft   = s_own_alt;
        out->have_velocity = true;
        out->heading_deg   = (int)s_yaw_deg;
    }
    if (src) *src = PK_OWN_SRC_BOUND_ADSB;
    return true;
}

/* ── 气压 ─────────────────────────────────────────────────────────── */
bool pk_baro_get(pk_baro_state_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->valid       = true;
    out->pressure_pa = 40000.0f;      /* ≈ 23 000 ft 标准大气 */
    out->temp_c      = -30.0f;
    out->alt_ft      = s_own_alt - 120;   /* 与 ADS-B 权威高度略有出入，真实情况如此 */
    out->vs_fpm      = 0;
    return true;
}

/* ── 目标表 ───────────────────────────────────────────────────────── */
/*
 * 绕本机撒一圈目标，方位刻意覆盖三种情形：
 *   - 前方 ±85° 内：正常画在罗盘外圈，带相对高度标签
 *   - 前方但无高度：只画菱形，不画标签
 *   - 后方（|rel| > 85°）：不画，计入右下角的后方计数
 * 相对方位 = 目标真方位 - 本机航向，所以这里按「相对本机机头的角度」布点，
 * 再换算回经纬度，随 yaw 转动时目标会绕着罗盘转 —— 正是要验证的行为。
 */
static void place(aircraft_t *a, uint32_t icao, float rel_deg, float dist_nm,
                  bool have_alt, int alt_ft)
{
    memset(a, 0, sizeof(*a));
    a->icao24        = icao;
    a->have_position = true;

    float brg_true = s_yaw_deg + rel_deg;
    float rad      = brg_true * (float)M_PI / 180.0f;
    double dlat    = (double)(dist_nm / 60.0f) * cos(rad);
    double dlon    = (double)(dist_nm / 60.0f) * sin(rad)
                   / cos(OWN_LAT * M_PI / 180.0);
    a->lat = OWN_LAT + dlat;
    a->lon = OWN_LON + dlon;

    a->have_altitude = have_alt;
    a->altitude_ft   = alt_ft;
    a->have_velocity = true;
    a->heading_deg   = (int)brg_true;
    a->vert_rate_fpm = 0;
}

size_t aircraft_state_snapshot(aircraft_t *out, size_t cap, int64_t now_us,
                               int64_t max_age_us)
{
    (void)now_us; (void)max_age_us;
    if (!out || cap == 0 || !s_traffic_ok) return 0;

    /* rel_deg, dist_nm, have_alt, alt_ft —— 相对高度取 ±(几百~几千) ft，
     * 好让标签把 +05 / -12 这类两位数与三位数的宽度都覆盖到。 */
    static const struct { float rel, dist; bool have_alt; int alt; } SET[] = {
        {  -70.0f,  8.0f, true,  23000 + 1500 },
        {  -35.0f,  5.0f, true,  23000 -  700 },
        {   -8.0f, 12.0f, true,  23000 + 9900 },   /* 相对高度会被夹到 +99 */
        {   20.0f,  6.0f, false, 0            },   /* 无高度 → 只画菱形 */
        {   58.0f,  9.0f, true,  23000 -  300 },
        {   84.0f,  7.0f, true,  23000 + 200  },   /* 贴近 85° 边界 */
        {  120.0f, 10.0f, true,  23000 + 100  },   /* 后方 */
        { -150.0f, 11.0f, true,  23000 - 400  },   /* 后方 */
    };

    size_t n = sizeof(SET) / sizeof(SET[0]);
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; ++i) {
        place(&out[i], 0x3C0000 + (uint32_t)i, SET[i].rel, SET[i].dist,
              SET[i].have_alt, SET[i].alt);
    }
    return n;
}
