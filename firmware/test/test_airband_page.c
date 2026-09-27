/* 航空电台一级页面的纯数据模型：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -DPK_AIRBAND_PAGE_HOST_TEST -o /tmp/test_airband_page \
 *      firmware/test/test_airband_page.c firmware/main/airband_page.c \
 *      firmware/main/airband_frequency_input.c \
 *      && /tmp/test_airband_page
 */
#include "airband_page.h"

#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); fail++; } } while (0)

static int s_window_calls;
static int s_full_calls;

static int fake_window_hit(double lat, double lon, void *out, int max)
{
    (void)lat; (void)lon; (void)out; (void)max;
    s_window_calls++;
    return 4;
}

static int fake_window_miss(double lat, double lon, void *out, int max)
{
    (void)lat; (void)lon; (void)out; (void)max;
    s_window_calls++;
    return 0;
}

static int fake_full_hit(double lat, double lon, void *out, int max)
{
    (void)lat; (void)lon; (void)out; (void)max;
    s_full_calls++;
    return 3;
}

static void test_nearby_query_prefers_loaded_window_and_falls_back(void)
{
    unsigned char opaque[32] = {0};
    s_window_calls = s_full_calls = 0;
    CHECK(pk_airband_nearest_with_fallback(23.139, 113.432, opaque, 4,
                                           fake_window_hit, fake_full_hit) == 4,
          "loaded window result was not used\n");
    CHECK(s_window_calls == 1 && s_full_calls == 0,
          "full DB was queried despite a usable window result\n");

    s_window_calls = s_full_calls = 0;
    CHECK(pk_airband_nearest_with_fallback(23.139, 113.432, opaque, 4,
                                           fake_window_miss, fake_full_hit) == 3,
          "full DB fallback result was lost\n");
    CHECK(s_window_calls == 1 && s_full_calls == 1,
          "window miss did not fall back exactly once\n");
}

static void test_page_retries_after_late_fix_or_database_ready(void)
{
    CHECK(!pk_airband_page_should_retry_loader(true, false, 5000, 0, 2000),
          "running loader was started twice\n");
    CHECK(!pk_airband_page_should_retry_loader(false, true, 5000, 0, 2000),
          "successful nearby list was needlessly reloaded\n");
    CHECK(!pk_airband_page_should_retry_loader(false, false, 1999, 0, 2000),
          "late-fix retry ignored its interval\n");
    CHECK(pk_airband_page_should_retry_loader(false, false, 2000, 0, 2000),
          "late-fix/database-ready state did not retry while page stayed open\n");
    CHECK(pk_airband_page_should_retry_loader(false, false, 100, 0xFFFFFF00u,
                                               200),
          "retry interval is not uint32 wrap-safe\n");
}

static void test_window_airports_without_frequencies_fall_back_to_full_db(void)
{
    const pk_airband_station_candidate_t invalid_only[] = {
        {117999u, 1u, 1u, "OUT LOW"},
        {137001u, 2u, 1u, "OUT HIGH"},
        {118896u, 3u, 1u, "OFF GRID"},
    };
    const pk_airband_station_candidate_t usable_with_duplicate[] = {
        {118700u, 4u, 1u, "VALID"},
        {118700u, 5u, 1u, "DUP"},
    };
    CHECK(pk_airband_page_should_query_full_db(0, NULL, 0),
          "empty window did not request full DB\n");
    CHECK(pk_airband_page_should_query_full_db(4, NULL, 0),
          "window airports without frequencies suppressed full DB fallback\n");
    CHECK(pk_airband_page_should_query_full_db(
              4, invalid_only,
              sizeof(invalid_only) / sizeof(invalid_only[0])),
          "invalid window frequencies suppressed filtered full DB fallback\n");
    CHECK(!pk_airband_page_should_query_full_db(
              4, usable_with_duplicate,
              sizeof(usable_with_duplicate) / sizeof(usable_with_duplicate[0])),
          "usable window frequencies needlessly requested full DB\n");
    CHECK(pk_airband_station_usable_candidate_count(
              usable_with_duplicate,
              sizeof(usable_with_duplicate) /
                  sizeof(usable_with_duplicate[0])) == 1,
          "duplicate carrier counted as multiple usable candidates\n");
}

static void test_invalid_window_rows_do_not_consume_full_db_capacity(void)
{
    pk_airband_station_candidate_t buffered[24] = {0};
    size_t count = 0;
    for (size_t i = 0; i < 24; ++i) {
        if (pk_airband_station_candidate_can_append(
                buffered, count, 24, 118896u)) {
            buffered[count++].freq_khz = 118896u;
        }
    }
    CHECK(count == 0,
          "invalid window rows consumed candidate capacity (%lu)\n",
          (unsigned long)count);
    CHECK(pk_airband_station_candidate_can_append(
              buffered, count, 24, 120400u),
          "valid full-DB row could not enter after invalid window rows\n");
    buffered[count++].freq_khz = 120400u;
    CHECK(!pk_airband_station_candidate_can_append(
              buffered, count, 24, 120400u),
          "duplicate full-DB carrier was accepted\n");
}

static void test_no_fix_still_has_safe_presets(void)
{
    pk_airband_station_t out[8] = {0};
    const size_t n = pk_airband_station_collect(NULL, 0, out, 8);
    CHECK(n == 2, "fallback count=%lu\n", (unsigned long)n);
    CHECK(out[0].frequency_hz == 121500000u &&
          strcmp(out[0].label, "GUARD") == 0,
          "first fallback is not 121.500 GUARD\n");
    CHECK(out[1].frequency_hz == 123450000u &&
          strcmp(out[1].label, "AIR-AIR") == 0,
          "second fallback is not 123.450 AIR-AIR\n");
}

static void test_nearby_airport_frequencies_are_filtered_and_deduplicated(void)
{
    const pk_airband_station_candidate_t in[] = {
        {118700u, 42u, 3u, "ZGGG TWR"},
        {118700u, 42u, 3u, "ZGGG TWR DUP"},
        {137001u, 43u, 3u, "OUT HIGH"},
        {117999u, 44u, 3u, "OUT LOW"},
        {125600u, 45u, 6u, "ZGSZ APP"},
    };
    pk_airband_station_t out[8] = {0};
    const size_t n = pk_airband_station_collect(
        in, sizeof(in) / sizeof(in[0]), out, 8);
    CHECK(n == 4, "nearby+fallback count=%lu\n", (unsigned long)n);
    CHECK(out[0].frequency_hz == 118700000u && out[0].airport_idx == 42u &&
          strcmp(out[0].label, "ZGGG TWR") == 0,
          "first nearby frequency lost identity\n");
    CHECK(out[1].frequency_hz == 125600000u && out[1].airport_idx == 45u,
          "second nearby frequency missing\n");
    CHECK(out[2].frequency_hz == 121500000u &&
          out[3].frequency_hz == 123450000u,
          "fallbacks missing after nearby stations\n");
}

static void test_station_channels_validate_and_map_833_carriers(void)
{
    const pk_airband_station_candidate_t in[] = {
        {118005u, 1u, 1u, "8K33-A"},
        {118010u, 2u, 1u, "8K33-B"},
        {118020u, 3u, 1u, "OMITTED"},
        {118896u, 4u, 1u, "OFF-GRID"},
    };
    pk_airband_station_t out[8] = {0};
    const size_t n = pk_airband_station_collect(in, 4, out, 8);
    CHECK(n == 4, "valid 8.33 channels + fallbacks count=%lu\n",
          (unsigned long)n);
    CHECK(out[0].channel_khz == 118005u &&
          out[0].frequency_hz == 118000000u,
          "118.005 channel/carrier mapping is wrong\n");
    CHECK(out[1].channel_khz == 118010u &&
          out[1].frequency_hz == 118008330u,
          "118.010 channel/carrier mapping is wrong\n");
    CHECK(strcmp(out[2].label, "GUARD") == 0,
          "invalid channel designators reached the station list\n");
}

static void test_capacity_is_never_exceeded(void)
{
    const pk_airband_station_candidate_t in[] = {
        {118100u, 1u, 1u, "A"},
        {118200u, 2u, 1u, "B"},
        {118300u, 3u, 1u, "C"},
    };
    pk_airband_station_t out[2] = {0};
    CHECK(pk_airband_station_collect(in, 3, out, 2) == 2,
          "collector exceeded capacity\n");
    CHECK(out[0].frequency_hz == 118100000u &&
          out[1].frequency_hz == 118200000u,
          "capacity clamp changed ordering\n");
    CHECK(pk_airband_station_collect(in, 3, NULL, 2) == 0,
          "NULL output accepted\n");
}

static void test_station_rows_and_speaker_button_are_reachable(void)
{
    pk_airband_page_action_t a = pk_airband_page_action_at(100, 80, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY,
          "manual-frequency button is not clickable\n");
    a = pk_airband_page_action_at(300, 120, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_TUNE && a.station_index == 0,
          "first station row is not clickable\n");
    a = pk_airband_page_action_at(100, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_SPEAKER_TEST,
          "speaker test button is not clickable\n");
    a = pk_airband_page_action_at(270, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_MUTE,
          "mute button is not clickable\n");
    a = pk_airband_page_action_at(600, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN,
          "volume-down button is not clickable\n");
    a = pk_airband_page_action_at(750, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_VOLUME_UP,
          "volume-up button is not clickable\n");
    a = pk_airband_page_action_at(300, 400, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_NONE,
          "blank space triggered an action\n");
}

static void test_manual_frequency_parser_uses_exact_mhz_decimal(void)
{
    uint32_t hz = 0;
    CHECK(pk_airband_frequency_parse_mhz("121.500", &hz) && hz == 121500000u,
          "121.500 did not parse exactly: %lu\n", (unsigned long)hz);
    CHECK(pk_airband_frequency_parse_mhz("118", &hz) && hz == 118000000u,
          "integer MHz did not parse exactly\n");
    CHECK(pk_airband_frequency_parse_mhz("136.975", &hz) && hz == 136975000u,
          "136.975 did not parse exactly\n");
    CHECK(pk_airband_frequency_parse_mhz("137.000", &hz) && hz == 137000000u,
          "137.000 upper boundary rejected\n");
    CHECK(pk_airband_frequency_parse_mhz("118.005", &hz) &&
          hz == 118000000u,
          "8.33 kHz channel 118.005 did not map to 118.0000 carrier\n");
    CHECK(pk_airband_frequency_parse_mhz("118.010", &hz) &&
          hz == 118008330u,
          "8.33 kHz channel 118.010 did not map to 118.0083 carrier\n");
    CHECK(!pk_airband_frequency_parse_mhz("118.896", &hz),
          "non-channel 118.896 accepted by final parser\n");
    CHECK(!pk_airband_frequency_parse_mhz("118.020", &hz),
          "omitted ICAO designator 118.020 accepted by final parser\n");
    CHECK(!pk_airband_frequency_parse_mhz("117.999", &hz),
          "below-band frequency accepted\n");
    CHECK(!pk_airband_frequency_parse_mhz("137.001", &hz),
          "above-band frequency accepted\n");
    CHECK(!pk_airband_frequency_parse_mhz("121.5000", &hz),
          "more than three decimal digits accepted\n");
    CHECK(!pk_airband_frequency_parse_mhz("121X500", &hz),
          "non-numeric frequency accepted\n");
    CHECK(!pk_airband_frequency_parse_mhz("", &hz),
          "empty frequency accepted\n");
}

static void test_fab_press_is_yielded_but_page_buttons_are_owned(void)
{
    CHECK(!pk_airband_page_owns_press(750, 330, true, 728, 312, 56, 56),
          "page swallowed a press inside the visible FAB\n");
    CHECK(pk_airband_page_owns_press(100, 440, true, 728, 312, 56, 56),
          "speaker button was yielded with unrelated FAB geometry\n");
    CHECK(pk_airband_page_owns_press(750, 330, false, 728, 312, 56, 56),
          "hidden FAB left a dead touch region\n");
}

int main(void)
{
    test_nearby_query_prefers_loaded_window_and_falls_back();
    test_page_retries_after_late_fix_or_database_ready();
    test_window_airports_without_frequencies_fall_back_to_full_db();
    test_invalid_window_rows_do_not_consume_full_db_capacity();
    test_no_fix_still_has_safe_presets();
    test_nearby_airport_frequencies_are_filtered_and_deduplicated();
    test_station_channels_validate_and_map_833_carriers();
    test_capacity_is_never_exceeded();
    test_station_rows_and_speaker_button_are_reachable();
    test_manual_frequency_parser_uses_exact_mhz_decimal();
    test_fab_press_is_yielded_but_page_buttons_are_owned();
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
