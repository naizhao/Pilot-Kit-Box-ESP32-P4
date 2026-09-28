#include "airband_page.h"
#include "airband_frequency_input.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

int pk_airband_nearest_with_fallback(double lat, double lon, void *out, int max,
                                     pk_airband_nearest_query_fn window_query,
                                     pk_airband_nearest_query_fn full_query)
{
    if (out == NULL || max <= 0 || window_query == NULL || full_query == NULL)
        return 0;
    const int n = window_query(lat, lon, out, max);
    return n != 0 ? n : full_query(lat, lon, out, max);
}

bool pk_airband_page_should_retry_loader(bool loader_running,
                                         bool nearby_loaded,
                                         uint32_t now, uint32_t last,
                                         uint32_t interval)
{
    return !loader_running && !nearby_loaded && interval != 0u &&
           (uint32_t)(now - last) >= interval;
}

size_t pk_airband_station_usable_candidate_count(
    const pk_airband_station_candidate_t *in, size_t in_count)
{
    size_t usable = 0;
    for (size_t i = 0; in != NULL && i < in_count; ++i) {
        uint32_t carrier_hz = 0;
        if (!pk_airband_channel_to_carrier_hz(in[i].freq_khz, &carrier_hz))
            continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            uint32_t previous_hz = 0;
            if (pk_airband_channel_to_carrier_hz(in[j].freq_khz,
                                                  &previous_hz) &&
                previous_hz == carrier_hz) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) usable++;
    }
    return usable;
}

bool pk_airband_page_should_query_full_db(
    int window_airport_count,
    const pk_airband_station_candidate_t *candidates,
    size_t candidate_count)
{
    return window_airport_count <= 0 ||
           pk_airband_station_usable_candidate_count(candidates,
                                                      candidate_count) == 0u;
}

bool pk_airband_station_candidate_can_append(
    const pk_airband_station_candidate_t *existing, size_t count,
    size_t capacity, uint32_t channel_khz)
{
    if (count >= capacity) return false;
    uint32_t carrier_hz = 0;
    if (!pk_airband_channel_to_carrier_hz(channel_khz, &carrier_hz))
        return false;
    for (size_t i = 0; existing != NULL && i < count; ++i) {
        uint32_t previous_hz = 0;
        if (pk_airband_channel_to_carrier_hz(existing[i].freq_khz,
                                              &previous_hz) &&
            previous_hz == carrier_hz) {
            return false;
        }
    }
    return true;
}

static bool contains_frequency(const pk_airband_station_t *out, size_t count,
                               uint32_t frequency_hz)
{
    for (size_t i = 0; i < count; ++i) {
        if (out[i].frequency_hz == frequency_hz) return true;
    }
    return false;
}

static void append_station(pk_airband_station_t *out, size_t index,
                           uint32_t channel_khz, uint32_t carrier_hz,
                           uint32_t airport_idx,
                           uint8_t service, const char *label)
{
    out[index].channel_khz = channel_khz;
    out[index].frequency_hz = carrier_hz;
    out[index].airport_idx = airport_idx;
    out[index].service = service;
    if (label == NULL) label = "";
    (void)snprintf(out[index].label, sizeof(out[index].label), "%s", label);
}

bool pk_airband_station_list_append(pk_airband_station_list_t *list,
                                    const pk_airband_station_candidate_t *row)
{
    uint32_t hz;
    if (!list || !row) return false;
    if (!pk_airband_channel_to_carrier_hz(row->freq_khz, &hz) ||
        contains_frequency(list->rows, list->count, hz)) return true;
    if (list->count == list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 16;
        if (cap < list->capacity || cap > SIZE_MAX / sizeof(*list->rows)) return false;
        void *rows = realloc(list->rows, cap * sizeof(*list->rows));
        if (!rows) return false;
        list->rows = rows;
        list->capacity = cap;
    }
    append_station(list->rows, list->count++, row->freq_khz, hz,
                   row->airport_idx, row->service, row->label);
    return true;
}

void pk_airband_station_list_free(pk_airband_station_list_t *list)
{
    free(list->rows);
    *list = (pk_airband_station_list_t){0};
}

bool pk_airband_page_list_open(pk_airband_page_list_t *page)
{
    page->complete=false;
    return page->stations.count==0;
}

pk_airband_station_list_t pk_airband_page_list_publish(pk_airband_page_list_t *page,
    pk_airband_station_list_t *next, bool complete)
{
    pk_airband_station_list_t old=page->stations;
    page->stations=*next;
    *next=(pk_airband_station_list_t){0};
    page->complete=complete;
    page->failed=false;
    return old;
}

void pk_airband_page_list_failed(pk_airband_page_list_t *page)
{
    page->complete=false;
    page->failed=true;
}

void pk_airband_page_list_task_created(pk_airband_page_list_t *page, bool created)
{
    if (!created) pk_airband_page_list_failed(page);
}

size_t pk_airband_station_collect(const pk_airband_station_candidate_t *in,
                                  size_t in_count,
                                  pk_airband_station_t *out,
                                  size_t capacity)
{
    if (out == NULL || capacity == 0) return 0;

    size_t count = 0;
    for (size_t i = 0; in != NULL && i < in_count; ++i) {
        const uint32_t khz = in[i].freq_khz;
        uint32_t hz = 0;
        if (!pk_airband_channel_to_carrier_hz(khz, &hz) ||
            contains_frequency(out, count, hz)) continue;
        if (count == capacity) return SIZE_MAX;
        append_station(out, count++, khz, hz, in[i].airport_idx, in[i].service,
                       in[i].label);
    }

    static const struct {
        uint32_t khz;
        const char *label;
    } fallback[] = {
        {121500u, "GUARD"},
        {123450u, "AIR-AIR"},
    };
    for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); ++i) {
        uint32_t hz = 0;
        if (!pk_airband_channel_to_carrier_hz(fallback[i].khz, &hz) ||
            contains_frequency(out, count, hz)) continue;
        if (count == capacity) return SIZE_MAX;
        append_station(out, count++, fallback[i].khz, hz, UINT32_MAX, 0,
                       fallback[i].label);
    }
    return count;
}

#define PAGE_LIST_TOP 112
#define PAGE_ROW_H 48
#define PAGE_CONTROL_TOP 408
#define PAGE_VISIBLE_ROWS ((PAGE_CONTROL_TOP - PAGE_LIST_TOP) / PAGE_ROW_H)
#define PAGE_MANUAL_X0 16
#define PAGE_MANUAL_X1 190
#define PAGE_MANUAL_Y0 54
#define PAGE_MANUAL_Y1 104
#define PAGE_PAIR_X0 16
#define PAGE_PAIR_X1 190
#define PAGE_RECORD_X0 206
#define PAGE_RECORD_X1 366
#define PAGE_CONTROL_Y0 (PAGE_CONTROL_TOP + 10)
#define PAGE_CONTROL_Y1 470
#define PAGE_CONTROL_ICON_W 30
#define PAGE_CONTROL_LABEL_GAP 8
#define PAGE_VOLUME_DOWN_X0 382
#define PAGE_VOLUME_DOWN_X1 510
#define PAGE_MUTE_X0 526
#define PAGE_MUTE_X1 654
#define PAGE_VOLUME_UP_X0 670
#define PAGE_VOLUME_UP_X1 798
#define PAGE_VOLUME_POPUP_MS 1200u
#define PAGE_VOLUME_FEEDBACK_MS 200u

_Static_assert(PAGE_VOLUME_DOWN_X1 - PAGE_VOLUME_DOWN_X0 ==
                   PAGE_MUTE_X1 - PAGE_MUTE_X0,
               "volume-down and mute controls must be equally wide");
_Static_assert(PAGE_MUTE_X1 - PAGE_MUTE_X0 ==
                   PAGE_VOLUME_UP_X1 - PAGE_VOLUME_UP_X0,
               "mute and volume-up controls must be equally wide");

int pk_airband_scroll_clamp(int offset, size_t count)
{
    size_t hidden = count > PAGE_VISIBLE_ROWS ? count - PAGE_VISIBLE_ROWS : 0;
    int max = hidden > INT_MAX / PAGE_ROW_H ? INT_MAX : (int)hidden * PAGE_ROW_H;
    return offset < 0 ? 0 : offset > max ? max : offset;
}

int pk_airband_scroll_drag(int initial_offset, int delta_y, size_t count)
{
    int64_t next = (int64_t)initial_offset - delta_y;
    return pk_airband_scroll_clamp(next < 0 ? 0 : next > INT_MAX ? INT_MAX : (int)next, count);
}

bool pk_airband_frequency_parse_mhz(const char *text, uint32_t *out_hz)
{
    if (text == NULL || out_hz == NULL || text[0] == '\0') return false;
    pk_airband_frequency_input_t state;
    char normalized[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1];
    pk_airband_frequency_input_init(&state, text);
    return state.replace_armed &&
           pk_airband_frequency_input_commit(&state, normalized, out_hz);
}

bool pk_airband_page_owns_press(int x, int y, bool fab_visible,
                               int fab_x, int fab_y, int fab_w, int fab_h)
{
    if (x < 0 || x >= 800 || y < 0 || y >= 480) return false;
    if (fab_visible && fab_w > 0 && fab_h > 0 &&
        x >= fab_x && x < fab_x + fab_w &&
        y >= fab_y && y < fab_y + fab_h) return false;
    return true;
}

int pk_airband_page_station_row_right(void)
{
    return 784;
}

bool pk_airband_volume_popup_visible(bool active, uint32_t now,
                                     uint32_t started, uint32_t duration)
{
    return active && (uint32_t)(now - started) < duration;
}

pk_airband_record_visual_t pk_airband_record_visual_resolve(
    bool stopping, bool error, bool sd_missing, bool interrupted,
    bool active, bool enabled)
{
    if (stopping)
        return (pk_airband_record_visual_t){PK_AIRBAND_REC_VIS_WRITING,
                                             false, false};
    if (error)
        return (pk_airband_record_visual_t){PK_AIRBAND_REC_VIS_ERROR,
                                             false, true};
    if (sd_missing)
        return (pk_airband_record_visual_t){PK_AIRBAND_REC_VIS_SD_MISSING,
                                             false, true};
    if (active)
        return (pk_airband_record_visual_t){PK_AIRBAND_REC_VIS_ACTIVE,
                                             true, interrupted};
    if (interrupted)
        return (pk_airband_record_visual_t){PK_AIRBAND_REC_VIS_DROPPED,
                                             false, true};
    if (enabled)
        return (pk_airband_record_visual_t){PK_AIRBAND_REC_VIS_ARMED,
                                             true, false};
    return (pk_airband_record_visual_t){PK_AIRBAND_REC_VIS_IDLE,
                                         false, false};
}

int pk_airband_record_content_start(int label_width)
{
    const int content_width = PAGE_CONTROL_ICON_W +
                              PAGE_CONTROL_LABEL_GAP + label_width;
    return PAGE_RECORD_X0 +
           (PAGE_RECORD_X1 - PAGE_RECORD_X0 - content_width) / 2;
}

pk_airband_button_color_role_t pk_airband_mute_button_color_role(bool muted)
{
    return muted ? PK_AIRBAND_BUTTON_COLOR_ORANGE
                 : PK_AIRBAND_BUTTON_COLOR_BLUE;
}

pk_airband_control_icon_role_t pk_airband_control_icon_role(
    pk_airband_page_action_kind_t action, bool muted)
{
    switch (action) {
    case PK_AIRBAND_PAGE_ACT_VOLUME_DOWN:
        return PK_AIRBAND_CONTROL_ICON_VOLUME_DOWN;
    case PK_AIRBAND_PAGE_ACT_MUTE:
        return muted ? PK_AIRBAND_CONTROL_ICON_VOLUME_OFF
                     : PK_AIRBAND_CONTROL_ICON_BRAND_AWARENESS;
    case PK_AIRBAND_PAGE_ACT_VOLUME_UP:
        return PK_AIRBAND_CONTROL_ICON_VOLUME_UP;
    default:
        return PK_AIRBAND_CONTROL_ICON_NONE;
    }
}

pk_icon_id_t pk_airband_control_icon_id(pk_airband_control_icon_role_t role)
{
    switch (role) {
    case PK_AIRBAND_CONTROL_ICON_VOLUME_DOWN:
        return PK_ICON_VOLUME_DOWN;
    case PK_AIRBAND_CONTROL_ICON_BRAND_AWARENESS:
        return PK_ICON_BRAND_AWARENESS;
    case PK_AIRBAND_CONTROL_ICON_VOLUME_OFF:
        return PK_ICON_VOLUME_OFF;
    case PK_AIRBAND_CONTROL_ICON_VOLUME_UP:
        return PK_ICON_VOLUME_UP;
    case PK_AIRBAND_CONTROL_ICON_NONE:
    default:
        return PK_ICON_VOLUME_OFF;
    }
}

pk_airband_page_action_t pk_airband_page_action_at(int x, int y,
                                                   size_t station_count)
{
    return pk_airband_page_action_scrolled(x, y, station_count, 0);
}

pk_airband_page_action_t pk_airband_page_action_scrolled(int x, int y,
                                                        size_t station_count, int offset)
{
    pk_airband_page_action_t action = {PK_AIRBAND_PAGE_ACT_NONE, -1};
    if (x < 0 || x >= 800 || y < 0 || y >= 480) return action;
    if (x >= PAGE_MANUAL_X0 && x < PAGE_MANUAL_X1 &&
        y >= PAGE_MANUAL_Y0 && y < PAGE_MANUAL_Y1) {
        action.kind = PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY;
        return action;
    }
    if (y >= PAGE_LIST_TOP && y < PAGE_LIST_TOP + PAGE_VISIBLE_ROWS * PAGE_ROW_H) {
        const int row = (y - PAGE_LIST_TOP + pk_airband_scroll_clamp(offset, station_count)) / PAGE_ROW_H;
        if (row >= 0 && (size_t)row < station_count) {
            action.kind = PK_AIRBAND_PAGE_ACT_TUNE;
            action.station_index = row;
        }
        return action;
    }
    if (y < PAGE_CONTROL_Y0 || y >= PAGE_CONTROL_Y1) return action;
    if (x >= PAGE_PAIR_X0 && x < PAGE_PAIR_X1)
        action.kind = PK_AIRBAND_PAGE_ACT_AUTHORIZE_PHONE;
    else if (x >= PAGE_MUTE_X0 && x < PAGE_MUTE_X1)
        action.kind = PK_AIRBAND_PAGE_ACT_MUTE;
    else if (x >= PAGE_RECORD_X0 && x < PAGE_RECORD_X1)
        action.kind = PK_AIRBAND_PAGE_ACT_RECORD;
    else if (x >= PAGE_VOLUME_DOWN_X0 && x < PAGE_VOLUME_DOWN_X1)
        action.kind = PK_AIRBAND_PAGE_ACT_VOLUME_DOWN;
    else if (x >= PAGE_VOLUME_UP_X0 && x < PAGE_VOLUME_UP_X1)
        action.kind = PK_AIRBAND_PAGE_ACT_VOLUME_UP;
    return action;
}

void pk_airband_page_gesture_press(pk_airband_page_gesture_t *g, int x, int y, int offset)
{
    *g = (pk_airband_page_gesture_t){.x=x, .y=y, .initial_offset=offset, .active=true};
}

int pk_airband_page_gesture_drag(pk_airband_page_gesture_t *g, int x, int y,
                                 size_t count, int offset)
{
    if (!g->active) return offset;
    const int dx=x-g->x, dy=y-g->y;
    if (dx > 12 || dx < -12 || dy > 12 || dy < -12) g->moved=true;
    if (g->moved && g->y >= PAGE_LIST_TOP &&
        g->y < PAGE_LIST_TOP+PAGE_VISIBLE_ROWS*PAGE_ROW_H) {
        int next=pk_airband_scroll_drag(g->initial_offset,dy,count);
        return (next/PAGE_ROW_H)*PAGE_ROW_H;
    }
    return offset;
}

pk_airband_page_action_t pk_airband_page_gesture_release(pk_airband_page_gesture_t *g,
                                                       size_t count, int offset)
{
    pk_airband_page_action_t a = {PK_AIRBAND_PAGE_ACT_NONE,-1};
    if (g->active && !g->moved) a=pk_airband_page_action_scrolled(g->x,g->y,count,offset);
    g->active=false;
    return a;
}

void pk_airband_page_gesture_cancel_for_list_publish(
    pk_airband_page_gesture_t *gesture)
{
    if (gesture != NULL && gesture->active &&
        gesture->y >= PAGE_LIST_TOP &&
        gesture->y < PAGE_LIST_TOP + PAGE_VISIBLE_ROWS * PAGE_ROW_H) {
        gesture->active = false;
    }
}

pk_airband_page_action_kind_t pk_airband_volume_feedback_action(
    const pk_airband_page_gesture_t *gesture,
    pk_airband_page_action_kind_t latched_action, bool latched_active,
    uint32_t now, uint32_t latched_started, uint32_t duration)
{
    if (gesture != NULL && gesture->active) {
        if (gesture->moved) return PK_AIRBAND_PAGE_ACT_NONE;
        const pk_airband_page_action_kind_t pressed =
            pk_airband_page_action_scrolled(gesture->x, gesture->y, 0, 0).kind;
        return pressed == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN ||
                       pressed == PK_AIRBAND_PAGE_ACT_VOLUME_UP
                   ? pressed
                   : PK_AIRBAND_PAGE_ACT_NONE;
    }
    if (!pk_airband_volume_popup_visible(latched_active, now,
                                         latched_started, duration))
        return PK_AIRBAND_PAGE_ACT_NONE;
    return latched_action == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN ||
                   latched_action == PK_AIRBAND_PAGE_ACT_VOLUME_UP
               ? latched_action
               : PK_AIRBAND_PAGE_ACT_NONE;
}

void pk_airband_volume_feedback_cancel_latch(
    pk_airband_page_action_kind_t *latched_action, bool *latched_active)
{
    if (latched_action != NULL) *latched_action = PK_AIRBAND_PAGE_ACT_NONE;
    if (latched_active != NULL) *latched_active = false;
}

#ifndef PK_AIRBAND_PAGE_HOST_TEST
#include <stdatomic.h>

#ifndef PK_SIM_BUILD
#include "esp_log.h"
#else
#define ESP_LOGI(tag, format, ...) fprintf(stderr, "%s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
#define xTaskGetTickCount() ((uint32_t)(esp_timer_get_time()/1000))
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "airband.h"
#include "display.h"
#include "gps.h"
#include "i18n.h"
#include "keyboard_page.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "pk_aero_db.h"
#include "pk_win.h"
#include "pk_ui_nav.h"
#include "ui_state.h"
#include "pfd_statusbar.h"
#include "airband_record.h"
#include "ble_gatt.h"
#include "aircraft_state.h"
#include "esp_timer.h"

_Static_assert(PK_ICON_W == PAGE_CONTROL_ICON_W,
               "record layout icon width must match generated icon font");

static const char *TAG = "airband_page";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pk_airband_page_list_t s_page_list;
static int s_scroll;
static atomic_bool s_loader_running;
static atomic_uint_fast32_t s_last_loader_tick;
static pk_airband_page_gesture_t s_gesture;
static bool s_volume_popup_active;
static uint32_t s_volume_popup_started;
static uint8_t s_volume_popup_value;
static bool s_volume_feedback_active;
static uint32_t s_volume_feedback_started;
static pk_airband_page_action_kind_t s_volume_feedback_action;

static uint16_t col_bg(void)    { return pk_rgb565(10, 15, 28); }
static uint16_t col_panel(void) { return pk_rgb565(28, 39, 64); }
static uint16_t col_text(void)  { return pk_rgb565(226, 236, 248); }
static uint16_t col_sub(void)   { return pk_rgb565(147, 168, 196); }
static uint16_t col_blue(void)  { return pk_rgb565(46, 109, 240); }
static uint16_t col_green(void) { return pk_rgb565(47, 178, 98); }
static uint16_t col_amber(void) { return pk_rgb565(255, 180, 64); }
static uint16_t col_red(void)   { return pk_rgb565(220, 40, 40); }

static void publish_stations(pk_airband_station_list_t *list, bool complete)
{
    portENTER_CRITICAL(&s_lock);
    pk_airband_station_list_t old = pk_airband_page_list_publish(&s_page_list,list,complete);
    s_scroll = pk_airband_scroll_clamp(s_scroll, s_page_list.stations.count);
    /* Only a row tap depends on list identity. Fixed controls are unaffected. */
    pk_airband_page_gesture_cancel_for_list_publish(&s_gesture);
    portEXIT_CRITICAL(&s_lock);
    pk_airband_station_list_free(&old);
}

static void mark_list_failed(void)
{
    portENTER_CRITICAL(&s_lock);
    pk_airband_page_list_failed(&s_page_list);
    portEXIT_CRITICAL(&s_lock);
}

static bool append_fallbacks(pk_airband_station_list_t *list)
{
    const pk_airband_station_candidate_t guard = {121500u, UINT32_MAX, 0, "GUARD"};
    const pk_airband_station_candidate_t air = {123450u, UINT32_MAX, 0, "AIR-AIR"};
    return pk_airband_station_list_append(list, &guard) &&
           pk_airband_station_list_append(list, &air);
}

static void load_fallbacks(void)
{
    pk_airband_station_list_t list = {0};
    if (append_fallbacks(&list)) publish_stations(&list,false);
    else {
        mark_list_failed();
        ESP_LOGE(TAG, "cannot allocate preset stations; previous list retained");
    }
    pk_airband_station_list_free(&list);
}

static bool append_nearby_candidates(
    const pk_aero_near_t *nearby, size_t nearby_count,
    pk_airband_station_list_t *list)
{
    for (size_t i = 0; i < nearby_count; ++i) {
        pk_aero_airport_t airport;
        uint32_t first = 0, count = 0;
        if (!pk_aero_db_airport_get(nearby[i].idx, &airport) ||
            !pk_aero_db_airport_freqs(nearby[i].idx, &first, &count)) {
            return false;
        }
        char icao[sizeof(airport.icao)];
        memcpy(icao, airport.icao, sizeof(icao));
        for (uint32_t j = 0; j < count; ++j) {
            pk_aero_freq_t freq;
            char callsign[PK_AIRBAND_STATION_LABEL_MAX];
            if (!pk_aero_db_freq_get_copy(first + j, &freq, callsign, sizeof(callsign))) return false;
            const pk_airband_station_candidate_t row = {
                .freq_khz = freq.freq_khz,
                .airport_idx = nearby[i].idx,
                .service = freq.service,
                .label = freq.callsign && freq.callsign[0] ? freq.callsign : icao,
            };
            if (!pk_airband_station_list_append(list, &row)) return false;
        }
    }
    return true;
}

static void load_nearby_task(void *arg)
{
    (void)arg;
    pk_airband_station_list_t list = {0};
    pk_gps_state_t gps = {0};
    pk_aero_near_t *nearby = NULL;
    size_t nearby_count = 0;
    const uint32_t generation = pk_aero_db_generation();
    const bool ready = pk_gps_get(&gps) && gps.have_fix &&
        pk_aero_db_airports_with_freqs_nearby_all(gps.lat, gps.lon, &nearby, &nearby_count);
    bool ok = ready && append_nearby_candidates(nearby, nearby_count, &list);
    ok = ok && append_fallbacks(&list) && generation == pk_aero_db_generation();
    if (ok) {
        const size_t station_count=list.count;
        publish_stations(&list,true);
        ESP_LOGI(TAG, "complete station list: airports=%u stations=%u",
                 (unsigned)nearby_count, (unsigned)station_count);
    } else if (gps.have_fix && pk_aero_db_state() == PK_AERO_DB_READY) {
        mark_list_failed();
        ESP_LOGE(TAG, "station load failed (allocation/read/database changed); previous list retained");
    }
    free(nearby);
    pk_airband_station_list_free(&list);
    atomic_store(&s_loader_running, false);
#ifndef PK_SIM_BUILD
    vTaskDelete(NULL);
#endif
}

static void start_nearby_loader(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_loader_running, &expected, true))
        return;
    atomic_store(&s_last_loader_tick, (uint32_t)xTaskGetTickCount());
#ifdef PK_SIM_BUILD
    load_nearby_task(NULL);
#else
    if (xTaskCreate(load_nearby_task, "airband_stations", 4096, NULL, 2, NULL) !=
        pdPASS) {
        portENTER_CRITICAL(&s_lock);
        pk_airband_page_list_task_created(&s_page_list,false);
        portEXIT_CRITICAL(&s_lock);
        atomic_store(&s_loader_running, false);
        ESP_LOGE(TAG, "cannot start station loader");
    }
#endif
}

void pk_airband_page_init(void)
{
    load_fallbacks();
    atomic_store(&s_loader_running, false);
    atomic_store(&s_last_loader_tick, 0u);
    s_volume_popup_active = false;
    s_volume_popup_started = 0;
    s_volume_popup_value = 0;
    s_volume_feedback_active = false;
    s_volume_feedback_started = 0;
    s_volume_feedback_action = PK_AIRBAND_PAGE_ACT_NONE;
}

void pk_airband_page_open(void)
{
    s_volume_popup_active = false;
    s_volume_feedback_active = false;
    s_volume_feedback_action = PK_AIRBAND_PAGE_ACT_NONE;
    portENTER_CRITICAL(&s_lock);
    const bool need_presets=pk_airband_page_list_open(&s_page_list);
    portEXIT_CRITICAL(&s_lock);
    if (need_presets) load_fallbacks();
    start_nearby_loader();
}

static void draw_button_at(uint16_t *fb, int x0, int y0, int x1, int y1,
                           const char *label, bool active)
{
    pk_pfd_fill_round_rect(fb, x0, y0, x1, y1, 10,
                           active ? col_blue() : col_panel());
    const int w = pk_aa_text_width(label, PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + (x1 - x0 - w) / 2,
               y0 + (y1 - y0 - PK_AA_S_H) / 2,
               label, col_text(), PK_AA_S);
}

static const uint8_t *control_icon(pk_icon_id_t id)
{
    return pk_icon_bitmap + (size_t)id * ((PK_ICON_W * PK_ICON_H) / 2);
}

static void fill_circle(uint16_t *fb, int cx, int cy, int radius, uint16_t color)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        int half = radius;
        while (half * half + dy * dy > radius * radius) --half;
        pk_pfd_fill_rect(fb, cx - half, cy + dy,
                        cx + half + 1, cy + dy + 1, color);
    }
}

static void draw_control_icon(uint16_t *fb, int x, int y,
                              pk_icon_id_t icon, uint16_t color)
{
    pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H, x, y,
                    control_icon(icon), PK_ICON_W, PK_ICON_H, color);
}

static void draw_icon_label_button(uint16_t *fb, int x0, int x1,
                                   pk_icon_id_t icon, const char *label,
                                   bool active)
{
    const int y0 = PAGE_CONTROL_TOP + 10;
    pk_pfd_fill_round_rect(fb, x0, y0, x1, 470, 10,
                           active ? col_blue() : col_panel());
    const int gap = 8;
    const int label_w = pk_aa_text_width(label, PK_AA_S);
    const int content_w = PK_ICON_W + gap + label_w;
    const int icon_x = x0 + (x1 - x0 - content_w) / 2;
    draw_control_icon(fb, icon_x, y0 + (52 - PK_ICON_H) / 2,
                      icon, col_text());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               icon_x + PK_ICON_W + gap,
               y0 + (52 - PK_AA_S_H) / 2,
               label, col_text(), PK_AA_S);
}

static void draw_mute_button(uint16_t *fb, bool muted)
{
    const int x0 = PAGE_MUTE_X0, x1 = PAGE_MUTE_X1;
    const pk_airband_button_color_role_t color_role =
        pk_airband_mute_button_color_role(muted);
    pk_pfd_fill_round_rect(fb, x0, PAGE_CONTROL_TOP + 10, x1, 470, 10,
                           color_role == PK_AIRBAND_BUTTON_COLOR_BLUE
                               ? col_blue() : col_amber());
    draw_control_icon(fb, (x0 + x1 - PK_ICON_W) / 2,
                      PAGE_CONTROL_TOP + 10 + (52 - PK_ICON_H) / 2,
                      pk_airband_control_icon_id(pk_airband_control_icon_role(
                          PK_AIRBAND_PAGE_ACT_MUTE, muted)),
                      col_text());
}

static pk_tr_id_t record_status_id(pk_airband_record_visual_label_t label)
{
    switch (label) {
    case PK_AIRBAND_REC_VIS_WRITING: return PK_TR_AIRBAND_REC_WRITING;
    case PK_AIRBAND_REC_VIS_ERROR: return PK_TR_AIRBAND_REC_ERROR;
    case PK_AIRBAND_REC_VIS_SD_MISSING: return PK_TR_AIRBAND_REC_SD_MISSING;
    case PK_AIRBAND_REC_VIS_DROPPED: return PK_TR_AIRBAND_REC_DROPPED;
    case PK_AIRBAND_REC_VIS_ACTIVE: return PK_TR_AIRBAND_REC_ACTIVE;
    case PK_AIRBAND_REC_VIS_ARMED: return PK_TR_AIRBAND_REC_ARMED;
    case PK_AIRBAND_REC_VIS_IDLE:
    default: return PK_TR_AIRBAND_REC_IDLE;
    }
}

static void draw_record_button(uint16_t *fb,
                               const pk_airband_record_status_t *rec)
{
    const pk_airband_record_visual_t visual =
        pk_airband_record_visual_resolve(rec->stopping, rec->error,
                                         rec->sd_missing, rec->interrupted,
                                         rec->active, rec->enabled);
    const uint16_t bg = visual.red ? col_red()
                                   : visual.warning ? col_amber() : col_panel();
    pk_pfd_fill_round_rect(fb, PAGE_RECORD_X0, PAGE_CONTROL_TOP + 10,
                           PAGE_RECORD_X1, 470, 10, bg);
    const char *label = pk_i18n_text(record_status_id(visual.label));
    const int label_w = pk_aa_text_width(label, PK_AA_XS);
    const int icon_x = pk_airband_record_content_start(label_w);
    draw_control_icon(fb, icon_x,
                      PAGE_CONTROL_TOP + 10 + (52 - PK_ICON_H) / 2,
                      rec->enabled ? PK_ICON_PAUSE : PK_ICON_REC,
                      rec->enabled ? col_text() : col_red());
    if (visual.red && visual.warning)
        fill_circle(fb, PAGE_RECORD_X1 - 10, PAGE_CONTROL_TOP + 20,
                    4, col_amber());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               icon_x + PK_ICON_W + PAGE_CONTROL_LABEL_GAP,
               PAGE_CONTROL_TOP + 10 + (52 - PK_AA_XS_H) / 2,
               label, col_text(), PK_AA_XS);
}

static void draw_icon_button(uint16_t *fb, int x0, int x1,
                             pk_icon_id_t icon, bool active)
{
    pk_pfd_fill_round_rect(fb, x0, PAGE_CONTROL_TOP + 10, x1, 470, 10,
                           active ? col_blue() : col_panel());
    draw_control_icon(fb, (x0 + x1 - PK_ICON_W) / 2,
                      PAGE_CONTROL_TOP + 10 + (52 - PK_ICON_H) / 2,
                      icon, col_text());
}

static void draw_volume_popup(uint16_t *fb, uint8_t volume)
{
    char label[32];
    (void)snprintf(label, sizeof(label),
                   pk_i18n_text(PK_TR_AIRBAND_VOLUME_FORMAT),
                   (unsigned)volume);
    const int w = pk_aa_text_width(label, PK_AA_L) + 56;
    const int x0 = (PK_DISPLAY_W - w) / 2;
    pk_pfd_fill_round_rect(fb, x0, 190, x0 + w, 262, 14,
                           pk_rgb565(18, 26, 44));
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + (w - pk_aa_text_width(label, PK_AA_L)) / 2,
               190 + (72 - PK_AA_L_H) / 2,
               label, col_text(), PK_AA_L);
}

static void manual_frequency_commit(const char *text)
{
    uint32_t channel_khz = 0;
    uint32_t hz = 0;
    if (!pk_airband_channel_text_parse(text, &channel_khz, &hz) ||
        !pk_airband_set_channel(channel_khz)) {
        ESP_LOGW(TAG, "manual frequency rejected: '%s'", text ? text : "");
        pk_ui_toast_show(PK_TR_AIRBAND_FREQ_INVALID, true);
        return;
    }
    ESP_LOGI(TAG, "manual frequency: %s MHz -> %lu Hz tuned", text,
             (unsigned long)hz);
}

static void open_manual_frequency(uint32_t current_channel_khz,
                                  uint32_t current_hz)
{
    static char initial[12];
    if (current_channel_khz != 0) {
        (void)snprintf(initial, sizeof(initial), "%lu.%03lu",
                       (unsigned long)(current_channel_khz / 1000u),
                       (unsigned long)(current_channel_khz % 1000u));
    } else {
        (void)snprintf(initial, sizeof(initial), "%lu.%03lu",
                       (unsigned long)(current_hz / 1000000u),
                       (unsigned long)((current_hz % 1000000u) / 1000u));
    }
    pk_keyboard_page_open_numeric(pk_i18n_text(PK_TR_AIRBAND_FREQ_INPUT),
                                  initial, 7, manual_frequency_commit, NULL);
}

void pk_airband_page_render(uint16_t *fb)
{
    const uint32_t now = (uint32_t)xTaskGetTickCount();
    portENTER_CRITICAL(&s_lock);
    const bool nearby_loaded=s_page_list.complete;
    const bool list_failed=s_page_list.failed;
    portEXIT_CRITICAL(&s_lock);
    if (pk_airband_page_should_retry_loader(
            atomic_load(&s_loader_running), nearby_loaded,
            now, (uint32_t)atomic_load(&s_last_loader_tick),
            (uint32_t)pdMS_TO_TICKS(2000))) {
        start_nearby_loader();
    }

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L, PK_UI_TITLE_Y,
               pk_i18n_text(PK_TR_APTD_AIRBAND), PK_UI_TITLE_COL,
               PK_UI_TITLE_SIZE);
    static aircraft_t aircraft[AIRCRAFT_TABLE_CAPACITY];
    pk_pfd_status_t topbar = {
        .aircraft_count = aircraft_state_snapshot(aircraft, AIRCRAFT_TABLE_CAPACITY,
                                                  esp_timer_get_time(), AIRCRAFT_STALE_AGE_US),
    };
    pk_ui_topbar_status_collect(&topbar);
    pk_ui_topbar_status_render(fb, &topbar);

    pk_airband_state_t state = {0};
    pk_airband_control_status_t ctl = {0};
    const bool have_state = pk_airband_get(&state);
    const bool have_ctl = pk_airband_control_get(&ctl);
    const uint32_t hz = have_ctl ? ctl.requested_hz : state.frequency_hz;
    const uint32_t display_khz = have_ctl && ctl.requested_channel_khz != 0
                                     ? ctl.requested_channel_khz
                                     : hz / 1000u;
    char line[96];
    draw_button_at(fb, PAGE_MANUAL_X0, PAGE_MANUAL_Y0, PAGE_MANUAL_X1,
                   PAGE_MANUAL_Y1,
                   pk_i18n_text(PK_TR_AIRBAND_FREQ_INPUT), false);

    (void)snprintf(line, sizeof(line), "%lu.%03lu MHz  RSSI %.1f",
                   (unsigned long)(display_khz / 1000u),
                   (unsigned long)(display_khz % 1000u),
                   have_state && state.online ? state.rf.rssi_dbm_x2 / 2.0 : 0.0);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 210, 66, line,
               col_text(), PK_AA_M);

    const char *status = list_failed ? pk_i18n_text(PK_TR_AIRBAND_LIST_ERROR)
                       : !have_state || !state.online
                           ? pk_i18n_text(PK_TR_AIRBAND_RF_OFFLINE)
                       : !state.speaker_online
                             ? pk_i18n_text(PK_TR_APTD_SPEAKER_OFFLINE)
                       : have_ctl && ctl.speaker_muted
                             ? pk_i18n_text(PK_TR_APTD_MUTED)
                       : state.squelch_enabled && !state.squelch_open
                             ? pk_i18n_text(PK_TR_APTD_SQUELCH_CLOSED)
                             : pk_i18n_text(PK_TR_APTD_PLAYING);
    const int status_x = 784 - pk_aa_text_width(status, PK_AA_XS);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, status_x, 70, status,
               !list_failed && have_state && state.online && state.speaker_online
                   ? col_green() : col_amber(), PK_AA_XS);

    pk_airband_station_t stations[PAGE_VISIBLE_ROWS];
    pk_airband_page_gesture_t gesture;
    size_t count, total, first;
    portENTER_CRITICAL(&s_lock);
    gesture = s_gesture;
    total = s_page_list.stations.count;
    first = (size_t)s_scroll / PAGE_ROW_H;
    count = total - first;
    if (count > PAGE_VISIBLE_ROWS) count = PAGE_VISIBLE_ROWS;
    if (count) memcpy(stations, s_page_list.stations.rows + first, count * sizeof(*stations));
    portEXIT_CRITICAL(&s_lock);
    for (size_t i = 0; i < count; ++i) {
        const int y = PAGE_LIST_TOP + (int)i * PAGE_ROW_H;
        const int right = pk_airband_page_station_row_right();
        const bool selected = hz == stations[i].frequency_hz;
        pk_pfd_fill_round_rect(fb, 16, y + 2, right, y + PAGE_ROW_H - 3, 8,
                               selected ? col_blue() : col_panel());
        const char *station_label = stations[i].label;
        if (stations[i].airport_idx == UINT32_MAX) {
            if (stations[i].channel_khz == 121500u)
                station_label = pk_i18n_text(PK_TR_AIRBAND_PRESET_GUARD);
            else if (stations[i].channel_khz == 123450u)
                station_label = pk_i18n_text(PK_TR_AIRBAND_PRESET_AIR_AIR);
        }
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 32, y + 14,
                   station_label, col_text(), PK_AA_S);
        char freq[20];
        (void)snprintf(freq, sizeof(freq), "%lu.%03lu",
                       (unsigned long)(stations[i].channel_khz / 1000u),
                       (unsigned long)(stations[i].channel_khz % 1000u));
        const int fw = pk_aa_text_width(freq, PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, right - 20 - fw, y + 10,
                   freq, col_text(), PK_AA_M);
    }
    if (total > PAGE_VISIBLE_ROWS) {
        const int height = PAGE_VISIBLE_ROWS * PAGE_ROW_H;
        int thumb = (int)((size_t)height * PAGE_VISIBLE_ROWS / total);
        if (thumb < 12) thumb = 12;
        const int top = PAGE_LIST_TOP + (int)((size_t)(height - thumb) * first /
                                             (total - PAGE_VISIBLE_ROWS));
        pk_pfd_fill_rect(fb, 788, PAGE_LIST_TOP, 793, PAGE_LIST_TOP + height, col_panel());
        pk_pfd_fill_round_rect(fb, 788, top, 793, top + thumb, 2, col_sub());
    }

    const uint32_t owner_window_s = ble_gatt_airband_owner_window_remaining_s();
    char pair_label[24];
    if (owner_window_s) {
        (void)snprintf(pair_label, sizeof(pair_label), "%s %lu",
                       pk_i18n_text(PK_TR_AIRBAND_PAIR),
                       (unsigned long)owner_window_s);
    } else {
        (void)snprintf(pair_label, sizeof(pair_label), "%s",
                       pk_i18n_text(PK_TR_AIRBAND_PAIR));
    }
    draw_icon_label_button(fb, PAGE_PAIR_X0, PAGE_PAIR_X1, PK_ICON_BLE,
                           pair_label, owner_window_s != 0);
    pk_airband_record_status_t rec;
    pk_airband_record_get(&rec);
    draw_record_button(fb, &rec);
    const pk_airband_page_action_kind_t volume_feedback =
        pk_airband_volume_feedback_action(
            &gesture, s_volume_feedback_action, s_volume_feedback_active,
            now, s_volume_feedback_started,
            (uint32_t)pdMS_TO_TICKS(PAGE_VOLUME_FEEDBACK_MS));
    if (!gesture.active && volume_feedback == PK_AIRBAND_PAGE_ACT_NONE)
        s_volume_feedback_active = false;
    draw_icon_button(fb, PAGE_VOLUME_DOWN_X0,
                     PAGE_VOLUME_DOWN_X1,
                     pk_airband_control_icon_id(pk_airband_control_icon_role(
                         PK_AIRBAND_PAGE_ACT_VOLUME_DOWN, false)),
                     volume_feedback == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN);
    draw_mute_button(fb, have_ctl && ctl.speaker_muted);
    draw_icon_button(fb, PAGE_VOLUME_UP_X0,
                     PAGE_VOLUME_UP_X1,
                     pk_airband_control_icon_id(pk_airband_control_icon_role(
                         PK_AIRBAND_PAGE_ACT_VOLUME_UP, false)),
                     volume_feedback == PK_AIRBAND_PAGE_ACT_VOLUME_UP);
    const uint32_t popup_duration = (uint32_t)pdMS_TO_TICKS(PAGE_VOLUME_POPUP_MS);
    if (pk_airband_volume_popup_visible(s_volume_popup_active, now,
                                        s_volume_popup_started,
                                        popup_duration)) {
        draw_volume_popup(fb, s_volume_popup_value);
    } else {
        s_volume_popup_active = false;
    }
}

bool pk_airband_page_touch(int x, int y)
{
    if (y < PFD_BAR_BOT || x >= PK_DISPLAY_W - 8) return false;
    int fx = 0, fy = 0, fw = 0, fh = 0;
    const bool fab_visible = pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh);
    if (!pk_airband_page_owns_press(x, y, fab_visible, fx, fy, fw, fh))
        return false;
    portENTER_CRITICAL(&s_lock);
    pk_airband_page_gesture_press(&s_gesture,x,y,s_scroll);
    pk_airband_volume_feedback_cancel_latch(&s_volume_feedback_action,
                                            &s_volume_feedback_active);
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool pk_airband_page_drag(int x, int y)
{
    portENTER_CRITICAL(&s_lock);
    const bool active=s_gesture.active;
    s_scroll=pk_airband_page_gesture_drag(&s_gesture,x,y,s_page_list.stations.count,s_scroll);
    portEXIT_CRITICAL(&s_lock);
    return active;
}

void pk_airband_page_touch_up(void)
{
    pk_airband_station_t station = {0};
    portENTER_CRITICAL(&s_lock);
    const pk_airband_page_action_t action =
        pk_airband_page_gesture_release(&s_gesture,s_page_list.stations.count,s_scroll);
    if (action.kind == PK_AIRBAND_PAGE_ACT_TUNE)
        station=s_page_list.stations.rows[action.station_index];
    portEXIT_CRITICAL(&s_lock);
    if (action.kind == PK_AIRBAND_PAGE_ACT_NONE) return;
    if (action.kind == PK_AIRBAND_PAGE_ACT_RECORD) {
        pk_airband_record_status_t rec;
        pk_airband_record_get(&rec);
        (void)pk_airband_record_set_enabled(!rec.enabled);
        return;
    }
    if (action.kind == PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY) {
        pk_airband_control_status_t ctl = {0};
        pk_airband_state_t state = {0};
        const bool have_ctl = pk_airband_control_get(&ctl);
        const bool have_state = pk_airband_get(&state);
        const uint32_t hz = have_ctl ? ctl.requested_hz
                            : have_state ? state.frequency_hz : 121500000u;
        open_manual_frequency(have_ctl ? ctl.requested_channel_khz : 0, hz);
        return;
    }
    if (action.kind == PK_AIRBAND_PAGE_ACT_AUTHORIZE_PHONE) {
        const bool active = ble_gatt_airband_owner_window_toggle();
        ESP_LOGI(TAG, "local AIRBAND phone owner enrollment %s",
                 active ? "authorized" : "cancelled");
        return;
    }
    if (action.kind == PK_AIRBAND_PAGE_ACT_TUNE) {
        const bool ok = pk_airband_set_channel(station.channel_khz);
        ESP_LOGI(TAG, "station tap: %s %lu Hz -> %s", station.label,
                     (unsigned long)station.frequency_hz,
                     ok ? "tuned" : "rejected");
        return;
    }
    pk_airband_control_status_t ctl = {0};
    uint32_t volume_feedback_started = 0;
    if (action.kind == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN ||
        action.kind == PK_AIRBAND_PAGE_ACT_VOLUME_UP) {
        volume_feedback_started = (uint32_t)xTaskGetTickCount();
        s_volume_feedback_action = action.kind;
        s_volume_feedback_started = volume_feedback_started;
        s_volume_feedback_active = true;
    }
    if (!pk_airband_control_get(&ctl)) return;
    if (action.kind == PK_AIRBAND_PAGE_ACT_MUTE) {
        pk_airband_set_speaker_muted(!ctl.speaker_muted);
    } else if (action.kind == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN) {
        const uint8_t volume = ctl.speaker_volume > 5
                                   ? ctl.speaker_volume - 5 : 0;
        pk_airband_set_speaker_volume(volume);
        s_volume_popup_value = volume;
        s_volume_popup_started = volume_feedback_started;
        s_volume_popup_active = true;
    } else if (action.kind == PK_AIRBAND_PAGE_ACT_VOLUME_UP) {
        const uint8_t volume = ctl.speaker_volume < 95
                                   ? ctl.speaker_volume + 5 : 100;
        pk_airband_set_speaker_volume(volume);
        s_volume_popup_value = volume;
        s_volume_popup_started = volume_feedback_started;
        s_volume_popup_active = true;
    }
}
#endif
