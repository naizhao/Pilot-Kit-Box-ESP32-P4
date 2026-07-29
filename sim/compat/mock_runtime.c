/*
 * mock_runtime.c — 见 mock_runtime.h。
 *
 * 提供 pfd_hsi_traffic.c 所需的四个数据源桩：IMU 采样、本机解算、气压、
 * 目标表快照。数值是合成的，但**结构与真实接口完全一致**，所以被测的
 * 渲染代码走的是与固件相同的分支。
 */

#include "mock_runtime.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
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
    /* PK_SIM_HDG=<deg> 把航向钉住。动画驱动的 yaw 每帧都在变，截不出
     * 「正好 60°」那一帧，而验证「正北朝上时本机符号跟着航向转」恰恰需要
     * 一个确定的角度。 */
    {
        const char *e = getenv("PK_SIM_HDG");
        out->yaw_deg = e ? (float)atof(e) : s_yaw_deg;
    }
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
 * 目标集刻意做成「乱」的，因为真实空域就是乱的：航向各指各的、高度层交错、
 * 方位上有扎堆也有孤点。整齐排布的假数据只会让布局问题藏起来。
 *
 * 三件事必须与真实情况一致，否则验证的是假象：
 *   - **航向与方位无关**。此前把 heading 设成了目标相对本机的方位角，等于
 *     所有飞机都在背离本机飞——迎头接近的那一类根本没被画出来过，而那恰是
 *     防撞最该看清的。
 *   - **高度跨越三个着色档**（±1000 ft 内 / 高于 / 低于），还要有无高度数据的。
 *   - **方位上要有拥挤处**，用来验证标签避让；否则永远撞不上，等于没测。
 */
static void place(aircraft_t *a, uint32_t icao, float rel_deg, float dist_nm,
                  bool have_alt, int alt_ft, bool have_vel, int track_deg,
                  const char *callsign, int squawk, int age_s, int64_t now_us)
{
    memset(a, 0, sizeof(*a));
    a->icao24        = icao;
    a->have_position = true;
    /* 「上次收到报文距今多久」是看板的 SEEN 列，也是判断其余七列还能不能信的
     * 唯一依据。此前这里 memset 成 0，于是每一架的 age 都被钳到 99——新鲜/
     * 转暗/转橙三档配色一档也压不到。按 15 / 30 秒两个阈值前后各铺几个。 */
    a->last_seen_us  = now_us - (int64_t)age_s * 1000000LL;

    /* 呼号/应答机码是**列表页**才用得上的列，雷达页压不到它们。
     * callsign = NULL 表示这架没广播过呼号——列表必须退回 ICAO 十六进制，
     * 而不是留一片空白让人以为渲染坏了。squawk < 0 同理表示没收到 DF5/DF21。 */
    if (callsign) {
        a->have_callsign = true;
        snprintf(a->callsign, sizeof(a->callsign), "%s", callsign);
    }
    if (squawk >= 0) {
        a->have_squawk = true;
        a->squawk      = squawk;
    }

    float brg_true = s_yaw_deg + rel_deg;
    float rad      = brg_true * (float)M_PI / 180.0f;
    double dlat    = (double)(dist_nm / 60.0f) * cos(rad);
    double dlon    = (double)(dist_nm / 60.0f) * sin(rad)
                   / cos(OWN_LAT * M_PI / 180.0);
    a->lat = OWN_LAT + dlat;
    a->lon = OWN_LON + dlon;

    a->have_altitude = have_alt;
    a->altitude_ft   = alt_ft;
    a->have_velocity = have_vel;
    a->heading_deg   = track_deg;      /* 与方位角无关，各飞各的 */
    /* 地速也要喂：右栏列表有这一列，不喂就显示成一个看似真实的 0——比缺数据
     * 更糟，因为 0 kt 是个合法读数（悬停/地面）。按 icao 散开，覆盖巡航到
     * 进近的范围。 */
    a->ground_speed_kt = have_vel ? (float)(120 + (int)(icao % 7) * 55) : 0.0f;
    /* 升降率跟着高度差走：高的在爬、低的在降，好让列表里的 ^v 有东西可显。 */
    a->vert_rate_fpm = have_alt ? ((alt_ft % 3 == 0) ? 800
                                : (alt_ft % 3 == 1) ? -650 : 0) : 0;
}

size_t aircraft_state_snapshot(aircraft_t *out, size_t cap, int64_t now_us,
                               int64_t max_age_us)
{
    (void)max_age_us;
    if (!out || cap == 0 || !s_traffic_ok) return 0;
    /* PK_SIM_NO_TRAFFIC=1 → 空快照。空列表是必须验证的一种状态：留白屏会被
     * 当成页面没画出来，得有明确的「当前无目标」文案。 */
    { const char *e = getenv("PK_SIM_NO_TRAFFIC"); if (e && e[0] == '1') return 0; }

    /* 高度一栏是**相对本机**的差值，不是绝对高度：本机高度由模拟器动画驱动
     * （每帧都在变），写死绝对值会让相对高度整体漂移，三档配色也就试不准。 */
    /* rel（相对机头方位）, 距离, 有无高度, 相对高度, 有无航迹, 真航迹 */
    static const struct {
        float rel, dist; bool have_alt; int alt; bool have_vel; int track;
        const char *call; int sqk; int age;
    } SET[] = {
        /* ── 威胁：同高度(±1000 ft)且 5 NM 内，看板要标红底 ──
         * 此前最近的目标是 5.0 NM，恰好卡在阈值外，红底一次都没触发过——
         * 「告警态从未被渲染」正是最该压的最糟情况。 */
        {  -3.0f,  2.4f, true,   -120, true,  175, "CSN9999",  7700 ,  1 },
        {  33.0f,  4.1f, true,    850, true,   88, NULL,       -1   ,  3 },

        /* ── 迎头 / 交叉：航迹指向本机附近，这类此前完全没出现过 ── */
        {  -6.0f, 11.0f, true,    200, true,  180, "CSN3825",  1234 ,  2 },  /* 正前迎头，同高度 */
        {   9.0f,  7.5f, true,   -150, true,  200, "CES2116W", 7700 ,  8 },  /* 迎头 + 8 字符满宽呼号 + 紧急码 */
        { -28.0f,  9.0f, true,   2600, true,   95, NULL,       -1   , 14 },  /* 无呼号无 SQK → 退回 ICAO hex */

        /* ── 方位扎堆：三个挤在 40°~52°，专门压标签避让 ── */
        {  40.0f,  6.0f, true,   -600, true,  355, "CHH7890",  2000 , 16 },
        {  46.0f,  8.5f, true,   3400, true,   20, "CQH8912",  4567 ,  5 },
        {  52.0f,  5.0f, true,  -4200, true,  140, "B-1234",   7600 , 22 },  /* 注册号当呼号 + 通信失效码 */

        /* ── 同向尾随 / 被超越 ── */
        { -55.0f, 10.0f, true,    900, true,   10, "UAL88",    3456 , 31 },
        {  70.0f,  9.5f, true,  -1800, true,   15, "DLH721",   -1   , 45 },

        /* ── 边界与降级 ── */
        {  84.0f,  7.0f, true,   9900, true,  270, "SIA12345", 7500 ,  9 },  /* 高差夹 +99 + 劫机码 */
        { -80.0f,  6.5f, false, 0,            true,   60, "CCA1501",  1000 , 58 },  /* 无高度 → 高度列必须显 --- */
        {  25.0f, 12.0f, true,   -300, false,   0, "GCR6543",  -1   , 12 },  /* 无航迹 → 速度/航向列显 --- */

        /* ── 后方：只计数，不画 ── */
        { 118.0f, 10.0f, true,    100, true,   30, "JAL999",   0000 , 28 },
        {-150.0f, 11.0f, true,   -400, true,  210, "AAL1",     6543 , 37 },  /* 最短呼号 */
        { 165.0f,  9.0f, true,   1500, true,  300, "KAL0012",  7777 ,  4 },
    };

    size_t n = sizeof(SET) / sizeof(SET[0]);
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; ++i) {
        place(&out[i], 0x3C0000 + (uint32_t)i, SET[i].rel, SET[i].dist,
              SET[i].have_alt, s_own_alt + SET[i].alt,
              SET[i].have_vel, SET[i].track, SET[i].call, SET[i].sqk,
              SET[i].age, now_us);
    }
    return n;
}
