/*
 * test_own_ship_gps_validity.c — GPS-only ownship 的空地/速度/航迹不得伪造
 * （Phase 1 独立复审 P1/B）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_own_ship_gps_validity \
 *      firmware/test/test_own_ship_gps_validity.c \
 *      firmware/main/own_ship.c firmware/main/gps_task.c \
 *      firmware/main/gps_nmea.c firmware/main/pk_clock.c \
 *      firmware/main/gdl90.c -lm \
 *   && /tmp/test_own_ship_gps_validity
 *
 * 缺陷（修复前）
 * --------------
 *   1. gps_task.c 的 parse_rmc 用 atof 解 RMC 的经纬度/速度/航迹：空字段和
 *      "12.3abc" 这类垃圾都变成一个**看起来合法的数**，而 atof 没有任何方式
 *      把"解析失败"与"值恰好是 0"分开。status='A' 但坐标字段是空的 RMC
 *      （模块半死时真实存在）会把本机定位到几内亚湾 0°,0°；
 *   2. own_ship.c 的 GPS 兜底无条件 have_ground_speed = have_heading = true，
 *      于是那个 0 变成"地速 0 kt、航迹正北"发上 GDL90 线：EFB 上本机是一个
 *      笃定停在原地、机头朝北的飞机；
 *   3. GPS-only 时 have_air_ground 恒为 false，而 GDL90 的 Misc bit3
 *      "Airborne" 是正向断言、协议没有"未知"编码——未知在线上表现为
 *      **surface**，多数 EFB 会据此抑制交通告警。
 *
 * 判据（跑生产链路：NMEA 行 → gps_task 解析 → own_ship 解算 → 线字节）
 * -------------------------------------------------------------------
 *   - 空 speed/track → GDL90 水平速度 0xFFF、tt=0，位置照常有效；
 *   - 非法纬度/半球/尾随垃圾/nan → 不得当成定位或速度；
 *   - A/G 取 pk_own_sampler_get_phase() 的明确结论：AIRBORNE → airborne 位
 *     为 1；地面族 → 0；UNKNOWN → **整帧不发**（不伪造 surface），且这个
 *     决定是一个可测的生产函数 pk_own_gdl90_should_emit()，不是 BLE 大任务
 *     里的一个 if。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aircraft_state.h"
#include "gdl90.h"
#include "gps.h"
#include "own_ship.h"
#include "pk_flight_phase.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }

bool pk_demo_enabled(void) { return false; }
bool pk_demo_gps(int64_t now_us, pk_gps_state_t *out)
{ (void)now_us; (void)out; return false; }

/* 本测试全程不绑定 ADS-B 本机（GPS-only 才是被测场景）。 */
uint32_t pk_ui_get_own_icao(void) { return 0; }
bool aircraft_state_get_own(uint32_t icao24, int64_t now_us,
                            int64_t max_age_us, aircraft_t *out)
{ (void)icao24; (void)now_us; (void)max_age_us; (void)out; return false; }

static pk_flight_phase_t g_phase = PK_PHASE_UNKNOWN;
pk_flight_phase_t pk_own_sampler_get_phase(void) { return g_phase; }

/* ── NMEA 喂线 ────────────────────────────────────────────────────── */
static void feed(const char *body)
{
    char line[128];
    unsigned char ck = 0;
    for (const char *p = body; *p; ++p) ck ^= (unsigned char)*p;
    snprintf(line, sizeof(line), "$%s*%02X", body, ck);
    pk_gps_feed_line(line);
}

static void reset_world(int64_t t0, pk_flight_phase_t phase)
{
    g_now_us = t0;
    g_phase  = phase;
    pk_gps_state_init();
}

/* ── GDL90 线字节解码（字段位置见 gdl90.c 的 27 字节 payload 布局） ── */
typedef struct {
    bool     ok;
    uint8_t  id;
    uint16_t alt_enc;
    uint8_t  misc;
    uint16_t horiz_vel;
    uint8_t  track_byte;
} report_t;

static report_t decode_report(const uint8_t *frame, size_t n)
{
    report_t r;
    memset(&r, 0, sizeof(r));
    if (n < 4 || frame[0] != 0x7E || frame[n - 1] != 0x7E) return r;
    uint8_t body[64];
    size_t  bn = 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        uint8_t b = frame[i];
        if (b == 0x7D) { ++i; b = (uint8_t)(frame[i] ^ 0x20); }
        if (bn < sizeof(body)) body[bn++] = b;
    }
    if (bn < 1 + 27 + 2) return r;
    const uint8_t *p = body + 1;
    r.ok         = true;
    r.id         = body[0];
    r.alt_enc    = (uint16_t)(((uint16_t)p[10] << 4) | (p[11] >> 4));
    r.misc       = (uint8_t)(p[11] & 0x0F);
    r.horiz_vel  = (uint16_t)(((uint16_t)p[13] << 4) | (p[14] >> 4));
    r.track_byte = p[16];
    return r;
}

static report_t encode_ownship(const aircraft_t *own)
{
    uint8_t frame[64];
    size_t n = gdl90_encode_traffic(frame, sizeof(frame),
        true, own->icao24,
        own->have_position, own->lat, own->lon,
        own->have_altitude, own->altitude_ft,
        own->have_ground_speed, own->ground_speed_kt,
        own->have_heading, own->heading_deg,
        own->have_vertical_rate, own->vert_rate_fpm,
        own->have_air_ground, own->on_ground,
        gdl90_emitter_from_wake((int)own->wake),
        "", sizeof(""));
    return decode_report(frame, n);
}

/* ════════════════════════════════════════════════════════════════════
 * 1. 空 speed/track：位置照常有效，速度/航迹必须是"未知"
 * ════════════════════════════════════════════════════════════════════ */
static void test_empty_speed_track_is_unknown_not_zero(void)
{
    reset_world(100000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,,,230326,,");

    pk_gps_state_t g;
    CHECK(pk_gps_get(&g), "速度字段缺失不得丢掉合法位置\n");
    CHECK(g.lat > 30.99 && g.lat < 31.01, "纬度落地错误 got=%f\n", g.lat);
    CHECK(!g.have_ground_speed, "空 speed 字段不得声明地速有效\n");
    CHECK(!g.have_track, "空 track 字段不得声明航迹有效\n");

    aircraft_t own; pk_own_src_t src;
    CHECK(pk_own_ship_resolve(g_now_us, 5000000, &own, &src), "应解算出本机\n");
    CHECK(own.have_position, "位置应有效\n");
    CHECK(!own.have_ground_speed,
          "ownship 不得声明地速有效（值=%d）\n", own.ground_speed_kt);
    CHECK(!own.have_heading,
          "ownship 不得声明航向有效（值=%d）\n", own.heading_deg);

    report_t r = encode_ownship(&own);
    CHECK(r.ok, "0x0A 解码失败\n");
    CHECK(r.horiz_vel == 0xFFF,
          "空 speed → 线上水平速度必须是 0xFFF got=0x%03X（%d kt 是编出来的）\n",
          r.horiz_vel, r.horiz_vel);
    CHECK((r.misc & 0x3) == 0,
          "空 track → tt 位必须是 0（否则 byte16 的 0 被读成航迹正北）got=%d\n",
          r.misc & 0x3);
}

/* 2. 正常 speed/track 必须照常上线（别把功能修没）。 */
static void test_valid_speed_track_still_encoded(void)
{
    reset_world(100000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,120.0,90.0,230326,,");

    pk_gps_state_t g;
    CHECK(pk_gps_get(&g), "应有 fix\n");
    CHECK(g.have_ground_speed && g.ground_speed_kt == 120,
          "地速应为 120 got(valid=%d, %d)\n", (int)g.have_ground_speed, g.ground_speed_kt);
    CHECK(g.have_track && g.track_deg == 90,
          "航迹应为 90 got(valid=%d, %d)\n", (int)g.have_track, g.track_deg);

    aircraft_t own; pk_own_src_t src;
    CHECK(pk_own_ship_resolve(g_now_us, 5000000, &own, &src), "应解算出本机\n");
    report_t r = encode_ownship(&own);
    CHECK(r.horiz_vel == 120, "线上水平速度应为 120 got=%d\n", r.horiz_vel);
    CHECK((r.misc & 0x3) == 1, "有航迹时 tt 应为 1 got=%d\n", r.misc & 0x3);
    /* 90° → round(90*256/360) = 64 */
    CHECK(r.track_byte == 64, "航迹字节应为 64 got=%d\n", r.track_byte);
}

/* 3. 严格解析：非法字段一律不得被采信。 */
static void test_malformed_rmc_fields_rejected(void)
{
    pk_gps_state_t g;
    aircraft_t own; pk_own_src_t src;

    /* 3a. 纬度 91°（分位换算后超范围） */
    reset_world(200000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,9100.0000,N,12100.0000,E,5.0,90.0,230326,,");
    CHECK(!pk_gps_get(&g), "纬度 91° 非法，不得当成定位\n");

    /* 3b. 分位 ≥60（3160.0000 = 31°60′） */
    reset_world(210000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,3160.0000,N,12100.0000,E,5.0,90.0,230326,,");
    CHECK(!pk_gps_get(&g), "分位 60′ 非法，不得当成定位\n");

    /* 3c. 半球字符非法 */
    reset_world(220000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,3100.0000,X,12100.0000,E,5.0,90.0,230326,,");
    CHECK(!pk_gps_get(&g), "半球 'X' 非法，不得当成定位\n");

    /* 3d. 经纬度为空（模块半死时的真实形态：status 仍报 A） */
    reset_world(230000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,,,,,5.0,90.0,230326,,");
    CHECK(!pk_gps_get(&g), "空经纬度不得被 atof 变成 0°,0°\n");

    /* 3e. 速度带尾随垃圾 */
    reset_world(240000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,12.3abc,90.0,230326,,");
    CHECK(pk_gps_get(&g), "速度字段坏掉不该连累位置\n");
    CHECK(!g.have_ground_speed, "尾随垃圾的速度不得被采信 got=%d\n", g.ground_speed_kt);
    CHECK(g.have_track && g.track_deg == 90, "同句的合法航迹应照常可用\n");
    CHECK(pk_own_ship_resolve(g_now_us, 5000000, &own, &src), "位置仍有效应解算出本机\n");
    CHECK(!own.have_ground_speed, "ownship 不得采信坏掉的地速\n");

    /* 3f. nan / inf */
    reset_world(250000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,nan,inf,230326,,");
    CHECK(pk_gps_get(&g), "位置合法应保留\n");
    CHECK(!g.have_ground_speed, "nan 速度不得被采信\n");
    CHECK(!g.have_track, "inf 航迹不得被采信\n");

    /* 3g. 航迹超范围（361°） */
    reset_world(260000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,5.0,361.0,230326,,");
    CHECK(pk_gps_get(&g), "位置合法应保留\n");
    CHECK(!g.have_track, "361° 航迹越界，不得被采信\n");

    /* 3h. status 不是恰好 "A" */
    reset_world(270000000, PK_PHASE_AIRBORNE);
    feed("GPRMC,123519,AV,3100.0000,N,12100.0000,E,5.0,90.0,230326,,");
    CHECK(!pk_gps_get(&g), "status='AV' 不是有效定位\n");
}

/* ════════════════════════════════════════════════════════════════════
 * 4. A/G：用相位状态机的明确结论，UNKNOWN 不得伪造 surface
 * ════════════════════════════════════════════════════════════════════ */
static void resolve_gps_only(pk_flight_phase_t phase, aircraft_t *own)
{
    reset_world(300000000, phase);
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,120.0,90.0,230326,,");
    pk_own_src_t src;
    CHECK(pk_own_ship_resolve(g_now_us, 5000000, own, &src), "应解算出本机\n");
    CHECK(src == PK_OWN_SRC_GPS, "来源应为 GPS\n");
}

static void test_air_ground_airborne(void)
{
    aircraft_t own;
    resolve_gps_only(PK_PHASE_AIRBORNE, &own);
    CHECK(own.have_air_ground && !own.on_ground,
          "相位 AIRBORNE 时应断言在空中 got(have=%d, on_ground=%d)\n",
          (int)own.have_air_ground, (int)own.on_ground);
    CHECK(pk_own_gdl90_should_emit(true, &own), "空地已知时 0x0A 应当发送\n");

    report_t r = encode_ownship(&own);
    CHECK((r.misc & 0x8) != 0,
          "线上 airborne 位应为 1 got misc=0x%X —— EFB 会把 0 读成本机在地面"
          "并抑制交通告警\n", r.misc);
}

static void test_air_ground_ground_family(void)
{
    const pk_flight_phase_t ground[] = {
        PK_PHASE_GROUND_STOPPED, PK_PHASE_TAXI,
        PK_PHASE_TAKEOFF_ROLL,   PK_PHASE_LANDING_ROLLOUT,
    };
    for (size_t i = 0; i < sizeof(ground) / sizeof(ground[0]); ++i) {
        aircraft_t own;
        resolve_gps_only(ground[i], &own);
        CHECK(own.have_air_ground && own.on_ground,
              "地面族相位 %d 应断言在地面 got(have=%d, on_ground=%d)\n",
              (int)ground[i], (int)own.have_air_ground, (int)own.on_ground);
        CHECK(pk_own_gdl90_should_emit(true, &own), "空地已知时应当发送\n");
        report_t r = encode_ownship(&own);
        CHECK((r.misc & 0x8) == 0, "地面时 airborne 位应为 0 got misc=0x%X\n", r.misc);
    }
}

static void test_air_ground_unknown_is_not_emitted_as_surface(void)
{
    aircraft_t own;
    resolve_gps_only(PK_PHASE_UNKNOWN, &own);
    CHECK(!own.have_air_ground,
          "相位 UNKNOWN 时不得断言空地状态 got(on_ground=%d)\n", (int)own.on_ground);
    CHECK(!pk_own_gdl90_should_emit(true, &own),
          "空地未知时 0x0A 不得发送——线上没有\"未知\"编码，发了就是宣称在地面\n");

    /* 反面守卫：没有本机时同样不发。 */
    CHECK(!pk_own_gdl90_should_emit(false, &own), "无本机时不得发送\n");
    CHECK(!pk_own_gdl90_should_emit(true, NULL), "空指针时不得发送\n");
}

int main(void)
{
    test_empty_speed_track_is_unknown_not_zero();
    test_valid_speed_track_still_encoded();
    test_malformed_rmc_fields_rejected();
    test_air_ground_airborne();
    test_air_ground_ground_family();
    test_air_ground_unknown_is_not_emitted_as_surface();

    if (g_fail) {
        printf("test_own_ship_gps_validity: %d assertion(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_own_ship_gps_validity: all passed\n");
    return 0;
}
