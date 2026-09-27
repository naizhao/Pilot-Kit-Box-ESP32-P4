#include "airband_page.h"
#include "airband_frequency_input.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

size_t pk_airband_station_collect(const pk_airband_station_candidate_t *in,
                                  size_t in_count,
                                  pk_airband_station_t *out,
                                  size_t capacity)
{
    if (out == NULL || capacity == 0) return 0;

    size_t count = 0;
    for (size_t i = 0; in != NULL && i < in_count && count < capacity; ++i) {
        const uint32_t khz = in[i].freq_khz;
        uint32_t hz = 0;
        if (!pk_airband_channel_to_carrier_hz(khz, &hz) ||
            contains_frequency(out, count, hz)) continue;
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
    for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]) &&
                       count < capacity; ++i) {
        uint32_t hz = 0;
        if (!pk_airband_channel_to_carrier_hz(fallback[i].khz, &hz) ||
            contains_frequency(out, count, hz)) continue;
        append_station(out, count++, fallback[i].khz, hz, UINT32_MAX, 0,
                       fallback[i].label);
    }
    return count;
}

#define PAGE_LIST_TOP 112
#define PAGE_ROW_H 48
#define PAGE_CONTROL_TOP 408
#define PAGE_MANUAL_X0 16
#define PAGE_MANUAL_X1 190
#define PAGE_MANUAL_Y0 54
#define PAGE_MANUAL_Y1 104

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

pk_airband_page_action_t pk_airband_page_action_at(int x, int y,
                                                   size_t station_count)
{
    pk_airband_page_action_t action = {PK_AIRBAND_PAGE_ACT_NONE, -1};
    if (x < 0 || x >= 800 || y < 0 || y >= 480) return action;
    if (x >= PAGE_MANUAL_X0 && x < PAGE_MANUAL_X1 &&
        y >= PAGE_MANUAL_Y0 && y < PAGE_MANUAL_Y1) {
        action.kind = PK_AIRBAND_PAGE_ACT_MANUAL_FREQUENCY;
        return action;
    }
    if (y >= PAGE_LIST_TOP && y < PAGE_CONTROL_TOP) {
        const int row = (y - PAGE_LIST_TOP) / PAGE_ROW_H;
        if (row >= 0 && (size_t)row < station_count) {
            action.kind = PK_AIRBAND_PAGE_ACT_TUNE;
            action.station_index = row;
        }
        return action;
    }
    if (y < PAGE_CONTROL_TOP) return action;
    if (x >= 16 && x < 190) action.kind = PK_AIRBAND_PAGE_ACT_SPEAKER_TEST;
    else if (x >= 206 && x < 350) action.kind = PK_AIRBAND_PAGE_ACT_MUTE;
    else if (x >= 560 && x < 632) action.kind = PK_AIRBAND_PAGE_ACT_VOLUME_DOWN;
    else if (x >= 712 && x < 784) action.kind = PK_AIRBAND_PAGE_ACT_VOLUME_UP;
    return action;
}

#ifndef PK_AIRBAND_PAGE_HOST_TEST
#include <stdatomic.h>

#include "esp_log.h"
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

#define PAGE_MAX_STATIONS 6
#define PAGE_NEAR_AIRPORTS 16
#define PAGE_MAX_CANDIDATES 24

static const char *TAG = "airband_page";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pk_airband_station_t s_stations[PAGE_MAX_STATIONS];
static size_t s_station_count;
static atomic_bool s_loader_running;
static atomic_bool s_nearby_loaded;
static atomic_uint_fast32_t s_last_loader_tick;
static int s_press_x, s_press_y;
static bool s_press_valid, s_moved;

static uint16_t col_bg(void)    { return pk_rgb565(10, 15, 28); }
static uint16_t col_panel(void) { return pk_rgb565(28, 39, 64); }
static uint16_t col_text(void)  { return pk_rgb565(226, 236, 248); }
static uint16_t col_sub(void)   { return pk_rgb565(147, 168, 196); }
static uint16_t col_blue(void)  { return pk_rgb565(46, 109, 240); }
static uint16_t col_green(void) { return pk_rgb565(47, 178, 98); }
static uint16_t col_amber(void) { return pk_rgb565(255, 180, 64); }

static void publish_stations(const pk_airband_station_t *stations, size_t count)
{
    if (count > PAGE_MAX_STATIONS) count = PAGE_MAX_STATIONS;
    portENTER_CRITICAL(&s_lock);
    memcpy(s_stations, stations, count * sizeof(*stations));
    s_station_count = count;
    portEXIT_CRITICAL(&s_lock);
}

static void load_fallbacks(void)
{
    pk_airband_station_t stations[PAGE_MAX_STATIONS] = {0};
    const size_t count = pk_airband_station_collect(
        NULL, 0, stations, PAGE_MAX_STATIONS);
    publish_stations(stations, count);
}

static int nearest_from_window(double lat, double lon, void *out, int max)
{
    return pk_win_nearest(PK_AERO_SEC_AIRPORTS, lat, lon,
                          (pk_aero_near_t *)out, max);
}

static int nearest_from_full_db(double lat, double lon, void *out, int max)
{
    return pk_aero_db_nearest_airports_with_freqs(
        lat, lon, (pk_aero_near_t *)out, max);
}

static void append_nearby_candidates(
    const pk_aero_near_t *nearby, int nearby_count,
    pk_airband_station_candidate_t *candidates,
    char labels[][PK_AIRBAND_STATION_LABEL_MAX], size_t *candidate_count)
{
    for (int i = 0; i < nearby_count &&
                    *candidate_count < PAGE_MAX_CANDIDATES; ++i) {
        pk_aero_airport_t airport;
        uint32_t first = 0, count = 0;
        if (!pk_aero_db_airport_get(nearby[i].idx, &airport) ||
            !pk_aero_db_airport_freqs(nearby[i].idx, &first, &count)) {
            continue;
        }
        for (uint32_t j = 0; j < count &&
                             *candidate_count < PAGE_MAX_CANDIDATES; ++j) {
            pk_aero_freq_t freq;
            if (!pk_aero_db_freq_get(first + j, &freq)) continue;
            if (!pk_airband_station_candidate_can_append(
                    candidates, *candidate_count, PAGE_MAX_CANDIDATES,
                    freq.freq_khz)) {
                continue;
            }
            const size_t n = *candidate_count;
            if (freq.callsign != NULL && freq.callsign[0] != '\0') {
                (void)snprintf(labels[n], sizeof(labels[n]), "%s",
                               freq.callsign);
            } else {
                (void)snprintf(labels[n], sizeof(labels[n]), "%s",
                               airport.icao);
            }
            candidates[n] = (pk_airband_station_candidate_t){
                .freq_khz = freq.freq_khz,
                .airport_idx = nearby[i].idx,
                .service = freq.service,
                .label = labels[n],
            };
            (*candidate_count)++;
        }
    }
}

static void load_nearby_task(void *arg)
{
    (void)arg;
    pk_airband_station_candidate_t candidates[PAGE_MAX_CANDIDATES] = {0};
    char labels[PAGE_MAX_CANDIDATES][PK_AIRBAND_STATION_LABEL_MAX] = {{0}};
    size_t candidate_count = 0;
    pk_gps_state_t gps = {0};

    if (pk_gps_get(&gps) && gps.have_fix) {
        pk_aero_near_t nearby[PAGE_NEAR_AIRPORTS];
        const int window_count = nearest_from_window(
            gps.lat, gps.lon, nearby, PAGE_NEAR_AIRPORTS);
        append_nearby_candidates(nearby, window_count, candidates, labels,
                                 &candidate_count);

        /* 视口窗口可能返回若干更近的小型机场，但它们没有频率记录。
         * 这不等价于“附近没有电台”；整库查询在全候选扫描阶段直接
         * 过滤 freq_count=0，才能找到稍远但有管制频率的机场。 */
        if (pk_airband_page_should_query_full_db(window_count, candidates,
                                                  candidate_count)) {
            const int full_count = nearest_from_full_db(
                gps.lat, gps.lon, nearby, PAGE_NEAR_AIRPORTS);
            append_nearby_candidates(nearby, full_count, candidates, labels,
                                     &candidate_count);
        }
    }

    pk_airband_station_t stations[PAGE_MAX_STATIONS] = {0};
    const size_t station_count = pk_airband_station_collect(
        candidates, candidate_count, stations, PAGE_MAX_STATIONS);
    publish_stations(stations, station_count);
    /* fallback 固定只有 GUARD/AIR-AIR 两项；只有出现数据库候选时才算
     * “附近台已加载”。否则保持可重试，让先开页面、后获得定位/数据库的
     * 正常启动时序无需退出页面重进。 */
    atomic_store(&s_nearby_loaded, station_count > 2u);
    ESP_LOGI(TAG, "station list ready: fix=%d candidates=%u stations=%u",
             gps.have_fix, (unsigned)candidate_count, (unsigned)station_count);
    atomic_store(&s_loader_running, false);
    vTaskDelete(NULL);
}

static void start_nearby_loader(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_loader_running, &expected, true))
        return;
    atomic_store(&s_last_loader_tick, (uint32_t)xTaskGetTickCount());
    if (xTaskCreate(load_nearby_task, "airband_stations", 4096, NULL, 2, NULL) !=
        pdPASS) {
        atomic_store(&s_loader_running, false);
        ESP_LOGE(TAG, "cannot start station loader");
    }
}

void pk_airband_page_init(void)
{
    load_fallbacks();
    atomic_store(&s_loader_running, false);
    atomic_store(&s_nearby_loaded, false);
    atomic_store(&s_last_loader_tick, 0u);
}

void pk_airband_page_open(void)
{
    load_fallbacks();
    atomic_store(&s_nearby_loaded, false);
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

static void draw_button(uint16_t *fb, int x0, int x1, const char *label,
                        bool active)
{
    draw_button_at(fb, x0, PAGE_CONTROL_TOP + 10, x1, 470, label, active);
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
    if (pk_airband_page_should_retry_loader(
            atomic_load(&s_loader_running), atomic_load(&s_nearby_loaded),
            now, (uint32_t)atomic_load(&s_last_loader_tick),
            (uint32_t)pdMS_TO_TICKS(2000))) {
        start_nearby_loader();
    }

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L, PK_UI_TITLE_Y,
               pk_i18n_text(PK_TR_APTD_AIRBAND), PK_UI_TITLE_COL,
               PK_UI_TITLE_SIZE);

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

    const char *status = !have_state || !state.online
                           ? pk_i18n_text(PK_TR_AIRBAND_RF_OFFLINE)
                       : !state.speaker_online
                             ? pk_i18n_text(PK_TR_APTD_SPEAKER_OFFLINE)
                       : have_ctl && ctl.speaker_muted
                             ? pk_i18n_text(PK_TR_APTD_MUTED)
                       : state.squelch_enabled && !state.squelch_open
                             ? pk_i18n_text(PK_TR_APTD_SQUELCH_CLOSED)
                             : pk_i18n_text(PK_TR_APTD_PLAYING);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 650, 70, status,
               have_state && state.online && state.speaker_online
                   ? col_green() : col_amber(), PK_AA_XS);

    pk_airband_station_t stations[PAGE_MAX_STATIONS];
    size_t count;
    portENTER_CRITICAL(&s_lock);
    count = s_station_count;
    memcpy(stations, s_stations, count * sizeof(*stations));
    portEXIT_CRITICAL(&s_lock);
    for (size_t i = 0; i < count; ++i) {
        const int y = PAGE_LIST_TOP + (int)i * PAGE_ROW_H;
        const bool selected = hz == stations[i].frequency_hz;
        pk_pfd_fill_round_rect(fb, 16, y + 2, 784, y + PAGE_ROW_H - 3, 8,
                               selected ? col_blue() : col_panel());
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 32, y + 14,
                   stations[i].label, col_text(), PK_AA_S);
        char freq[20];
        (void)snprintf(freq, sizeof(freq), "%lu.%03lu",
                       (unsigned long)(stations[i].channel_khz / 1000u),
                       (unsigned long)(stations[i].channel_khz % 1000u));
        const int fw = pk_aa_text_width(freq, PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 764 - fw, y + 10,
                   freq, col_text(), PK_AA_M);
    }

    const bool tone_active = pk_airband_speaker_test_active();
    draw_button(fb, 16, 190,
                pk_i18n_text(tone_active
                                 ? PK_TR_AIRBAND_SPEAKER_STOP
                                 : PK_TR_AIRBAND_SPEAKER_TEST),
                tone_active);
    draw_button(fb, 206, 350, pk_i18n_text(PK_TR_APTD_MUTED),
                have_ctl && ctl.speaker_muted);
    draw_button(fb, 560, 632, "-", false);
    (void)snprintf(line, sizeof(line), "%u%%",
                   have_ctl ? (unsigned)ctl.speaker_volume : 0u);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 650, PAGE_CONTROL_TOP + 25,
               line, col_sub(), PK_AA_S);
    draw_button(fb, 712, 784, "+", false);
}

bool pk_airband_page_touch(int x, int y)
{
    if (y < PFD_BAR_BOT || x >= PK_DISPLAY_W - 8) return false;
    int fx = 0, fy = 0, fw = 0, fh = 0;
    const bool fab_visible = pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh);
    if (!pk_airband_page_owns_press(x, y, fab_visible, fx, fy, fw, fh))
        return false;
    s_press_x = x;
    s_press_y = y;
    s_press_valid = true;
    s_moved = false;
    return true;
}

bool pk_airband_page_drag(int x, int y)
{
    if (!s_press_valid) return false;
    const int dx = x - s_press_x, dy = y - s_press_y;
    if (dx > 12 || dx < -12 || dy > 12 || dy < -12) s_moved = true;
    return true;
}

void pk_airband_page_touch_up(void)
{
    const bool click = s_press_valid && !s_moved;
    const int x = s_press_x, y = s_press_y;
    s_press_valid = false;
    s_moved = false;
    if (!click) return;

    size_t station_count;
    portENTER_CRITICAL(&s_lock);
    station_count = s_station_count;
    portEXIT_CRITICAL(&s_lock);
    const pk_airband_page_action_t action =
        pk_airband_page_action_at(x, y, station_count);
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
    if (action.kind == PK_AIRBAND_PAGE_ACT_TUNE) {
        const int row = action.station_index;
        pk_airband_station_t station = {0};
        bool valid = false;
        portENTER_CRITICAL(&s_lock);
        if (row >= 0 && (size_t)row < s_station_count) {
            station = s_stations[row];
            valid = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (valid) {
            const bool ok = pk_airband_set_channel(station.channel_khz);
            ESP_LOGI(TAG, "station tap: %s %lu Hz -> %s", station.label,
                     (unsigned long)station.frequency_hz,
                     ok ? "tuned" : "rejected");
        }
        return;
    }
    pk_airband_control_status_t ctl = {0};
    if (!pk_airband_control_get(&ctl)) return;
    if (action.kind == PK_AIRBAND_PAGE_ACT_SPEAKER_TEST) {
        const pk_airband_speaker_test_click_t click_action =
            pk_airband_speaker_test_action();
        const bool ok = click_action != PK_AIRBAND_SPEAKER_TEST_CLICK_REJECT;
        const bool stopping =
            click_action == PK_AIRBAND_SPEAKER_TEST_CLICK_STOP;
        ESP_LOGI(TAG, "speaker test button -> %s %s",
                 stopping ? "stop" : "start", ok ? "accepted" : "rejected");
        pk_ui_toast_show(!ok ? PK_TR_AIRBAND_TEST_FAILED
                         : stopping ? PK_TR_AIRBAND_TEST_STOPPED
                                    : PK_TR_AIRBAND_TEST_STARTED,
                         !ok);
    } else if (action.kind == PK_AIRBAND_PAGE_ACT_MUTE) {
        pk_airband_set_speaker_muted(!ctl.speaker_muted);
    } else if (action.kind == PK_AIRBAND_PAGE_ACT_VOLUME_DOWN) {
        pk_airband_set_speaker_volume(ctl.speaker_volume > 5
                                          ? ctl.speaker_volume - 5 : 0);
    } else if (action.kind == PK_AIRBAND_PAGE_ACT_VOLUME_UP) {
        pk_airband_set_speaker_volume(ctl.speaker_volume < 95
                                          ? ctl.speaker_volume + 5 : 100);
    }
}
#endif
