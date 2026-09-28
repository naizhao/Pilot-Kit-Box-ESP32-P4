/*
 * test_uat_downlink_ingest.c — 978 MHz downlink RF frame front door.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/main \
 *      -o /tmp/test_uat_downlink_ingest \
 *      firmware/test/test_uat_downlink_ingest.c \
 *      firmware/main/uat_ingest.c firmware/main/uat_decode.c -lm \
 *   && /tmp/test_uat_downlink_ingest
 *
 * The vectors are DO-282B Table 2-104 conformance frames already used by
 * test_uat_decode.c. This test covers the production boundary which receives
 * the exact 30/48-byte RF packet, applies FEC, parses an aircraft message and
 * preserves receiver metadata.
 */
#include <stdio.h>
#include <string.h>

#include "uat_ingest.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; \
} } while (0)

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
        unsigned lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static const char VEC_DL_SHORT_IN[] =
    "FF8196782DD44238C1453855F89980C7524F5970940ED83AD89CE7A9BEF8761B"
    "BCD9FCC817D82E2D1ACF90CA78DA3C49";
static const char VEC_DL_LONG_IN[] =
    "5A8CAA4ABC7AEE2AD0929EB80DA044D556B452A7A73A5716CD1DB40964F5BA105D9D"
    "AAD75342196DEBB63CC972994DEA";

static int g_sink_calls;
static uat_aircraft_msg_t g_msg;
static uat_downlink_meta_t g_meta;
static uint8_t g_rs;

static void sink(const uat_aircraft_msg_t *msg,
                 const uat_downlink_meta_t *meta,
                 uint8_t rs_corrected, void *user)
{
    (void)user;
    g_sink_calls++;
    g_msg = *msg;
    g_meta = *meta;
    g_rs = rs_corrected;
}

static void test_short_aircraft_reaches_sink(void)
{
    uint8_t frame[48];
    CHECK(unhex(VEC_DL_SHORT_IN, frame, sizeof frame) == 48);
    uat_downlink_ingest_init(sink, NULL);
    const uat_downlink_meta_t meta = {
        .rssi_dbm = -73, .frame_seq = 0x1234, .cc_time = 0x89ABCDEF,
    };
    uat_downlink_ingest_feed(frame, 30, &meta);

    CHECK(g_sink_calls == 1);
    CHECK(g_msg.address == 0x7E6987);
    CHECK(g_msg.address_is_icao);
    CHECK(g_msg.has_state_vector && g_msg.position_valid);
    CHECK(g_meta.rssi_dbm == -73 && g_meta.frame_seq == 0x1234);
    CHECK(g_meta.cc_time == 0x89ABCDEF);
    CHECK(g_rs <= 6);

    uat_downlink_ingest_stats_t st;
    uat_downlink_ingest_get_stats(&st);
    CHECK(st.frames_total == 1 && st.aircraft_msgs == 1);
    CHECK(st.bad_rs == 0 && st.no_sync == 0 && st.bad_length == 0);
}

static void test_valid_non_aircraft_and_rejections_are_distinct(void)
{
    uint8_t frame[48];
    CHECK(unhex(VEC_DL_LONG_IN, frame, sizeof frame) == 48);
    uat_downlink_ingest_feed(frame, 48, NULL);
    CHECK(g_sink_calls == 1); /* valid type 20, but not an aircraft report */

    memset(frame, 0, sizeof frame);
    uat_downlink_ingest_feed(frame, 30, NULL);
    uat_downlink_ingest_feed(frame, 31, NULL);

    uat_downlink_ingest_stats_t st;
    uat_downlink_ingest_get_stats(&st);
    CHECK(st.frames_total == 2 && st.aircraft_msgs == 1);
    CHECK(st.unhandled_type == 1);
    CHECK(st.no_sync == 1 && st.bad_length == 1 && st.bad_rs == 0);
}

int main(void)
{
    test_short_aircraft_reaches_sink();
    test_valid_non_aircraft_and_rejections_are_distinct();
    if (g_fail) return 1;
    puts("test_uat_downlink_ingest: all OK");
    return 0;
}
