/* 航空电台一级页面的纯数据模型：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -DPK_AIRBAND_PAGE_HOST_TEST -o /tmp/test_airband_page \
 *      firmware/test/test_airband_page.c firmware/main/airband_page.c \
 *      firmware/main/airband_frequency_input.c \
 *      && /tmp/test_airband_page
 */
#include "airband_page.h"
#include "pfd_icon_font.h"

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

static void test_reopen_failed_refresh_keeps_last_good_list(void)
{
    pk_airband_page_list_t page = {0};
    CHECK(pk_airband_page_list_open(&page), "first open needs presets\n");
    pk_airband_station_list_t loaded={0};
    for (unsigned i=0; i<40; ++i) {
        pk_airband_station_candidate_t row={118000u+i*25u,i,1,"REAL TWR"};
        CHECK(pk_airband_station_list_append(&loaded,&row), "fixture allocation\n");
    }
    pk_airband_station_list_t old=pk_airband_page_list_publish(&page,&loaded,true);
    pk_airband_station_list_free(&old);
    pk_airband_station_t saved[40];
    memcpy(saved,page.stations.rows,sizeof(saved));
    CHECK(!pk_airband_page_list_open(&page), "reopen replaced last good with presets\n");
    /* Failed database query, frequency copy or allocation has no publishable result. */
    for (unsigned fault=0; fault<3; ++fault) {
        pk_airband_page_list_failed(&page);
        CHECK(page.failed && !page.complete, "failed refresh did not show failure\n");
        CHECK(page.stations.count==40 &&
              memcmp(page.stations.rows,saved,sizeof(saved))==0,
              "failed refresh lost last-good count/content\n");
    }
    pk_airband_station_list_free(&page.stations);
}

static void test_task_creation_failure_is_visible(void)
{
    pk_airband_page_list_t page={0};
    pk_airband_page_list_task_created(&page,false);
    CHECK(page.failed && !page.complete, "task creation failure hid LIST ERROR\n");
}

static void test_successful_empty_query_is_complete_and_does_not_interrupt_gesture(void)
{
    pk_airband_page_list_t page={0};
    pk_airband_station_list_t presets={0};
    const pk_airband_station_candidate_t guard={121500,UINT32_MAX,0,"GUARD"};
    const pk_airband_station_candidate_t air={123450,UINT32_MAX,0,"AIR-AIR"};
    CHECK(pk_airband_station_list_append(&presets,&guard) &&
          pk_airband_station_list_append(&presets,&air), "preset allocation\n");
    pk_airband_station_list_t old=pk_airband_page_list_publish(&page,&presets,true);
    pk_airband_station_list_free(&old);
    unsigned queries=1;
    pk_airband_page_gesture_t g;
    pk_airband_page_gesture_press(&g,300,120,0);
    for (uint32_t now=2000; now<=10000; now+=2000) {
        if (pk_airband_page_should_retry_loader(false,page.complete,now,0,2000)) {
            queries++;
            g.active=false; /* Publication invalidates the production gesture. */
        }
    }
    CHECK(queries==1, "successful empty result re-queried %u times\n",queries);
    CHECK(page.stations.count==2 && !page.failed && page.complete,
          "successful empty result was treated as failure/not ready\n");
    const pk_airband_page_action_t a=pk_airband_page_gesture_release(&g,2,0);
    CHECK(a.kind==PK_AIRBAND_PAGE_ACT_TUNE && a.station_index==0,
          "empty-result rescan interrupted pending tap\n");
    pk_airband_station_list_free(&page.stations);
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
    CHECK(pk_airband_station_collect(in, 3, out, 2) == SIZE_MAX,
          "insufficient output silently truncated legal stations\n");
    CHECK(pk_airband_station_collect(in, 3, NULL, 2) == 0,
          "NULL output accepted\n");
}

static void test_growing_list_and_scroll(void)
{
    pk_airband_station_list_t list = {0};
    for (unsigned i = 0; i < 130; ++i) {
        pk_airband_station_candidate_t row = {118000u + i * 25u, i, 1, "TWR"};
        CHECK(pk_airband_station_list_append(&list, &row), "allocation failed\n");
    }
    CHECK(list.count == 130, "dense region truncated to %zu\n", list.count);
    pk_airband_station_candidate_t dup = {118005u, 999, 1, "DUP CARRIER"};
    CHECK(pk_airband_station_list_append(&list, &dup) && list.count == 130,
          "carrier duplicate entered list\n");
    CHECK(pk_airband_scroll_clamp(-60, list.count) == 0, "upper boundary lost\n");
    CHECK(pk_airband_scroll_clamp(99999, list.count) == (130 - 6) * 48,
          "lower boundary lost\n");
    CHECK(pk_airband_scroll_drag(0, -144, list.count) == 144, "drag did not scroll\n");
    pk_airband_page_action_t a = pk_airband_page_action_scrolled(300, 120, list.count, 144);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_TUNE && a.station_index == 3,
          "scroll tap tuned visible index instead of actual index\n");
    CHECK(pk_airband_scroll_clamp(144, 2) == 0, "refresh did not clamp scroll\n");
    pk_airband_page_gesture_t g;
    pk_airband_page_gesture_press(&g, 300, 150, 0);
    int offset = pk_airband_page_gesture_drag(&g, 300, 6, list.count, 0);
    CHECK(offset == 144, "gesture did not reach third row\n");
    a = pk_airband_page_gesture_release(&g, list.count, offset);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_NONE, "drag release accidentally tuned\n");
    pk_airband_page_gesture_press(&g, 300, 150, offset);
    a = pk_airband_page_gesture_release(&g, list.count, offset);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_TUNE && a.station_index == 3,
          "tap after drag mapped wrong station\n");
    a = pk_airband_page_gesture_release(&g, list.count, offset);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_NONE, "duplicate release tuned twice\n");
    pk_airband_station_list_free(&list);
}

static void test_station_rows_and_compact_bottom_controls_are_reachable(void)
{
    pk_airband_page_action_t a = pk_airband_page_action_at(100, 80, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY,
          "manual-frequency button is not clickable\n");
    a = pk_airband_page_action_at(300, 120, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_TUNE && a.station_index == 0,
          "first station row is not clickable\n");
    a = pk_airband_page_action_at(100, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_AUTHORIZE_PHONE,
          "bottom-left phone-owner authorization button is not clickable\n");
    a = pk_airband_page_action_at(250, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_RECORD,
          "record button is not clickable\n");
    a = pk_airband_page_action_at(440, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN,
          "volume-down button is not clickable\n");
    a = pk_airband_page_action_at(590, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_MUTE,
          "center mute button is not clickable\n");
    a = pk_airband_page_action_at(700, 440, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_VOLUME_UP,
          "volume-up button is not clickable\n");
    a = pk_airband_page_action_at(600, 80, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_NONE,
          "removed top pair button still owns touches\n");
    a = pk_airband_page_action_at(300, 400, 6);
    CHECK(a.kind == PK_AIRBAND_PAGE_ACT_NONE,
          "blank space triggered an action\n");
}

static void test_fab_never_shrinks_station_rows(void)
{
    CHECK(pk_airband_page_station_row_right() == 784,
          "station row still yields visual width to the floating FAB\n");
}

static void test_volume_popup_timeout_handles_tick_wrap(void)
{
    CHECK(pk_airband_volume_popup_visible(true, 100u, 100u, 1200u),
          "fresh volume popup is hidden\n");
    CHECK(!pk_airband_volume_popup_visible(true, 1300u, 100u, 1200u),
          "expired volume popup remains visible\n");
    CHECK(!pk_airband_volume_popup_visible(false, 0x80001000u, 0u, 1200u),
          "an inactive volume popup resurrected after half a tick cycle\n");
    CHECK(!pk_airband_volume_popup_visible(true, 0x80001000u, 0u, 1200u),
          "a stale active volume popup resurrected after half a tick cycle\n");
    CHECK(pk_airband_volume_popup_visible(true, 100u,
                                          UINT32_MAX - 100u, 1200u),
          "volume popup failed across the uint32 tick wrap\n");
}

static void test_volume_button_feedback_tracks_press_drag_and_release(void)
{
    pk_airband_page_gesture_t gesture;

    pk_airband_page_gesture_press(&gesture, 440, 440, 0);
    CHECK(pk_airband_volume_feedback_action(
              &gesture, PK_AIRBAND_PAGE_ACT_NONE, false,
              1000u, 0u, 200u) == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN,
          "volume-down press has no immediate visual feedback\n");

    (void)pk_airband_page_gesture_drag(&gesture, 460, 440, 6, 0);
    CHECK(pk_airband_volume_feedback_action(
              &gesture, PK_AIRBAND_PAGE_ACT_NONE, false,
              1000u, 0u, 200u) == PK_AIRBAND_PAGE_ACT_NONE,
          "dragged volume press remained highlighted\n");

    gesture.active = false;
    CHECK(pk_airband_volume_feedback_action(
              &gesture, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN, true,
              1199u, 1000u, 200u) == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN,
          "released volume feedback disappeared before one display frame\n");
    CHECK(pk_airband_volume_feedback_action(
              &gesture, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN, true,
              1200u, 1000u, 200u) == PK_AIRBAND_PAGE_ACT_NONE,
          "released volume feedback exceeded its 200 ms window\n");

    CHECK(pk_airband_volume_feedback_action(
              &gesture, PK_AIRBAND_PAGE_ACT_VOLUME_UP, true,
              50u, UINT32_MAX - 100u, 200u) ==
              PK_AIRBAND_PAGE_ACT_VOLUME_UP,
          "released volume feedback failed across uint32 tick wrap\n");
    CHECK(pk_airband_volume_feedback_action(
              &gesture, PK_AIRBAND_PAGE_ACT_MUTE, true,
              1050u, 1000u, 200u) == PK_AIRBAND_PAGE_ACT_NONE,
          "non-volume action was accepted as volume feedback\n");

    pk_airband_page_gesture_press(&gesture, 700, 440, 0);
    CHECK(pk_airband_volume_feedback_action(
              &gesture, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN, true,
              1050u, 1000u, 200u) == PK_AIRBAND_PAGE_ACT_VOLUME_UP,
          "new volume press did not override stale released feedback\n");
}

static void test_new_gesture_prevents_old_volume_feedback_from_resurrecting(void)
{
    pk_airband_page_gesture_t gesture;
    pk_airband_page_action_kind_t latched_action =
        PK_AIRBAND_PAGE_ACT_VOLUME_DOWN;
    bool latched_active = true;

    /* A rapid press on the other volume key must replace, not stack on top of,
     * the previous key's post-release feedback. */
    pk_airband_page_gesture_press(&gesture, 700, 440, 0);
    pk_airband_volume_feedback_cancel_latch(&latched_action, &latched_active);
    CHECK(!latched_active && latched_action == PK_AIRBAND_PAGE_ACT_NONE,
          "new volume press did not retire the previous feedback latch\n");
    CHECK(pk_airband_volume_feedback_action(
              &gesture, latched_action, latched_active,
              1050u, 1000u, 200u) == PK_AIRBAND_PAGE_ACT_VOLUME_UP,
          "new volume-up press did not immediately replace old volume-down feedback\n");

    /* Once that new gesture becomes a drag, releasing it must not expose the
     * old volume-down latch again. */
    (void)pk_airband_page_gesture_drag(&gesture, 720, 440, 6, 0);
    (void)pk_airband_page_gesture_release(&gesture, 6, 0);
    CHECK(pk_airband_volume_feedback_action(
              &gesture, latched_action, latched_active,
              1100u, 1000u, 200u) == PK_AIRBAND_PAGE_ACT_NONE,
          "cancelled drag resurrected the previous volume feedback latch\n");

    /* Non-volume interactions retire the latch too; their release must not
     * flash the most recently adjusted volume key. */
    latched_action = PK_AIRBAND_PAGE_ACT_VOLUME_DOWN;
    latched_active = true;
    pk_airband_page_gesture_press(&gesture, 100, 440, 0);
    pk_airband_volume_feedback_cancel_latch(&latched_action, &latched_active);
    (void)pk_airband_page_gesture_release(&gesture, 6, 0);
    CHECK(pk_airband_volume_feedback_action(
              &gesture, latched_action, latched_active,
              1150u, 1000u, 200u) == PK_AIRBAND_PAGE_ACT_NONE,
          "non-volume gesture release resurrected volume feedback\n");
}

static void test_list_publish_cancels_only_station_row_gestures(void)
{
    pk_airband_page_gesture_t gesture;

    pk_airband_page_gesture_press(&gesture, 300, 120, 0);
    pk_airband_page_gesture_cancel_for_list_publish(&gesture);
    CHECK(!gesture.active,
          "station-row press survived an asynchronous list replacement\n");
    CHECK(pk_airband_page_gesture_release(&gesture, 6, 0).kind ==
              PK_AIRBAND_PAGE_ACT_NONE,
          "replaced station list allowed the pending row tap to tune\n");

    static const struct {
        int x;
        int y;
        pk_airband_page_action_kind_t action;
    } fixed_controls[] = {
        {100, 80, PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY},
        {100, 440, PK_AIRBAND_PAGE_ACT_AUTHORIZE_PHONE},
        {250, 440, PK_AIRBAND_PAGE_ACT_RECORD},
        {440, 440, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN},
        {590, 440, PK_AIRBAND_PAGE_ACT_MUTE},
        {700, 440, PK_AIRBAND_PAGE_ACT_VOLUME_UP},
    };
    for (size_t i = 0;
         i < sizeof(fixed_controls) / sizeof(fixed_controls[0]); ++i) {
        pk_airband_page_gesture_press(&gesture, fixed_controls[i].x,
                                      fixed_controls[i].y, 0);
        pk_airband_page_gesture_cancel_for_list_publish(&gesture);
        CHECK(gesture.active,
              "list publish cancelled fixed control action=%d\n",
              fixed_controls[i].action);
        const pk_airband_page_action_t released =
            pk_airband_page_gesture_release(&gesture, 6, 0);
        CHECK(released.kind == fixed_controls[i].action,
              "fixed control action=%d was lost after list publish (got %d)\n",
              fixed_controls[i].action, released.kind);
    }
}

static void test_recording_gap_is_diagnostic_only(void)
{
    const pk_airband_record_visual_t active =
        pk_airband_record_visual_resolve(false, false, false, true, true, true);
    CHECK(active.label == PK_AIRBAND_REC_VIS_ACTIVE && active.red &&
              !active.warning,
          "recovered active recording must be red/active without a GAP marker\n");
    const pk_airband_record_visual_t held =
        pk_airband_record_visual_resolve(false, false, false, true, false, true);
    CHECK(held.label == PK_AIRBAND_REC_VIS_ARMED && held.red &&
              !held.warning,
          "enabled recording with historical GAP must remain ARMED\n");
    const pk_airband_record_visual_t error =
        pk_airband_record_visual_resolve(false, true, false, true, true, true);
    CHECK(error.label == PK_AIRBAND_REC_VIS_ERROR && error.warning,
          "interrupted recording must retain a real write error\n");
    const pk_airband_record_visual_t missing =
        pk_airband_record_visual_resolve(false, false, true, true, true, true);
    CHECK(missing.label == PK_AIRBAND_REC_VIS_SD_MISSING && missing.warning,
          "interrupted recording must retain missing-card warning\n");
}

static void test_stopped_recording_does_not_show_historical_gap(void)
{
    const pk_airband_record_visual_t idle =
        pk_airband_record_visual_resolve(false, false, false, true, false, false);
    CHECK(idle.label == PK_AIRBAND_REC_VIS_IDLE && !idle.red && !idle.warning,
          "stopped recording still shows an amber historical GAP\n");
    const pk_airband_record_visual_t writing =
        pk_airband_record_visual_resolve(true, false, false, true, true, false);
    CHECK(writing.label == PK_AIRBAND_REC_VIS_WRITING && !writing.warning,
          "stopping interrupted recording must remain WRITING until drained\n");
    CHECK(pk_airband_record_visual_resolve(false, true, false, true, false, false)
              .label == PK_AIRBAND_REC_VIS_ERROR,
          "stopping recording hides a real write error\n");
    CHECK(pk_airband_record_visual_resolve(false, false, true, true, false, false)
              .label == PK_AIRBAND_REC_VIS_SD_MISSING,
          "stopping recording hides a missing SD card\n");
}

static void test_record_icon_and_label_are_centered_as_one_group(void)
{
    /* Button [206,366), icon 30 px, gap 8 px.  A 50 px label makes an
     * 88 px group, leaving exactly 36 px on both sides. */
    CHECK(pk_airband_record_content_start(50) == 242,
          "record content group is not centered for a short label\n");
    /* A longer translated label must shift the entire group left instead of
     * leaving the icon pinned at a fixed coordinate. */
    CHECK(pk_airband_record_content_start(70) == 232,
          "record content group does not adapt to translated label width\n");
}

static void test_volume_controls_are_three_equal_separated_buttons(void)
{
    static const struct {
        int x;
        pk_airband_page_action_kind_t action;
    } probes[] = {
        {381, PK_AIRBAND_PAGE_ACT_NONE},
        {382, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN},
        {509, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN},
        {510, PK_AIRBAND_PAGE_ACT_NONE},
        {525, PK_AIRBAND_PAGE_ACT_NONE},
        {526, PK_AIRBAND_PAGE_ACT_MUTE},
        {653, PK_AIRBAND_PAGE_ACT_MUTE},
        {654, PK_AIRBAND_PAGE_ACT_NONE},
        {669, PK_AIRBAND_PAGE_ACT_NONE},
        {670, PK_AIRBAND_PAGE_ACT_VOLUME_UP},
        {797, PK_AIRBAND_PAGE_ACT_VOLUME_UP},
        {798, PK_AIRBAND_PAGE_ACT_NONE},
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        const pk_airband_page_action_t a =
            pk_airband_page_action_at(probes[i].x, 440, 6);
        CHECK(a.kind == probes[i].action,
              "audio control boundary x=%d mapped to %d, expected %d\n",
              probes[i].x, a.kind, probes[i].action);
    }

    static const struct {
        int y;
        pk_airband_page_action_kind_t action;
    } vertical_probes[] = {
        {417, PK_AIRBAND_PAGE_ACT_NONE},
        {418, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN},
        {469, PK_AIRBAND_PAGE_ACT_VOLUME_DOWN},
        {470, PK_AIRBAND_PAGE_ACT_NONE},
    };
    for (size_t i = 0;
         i < sizeof(vertical_probes) / sizeof(vertical_probes[0]); ++i) {
        const pk_airband_page_action_t a =
            pk_airband_page_action_at(440, vertical_probes[i].y, 6);
        CHECK(a.kind == vertical_probes[i].action,
              "audio control boundary y=%d mapped to %d, expected %d\n",
              vertical_probes[i].y, a.kind, vertical_probes[i].action);
    }
}

static void test_mute_button_uses_unambiguous_state_colors(void)
{
    CHECK(pk_airband_mute_button_color_role(false) ==
              PK_AIRBAND_BUTTON_COLOR_BLUE,
          "audible speaker is not mapped to blue\n");
    CHECK(pk_airband_mute_button_color_role(true) ==
              PK_AIRBAND_BUTTON_COLOR_ORANGE,
          "muted speaker is not mapped to orange\n");
}

static void test_audio_controls_use_unambiguous_semantic_icons(void)
{
    CHECK(pk_airband_control_icon_role(PK_AIRBAND_PAGE_ACT_VOLUME_DOWN,
                                       false) ==
              PK_AIRBAND_CONTROL_ICON_VOLUME_DOWN,
          "volume-down action does not use the volume-down icon\n");
    CHECK(pk_airband_control_icon_role(PK_AIRBAND_PAGE_ACT_MUTE, false) ==
              PK_AIRBAND_CONTROL_ICON_BRAND_AWARENESS,
          "audible state does not use brand_awareness\n");
    CHECK(pk_airband_control_icon_role(PK_AIRBAND_PAGE_ACT_MUTE, true) ==
              PK_AIRBAND_CONTROL_ICON_VOLUME_OFF,
          "muted state does not use volume_off\n");
    CHECK(pk_airband_control_icon_role(PK_AIRBAND_PAGE_ACT_VOLUME_UP,
                                       false) ==
              PK_AIRBAND_CONTROL_ICON_VOLUME_UP,
          "volume-up action does not use the volume-up icon\n");

    CHECK(pk_airband_control_icon_id(PK_AIRBAND_CONTROL_ICON_VOLUME_DOWN) ==
              PK_ICON_VOLUME_DOWN,
          "volume-down role is not mapped to the generated volume_down glyph\n");
    CHECK(pk_airband_control_icon_id(
              PK_AIRBAND_CONTROL_ICON_BRAND_AWARENESS) ==
              PK_ICON_BRAND_AWARENESS,
          "audible role is not mapped to the generated brand_awareness glyph\n");
    CHECK(pk_airband_control_icon_id(PK_AIRBAND_CONTROL_ICON_VOLUME_OFF) ==
              PK_ICON_VOLUME_OFF,
          "muted role is not mapped to the generated volume_off glyph\n");
    CHECK(pk_airband_control_icon_id(PK_AIRBAND_CONTROL_ICON_VOLUME_UP) ==
              PK_ICON_VOLUME_UP,
          "volume-up role is not mapped to the generated volume_up glyph\n");
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
    test_reopen_failed_refresh_keeps_last_good_list();
    test_task_creation_failure_is_visible();
    test_successful_empty_query_is_complete_and_does_not_interrupt_gesture();
    test_nearby_airport_frequencies_are_filtered_and_deduplicated();
    test_station_channels_validate_and_map_833_carriers();
    test_capacity_is_never_exceeded();
    test_growing_list_and_scroll();
    test_station_rows_and_compact_bottom_controls_are_reachable();
    test_manual_frequency_parser_uses_exact_mhz_decimal();
    test_fab_press_is_yielded_but_page_buttons_are_owned();
    test_fab_never_shrinks_station_rows();
    test_volume_popup_timeout_handles_tick_wrap();
    test_volume_button_feedback_tracks_press_drag_and_release();
    test_new_gesture_prevents_old_volume_feedback_from_resurrecting();
    test_list_publish_cancels_only_station_row_gestures();
    test_recording_gap_is_diagnostic_only();
    test_stopped_recording_does_not_show_historical_gap();
    test_record_icon_and_label_are_centered_as_one_group();
    test_volume_controls_are_three_equal_separated_buttons();
    test_mute_button_uses_unambiguous_state_colors();
    test_audio_controls_use_unambiguous_semantic_icons();
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
