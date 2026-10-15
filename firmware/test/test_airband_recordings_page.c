/* AIRBAND 录音页纯交互模型测试。
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *    -DPK_AIRBAND_RECORDINGS_PAGE_HOST_TEST \
 *    -o /tmp/test_airband_recordings_page \
 *    firmware/test/test_airband_recordings_page.c \
 *    firmware/main/airband_recordings_page.c && /tmp/test_airband_recordings_page
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "airband_recordings_page.h"

static int failures;
static int playback_starts;
static void start_playback(void) { playback_starts++; }

#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; \
} } while (0)

static void test_all_53_rows_are_reachable(void)
{
    const size_t rows = pk_airband_recordings_row_count(53u, false);
    CHECK(rows == 53u);
    CHECK(pk_airband_recordings_max_scroll(rows) > 0);
    CHECK(pk_airband_recordings_row_at_y(
              PK_AIRBAND_RECORDINGS_LIST_TOP,
              pk_airband_recordings_max_scroll(rows), rows) == 48u);
    CHECK(pk_airband_recordings_row_at_y(
              PK_AIRBAND_RECORDINGS_LIST_BOTTOM - 1,
              pk_airband_recordings_max_scroll(rows), rows) == 52u);
}

static void test_current_recording_is_visible_but_disabled(void)
{
    CHECK(pk_airband_recordings_row_count(3u, true) == 4u);
    CHECK(!pk_airband_recordings_row_playable(0u, true, 3u));
    CHECK(pk_airband_recordings_row_playable(1u, true, 3u));
    CHECK(pk_airband_recordings_catalog_index(1u, true, 3u) == 0);
    CHECK(pk_airband_recordings_catalog_index(0u, true, 3u) == -1);
}

static void test_drag_scrolls_without_selecting(void)
{
    pk_airband_recordings_gesture_t g = {0};
    pk_airband_recordings_action_t a;
    const size_t rows = 53u;

    pk_airband_recordings_gesture_press(&g, 200,
                                        PK_AIRBAND_RECORDINGS_LIST_TOP + 20,
                                        0, false, rows);
    int scroll = pk_airband_recordings_gesture_drag(&g, 200,
                                                     PK_AIRBAND_RECORDINGS_LIST_TOP - 100,
                                                     0);
    CHECK(scroll > 0);
    a = pk_airband_recordings_gesture_release(&g, -1, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);
}

static void test_sub_slop_jitter_keeps_press_layout_and_scroll(void)
{
    pk_airband_recordings_gesture_t g = {0};
    const size_t rows = 10u; /* current recording + enough rows to scroll */

    /* y=141 is the last pixel of the current-recording row.  An 11 px
     * finger wobble must remain a tap on that row, not reveal/select row 1. */
    pk_airband_recordings_gesture_press(
        &g, 200, PK_AIRBAND_RECORDINGS_LIST_TOP +
                     PK_AIRBAND_RECORDINGS_ROW_H - 1, 0, true, rows - 1u);
    const int scroll = pk_airband_recordings_gesture_drag(
        &g, 200, PK_AIRBAND_RECORDINGS_LIST_TOP +
                     PK_AIRBAND_RECORDINGS_ROW_H - 12,
        0);
    CHECK(scroll == 0);
    const pk_airband_recordings_action_t a =
        pk_airband_recordings_gesture_release(&g, 0, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);
}

static void test_press_layout_is_stable_when_recording_state_changes(void)
{
    pk_airband_recordings_gesture_t g = {0};
    pk_airband_recordings_action_t a;

    /* A press on the disabled current row stays disabled even when recording
     * finishes before release. */
    pk_airband_recordings_gesture_press(
        &g, 100, PK_AIRBAND_RECORDINGS_LIST_TOP + 10, 0, true, 3u);
    a = pk_airband_recordings_gesture_release(&g, 0, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);

    /* Conversely, a press on catalog[0] must remain bound to catalog[0] if a
     * new current-recording row appears before release. */
    pk_airband_recordings_gesture_press(
        &g, 100, PK_AIRBAND_RECORDINGS_LIST_TOP + 10, 0, false, 3u);
    a = pk_airband_recordings_gesture_release(&g, 0, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_SELECT);
    CHECK(a.catalog_index == 0);
}

static void test_tap_and_transport_disabled_states(void)
{
    pk_airband_recordings_gesture_t g = {0};
    pk_airband_recordings_action_t a;

    /* 当前录音行不可操作，不采用“点下去再报错”。 */
    pk_airband_recordings_gesture_press(&g, 100,
                                        PK_AIRBAND_RECORDINGS_LIST_TOP + 10,
                                        0, true, 3u);
    a = pk_airband_recordings_gesture_release(&g, 0, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);

    /* 下一行是首个已完成录音。 */
    pk_airband_recordings_gesture_press(&g, 100,
                                        PK_AIRBAND_RECORDINGS_LIST_TOP +
                                            PK_AIRBAND_RECORDINGS_ROW_H + 10,
                                        0, true, 3u);
    a = pk_airband_recordings_gesture_release(&g, 0, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_SELECT);
    CHECK(a.catalog_index == 0);

    CHECK(!pk_airband_recordings_can_previous(3u, -1));
    CHECK(!pk_airband_recordings_can_next(3u, -1));
    CHECK(!pk_airband_recordings_can_previous(3u, 0));
    CHECK(pk_airband_recordings_can_next(3u, 0));
    CHECK(pk_airband_recordings_can_previous(3u, 2));
    CHECK(!pk_airband_recordings_can_next(3u, 2));

    /* 控件 disabled 必须在命中层拦截，不允许“点完再报错”。 */
    pk_airband_recordings_gesture_press(
        &g, (PK_AIRBAND_RECORDINGS_PREV_X0 + PK_AIRBAND_RECORDINGS_PREV_X1) / 2,
        PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10, 0, false, 3u);
    a = pk_airband_recordings_gesture_release(&g, 0, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);

    pk_airband_recordings_gesture_press(
        &g, (PK_AIRBAND_RECORDINGS_NEXT_X0 + PK_AIRBAND_RECORDINGS_NEXT_X1) / 2,
        PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10, 0, false, 3u);
    a = pk_airband_recordings_gesture_release(&g, 0, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NEXT);

    pk_airband_recordings_gesture_press(
        &g, (PK_AIRBAND_RECORDINGS_TOGGLE_X0 + PK_AIRBAND_RECORDINGS_TOGGLE_X1) / 2,
        PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10, 0, false, 3u);
    a = pk_airband_recordings_gesture_release(&g, -1, true);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);
}

static void test_playback_volume_controls_are_first_class_actions(void)
{
    pk_airband_recordings_gesture_t g = {0};
    const int y = PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10;
    const int xs[] = {52, 138, 224};
    const int expected[] = {5, 6, 7}; /* volume down, mute, volume up */
    for (size_t i = 0; i < 3; ++i) {
        pk_airband_recordings_gesture_press(&g, xs[i], y, 0, false, 3u);
        const pk_airband_recordings_action_t a =
            pk_airband_recordings_gesture_release(&g, 0, true);
        CHECK((int)a.kind == expected[i]);
    }
}

static void test_playback_volume_disabled_state_blocks_the_action(void)
{
    CHECK(!pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN, false, 50));
    CHECK(!pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_MUTE, false, 50));
    CHECK(!pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, false, 50));
    CHECK(!pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN, true, 0));
    CHECK(!pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, true, 100));
    CHECK(pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_MUTE, true, 0));
    CHECK(pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN, true, 5));
    CHECK(pk_airband_recordings_audio_action_enabled(
        PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, true, 95));

    pk_airband_recordings_gesture_t g = {0};
    pk_airband_recordings_gesture_press(
        &g, 52, PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10, 0, false, 3u);
    CHECK(pk_airband_recordings_gesture_release_ex(
              &g, 0, true, false, 50).kind ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
    pk_airband_recordings_gesture_press(
        &g, 52, PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10, 0, false, 3u);
    CHECK(pk_airband_recordings_gesture_release_ex(
              &g, 0, true, true, 0).kind ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
    pk_airband_recordings_gesture_press(
        &g, 224, PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10, 0, false, 3u);
    CHECK(pk_airband_recordings_gesture_release_ex(
              &g, 0, true, true, 100).kind ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
}

static void test_selection_success_explicitly_starts_playback(void)
{
    playback_starts = 0;
    CHECK(pk_airband_recordings_start_after_select(true, start_playback));
    CHECK(playback_starts == 1);
    CHECK(!pk_airband_recordings_start_after_select(false, start_playback));
    CHECK(playback_starts == 1);
    CHECK(!pk_airband_recordings_start_after_select(true, NULL));
}

static void test_unavailable_playback_disables_rows_and_transport(void)
{
    pk_airband_recordings_gesture_t g = {0};
    pk_airband_recordings_gesture_press(
        &g, 100, PK_AIRBAND_RECORDINGS_LIST_TOP + 10,
        0, false, 3u);
    pk_airband_recordings_action_t a =
        pk_airband_recordings_gesture_release(&g, 0, false);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);

    pk_airband_recordings_gesture_press(
        &g, (PK_AIRBAND_RECORDINGS_TOGGLE_X0 +
             PK_AIRBAND_RECORDINGS_TOGGLE_X1) / 2,
        PK_AIRBAND_RECORDINGS_CONTROL_Y0 + 10,
        0, false, 3u);
    a = pk_airband_recordings_gesture_release(&g, 0, false);
    CHECK(a.kind == PK_AIRBAND_RECORDINGS_ACT_NONE);
}

static void test_catalog_failure_disables_playback_without_discarding_stale_data(void)
{
    CHECK(pk_airband_recordings_controls_available(
        PK_AIRBAND_RECORDINGS_CATALOG_READY, false, true));
    CHECK(!pk_airband_recordings_controls_available(
        PK_AIRBAND_RECORDINGS_CATALOG_UNAVAILABLE, false, true));
    CHECK(!pk_airband_recordings_controls_available(
        PK_AIRBAND_RECORDINGS_CATALOG_SD_UNAVAILABLE, false, true));
    CHECK(pk_airband_recordings_controls_available(
        PK_AIRBAND_RECORDINGS_CATALOG_SCAN_ERROR, true, true));
    /* 只有“扫描失败 + 仍持有旧目录”允许继续操作；内存错误即使旧目录
     * 尚在也不能被 stale 这个单一布尔值越权放行。 */
    CHECK(!pk_airband_recordings_controls_available(
        PK_AIRBAND_RECORDINGS_CATALOG_MEMORY_ERROR, true, true));
    CHECK(!pk_airband_recordings_controls_available(
        PK_AIRBAND_RECORDINGS_CATALOG_READY, false, false));
}

static void test_catalog_copy_failure_is_visible_as_memory_error(void)
{
    CHECK(pk_airband_recordings_catalog_view(
              PK_AIRBAND_RECORDING_SNAPSHOT_OOM,
              PK_AIRBAND_RECORDING_CATALOG_READY) ==
          PK_AIRBAND_RECORDINGS_CATALOG_MEMORY_ERROR);
    CHECK(pk_airband_recordings_catalog_view(
              PK_AIRBAND_RECORDING_SNAPSHOT_OK,
              PK_AIRBAND_RECORDING_CATALOG_NOT_READY) ==
          PK_AIRBAND_RECORDINGS_CATALOG_SCANNING);
    CHECK(pk_airband_recordings_catalog_view(
              PK_AIRBAND_RECORDING_SNAPSHOT_OK,
              PK_AIRBAND_RECORDING_CATALOG_SD_UNAVAILABLE) ==
          PK_AIRBAND_RECORDINGS_CATALOG_SD_UNAVAILABLE);
    CHECK(pk_airband_recordings_catalog_view(
              PK_AIRBAND_RECORDING_SNAPSHOT_OK,
              PK_AIRBAND_RECORDING_CATALOG_SCAN_ERROR) ==
          PK_AIRBAND_RECORDINGS_CATALOG_SCAN_ERROR);
    CHECK(pk_airband_recordings_catalog_view(
              PK_AIRBAND_RECORDING_SNAPSHOT_OK,
              PK_AIRBAND_RECORDING_CATALOG_OOM) ==
          PK_AIRBAND_RECORDINGS_CATALOG_MEMORY_ERROR);
    CHECK(pk_airband_recordings_catalog_view(
              PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED,
              PK_AIRBAND_RECORDING_CATALOG_READY) ==
          PK_AIRBAND_RECORDINGS_CATALOG_UNAVAILABLE);
    CHECK(pk_airband_recordings_catalog_view(
              PK_AIRBAND_RECORDING_SNAPSHOT_START_FAILED,
              PK_AIRBAND_RECORDING_CATALOG_READY) ==
          PK_AIRBAND_RECORDINGS_CATALOG_UNAVAILABLE);
}

static void test_catalog_does_not_swap_rows_during_a_press(void)
{
    CHECK(pk_airband_recordings_refresh_allowed(false));
    CHECK(!pk_airband_recordings_refresh_allowed(true));
}

static void test_catalog_copy_only_runs_for_a_new_revision(void)
{
    const pk_airband_recording_store_token_t old = {
        .revision = 7u, .mount_generation = 3u, .mounted = true,
    };
    pk_airband_recording_store_token_t changed = old;
    CHECK(pk_airband_recordings_snapshot_refresh_needed(
        true, PK_AIRBAND_RECORDING_SNAPSHOT_OK, old, old));
    CHECK(!pk_airband_recordings_snapshot_refresh_needed(
        false, PK_AIRBAND_RECORDING_SNAPSHOT_OK, old, old));
    /* 失败结果不能缓存：即使目录 token 没变，下一轮也必须重试。 */
    CHECK(pk_airband_recordings_snapshot_refresh_needed(
        false, PK_AIRBAND_RECORDING_SNAPSHOT_OOM, old, old));
    CHECK(pk_airband_recordings_snapshot_refresh_needed(
        false, PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED, old, old));
    changed.revision++;
    CHECK(pk_airband_recordings_snapshot_refresh_needed(
        false, PK_AIRBAND_RECORDING_SNAPSHOT_OK, old, changed));
    changed = old;
    changed.mount_generation++;
    CHECK(pk_airband_recordings_snapshot_refresh_needed(
        false, PK_AIRBAND_RECORDING_SNAPSHOT_OK, old, changed));
    changed = old;
    changed.mounted = false;
    CHECK(pk_airband_recordings_snapshot_refresh_needed(
        false, PK_AIRBAND_RECORDING_SNAPSHOT_OK, old, changed));
}

static void test_snapshot_commit_rejects_a_finished_page_lifetime(void)
{
    /* acquire 在锁外执行：只有页面仍活跃且 epoch 未变化时才能安装结果。 */
    CHECK(pk_airband_recordings_snapshot_commit_allowed(true, 7u, 7u));
    CHECK(!pk_airband_recordings_snapshot_commit_allowed(false, 7u, 7u));
    CHECK(!pk_airband_recordings_snapshot_commit_allowed(true, 7u, 8u));
}

static void test_control_drag_neither_scrolls_nor_activates(void)
{
    pk_airband_recordings_gesture_t gesture = {0};
    pk_airband_recordings_gesture_press(&gesture, 224, 430, 30, false, 53);
    CHECK(pk_airband_recordings_gesture_drag(&gesture, 224, 400, 30) == 30);
    CHECK(pk_airband_recordings_gesture_release(&gesture, 0, true).kind ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
    pk_airband_recordings_gesture_press(&gesture, 224, 430, 30, false, 53);
    CHECK(pk_airband_recordings_gesture_drag(&gesture, 270, 430, 30) == 30);
    CHECK(pk_airband_recordings_gesture_release(&gesture, 0, true).kind ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
}

static void test_feedback_expires_and_disabled_controls_never_highlight(void)
{
    pk_airband_recordings_gesture_t gesture = {0};
    pk_airband_recordings_gesture_press(&gesture, 224, 430, 0, false, 3);
    CHECK(pk_airband_recordings_feedback(&gesture, 0, true, true, 50,
          PK_AIRBAND_RECORDINGS_ACT_NONE, false, 100, 0, 150) ==
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP);
    CHECK(gesture.active);
    CHECK(pk_airband_recordings_feedback(&gesture, 0, true, true, 100,
          PK_AIRBAND_RECORDINGS_ACT_NONE, false, 100, 0, 150) ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
    gesture.active = false;
    CHECK(pk_airband_recordings_feedback(&gesture, 0, true, true, 100,
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, true, 149, 0, 150) ==
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP);
    CHECK(pk_airband_recordings_feedback(&gesture, 0, true, true, 50,
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, true, 150, 0, 150) ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
    CHECK(pk_airband_recordings_feedback(&gesture, 0, true, true, 50,
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, false, 1, 0, 150) ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
    CHECK(pk_airband_recordings_feedback(&gesture, 0, true, true, 50,
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, true, 20, UINT32_MAX - 50, 150) ==
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP);
    pk_airband_recordings_gesture_press(&gesture, 400, 150, 0, false, 3);
    CHECK(pk_airband_recordings_feedback(&gesture, 0, true, true, 50,
          PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP, true, 10, 0, 150) ==
          PK_AIRBAND_RECORDINGS_ACT_NONE);
}

static void test_audio_duration_and_invalid_clock_are_unambiguous(void)
{
    char duration[16];
    pk_airband_recordings_format_duration(duration, sizeof(duration), 18000);
    CHECK(strcmp(duration, "00:18") == 0);
    pk_airband_recordings_format_duration(duration, sizeof(duration), 3601000);
    CHECK(strcmp(duration, "60:01") == 0);
    CHECK(!pk_airband_recordings_time_valid(0));
    CHECK(!pk_airband_recordings_time_valid(315532800));
    CHECK(pk_airband_recordings_time_valid(1790640000));
}

int main(void)
{
    test_all_53_rows_are_reachable();
    test_current_recording_is_visible_but_disabled();
    test_drag_scrolls_without_selecting();
    test_sub_slop_jitter_keeps_press_layout_and_scroll();
    test_press_layout_is_stable_when_recording_state_changes();
    test_tap_and_transport_disabled_states();
    test_playback_volume_controls_are_first_class_actions();
    test_playback_volume_disabled_state_blocks_the_action();
    test_selection_success_explicitly_starts_playback();
    test_unavailable_playback_disables_rows_and_transport();
    test_catalog_failure_disables_playback_without_discarding_stale_data();
    test_catalog_copy_failure_is_visible_as_memory_error();
    test_catalog_does_not_swap_rows_during_a_press();
    test_catalog_copy_only_runs_for_a_new_revision();
    test_snapshot_commit_rejects_a_finished_page_lifetime();
    test_control_drag_neither_scrolls_nor_activates();
    test_feedback_expires_and_disabled_controls_never_highlight();
    test_audio_duration_and_invalid_clock_are_unambiguous();
    if (failures) return 1;
    puts("PASS airband recordings page");
    return 0;
}
