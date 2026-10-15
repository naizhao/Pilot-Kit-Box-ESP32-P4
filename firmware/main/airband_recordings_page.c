#include "airband_recordings_page.h"

#include <limits.h>
#include <stdio.h>

#define PAGE_DRAG_SLOP 12
#define PAGE_PREV_X0   PK_AIRBAND_RECORDINGS_PREV_X0
#define PAGE_PREV_X1   PK_AIRBAND_RECORDINGS_PREV_X1
#define PAGE_TOGGLE_X0 PK_AIRBAND_RECORDINGS_TOGGLE_X0
#define PAGE_TOGGLE_X1 PK_AIRBAND_RECORDINGS_TOGGLE_X1
#define PAGE_NEXT_X0   PK_AIRBAND_RECORDINGS_NEXT_X0
#define PAGE_NEXT_X1   PK_AIRBAND_RECORDINGS_NEXT_X1
#define PAGE_CONTROL_Y0 PK_AIRBAND_RECORDINGS_CONTROL_Y0
#define PAGE_CONTROL_Y1 PK_AIRBAND_RECORDINGS_CONTROL_Y1

size_t pk_airband_recordings_row_count(size_t catalog_count, bool recording)
{
    return catalog_count + (recording ? 1u : 0u);
}

int pk_airband_recordings_max_scroll(size_t rows)
{
    const size_t visible_px = PK_AIRBAND_RECORDINGS_LIST_BOTTOM -
                              PK_AIRBAND_RECORDINGS_LIST_TOP;
    const size_t content_px = rows * PK_AIRBAND_RECORDINGS_ROW_H;
    if (content_px <= visible_px) return 0;
    const size_t max = content_px - visible_px;
    return max > INT_MAX ? INT_MAX : (int)max;
}

size_t pk_airband_recordings_row_at_y(int y, int scroll, size_t rows)
{
    if (rows == 0 || y < PK_AIRBAND_RECORDINGS_LIST_TOP ||
        y >= PK_AIRBAND_RECORDINGS_LIST_BOTTOM) return SIZE_MAX;
    if (scroll < 0) scroll = 0;
    size_t row = (size_t)(y - PK_AIRBAND_RECORDINGS_LIST_TOP + scroll) /
                 PK_AIRBAND_RECORDINGS_ROW_H;
    return row < rows ? row : SIZE_MAX;
}

bool pk_airband_recordings_row_playable(size_t row, bool recording,
                                        size_t catalog_count)
{
    return pk_airband_recordings_catalog_index(row, recording, catalog_count) >= 0;
}

int pk_airband_recordings_catalog_index(size_t row, bool recording,
                                        size_t catalog_count)
{
    if (recording) {
        if (row == 0) return -1;
        row--;
    }
    return row < catalog_count && row <= INT_MAX ? (int)row : -1;
}

bool pk_airband_recordings_can_previous(size_t catalog_count, int selected)
{
    return selected > 0 && (size_t)selected < catalog_count;
}

bool pk_airband_recordings_can_next(size_t catalog_count, int selected)
{
    return selected >= 0 && (size_t)(selected + 1) < catalog_count;
}

bool pk_airband_recordings_start_after_select(bool select_succeeded,
                                              void (*start_playback)(void))
{
    if (!select_succeeded || !start_playback) return false;
    start_playback();
    return true;
}

bool pk_airband_recordings_controls_available(
    pk_airband_recordings_catalog_view_t catalog_view, bool stale,
    bool playback_service_available)
{
    if (!playback_service_available) return false;
    return catalog_view == PK_AIRBAND_RECORDINGS_CATALOG_READY ||
           (catalog_view == PK_AIRBAND_RECORDINGS_CATALOG_SCAN_ERROR && stale);
}

pk_airband_recordings_catalog_view_t pk_airband_recordings_catalog_view(
    pk_airband_recording_snapshot_result_t snapshot_result,
    int catalog_state)
{
    if (snapshot_result == PK_AIRBAND_RECORDING_SNAPSHOT_OOM ||
        catalog_state == PK_AIRBAND_RECORDING_CATALOG_OOM)
        return PK_AIRBAND_RECORDINGS_CATALOG_MEMORY_ERROR;
    if (snapshot_result != PK_AIRBAND_RECORDING_SNAPSHOT_OK ||
        catalog_state == PK_AIRBAND_RECORDING_CATALOG_NOT_STARTED ||
        catalog_state == PK_AIRBAND_RECORDING_CATALOG_START_FAILED)
        return PK_AIRBAND_RECORDINGS_CATALOG_UNAVAILABLE;
    if (catalog_state == PK_AIRBAND_RECORDING_CATALOG_SD_UNAVAILABLE)
        return PK_AIRBAND_RECORDINGS_CATALOG_SD_UNAVAILABLE;
    if (catalog_state == PK_AIRBAND_RECORDING_CATALOG_SCAN_ERROR)
        return PK_AIRBAND_RECORDINGS_CATALOG_SCAN_ERROR;
    if (catalog_state == PK_AIRBAND_RECORDING_CATALOG_NOT_READY)
        return PK_AIRBAND_RECORDINGS_CATALOG_SCANNING;
    return PK_AIRBAND_RECORDINGS_CATALOG_READY;
}

bool pk_airband_recordings_refresh_allowed(bool gesture_active)
{
    return !gesture_active;
}

bool pk_airband_recordings_snapshot_refresh_needed(
    bool force, pk_airband_recording_snapshot_result_t previous_result,
    pk_airband_recording_store_token_t observed,
    pk_airband_recording_store_token_t current)
{
    return force || previous_result != PK_AIRBAND_RECORDING_SNAPSHOT_OK ||
           observed.revision != current.revision ||
           observed.mount_generation != current.mount_generation ||
           observed.mounted != current.mounted;
}

bool pk_airband_recordings_snapshot_commit_allowed(
    bool page_active, uint32_t acquire_epoch, uint32_t current_epoch)
{
    return page_active && acquire_epoch == current_epoch;
}

void pk_airband_recordings_gesture_press(pk_airband_recordings_gesture_t *g,
                                         int x, int y, int scroll,
                                         bool recording,
                                         size_t catalog_count)
{
    if (!g) return;
    *g = (pk_airband_recordings_gesture_t){
        .active = true, .start_x = x, .start_y = y, .last_y = y, .last_x = x,
        .start_scroll = scroll, .recording_at_press = recording,
        .catalog_count_at_press = catalog_count,
    };
}

int pk_airband_recordings_gesture_drag(pk_airband_recordings_gesture_t *g,
                                       int x, int y, int scroll)
{
    if (!g || !g->active) return scroll;
    const int dy = y - g->start_y;
    const int dx = x - g->start_x;
    g->last_x = x;
    g->last_y = y;
    if (!g->moved && dy <= PAGE_DRAG_SLOP && dy >= -PAGE_DRAG_SLOP &&
        dx <= PAGE_DRAG_SLOP && dx >= -PAGE_DRAG_SLOP)
        return g->start_scroll;
    g->moved = true;
    if (g->start_y < PK_AIRBAND_RECORDINGS_LIST_TOP ||
        g->start_y >= PK_AIRBAND_RECORDINGS_LIST_BOTTOM)
        return g->start_scroll;
    int next = g->start_scroll - dy;
    const int max = pk_airband_recordings_max_scroll(
        pk_airband_recordings_row_count(g->catalog_count_at_press,
                                        g->recording_at_press));
    if (next < 0) next = 0;
    if (next > max) next = max;
    return next;
}

bool pk_airband_recordings_audio_action_enabled(
    pk_airband_recordings_action_kind_t action, bool speaker_available,
    uint8_t playback_volume)
{
    if (!speaker_available) return false;
    if (action == PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN)
        return playback_volume > 0;
    if (action == PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP)
        return playback_volume < 100;
    return action == PK_AIRBAND_RECORDINGS_ACT_MUTE;
}

pk_airband_recordings_action_t pk_airband_recordings_gesture_release_ex(
    pk_airband_recordings_gesture_t *g, int selected,
    bool playback_available, bool audio_controls_available,
    uint8_t playback_volume)
{
    pk_airband_recordings_action_t out = { PK_AIRBAND_RECORDINGS_ACT_NONE, -1 };
    if (!g || !g->active) return out;
    const int x = g->start_x, y = g->start_y;
    const bool moved = g->moved;
    g->active = false;
    if (moved) return out;

    const size_t rows = pk_airband_recordings_row_count(
        g->catalog_count_at_press, g->recording_at_press);
    const size_t row = pk_airband_recordings_row_at_y(
        y, g->start_scroll, rows);
    const int catalog_index = row == SIZE_MAX ? -1 :
        pk_airband_recordings_catalog_index(row, g->recording_at_press,
                                            g->catalog_count_at_press);
    if (playback_available && catalog_index >= 0) {
        out.kind = PK_AIRBAND_RECORDINGS_ACT_SELECT;
        out.catalog_index = catalog_index;
        return out;
    }
    if (y < PAGE_CONTROL_Y0 || y >= PAGE_CONTROL_Y1) return out;
    const size_t catalog_count = g->catalog_count_at_press;
    if (x >= PK_AIRBAND_RECORDINGS_VOL_DOWN_X0 &&
        x < PK_AIRBAND_RECORDINGS_VOL_DOWN_X1) {
        if (pk_airband_recordings_audio_action_enabled(
                PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN,
                audio_controls_available, playback_volume))
            out.kind = PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN;
    }
    else if (x >= PK_AIRBAND_RECORDINGS_MUTE_X0 &&
             x < PK_AIRBAND_RECORDINGS_MUTE_X1) {
        if (pk_airband_recordings_audio_action_enabled(
                PK_AIRBAND_RECORDINGS_ACT_MUTE,
                audio_controls_available, playback_volume))
            out.kind = PK_AIRBAND_RECORDINGS_ACT_MUTE;
    }
    else if (x >= PK_AIRBAND_RECORDINGS_VOL_UP_X0 &&
             x < PK_AIRBAND_RECORDINGS_VOL_UP_X1) {
        if (pk_airband_recordings_audio_action_enabled(
                PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP,
                audio_controls_available, playback_volume))
            out.kind = PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP;
    }
    else if (playback_available && x >= PAGE_PREV_X0 && x < PAGE_PREV_X1 &&
        pk_airband_recordings_can_previous(catalog_count, selected))
        out.kind = PK_AIRBAND_RECORDINGS_ACT_PREVIOUS;
    else if (playback_available && x >= PAGE_TOGGLE_X0 &&
             x < PAGE_TOGGLE_X1 && selected >= 0)
        out.kind = PK_AIRBAND_RECORDINGS_ACT_TOGGLE;
    else if (playback_available && x >= PAGE_NEXT_X0 && x < PAGE_NEXT_X1 &&
             pk_airband_recordings_can_next(catalog_count, selected))
        out.kind = PK_AIRBAND_RECORDINGS_ACT_NEXT;
    return out;
}

pk_airband_recordings_action_t pk_airband_recordings_gesture_release(
    pk_airband_recordings_gesture_t *g, int selected,
    bool playback_available)
{
    return pk_airband_recordings_gesture_release_ex(
        g, selected, playback_available, playback_available, 50);
}

pk_airband_recordings_action_kind_t pk_airband_recordings_feedback(
    const pk_airband_recordings_gesture_t *gesture, int selected,
    bool playback_available, bool audio_available, uint8_t volume,
    pk_airband_recordings_action_kind_t latched, bool latch_active,
    uint32_t now, uint32_t started, uint32_t duration)
{
    if (gesture && gesture->active) {
        pk_airband_recordings_gesture_t copy = *gesture;
        const pk_airband_recordings_action_t action =
            pk_airband_recordings_gesture_release_ex(
                &copy, selected, playback_available, audio_available, volume);
        return action.kind == PK_AIRBAND_RECORDINGS_ACT_SELECT
                   ? PK_AIRBAND_RECORDINGS_ACT_NONE : action.kind;
    }
    return latch_active && (uint32_t)(now - started) < duration
               ? latched : PK_AIRBAND_RECORDINGS_ACT_NONE;
}

void pk_airband_recordings_format_duration(char *out, size_t capacity,
                                          uint32_t milliseconds)
{
    const unsigned seconds = milliseconds / 1000u;
    (void)snprintf(out, capacity, "%02u:%02u", seconds / 60u, seconds % 60u);
}

bool pk_airband_recordings_time_valid(int64_t timestamp)
{
    return timestamp >= INT64_C(1577836800);
}

#ifndef PK_AIRBAND_RECORDINGS_PAGE_HOST_TEST
/* 平台渲染与服务胶水在纯状态机之后，host 测试不拉入 IDF。 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "airband_playback.h"
#include "airband_record.h"
#include "airband_recording_store.h"
#include "airband_speaker.h"
#include "aircraft_state.h"
#include "display.h"
#include "esp_attr.h"
#include "esp_timer.h"
#ifdef PK_SIM_BUILD
#define xTaskGetTickCount() ((uint32_t)(esp_timer_get_time() / 1000))
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i18n.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_icon_font.h"
#include "pfd_layout.h"
#include "pfd_statusbar.h"
#include "pk_ui_nav.h"
#include "ui_state.h"

static pk_airband_recording_snapshot_t s_snapshot;
static pk_airband_recordings_gesture_t s_gesture;
static pk_airband_recordings_action_kind_t s_feedback;
static bool s_feedback_active;
static uint32_t s_feedback_started;
static bool s_volume_popup_active;
static uint32_t s_volume_popup_started;
static uint8_t s_volume_popup_value;
#ifndef PK_SIM_BUILD
static SemaphoreHandle_t s_page_lock;
#endif
static int s_scroll;
static uint32_t s_last_refresh;
static bool s_notice_shown;
static bool s_page_active;
static uint32_t s_page_epoch;
static pk_airband_recording_store_token_t s_snapshot_token;
static pk_airband_recording_snapshot_result_t s_snapshot_result =
    PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED;

static void page_lock_take(void)
{
#ifndef PK_SIM_BUILD
    if (s_page_lock) xSemaphoreTake(s_page_lock, portMAX_DELAY);
#endif
}

static void page_lock_give(void)
{
#ifndef PK_SIM_BUILD
    if (s_page_lock) xSemaphoreGive(s_page_lock);
#endif
}

static uint16_t col_bg(void)    { return pk_rgb565(10, 15, 28); }
static uint16_t col_panel(void) { return pk_rgb565(25, 34, 51); }
static uint16_t col_line(void)  { return pk_rgb565(51, 65, 85); }
static uint16_t col_text(void)  { return pk_rgb565(234, 241, 255); }
static uint16_t col_sub(void)   { return pk_rgb565(145, 160, 184); }
static uint16_t col_blue(void)  { return pk_rgb565(46, 109, 240); }
static uint16_t col_green(void) { return pk_rgb565(34, 197, 94); }
static uint16_t col_amber(void) { return pk_rgb565(245, 158, 11); }

static bool same_id(pk_airband_recording_id_t a,
                    pk_airband_recording_id_t b)
{
    return a.session == b.session && a.volume == b.volume &&
           a.interrupted == b.interrupted;
}

static int selected_index(const pk_airband_playback_status_t *pb)
{
    if (!pb || !pb->selected) return -1;
    for (size_t i = 0; i < s_snapshot.count; ++i)
        if (same_id(s_snapshot.entries[i].id, pb->id)) return (int)i;
    return -1;
}

static void refresh_snapshot(bool force)
{
    const uint32_t now = (uint32_t)xTaskGetTickCount();
    page_lock_take();
    if (!s_page_active ||
        (!force && s_last_refresh != 0 &&
         now - s_last_refresh < (uint32_t)pdMS_TO_TICKS(1000))) {
        page_lock_give();
        return;
    }
    const bool refresh_allowed =
        pk_airband_recordings_refresh_allowed(s_gesture.active);
    const uint32_t acquire_epoch = s_page_epoch;
    page_lock_give();
    if (!refresh_allowed) return; /* 按下到松手期间行号必须绑定同一份快照。 */
    const pk_airband_recording_store_token_t current_token =
        pk_airband_recording_store_token();
    page_lock_take();
    const bool need_refresh = pk_airband_recordings_snapshot_refresh_needed(
        force, s_snapshot_result, s_snapshot_token, current_token);
    page_lock_give();
    if (!need_refresh) {
        page_lock_take();
        if (pk_airband_recordings_snapshot_commit_allowed(
                s_page_active, acquire_epoch, s_page_epoch))
            s_last_refresh = now;
        page_lock_give();
        return;
    }
    pk_airband_recording_snapshot_t next = {0};
    const pk_airband_recording_snapshot_result_t result =
        pk_airband_recording_store_snapshot_acquire_ex(&next);
    pk_airband_recording_snapshot_t old = {0};
    bool installed = false;
    page_lock_take();
    const bool commit = pk_airband_recordings_snapshot_commit_allowed(
                            s_page_active, acquire_epoch, s_page_epoch) &&
                        !s_gesture.active;
    if (result == PK_AIRBAND_RECORDING_SNAPSHOT_OK) {
        if (commit) {
            old = s_snapshot;
            s_snapshot = next;
            s_snapshot_result = result;
            s_snapshot_token = current_token;
            installed = true;
        }
    } else if (commit) {
        s_snapshot_result = result;
        s_snapshot_token = current_token;
    }
    if (commit) s_last_refresh = now;
    page_lock_give();
    if (installed) pk_airband_recording_snapshot_release(&old);
    else pk_airband_recording_snapshot_release(&next);
}

void pk_airband_recordings_page_init(void)
{
#ifndef PK_SIM_BUILD
    if (!s_page_lock) s_page_lock = xSemaphoreCreateMutex();
#endif
    page_lock_take();
    pk_airband_recording_store_set_visible(false);
    s_page_active = false;
    s_page_epoch++;
    s_scroll = 0;
    s_last_refresh = 0;
    s_notice_shown = false;
    s_snapshot_token = (pk_airband_recording_store_token_t){0};
    s_snapshot_result = PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED;
    memset(&s_gesture, 0, sizeof(s_gesture));
    s_feedback_active = false;
    s_volume_popup_active = false;
    page_lock_give();
}

void pk_airband_recordings_page_open(void)
{
    page_lock_take();
    pk_airband_recording_store_set_visible(true);
    s_page_active = true;
    s_feedback_active = false;
    s_volume_popup_active = false;
    s_page_epoch++;
    s_last_refresh = 0;
    page_lock_give();
    refresh_snapshot(true);
}

void pk_airband_recordings_page_leave(void)
{
    pk_airband_recording_snapshot_t old = {0};
    page_lock_take();
    pk_airband_recording_store_set_visible(false);
    s_page_active = false;
    s_page_epoch++;
    old = s_snapshot;
    s_snapshot = (pk_airband_recording_snapshot_t){0};
    s_snapshot_result = PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED;
    s_snapshot_token = (pk_airband_recording_store_token_t){0};
    s_last_refresh = 0;
    memset(&s_gesture, 0, sizeof(s_gesture));
    s_feedback_active = false;
    s_volume_popup_active = false;
    page_lock_give();
    pk_airband_playback_pause(PK_AIRBAND_PLAYBACK_PAUSE_USER);
    pk_airband_recording_snapshot_release(&old);
}

static const char *pause_text(pk_airband_playback_pause_reason_t reason)
{
    switch (reason) {
    case PK_AIRBAND_PLAYBACK_PAUSE_LIVE: return pk_i18n_text(PK_TR_RECORDINGS_PAUSED_LIVE);
    case PK_AIRBAND_PLAYBACK_PAUSE_RECORD_PRESSURE: return pk_i18n_text(PK_TR_RECORDINGS_PAUSED_PRESSURE);
    case PK_AIRBAND_PLAYBACK_PAUSE_TEST_TONE: return pk_i18n_text(PK_TR_RECORDINGS_PAUSED_TEST);
    case PK_AIRBAND_PLAYBACK_PAUSE_SD_UNAVAILABLE: return pk_i18n_text(PK_TR_RECORDINGS_SD_MISSING);
    case PK_AIRBAND_PLAYBACK_PAUSE_FILE_CHANGED: return pk_i18n_text(PK_TR_RECORDINGS_FILE_CHANGED);
    case PK_AIRBAND_PLAYBACK_PAUSE_IO_ERROR: return pk_i18n_text(PK_TR_RECORDINGS_IO_ERROR);
    case PK_AIRBAND_PLAYBACK_PAUSE_SPEAKER_ERROR: return pk_i18n_text(PK_TR_RECORDINGS_SPEAKER_ERROR);
    case PK_AIRBAND_PLAYBACK_PAUSE_EOF: return pk_i18n_text(PK_TR_RECORDINGS_COMPLETE);
    case PK_AIRBAND_PLAYBACK_PAUSE_USER: return pk_i18n_text(PK_TR_RECORDINGS_PAUSED);
    default: return pk_i18n_text(PK_TR_RECORDINGS_READY);
    }
}

static void draw_icon(uint16_t *fb, int x, int y, pk_icon_id_t icon,
                      uint16_t color)
{
    pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H, x, y,
                    pk_icon_bitmap + (size_t)icon * PK_ICON_W * PK_ICON_H / 2,
                    PK_ICON_W, PK_ICON_H, color);
}

static void draw_transport(uint16_t *fb, int x0, int x1, pk_icon_id_t icon,
                           bool enabled, bool active, bool pressed)
{
    pk_pfd_fill_round_rect(fb, x0, PAGE_CONTROL_Y0, x1, PAGE_CONTROL_Y1, 12,
                           pressed ? pk_rgb565(84, 148, 255) :
                           active ? col_blue() : col_panel());
    draw_icon(fb, (x0 + x1 - PK_ICON_W) / 2,
              PAGE_CONTROL_Y0 + (PAGE_CONTROL_Y1 - PAGE_CONTROL_Y0 - PK_ICON_H) / 2,
              icon, enabled ? col_text() : col_line());
}

static void draw_playback_audio_control(uint16_t *fb, int x0, int x1,
                                        pk_icon_id_t icon, uint16_t bg,
                                        bool enabled, bool pressed)
{
    pk_pfd_fill_round_rect(fb, x0, PAGE_CONTROL_Y0, x1, PAGE_CONTROL_Y1, 12,
                           pressed ? pk_rgb565(84, 148, 255) : bg);
    draw_icon(fb, (x0 + x1 - PK_ICON_W) / 2,
              PAGE_CONTROL_Y0 + (PAGE_CONTROL_Y1 - PAGE_CONTROL_Y0 - PK_ICON_H) / 2,
              icon, enabled ? col_text() : col_line());
}

static void format_recording_title(char *out, size_t cap,
                                   const pk_airband_recording_entry_t *e)
{
    if (pk_airband_recordings_time_valid(e->mtime_sec)) {
        time_t t = (time_t)e->mtime_sec;
        struct tm tmv;
        char date[24];
        if (localtime_r(&t, &tmv) &&
            strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tmv) > 0) {
            (void)snprintf(out, cap, pk_i18n_text(PK_TR_RECORDINGS_SAVED_FORMAT), date);
            return;
        }
    }
    (void)snprintf(out, cap,
                   pk_i18n_text(PK_TR_RECORDINGS_TIME_UNKNOWN_FORMAT),
                   (unsigned long)e->id.session,
                   (unsigned long)e->id.volume);
}

static void draw_volume_popup(uint16_t *fb, uint8_t volume)
{
    char label[32];
    (void)snprintf(label, sizeof(label),
                   pk_i18n_text(PK_TR_AIRBAND_VOLUME_FORMAT),
                   (unsigned)volume);
    const int text_width = pk_aa_text_width(label, PK_AA_L);
    const int width = text_width + 56;
    const int x0 = (PK_DISPLAY_W - width) / 2;
    pk_pfd_fill_round_rect(fb, x0, 190, x0 + width, 262, 14,
                           pk_rgb565(18, 26, 44));
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + (width - text_width) / 2,
               190 + (72 - PK_AA_L_H) / 2,
               label, col_text(), PK_AA_L);
}

void pk_airband_recordings_page_render(uint16_t *fb)
{
    refresh_snapshot(false);
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg());
    static EXT_RAM_BSS_ATTR aircraft_t aircraft[AIRCRAFT_TABLE_CAPACITY];
    pk_pfd_status_t topbar = {
        .aircraft_count = aircraft_state_snapshot(aircraft, AIRCRAFT_TABLE_CAPACITY,
                                                  esp_timer_get_time(),
                                                  AIRCRAFT_STALE_AGE_US),
    };

    pk_airband_record_status_t rec = {0};
    pk_airband_record_get(&rec);
    const bool recording = rec.active || rec.stopping;
    pk_airband_playback_status_t pb = {0};
    (void)pk_airband_playback_get(&pb);
    pk_airband_speaker_status_t speaker = {0};
    const bool speaker_available = pk_airband_speaker_get(&speaker) && speaker.online;
    const bool playback_service_available =
        pb.started &&
        pb.service_state == PK_AIRBAND_PLAYBACK_SERVICE_READY;
    page_lock_take();
    const int selected = selected_index(&pb);
    const size_t rows = pk_airband_recordings_row_count(s_snapshot.count, recording);
    const int max_scroll = pk_airband_recordings_max_scroll(rows);
    if (s_scroll > max_scroll) s_scroll = max_scroll;

    const char *summary = pause_text(pb.pause_reason);
    uint16_t summary_col = pb.playing ? col_green() : col_sub();
    const pk_airband_recordings_catalog_view_t catalog_view =
        pk_airband_recordings_catalog_view(s_snapshot_result,
                                            s_snapshot.catalog_state);
    const bool controls_available = pk_airband_recordings_controls_available(
        catalog_view, s_snapshot.stale, playback_service_available);
    const bool audio_controls_available = controls_available && speaker_available;
    if (catalog_view == PK_AIRBAND_RECORDINGS_CATALOG_SD_UNAVAILABLE) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_SD_MISSING); summary_col = col_amber();
    } else if (catalog_view == PK_AIRBAND_RECORDINGS_CATALOG_MEMORY_ERROR) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_MEMORY_ERROR); summary_col = col_amber();
    } else if (catalog_view == PK_AIRBAND_RECORDINGS_CATALOG_SCAN_ERROR) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_SCAN_ERROR); summary_col = col_amber();
    } else if (catalog_view == PK_AIRBAND_RECORDINGS_CATALOG_UNAVAILABLE) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_CATALOG_UNAVAILABLE); summary_col = col_amber();
    } else if (!playback_service_available) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_PLAYBACK_UNAVAILABLE); summary_col = col_amber();
    } else if (catalog_view == PK_AIRBAND_RECORDINGS_CATALOG_SCANNING) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_SCANNING);
    } else if (s_snapshot.stale) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_STALE); summary_col = col_amber();
    } else if (pb.playing) {
        summary = pk_i18n_text(PK_TR_RECORDINGS_PLAYING);
    }

    if (rows == 0 && s_snapshot.catalog_state == PK_AIRBAND_RECORDING_CATALOG_READY) {
        const char *empty = pk_i18n_text(PK_TR_RECORDINGS_EMPTY);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - pk_aa_text_width(empty, PK_AA_M)) / 2,
                   205, empty, col_sub(), PK_AA_M);
    }

    const size_t first = (size_t)s_scroll / PK_AIRBAND_RECORDINGS_ROW_H;
    const int y_offset = -(s_scroll % PK_AIRBAND_RECORDINGS_ROW_H);
    for (size_t row = first; row < rows; ++row) {
        const int y = PK_AIRBAND_RECORDINGS_LIST_TOP + y_offset +
                      (int)(row - first) * PK_AIRBAND_RECORDINGS_ROW_H;
        if (y >= PK_AIRBAND_RECORDINGS_LIST_BOTTOM) break;
        if (y + PK_AIRBAND_RECORDINGS_ROW_H <= PK_AIRBAND_RECORDINGS_LIST_TOP) continue;
        const int ci = pk_airband_recordings_catalog_index(row, recording,
                                                           s_snapshot.count);
        const bool current = ci < 0;
        const bool chosen = ci >= 0 && ci == selected;
        pk_pfd_fill_round_rect(fb, 16, y + 2, 784,
                               y + PK_AIRBAND_RECORDINGS_ROW_H - 3, 8,
                               chosen ? col_blue() : col_panel());
        if (current) {
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 32, y + 10,
                       pk_i18n_text(PK_TR_RECORDINGS_CURRENT), col_text(), PK_AA_S);
            char bytes[32];
            (void)snprintf(bytes, sizeof(bytes), "%lu KB",
                           (unsigned long)(rec.bytes / 1024u));
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 32, y + 34,
                       bytes, col_sub(), PK_AA_XS);
            const char *tag = pk_i18n_text(PK_TR_AIRBAND_REC_ACTIVE);
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                       764 - pk_aa_text_width(tag, PK_AA_XS), y + 22,
                       tag, col_green(), PK_AA_XS);
            continue;
        }
        const pk_airband_recording_entry_t *e = &s_snapshot.entries[ci];
        char title[80], detail[64], dur[16], duration_label[48];
        format_recording_title(title, sizeof(title), e);
        pk_airband_recordings_format_duration(dur, sizeof(dur), e->wav.duration_ms);
        (void)snprintf(duration_label, sizeof(duration_label),
                       pk_i18n_text(PK_TR_RECORDINGS_DURATION_FORMAT), dur);
        if (e->name.carrier_hz / 1000u != e->name.channel_khz) {
            (void)snprintf(detail, sizeof(detail),
                           pk_i18n_text(PK_TR_RECORDINGS_RF_FORMAT),
                           (unsigned long)(e->name.channel_khz / 1000u),
                           (unsigned long)(e->name.channel_khz % 1000u),
                           (unsigned long)(e->name.carrier_hz / 1000000u),
                           (unsigned long)((e->name.carrier_hz % 1000000u) / 1000u),
                           "");
        } else {
            (void)snprintf(detail, sizeof(detail), "%lu.%03lu MHz",
                           (unsigned long)(e->name.channel_khz / 1000u),
                           (unsigned long)(e->name.channel_khz % 1000u));
        }
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 32, y + 8,
                   title, controls_available ? col_text() : col_line(), PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 32, y + 34,
                   detail, chosen ? col_text() : col_sub(), PK_AA_XS);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   764 - pk_aa_text_width(duration_label, PK_AA_XS), y + 34,
                   duration_label, chosen ? col_text() : col_sub(), PK_AA_XS);
        if (chosen && pb.data_bytes > 0) {
            const int w = (int)((uint64_t)(764 - 32) * pb.cursor_bytes / pb.data_bytes);
            pk_pfd_fill_rect(fb, 32, y + PK_AIRBAND_RECORDINGS_ROW_H - 8,
                             32 + w, y + PK_AIRBAND_RECORDINGS_ROW_H - 5,
                             col_green());
        }
    }
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W,
                     PK_AIRBAND_RECORDINGS_LIST_TOP, col_bg());
    pk_pfd_fill_rect(fb, 0, PK_AIRBAND_RECORDINGS_LIST_BOTTOM,
                     PK_DISPLAY_W, PK_DISPLAY_H, col_bg());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L, PK_UI_TITLE_Y,
               pk_i18n_text(PK_TR_RECORDINGS_TITLE), PK_UI_TITLE_COL,
               PK_UI_TITLE_SIZE);
    pk_ui_topbar_status_collect(&topbar);
    pk_ui_topbar_status_render(fb, &topbar);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 16, 56, summary,
               summary_col, PK_AA_XS);
    if (s_snapshot.invalid_files > 0) {
        char bad[48];
        (void)snprintf(bad, sizeof(bad), pk_i18n_text(PK_TR_RECORDINGS_INVALID_FORMAT),
                       (unsigned)s_snapshot.invalid_files);
        const int x = 784 - pk_aa_text_width(bad, PK_AA_XS);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x, 56, bad, col_amber(), PK_AA_XS);
    }
    if (max_scroll > 0) {
        const int h = PK_AIRBAND_RECORDINGS_LIST_BOTTOM - PK_AIRBAND_RECORDINGS_LIST_TOP;
        int thumb = (int)((int64_t)h * h /
                          ((int64_t)rows * PK_AIRBAND_RECORDINGS_ROW_H));
        if (thumb < 12) thumb = 12;
        const int top = PK_AIRBAND_RECORDINGS_LIST_TOP +
                        (int)((int64_t)(h - thumb) * s_scroll / max_scroll);
        pk_pfd_fill_rect(fb, 789, PK_AIRBAND_RECORDINGS_LIST_TOP, 794,
                         PK_AIRBAND_RECORDINGS_LIST_BOTTOM, col_panel());
        pk_pfd_fill_round_rect(fb, 789, top, 794, top + thumb, 2, col_sub());
    }

    const pk_airband_recordings_action_kind_t feedback =
        pk_airband_recordings_feedback(&s_gesture, selected, controls_available,
            audio_controls_available, speaker.playback_volume, s_feedback,
            s_feedback_active, (uint32_t)xTaskGetTickCount(), s_feedback_started,
            (uint32_t)pdMS_TO_TICKS(150));
    if (!s_gesture.active && feedback == PK_AIRBAND_RECORDINGS_ACT_NONE)
        s_feedback_active = false;
    draw_playback_audio_control(
        fb, PK_AIRBAND_RECORDINGS_VOL_DOWN_X0,
        PK_AIRBAND_RECORDINGS_VOL_DOWN_X1, PK_ICON_VOLUME_DOWN, col_panel(),
        audio_controls_available && speaker.playback_volume > 0,
        feedback == PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN);
    draw_playback_audio_control(
        fb, PK_AIRBAND_RECORDINGS_MUTE_X0, PK_AIRBAND_RECORDINGS_MUTE_X1,
        speaker.playback_muted ? PK_ICON_VOLUME_OFF : PK_ICON_BRAND_AWARENESS,
        speaker.playback_muted ? col_amber() : col_blue(),
        audio_controls_available, feedback == PK_AIRBAND_RECORDINGS_ACT_MUTE);
    draw_playback_audio_control(
        fb, PK_AIRBAND_RECORDINGS_VOL_UP_X0,
        PK_AIRBAND_RECORDINGS_VOL_UP_X1, PK_ICON_VOLUME_UP, col_panel(),
        audio_controls_available && speaker.playback_volume < 100,
        feedback == PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP);
    draw_transport(fb, PAGE_PREV_X0, PAGE_PREV_X1, PK_ICON_SKIP_PREVIOUS,
                   controls_available &&
                       pk_airband_recordings_can_previous(s_snapshot.count, selected),
                   false, feedback == PK_AIRBAND_RECORDINGS_ACT_PREVIOUS);
    draw_transport(fb, PAGE_TOGGLE_X0, PAGE_TOGGLE_X1,
                   pb.playing ? PK_ICON_PAUSE : PK_ICON_PLAY_ARROW,
                   controls_available && selected >= 0, pb.playing,
                   feedback == PK_AIRBAND_RECORDINGS_ACT_TOGGLE);
    draw_transport(fb, PAGE_NEXT_X0, PAGE_NEXT_X1, PK_ICON_SKIP_NEXT,
                   controls_available &&
                       pk_airband_recordings_can_next(s_snapshot.count, selected),
                   false, feedback == PK_AIRBAND_RECORDINGS_ACT_NEXT);
    if (s_volume_popup_active &&
        (uint32_t)((uint32_t)xTaskGetTickCount() - s_volume_popup_started) <
            (uint32_t)pdMS_TO_TICKS(1200)) {
        draw_volume_popup(fb, s_volume_popup_value);
    } else {
        s_volume_popup_active = false;
    }
    page_lock_give();
}

bool pk_airband_recordings_page_touch(int x, int y)
{
    if (y < PFD_BAR_BOT || x >= PK_DISPLAY_W - 6) return false;
    int fx, fy, fw, fh;
    if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh) &&
        x >= fx && x < fx + fw && y >= fy && y < fy + fh) return false;
    if (!((y >= PK_AIRBAND_RECORDINGS_LIST_TOP &&
           y < PK_AIRBAND_RECORDINGS_LIST_BOTTOM) ||
          (y >= PAGE_CONTROL_Y0 && y < PAGE_CONTROL_Y1))) return false;
    pk_airband_record_status_t rec = {0};
    pk_airband_record_get(&rec);
    page_lock_take();
    pk_airband_recordings_gesture_press(
        &s_gesture, x, y, s_scroll, rec.active || rec.stopping,
        s_snapshot.count);
    s_feedback_active = false;
    page_lock_give();
    return true;
}

bool pk_airband_recordings_page_drag(int x, int y)
{
    page_lock_take();
    if (!s_gesture.active) {
        page_lock_give();
        return false;
    }
    s_scroll = pk_airband_recordings_gesture_drag(&s_gesture, x, y, s_scroll);
    page_lock_give();
    return true;
}

void pk_airband_recordings_page_touch_up(void)
{
    page_lock_take();
    const bool gesture_active = s_gesture.active;
    page_lock_give();
    if (!gesture_active) return;
    pk_airband_playback_status_t pb = {0};
    (void)pk_airband_playback_get(&pb);
    const bool playback_service_available =
        pb.started &&
        pb.service_state == PK_AIRBAND_PLAYBACK_SERVICE_READY;
    page_lock_take();
    const int selected = selected_index(&pb);
    const pk_airband_recordings_catalog_view_t catalog_view =
        pk_airband_recordings_catalog_view(s_snapshot_result,
                                            s_snapshot.catalog_state);
    const bool controls_available = pk_airband_recordings_controls_available(
        catalog_view, s_snapshot.stale, playback_service_available);
    pk_airband_speaker_status_t speaker = {0};
    const bool speaker_available = pk_airband_speaker_get(&speaker) &&
                                   speaker.online;
    const pk_airband_recordings_action_t a =
        pk_airband_recordings_gesture_release_ex(
            &s_gesture, selected, controls_available,
            controls_available && speaker_available,
            speaker.playback_volume);
    s_feedback = a.kind;
    s_feedback_active = a.kind != PK_AIRBAND_RECORDINGS_ACT_NONE &&
                        a.kind != PK_AIRBAND_RECORDINGS_ACT_SELECT;
    s_feedback_started = (uint32_t)xTaskGetTickCount();
    const uint32_t action_epoch = s_page_epoch;
    int index = -1;
    if (a.kind == PK_AIRBAND_RECORDINGS_ACT_SELECT) index = a.catalog_index;
    else if (a.kind == PK_AIRBAND_RECORDINGS_ACT_PREVIOUS) index = selected - 1;
    else if (a.kind == PK_AIRBAND_RECORDINGS_ACT_NEXT) index = selected + 1;
    pk_airband_recording_id_t target = {0};
    const bool have_target = index >= 0 && (size_t)index < s_snapshot.count;
    if (have_target) target = s_snapshot.entries[index].id;
    page_lock_give();
    if (have_target) {
        if (!s_notice_shown) {
            pk_ui_toast_show(PK_TR_RECORDINGS_PLAYBACK_NOTICE, false);
            s_notice_shown = true;
        }
        /* select() 只装载并停在用户暂停态；显式 toggle 才开始发声。 */
        const pk_airband_playback_select_result_t result =
            pk_airband_playback_select_ex(target);
        if (result == PK_AIRBAND_PLAYBACK_SELECT_OK) {
            (void)pk_airband_recordings_start_after_select(
                true, pk_airband_playback_toggle);
        } else if (result == PK_AIRBAND_PLAYBACK_SELECT_SERVICE_UNAVAILABLE) {
            pk_ui_toast_show(PK_TR_RECORDINGS_PLAYBACK_UNAVAILABLE, true);
        } else if (result == PK_AIRBAND_PLAYBACK_SELECT_SD_UNAVAILABLE) {
            pk_ui_toast_show(PK_TR_RECORDINGS_SD_MISSING, true);
        } else {
            pk_ui_toast_show(PK_TR_RECORDINGS_FILE_CHANGED, true);
        }
    } else if (a.kind == PK_AIRBAND_RECORDINGS_ACT_TOGGLE) {
        pk_airband_playback_toggle();
    } else if (a.kind == PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN ||
               a.kind == PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP ||
               a.kind == PK_AIRBAND_RECORDINGS_ACT_MUTE) {
        if (a.kind == PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN) {
            speaker.playback_volume =
                speaker.playback_volume > 5 ? speaker.playback_volume - 5 : 0;
            pk_airband_speaker_playback_set_volume(speaker.playback_volume);
        } else if (a.kind == PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP) {
            speaker.playback_volume =
                speaker.playback_volume < 95 ? speaker.playback_volume + 5 : 100;
            pk_airband_speaker_playback_set_volume(speaker.playback_volume);
        } else {
            pk_airband_speaker_playback_set_muted(!speaker.playback_muted);
        }
        if (a.kind != PK_AIRBAND_RECORDINGS_ACT_MUTE) {
            page_lock_take();
            if (pk_airband_recordings_snapshot_commit_allowed(
                    s_page_active, action_epoch, s_page_epoch)) {
                s_volume_popup_value = speaker.playback_volume;
                s_volume_popup_started = (uint32_t)xTaskGetTickCount();
                s_volume_popup_active = true;
            }
            page_lock_give();
        }
    }
}
#endif
