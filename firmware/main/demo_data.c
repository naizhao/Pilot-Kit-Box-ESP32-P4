/*
 * demo_data.c — 见 demo_data.h。
 *
 * 这份数据的前身是 sim/compat/mock_runtime.c 里的桩。搬过来时**一个数值都没
 * 改**，只把「本机状态从哪来」从模拟器的动画变量换成了 now_us 的函数，好让
 * 固件（演示模式）与模拟器（布局回归）演的是同一架飞机、同一片空域。
 */

#include "demo_data.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "demo_track.h"

/* 轨迹表取不出来时的兜底位置：ZGGG 停机坪，也就是轨迹的起点。
 * 只有在 demo_track_data.c 没生成（表为空）时才会走到，等于"演示模式退化成
 * 老的静止行为"——宁可不动，也不要让各页拿到未初始化的经纬度。 */
#define DEMO_FALLBACK_LAT   23.3853
#define DEMO_FALLBACK_LON  113.2924

/* now_us → 本机状态。纯查表 + 插值，没有任何缓存（见 demo_track.h 的无状态
 * 约束）。578 条记录的二分查找是 10 次比较，比缓存带来的不可复现划算。 */
static void own_state(int64_t now_us, pk_demo_track_state_t *st)
{
    if (pk_demo_track_at(now_us, st)) return;
    memset(st, 0, sizeof(*st));
    st->lat = DEMO_FALLBACK_LAT;
    st->lon = DEMO_FALLBACK_LON;
}

/* now_us → 秒。用 double 中转再落到 float：直接 (float)now_us 在开机 4 小时后
 * 就丢掉毫秒位（float 只有 24 位尾数），动画会开始一顿一顿地跳。 */
static float demo_t(int64_t now_us)
{
    return (float)((double)now_us / 1000000.0);
}

float pk_demo_roll_deg(int64_t now_us)
{
    pk_demo_track_state_t st; own_state(now_us, &st); return st.roll_deg;
}

float pk_demo_pitch_deg(int64_t now_us)
{
    pk_demo_track_state_t st; own_state(now_us, &st); return st.pitch_deg;
}

float pk_demo_yaw_deg(int64_t now_us)
{
    pk_demo_track_state_t st; own_state(now_us, &st); return st.track_deg;
}

int pk_demo_own_alt_ft(int64_t now_us)
{
    pk_demo_track_state_t st; own_state(now_us, &st); return (int)st.alt_ft;
}

int pk_demo_own_gs_kt(int64_t now_us)
{
    pk_demo_track_state_t st; own_state(now_us, &st); return (int)st.gs_kt;
}

int pk_demo_own_vs_fpm(int64_t now_us)
{
    pk_demo_track_state_t st; own_state(now_us, &st); return st.vs_fpm;
}

void pk_demo_own_pos(int64_t t_us, double *lat, double *lon)
{
    pk_demo_track_state_t st;
    own_state(t_us, &st);
    if (lat) *lat = st.lat;
    if (lon) *lon = st.lon;
}

/* ── IMU ──────────────────────────────────────────────────────────── */
bool pk_demo_imu_sample(int64_t now_us, pk_imu_sample_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->ts_us     = now_us;
    out->roll_deg  = pk_demo_roll_deg(now_us);
    out->pitch_deg = pk_demo_pitch_deg(now_us);
    out->yaw_deg   = pk_demo_yaw_deg(now_us);
    /* accuracy=3（已收敛）而不是 0：演示模式要展示的是**正常态**的版面，
     * acc<2 会让调平向导和诊断页都进"等磁力计收敛"的分支。 */
    out->accuracy  = 3;
    out->valid     = true;
    return true;
}

/* ── 气压 ─────────────────────────────────────────────────────────── */
/* 高度（m）→ ISA 标准大气压（Pa）。对流层 + 平流层下段两段式，
 * 覆盖 0–20 km，够本轨迹的 0–12.7 km。 */
static float isa_pressure_pa(float alt_m)
{
    if (alt_m < 11000.0f)
        return 101325.0f * powf(1.0f - 2.25577e-5f * alt_m, 5.25588f);
    return 22632.1f * expf(-(alt_m - 11000.0f) / 6341.62f);
}

bool pk_demo_baro(int64_t now_us, pk_baro_state_t *out)
{
    if (!out) return false;
    pk_demo_track_state_t st;
    own_state(now_us, &st);

    memset(out, 0, sizeof(*out));
    out->valid = true;
    /* 与本机权威高度差 120 ft。真实飞行里气压高度与 ADS-B/GPS 高度本来就对不
     * 齐（QNH 设定、几何高 vs 气压高），两个数完全相等反而是假的。 */
    out->alt_ft = (int)st.alt_ft - 120;
    /* 气压随高度走，不能再钉死在 40 000 Pa：本机现在从停机坪一路爬到 FL410，
     * 一个恒定的舱压读数与旁边那个正在滚动的高度带自相矛盾，而诊断页把两个
     * 数并排显示。 */
    out->pressure_pa = isa_pressure_pa((float)out->alt_ft * 0.3048f);
    /* 外温同样跟着高度走：对流层每千米 −6.5 °C，11 km 以上恒定 −56.5 °C。 */
    {
        const float h_km = (float)out->alt_ft * 0.0003048f;
        out->temp_c = (h_km < 11.0f) ? 15.0f - 6.5f * h_km : -56.5f;
    }
    /* 升降率现在就是轨迹高度的真实导数（±4000 fpm 以内，见 demo_track）。
     * 早先那条曲线的导数高达 ±7900 fpm，只好另给一条独立的假曲线；真实轨迹
     * 没有这个问题，VS 与高度带的滚动方向从此永远一致。 */
    out->vs_fpm     = st.vs_fpm;
    out->updated_us = now_us;
    return true;
}

/* ── GPS ──────────────────────────────────────────────────────────── */
bool pk_demo_gps(int64_t now_us, pk_gps_state_t *out)
{
    if (!out) return false;
    pk_demo_track_state_t st;
    own_state(now_us, &st);

    memset(out, 0, sizeof(*out));
    out->have_fix        = true;
    out->lat             = st.lat;
    out->lon             = st.lon;
    out->have_altitude   = true;
    out->altitude_ft     = (int)st.alt_ft;
    out->ground_speed_kt = (int)st.gs_kt;
    out->track_deg       = (int)st.track_deg;
    out->sats            = 11;
    out->updated_us      = now_us;

    out->sats_in_view     = 17;
    out->sats_in_view_gps = 10;
    out->sats_in_view_bds = 7;
    out->hdop             = 0.9f;
    out->ant_status       = PK_GPS_ANT_OK;
    /* 诊断页的 SNR 柱状图按星座分行。只喂"有几颗星"而不喂逐星 SNR，那张图就
     * 永远停在 "(no satellites in view)"——演示模式的用处正是把这类只有在真
     * 装了天线、且在户外才看得到的画面搬到桌面上。 */
    {
        static const uint8_t snr[] = { 44, 38, 31, 22, 41, 36, 28, 19, 33, 25 };
        static const uint8_t con[] = { PK_GNSS_GPS, PK_GNSS_GPS, PK_GNSS_GPS,
                                       PK_GNSS_GPS, PK_GNSS_BDS, PK_GNSS_BDS,
                                       PK_GNSS_BDS, PK_GNSS_BDS, PK_GNSS_GLO,
                                       PK_GNSS_GAL };
        memcpy(out->snr, snr, sizeof(snr));
        memcpy(out->snr_con, con, sizeof(con));
        out->snr_count = (int)sizeof(snr);
        out->snr_max   = 44;
    }
    /* "模块在不在"的唯一依据。0 会让诊断页显示"没插 GPS 板卡"，那与本机有
     * 定位是自相矛盾的两句话。 */
    out->last_nmea_us = now_us;
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
                  const char *callsign, int squawk, int age_s,
                  int64_t now_us, int64_t pos_us, float own_yaw_deg)
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

    /* 目标的绝对经纬度 = **此刻的**本机位置 + 相对方位 + 距离。
     * 本机现在沿真实轨迹飞，这里必须用同一个动画时钟取位置，否则飞机一起飞
     * 就把整片目标留在了广州上空——这正是"假目标跟着本机走"的实现方式，
     * 一行专门的跟随代码都不需要。 */
    double own_lat, own_lon;
    pk_demo_own_pos(pos_us, &own_lat, &own_lon);
    float brg_true = own_yaw_deg + rel_deg;
    float rad      = brg_true * (float)M_PI / 180.0f;
    double dlat    = (double)(dist_nm / 60.0f) * cos(rad);
    double dlon    = (double)(dist_nm / 60.0f) * sin(rad)
                   / cos(own_lat * M_PI / 180.0);
    a->lat = own_lat + dlat;
    a->lon = own_lon + dlon;

    a->have_altitude = have_alt;
    a->altitude_ft   = alt_ft;
    a->have_velocity = have_vel;
    a->heading_deg   = track_deg;      /* 与方位角无关，各飞各的 */
    /* 地速也要喂：右栏列表有这一列，不喂就显示成一个看似真实的 0——比缺数据
     * 更糟，因为 0 kt 是个合法读数（悬停/地面）。按 icao 散开，覆盖巡航到
     * 进近的范围。 */
    a->ground_speed_kt = have_vel ? (120 + (int)(icao % 7) * 55) : 0;
    /* 升降率跟着高度差走：高的在爬、低的在降，好让列表里的 ^v 有东西可显。 */
    a->vert_rate_fpm = have_alt ? ((alt_ft % 3 == 0) ? 800
                                : (alt_ft % 3 == 1) ? -650 : 0) : 0;
}

size_t pk_demo_traffic(aircraft_t *out, size_t cap,
                       int64_t now_us, int64_t anim_us,
                       float own_yaw_deg, int own_alt_ft,
                       float extra_dist_nm, bool bare)
{
    if (!out || cap == 0) return 0;

    /*
     * 高度一栏是**相对本机**的差值，不是绝对高度：本机高度随时间起落，写死
     * 绝对值会让相对高度整体漂移，三档配色也就试不准。
     *
     * drift / d_amp / d_rate 是「让画面活起来」的两个旋钮，默认 0：
     *   drift  °/s，相对方位的漂移速率。真实空域里两机的相对方位一直在变，
     *          全部钉死的话，机头朝上模式下整幅雷达是**静止**的——标签避让、
     *          进出后方计数这些只在目标移动时才发生的路径一次都压不到。
     *   d_amp / d_rate  距离上叠加的正弦，用来做「进出量程」。
     *
     * 前五行（同高度告警 + 迎头交叉）与方位扎堆那三行**不给漂移**：它们的存在
     * 就是为了把某一类版面钉在一个可复现的位置上（红底告警、标签重叠），一漂
     * 就散了，回归截图也就失去基线的意义。
     */
    static const struct {
        float rel, dist; bool have_alt; int alt; bool have_vel; int track;
        const char *call; int sqk; int age;
        float drift, d_amp, d_rate;
    } SET[] = {
        /* ── 威胁：同高度(±1000 ft)且 5 NM 内，看板要标红底 ──
         * 此前最近的目标是 5.0 NM，恰好卡在阈值外，红底一次都没触发过——
         * 「告警态从未被渲染」正是最该压的最糟情况。 */
        {  -3.0f,  2.4f, true,   -120, true,  175, "CSN9999",  7700 ,  1,  0.0f, 0.0f, 0.0f },
        {  33.0f,  4.1f, true,    850, true,   88, NULL,       -1   ,  3,  0.0f, 0.0f, 0.0f },

        /* ── 迎头 / 交叉：航迹指向本机附近，这类此前完全没出现过 ── */
        {  -6.0f, 11.0f, true,    200, true,  180, "CSN3825",  1234 ,  2,  0.0f, 0.0f, 0.0f },  /* 正前迎头，同高度 */
        {   9.0f,  7.5f, true,   -150, true,  200, "CES2116W", 7700 ,  8,  0.0f, 0.0f, 0.0f },  /* 迎头 + 8 字符满宽呼号 + 紧急码 */
        { -28.0f,  9.0f, true,   2600, true,   95, NULL,       -1   , 14,  0.0f, 0.0f, 0.0f },  /* 无呼号无 SQK → 退回 ICAO hex */

        /* ── 方位扎堆：三个挤在 40°~52°，专门压标签避让 ── */
        {  40.0f,  6.0f, true,   -600, true,  355, "CHH7890",  2000 , 16,  0.0f, 0.0f, 0.0f },
        {  46.0f,  8.5f, true,   3400, true,   20, "CQH8912",  4567 ,  5,  0.0f, 0.0f, 0.0f },
        {  52.0f,  5.0f, true,  -4200, true,  140, "B-1234",   7600 , 22,  0.0f, 0.0f, 0.0f },  /* 注册号当呼号 + 通信失效码 */

        /* ── 同向尾随 / 被超越 ── */
        { -55.0f, 10.0f, true,    900, true,   10, "UAL88",    3456 , 31, -0.6f, 0.0f, 0.0f },
        {  70.0f,  9.5f, true,  -1800, true,   15, "DLH721",   -1   , 45,  0.9f, 0.0f, 0.0f },

        /* ── 边界与降级 ── */
        {  84.0f,  7.0f, true,   9900, true,  270, "SIA12345", 7500 ,  9,  0.4f, 0.0f, 0.0f },  /* 高差夹 +99 + 劫机码 */
        { -80.0f,  6.5f, false, 0,            true,   60, "CCA1501",  1000 , 58, -0.5f, 0.0f, 0.0f },  /* 无高度 → 高度列必须显 --- */
        {  25.0f, 12.0f, true,   -300, false,   0, "GCR6543",  -1   , 12,  0.7f, 0.0f, 0.0f },  /* 无航迹 → 速度/航向列显 --- */

        /* ── 后方：只计数，不画 ── */
        { 118.0f, 10.0f, true,    100, true,   30, "JAL999",   0000 , 28, -1.1f, 0.0f, 0.0f },
        {-150.0f, 11.0f, true,   -400, true,  210, "AAL1",     6543 , 37,  1.2f, 0.0f, 0.0f },  /* 最短呼号 */
        { 165.0f,  9.0f, true,   1500, true,  300, "KAL0012",  7777 ,  4,  0.8f, 0.0f, 0.0f },

        /* ── 进出量程：距离在 8~26 NM 之间往返，周期约 52 s ──
         * 最大量程档是 20 NM，所以这一架会周期性地从雷达上消失又出现，而顶栏
         * 的目标计数**不跟着变**（计数按 60 s 新鲜度算，与量程无关）。这两个数
         * 对不上时用户会以为渲染坏了，是必须演一遍的一种正常现象。 */
        {  15.0f, 17.0f, true,    450, true,  240, "CPA901",   2456 ,  6,  0.0f, 9.0f, 0.12f },
    };

    size_t n = sizeof(SET) / sizeof(SET[0]);
    if (n > cap) n = cap;
    const float t = demo_t(anim_us);
    for (size_t i = 0; i < n; ++i) {
        float rel  = SET[i].rel + SET[i].drift * t;
        float dist = SET[i].dist + extra_dist_nm;
        if (SET[i].d_amp != 0.0f)
            dist += SET[i].d_amp * sinf(t * SET[i].d_rate);
        if (dist < 0.5f) dist = 0.5f;
        place(&out[i], 0x3C0000 + (uint32_t)i, rel, dist,
              bare ? false : SET[i].have_alt, own_alt_ft + SET[i].alt,
              bare ? false : SET[i].have_vel, SET[i].track,
              bare ? NULL : SET[i].call, bare ? -1 : SET[i].sqk,
              SET[i].age, now_us, anim_us, own_yaw_deg);
    }
    return n;
}
