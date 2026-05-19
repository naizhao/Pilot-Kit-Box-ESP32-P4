/*
 * pfd.c — Primary Flight Display task + frame dispatcher.
 *
 * Renders one of four UI modes into the 320×240 ST7789 framebuffer at
 * 30 FPS:
 *
 *   PFD         — G1000-style attitude + statusbar + HSI + (later) ALT
 *                 tape + GS / VS readouts.
 *   CAL_WIZARD  — IMU compass-calibration figure-8 prompt.
 *   ABOUT       — system info page.
 *   ADSB_LIST   — live ADS-B contacts table + detail pane.
 *
 * Each PFD widget owns its screen region and reads from a small
 * per-frame POD assembled here (pk_pfd_imu_t, pk_pfd_status_t,
 * pk_pfd_hsi_t). Phase E adds an own-ship ADS-B snapshot to the
 * PFD-mode case; phase F adds the GS/VS inline readouts.
 */

#include "pfd.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "about_page.h"
#include "adsb_list.h"
#include "aircraft_state.h"
#include "cal_wizard.h"
#include "display.h"
#include "imu_task.h"
#include "pfd_attitude.h"
#include "pfd_draw.h"
#include "pfd_font.h"
#include "pfd_hsi.h"
#include "pfd_statusbar.h"
#include "pfd_tape.h"
#include "sdkconfig.h"
#include "ui_state.h"

#include <stdio.h>

static const char *TAG = "pfd";

#define COL_PANEL_BG  pk_rgb565(8, 8, 12)

static void pfd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pfd_task running (G1000 landscape)");

    uint16_t *fb = pk_display_framebuffer();
    if (fb == NULL) {
        ESP_LOGE(TAG, "no framebuffer — exiting");
        vTaskDelete(NULL);
    }

    /* Big stack-eaters live here as file-static so they don't blow the
     * task stack. Pinned to PSRAM (.ext_ram.bss) so they don't compete
     * with FreeRTOS / ESP-Hosted tasks for scarce DMA-capable internal
     * RAM during the early-boot constructor storm. */
    static EXT_RAM_BSS_ATTR aircraft_t scratch[AIRCRAFT_TABLE_CAPACITY];
    int64_t  fps_window_start_us = esp_timer_get_time();
    uint32_t frames_in_window = 0;

    while (1) {
        TickType_t frame_start = xTaskGetTickCount();

        pk_imu_sample_t s;
        bool have = pk_imu_sample_get(&s);
        pk_ui_cal_wizard_tick(have, have ? s.accuracy : 0);

        pk_ui_mode_t mode = pk_ui_get_mode();

        switch (mode) {
        case PK_UI_MODE_CAL_WIZARD:
            pk_cal_wizard_render(fb);
            break;

        case PK_UI_MODE_ABOUT:
            pk_about_page_render(fb);
            break;

        case PK_UI_MODE_ADSB_LIST:
            pk_adsb_list_render(fb);
            break;

        case PK_UI_MODE_PFD:
        default: {
            int64_t now_us = esp_timer_get_time();

            pk_pfd_imu_t imu = {
                .valid     = have,
                .roll_deg  = have ? s.roll_deg  : 0.0f,
                .pitch_deg = have ? s.pitch_deg : 0.0f,
            };

            /* Same "fresh contact" 60s window the BLE GDL90 emitter uses. */
            size_t n_aircraft = aircraft_state_snapshot(
                scratch, sizeof(scratch) / sizeof(scratch[0]),
                now_us, AIRCRAFT_STALE_AGE_US);

            /* Own-ship: ALT/VS/GS sourced from the bound transponder's
             * ADS-B reports. Stale window is PK_OWN_STALE_AGE_MS. */
            aircraft_t own;
            bool own_valid = aircraft_state_get_own(
                CONFIG_PK_OWN_ICAO, now_us,
                (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own);

            pk_pfd_status_t stat = {
                .imu_valid      = have,
                .yaw_deg        = have ? s.yaw_deg : 0.0f,
                .aircraft_count = n_aircraft,
            };
            pk_pfd_hsi_t hsi = {
                .imu_valid = have,
                .yaw_deg   = have ? s.yaw_deg : 0.0f,
            };
            pk_pfd_alt_tape_t alt = {
                .valid       = own_valid && own.have_altitude,
                .altitude_ft = (own_valid && own.have_altitude)
                                   ? own.altitude_ft : 0,
            };

            pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H,
                             COL_PANEL_BG);

            pk_pfd_statusbar_render(fb, &stat);
            pk_pfd_attitude_render(fb, &imu);
            pk_pfd_alt_tape_render(fb, &alt);
            pk_pfd_hsi_render(fb, &hsi);

            /* GS readout — bottom-left, x in [0, 90), y in [138, 168). */
            {
                const uint16_t COL_LABEL = pk_rgb565( 70, 220, 250);
                const uint16_t COL_VALUE = pk_rgb565(240, 240, 240);
                const uint16_t COL_STALE = pk_rgb565(100, 100, 100);
                char buf[8];
                bool gs_valid = own_valid && own.have_velocity;
                pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             4, 142, "GS", COL_LABEL, 2);
                if (gs_valid) {
                    snprintf(buf, sizeof(buf), "%3d", own.ground_speed_kt);
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 32, 142, buf, COL_VALUE, 2);
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 70, 142, "KT", COL_LABEL, 2);
                } else {
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 32, 142, "---", COL_STALE, 2);
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 70, 142, "KT", COL_STALE, 2);
                }
            }

            /* VS readout — bottom-right. "VS" label + signed value;
             * the FPM suffix is intentionally omitted (would overflow
             * the 320 px panel at scale 2; "VS" + signed integer
             * conveys the unit by aviation convention). */
            {
                const uint16_t COL_LABEL = pk_rgb565( 70, 220, 250);
                const uint16_t COL_VALUE = pk_rgb565(240, 240, 240);
                const uint16_t COL_STALE = pk_rgb565(100, 100, 100);
                char buf[12];
                bool vs_valid = own_valid && own.have_velocity;
                pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             224, 142, "VS", COL_LABEL, 2);
                if (vs_valid) {
                    snprintf(buf, sizeof(buf), "%+5d", own.vert_rate_fpm);
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 252, 142, buf, COL_VALUE, 2);
                } else {
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 252, 142, "-----", COL_STALE, 2);
                }
            }
            break;
        }
        }

        pk_display_flush_full();
        frames_in_window++;

        int64_t now = esp_timer_get_time();
        if (now - fps_window_start_us >= 1000000) {
            const char *mode_label = (mode == PK_UI_MODE_ADSB_LIST)  ? "LIST"
                                   : (mode == PK_UI_MODE_ABOUT)      ? "ABOUT"
                                   : (mode == PK_UI_MODE_CAL_WIZARD) ? "CAL"
                                   :                                   "PFD";
            ESP_LOGI(TAG, "%s %lu FPS  | roll=%+6.2f pitch=%+6.2f yaw=%6.2f"
                          "  imu_valid=%d",
                     mode_label,
                     (unsigned long)frames_in_window,
                     have ? s.roll_deg : 0.0f,
                     have ? s.pitch_deg : 0.0f,
                     have ? s.yaw_deg : 0.0f,
                     have);
            frames_in_window = 0;
            fps_window_start_us = now;
        }
        vTaskDelayUntil(&frame_start, pdMS_TO_TICKS(33));   /* 30 FPS target */
    }
}

esp_err_t pk_pfd_start(void)
{
    /* 6 KiB stack — generous because trig / floating-point ESP_LOGI
     * format strings can each chew 1 KiB on RISC-V. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        pfd_task, "pfd", 6 * 1024, NULL, 4, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
