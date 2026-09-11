/*
 * test_adsb_link_dispatch.c — execute the production modes_ingest sink in
 * adsb_link_task.c and verify post-decoder fan-out.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -DPK_HOST_TEST -I firmware/test/host_stubs -I firmware/main \
 *      -I firmware/components/adsb_link_codec \
 *      -o /tmp/test_adsb_link_dispatch \
 *      firmware/test/test_adsb_link_dispatch.c \
 *      firmware/main/mode_s.c firmware/main/modes_ingest.c \
 *      firmware/main/aircraft_state.c firmware/main/pk_callsign.c \
 *      firmware/main/adsb_link_task.c -lm \
 *   && /tmp/test_adsb_link_dispatch
 *
 * The ESP/FreeRTOS pieces are stubbed, but mode_s_decode(), the CRC gate,
 * aircraft_state_ingest(), and the production on_ingest_msg() dispatcher are
 * the real implementations.  This targets the boundary missed by decoder-only
 * tests: DF18 is decoded and fused, but the link-layer switch must not drop
 * its identity/position fan-out on the floor.
 */
#include "aircraft_state.h"
#include "adsb_link_task.h"
#include "cpr_decode.h"
#include "gps.h"
#include "mode_s.h"
#include "modes_ingest.h"
#include "pk_rec_ingest.h"
#include "record_sink.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int64_t esp_timer_get_time(void) { return 1000000; }

static bool g_gps_valid;
static pk_gps_state_t g_gps;
static uint32_t g_position_icao;
static bool g_airborne_decode_called;
static uint32_t g_airborne_icao;

bool pk_gps_get(pk_gps_state_t *out)
{
    if (!g_gps_valid) return false;
    *out = g_gps;
    return true;
}

bool pk_demo_enabled(void) { return false; }

/* Antenna config lives in NVS on the P4 and is pushed to the RP2040 over
 * CONFIG_REQ.  This test exercises the message dispatcher, not the config
 * store, so the payload builder is stubbed out to "nothing to send" — which
 * also keeps link_tx() off the UART path entirely. */
size_t pk_antenna_build_config_payload(uint8_t *out, size_t cap)
{
    (void)out; (void)cap;
    return 0;
}

void pk_rec_ingest_init(void) {}
void pk_rec_ingest_position(uint32_t icao24, int64_t ts_ms, double lat,
                            double lon, bool have_alt, int alt_ft,
                            bool have_gs, int gs_kt, bool have_track,
                            int track_deg, bool have_vs, int vs_fpm,
                            bool on_ground, bool from_surface_cpr)
{
    (void)icao24; (void)ts_ms; (void)lat; (void)lon; (void)have_alt;
    (void)alt_ft; (void)have_gs; (void)gs_kt; (void)have_track;
    (void)track_deg; (void)have_vs; (void)vs_fpm; (void)on_ground;
    if (from_surface_cpr) g_position_icao = icao24;
}

static uint32_t g_identity_calls;
static uint32_t g_identity_icao;
static char g_identity_callsign[AIRCRAFT_CALLSIGN_LEN];
static bool g_surface_decode_called;

bool pk_demo_traffic(void) { return false; }
float pk_demo_yaw_deg(int64_t now_us) { (void)now_us; return 0.0f; }
int pk_demo_own_alt_ft(int64_t now_us) { (void)now_us; return 0; }
uint32_t pk_ui_get_own_icao(void) { return 0; }

void adsb_link_dec_init(void) {}
void adsb_link_encode(void) {}
void adsb_link_uat_uplink_decode(void) {}
void cpr_init(void) {}
bool cpr_decode_position(uint32_t icao24, int fflag, bool is_surface,
                         int lat_cpr, int lon_cpr, int64_t now_us,
                         cpr_position_t *out_pos)
{
    (void)icao24; (void)fflag; (void)is_surface; (void)lat_cpr;
    (void)lon_cpr; (void)now_us;
    g_airborne_decode_called = true;
    g_airborne_icao = icao24;
    out_pos->valid = true;
    out_pos->lat = 37.0;
    out_pos->lon = -122.0;
    return true;
}
bool cpr_decode_surface_local(int fflag, int lat_cpr, int lon_cpr,
                               double ref_lat, double ref_lon,
                               cpr_position_t *out_pos)
{
    (void)fflag; (void)lat_cpr; (void)lon_cpr;
    (void)ref_lat; (void)ref_lon;
    g_surface_decode_called = true;
    out_pos->valid = true;
    out_pos->lat = 37.0;
    out_pos->lon = -122.0;
    return true;
}
void uat_ingest_init(void) {}
void uat_ingest_feed(void) {}
void uat_ingest_get_stats(void) {}
void record_dispatch(const record_t *rec) { (void)rec; }
bool record_sink_uart_stats(uint32_t *written, uint32_t *dropped,
                            uint32_t *pending)
{
    (void)written; (void)dropped; (void)pending;
    return false;
}

void pk_rec_ingest_identity(uint32_t icao24, int64_t ts_ms,
                            const char *callsign, uint8_t emitter_category)
{
    (void)ts_ms; (void)emitter_category;
    ++g_identity_calls;
    g_identity_icao = icao24;
    snprintf(g_identity_callsign, sizeof(g_identity_callsign), "%s", callsign);
}

bool pk_rec_ingest_stats(uint32_t *out_written, uint32_t *out_dropped)
{
    (void)out_written; (void)out_dropped;
    return false;
}

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); ++g_fail; \
    } } while (0)

static void set_crc(unsigned char msg[14])
{
    const uint32_t crc = mode_s_checksum(msg, 112);
    msg[11] = (unsigned char)(crc >> 16);
    msg[12] = (unsigned char)(crc >> 8);
    msg[13] = (unsigned char)crc;
}

static void build_df18_ident(unsigned char msg[14], uint32_t address)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(18 << 3);             /* CF0, IMF=0 */
    msg[1] = (unsigned char)(address >> 16);
    msg[2] = (unsigned char)(address >> 8);
    msg[3] = (unsigned char)address;
    msg[4] = 4 << 3;                               /* TC4, category B2 */
    /* Callsign charset: A=1..Z=26, blank=32. Build "DS18    ". */
    static const unsigned char chars[8] = {4, 19, 24, 32, 32, 32, 32, 32};
    msg[5] = (unsigned char)((chars[0] << 2) | (chars[1] >> 4));
    msg[6] = (unsigned char)((chars[1] << 4) | (chars[2] >> 2));
    msg[7] = (unsigned char)((chars[2] << 6) | chars[3]);
    msg[8] = (unsigned char)((chars[3] << 2) | (chars[4] >> 4));
    msg[9] = (unsigned char)((chars[4] << 4) | (chars[5] >> 2));
    msg[10] = (unsigned char)((chars[5] << 6) | chars[6]);
    set_crc(msg);
}

static void build_df18_surface(unsigned char msg[14], int cf)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)((18 << 3) | cf);
    msg[1] = 0xB0;
    msg[2] = 0x00;
    msg[3] = 0x03;
    msg[4] = 6 << 3;           /* TC6; ME position fields remain zero. */
    set_crc(msg);
}

static void from_hex(const char *hex, unsigned char msg[14])
{
    for (int i = 0; i < 14; ++i) {
        unsigned int value = 0;
        (void)sscanf(hex + i * 2, "%2x", &value);
        msg[i] = (unsigned char)value;
    }
}

static bool get_aircraft(uint32_t icao, aircraft_t *out)
{
    aircraft_t all[AIRCRAFT_TABLE_CAPACITY];
    const size_t n = aircraft_state_snapshot(all, AIRCRAFT_TABLE_CAPACITY,
                                             esp_timer_get_time() + 1000,
                                             1000000);
    for (size_t i = 0; i < n; ++i) {
        if (all[i].icao24 == icao) { *out = all[i]; return true; }
    }
    return false;
}

static void test_df18_identity_reaches_traffic_record(void)
{
    unsigned char msg[14];
    aircraft_t a;
    g_identity_calls = 0;
    g_identity_icao = 0;
    memset(g_identity_callsign, 0, sizeof(g_identity_callsign));
    aircraft_state_init();

    build_df18_ident(msg, 0xA00001);
    modes_ingest_feed(msg, 112, NULL);

    CHECK(get_aircraft(0xA00001, &a), "DF18 aircraft missing from fusion table\n");
    CHECK(a.have_callsign, "DF18 callsign missing from fusion table\n");
    CHECK(g_identity_calls == 1,
          "DF18 identity frame must write exactly one traffic.trk record\n");
    CHECK(g_identity_icao == 0xA00001, "DF18 identity record ICAO");
    CHECK(strcmp(g_identity_callsign, "DSX") == 0,
          "DF18 identity record callsign got='%s'\n", g_identity_callsign);
}

static void test_nonicao_df18_surface_does_not_create_target(void)
{
    unsigned char msg[14];
    aircraft_t a;
    aircraft_state_init();
    g_gps_valid = true;
    g_gps.have_fix = true;
    g_gps.lat = 37.0;
    g_gps.lon = -122.0;
    g_surface_decode_called = false;
    g_position_icao = 0;

    build_df18_surface(msg, 3);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(!g_surface_decode_called,
          "non-ICAO DF18 must not reach the standard surface CPR decoder\n");
    CHECK(g_position_icao == 0,
          "non-ICAO DF18 must not write a traffic record\n");
    CHECK(!get_aircraft(0xB00003, &a),
          "non-ICAO DF18 CF3 must not create a fusion target\n");

    build_df18_surface(msg, 1);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(!get_aircraft(0xB00001, &a),
          "non-ICAO DF18 CF1 must not create a fusion target\n");

    g_gps_valid = false;
}

static void test_tc20_position_reaches_airborne_cpr(void)
{
    unsigned char msg[14];
    aircraft_t a;
    aircraft_state_init();
    g_airborne_decode_called = false;
    g_airborne_icao = 0;

    /* Captured TC20 vector from test_mode_s_extended.c. DF17 here isolates
     * metype routing from DF18 address semantics. */
    from_hex("8E7C6296A0A0A59C64468D6F4EDD", msg);
    modes_ingest_feed(msg, 112, NULL);

    CHECK(g_airborne_decode_called,
          "TC20 GNSS position frame must reach airborne CPR decoder\n");
    CHECK(g_airborne_icao == 0x7C6296, "TC20 airborne CPR ICAO");
    CHECK(get_aircraft(0x7C6296, &a), "TC20 aircraft missing from fusion table\n");
    CHECK(a.have_position, "TC20 decoded position missing from fusion table\n");
}

int main(void)
{
    /* adsb_link_task.c registers this production sink with modes_ingest_init()
     * in its task loop.  The host cannot run that loop, so expose the same
     * registration directly through the production initialization function. */
    modes_ingest_init(on_ingest_msg, NULL);
    test_df18_identity_reaches_traffic_record();
    test_nonicao_df18_surface_does_not_create_target();
    test_tc20_position_reaches_airborne_cpr();

    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("OK: all ADS-B link dispatch checks passed\n");
    return 0;
}
