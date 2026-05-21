/*
 * ui_state.c — implementation of the UI mode + list cursor.
 *
 * Single static mutex protects mode and selection. Operations are
 * trivially short (load/store one int), so contention is negligible
 * and we don't bother with atomic primitives.
 */

#include "ui_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "ui";

#define UI_LIST_PENDING_DELTA_MAX  999   /* upper saturation on the
                                            pending scroll delta — no
                                            aircraft list will ever
                                            come close, this just
                                            prevents int wrap-around
                                            if someone holds UP/DOWN
                                            forever before the next
                                            renderer tick consumes the
                                            buffered presses */

/* Calibration-wizard auto-trigger thresholds. The 10 s enter window
 * is long enough that we don't bother the user with the wizard
 * during a brief acc=0 dip on first boot before fusion converges
 * normally; the 3 s exit window with acc≥2 confirms fusion has
 * really converged before we dismiss. */
#define UI_CAL_WIZARD_ENTER_MS        10000
#define UI_CAL_WIZARD_EXIT_MS          3000
#define UI_CAL_WIZARD_EXIT_ACCURACY    2

static SemaphoreHandle_t s_lock;
static pk_ui_mode_t      s_mode               = PK_UI_MODE_PFD;

/* List selection is tracked by ICAO, not row index. s_list_selected_icao
 * is the aircraft the user last had highlighted (0 = "no commitment
 * yet"); s_list_pending_delta is the un-applied scroll-button intent,
 * cleared by pk_ui_list_resolve_row(). Tracking by ICAO keeps the
 * highlight stuck to the same aircraft even when the snapshot row
 * order shifts (aircraft enters/leaves the trailing-60s window). */
static uint32_t          s_list_selected_icao;
static int               s_list_pending_delta;

/* Runtime own-ship binding. s_own_icao_set distinguishes "user
 * explicitly bound something" (even if to 0) from "never set, use
 * Kconfig default". RAM-only — wiped on every reboot. */
static uint32_t          s_own_icao_runtime;
static bool              s_own_icao_set;

/* Calibration wizard state. acc_first_low_us = first time we saw
 * acc=0 in the current "low streak"; acc_first_high_us = first time
 * we saw acc≥2 in the current "high streak". 0 = no current streak. */
static int64_t           s_cal_acc_first_low_us;
static int64_t           s_cal_acc_first_high_us;
static uint8_t           s_cal_last_accuracy;

esp_err_t pk_ui_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ui_state ready (default mode: PFD)");
    return ESP_OK;
}

pk_ui_mode_t pk_ui_get_mode(void)
{
    if (s_lock == NULL) return PK_UI_MODE_PFD;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pk_ui_mode_t m = s_mode;
    xSemaphoreGive(s_lock);
    return m;
}

static const char *mode_name(pk_ui_mode_t m)
{
    switch (m) {
    case PK_UI_MODE_PFD:         return "PFD";
    case PK_UI_MODE_ADSB_LIST:   return "ADSB_LIST";
    case PK_UI_MODE_ABOUT:       return "ABOUT";
    case PK_UI_MODE_CAL_WIZARD:  return "CAL_WIZARD";
    default:                     return "?";
    }
}

void pk_ui_toggle_mode(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Three-way cycle: PFD → ADSB_LIST → ABOUT → PFD …
     * The CAL_WIZARD mode is outside the cycle: pressing MODE while
     * in it returns to PFD and dismisses the wizard (the auto-
     * trigger state machine will re-arm next time accuracy drops). */
    switch (s_mode) {
    case PK_UI_MODE_PFD:         s_mode = PK_UI_MODE_ADSB_LIST; break;
    case PK_UI_MODE_ADSB_LIST:   s_mode = PK_UI_MODE_ABOUT;     break;
    case PK_UI_MODE_ABOUT:       s_mode = PK_UI_MODE_PFD;       break;
    case PK_UI_MODE_CAL_WIZARD:  s_mode = PK_UI_MODE_PFD;       break;
    default:                     s_mode = PK_UI_MODE_PFD;       break;
    }
    pk_ui_mode_t new_mode = s_mode;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mode → %s", mode_name(new_mode));
}

void pk_ui_cal_wizard_tick(bool valid, uint8_t accuracy)
{
    if (s_lock == NULL) return;

    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cal_last_accuracy = valid ? accuracy : 0;

    if (valid && accuracy == 0) {
        if (s_cal_acc_first_low_us == 0) s_cal_acc_first_low_us = now;
        s_cal_acc_first_high_us = 0;
    } else if (valid && accuracy >= UI_CAL_WIZARD_EXIT_ACCURACY) {
        if (s_cal_acc_first_high_us == 0) s_cal_acc_first_high_us = now;
        s_cal_acc_first_low_us = 0;
    } else {
        /* acc=1 or invalid: don't progress either timer, but
         * don't reset them either — fusion is in transit. */
    }

    /* Auto-enter wizard from any non-wizard mode if acc has been
     * stuck at 0 for the enter window. */
    if (s_mode != PK_UI_MODE_CAL_WIZARD &&
        s_cal_acc_first_low_us != 0 &&
        (now - s_cal_acc_first_low_us) / 1000 >= UI_CAL_WIZARD_ENTER_MS) {
        s_mode = PK_UI_MODE_CAL_WIZARD;
        s_cal_acc_first_low_us = 0;       /* reset so we don't re-trigger immediately */
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "mode → CAL_WIZARD (auto: acc=0 for >%dms — "
                       "device needs figure-8 motion)",
                 UI_CAL_WIZARD_ENTER_MS);
        return;
    }

    /* Auto-exit wizard back to PFD when acc has been ≥2 for the
     * exit window. */
    if (s_mode == PK_UI_MODE_CAL_WIZARD &&
        s_cal_acc_first_high_us != 0 &&
        (now - s_cal_acc_first_high_us) / 1000 >= UI_CAL_WIZARD_EXIT_MS) {
        s_mode = PK_UI_MODE_PFD;
        s_cal_acc_first_high_us = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "mode → PFD (auto: acc≥%u for >%dms — "
                       "fusion converged, dismissing wizard)",
                 UI_CAL_WIZARD_EXIT_ACCURACY, UI_CAL_WIZARD_EXIT_MS);
        return;
    }

    xSemaphoreGive(s_lock);
}

uint8_t pk_ui_cal_wizard_last_accuracy(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t a = s_cal_last_accuracy;
    xSemaphoreGive(s_lock);
    return a;
}

void pk_ui_list_scroll(int delta)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_list_pending_delta + delta;
    if (v < -UI_LIST_PENDING_DELTA_MAX) v = -UI_LIST_PENDING_DELTA_MAX;
    if (v >  UI_LIST_PENDING_DELTA_MAX) v =  UI_LIST_PENDING_DELTA_MAX;
    s_list_pending_delta = v;
    xSemaphoreGive(s_lock);
}

int pk_ui_list_resolve_row(const uint32_t *icaos, size_t n)
{
    if (s_lock == NULL || icaos == NULL || n == 0) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Find row currently occupied by the previously-selected ICAO. */
    int cur_row = -1;
    if (s_list_selected_icao != 0) {
        for (size_t i = 0; i < n; ++i) {
            if (icaos[i] == s_list_selected_icao) {
                cur_row = (int)i;
                break;
            }
        }
    }
    /* No prior selection, or the aircraft expired out of the snapshot:
     * anchor at row 0. */
    if (cur_row < 0) cur_row = 0;

    int new_row = cur_row + s_list_pending_delta;
    s_list_pending_delta = 0;
    if (new_row < 0)         new_row = 0;
    if (new_row >= (int)n)   new_row = (int)n - 1;

    s_list_selected_icao = icaos[new_row];

    xSemaphoreGive(s_lock);
    return new_row;
}

uint32_t pk_ui_list_get_selected_icao(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = s_list_selected_icao;
    xSemaphoreGive(s_lock);
    return v;
}

void pk_ui_set_own_icao(uint32_t icao24)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_own_icao_runtime = icao24 & 0xFFFFFF;
    s_own_icao_set     = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "own ICAO bound at runtime → %06lX",
             (unsigned long)(icao24 & 0xFFFFFF));
}

uint32_t pk_ui_get_own_icao(void)
{
    if (s_lock == NULL) return (uint32_t)CONFIG_PK_OWN_ICAO;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = s_own_icao_set ? s_own_icao_runtime
                                : (uint32_t)CONFIG_PK_OWN_ICAO;
    xSemaphoreGive(s_lock);
    return v;
}
