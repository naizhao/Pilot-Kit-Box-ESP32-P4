/*
 * pfd.c — Primary Flight Display task + frame dispatcher.
 *
 * Renders one of four UI modes into the 320×240 ST7789 framebuffer at
 * 30 FPS:
 *
 *   PFD         — G1000-style attitude + statusbar + HSI + ALT tape +
 *                 GS / VS readouts.
 *   CAL_WIZARD  — IMU compass-calibration figure-8 prompt.
 *   ABOUT       — system info page.
 *   ADSB_LIST   — live ADS-B contacts table + detail pane.
 *
 * Each PFD widget owns its screen region and reads from a small
 * per-frame POD assembled here (pk_pfd_imu_t, pk_pfd_status_t,
 * pk_pfd_hsi_t, plus the optional own-ship ADS-B snapshot).
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
#include "diag_page.h"
#include "aircraft_state.h"
#include "gps.h"
#include "own_ship.h"
#include "cal_wizard.h"
#include "display.h"
#include "imu_task.h"
#include "pfd_attitude.h"
#include "pfd_draw.h"
#include "pfd_font.h"
#include "pfd_hsi.h"
#include "pfd_statusbar.h"
#include "pfd_speed_tape.h"
#include "pfd_tape.h"
#include "baro.h"
#include "sdkconfig.h"
#include "settings_page.h"
#include "traffic_page.h"
#include "ui_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pfd";

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

        case PK_UI_MODE_DIAG:
            pk_diag_page_render(fb);
            break;

        case PK_UI_MODE_ADSB_LIST:
            pk_adsb_list_render(fb);
            break;

        case PK_UI_MODE_SETTINGS:
            pk_settings_page_render(fb);
            break;

        case PK_UI_MODE_TRAFFIC:
            pk_traffic_page_render(fb);
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
             * ADS-B reports. pk_ui_get_own_icao() returns the runtime
             * binding (set via TARE short-press in ADS-B list mode)
             * or falls back to the compile-time CONFIG_PK_OWN_ICAO.
             * Stale window is PK_OWN_STALE_AGE_MS. */
            aircraft_t own = {0};
            pk_own_src_t own_src;
            bool own_valid = pk_own_ship_resolve(
                now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own, &own_src);

            static int64_t own_log = 0;
            if(now_us - own_log > 1000000){
                own_log = now_us;
                /* TODO: 硬件验证后降级为 ESP_LOGD 或删除 */
                ESP_LOGI("pfd", "own src=%d valid=%d lat=%.5f lon=%.5f",
                         own_src, own_valid, own.lat, own.lon);
            }

            /* HDG source priority: bound own-ship's ADS-B ground track
             * (DF17 metype 19) beats IMU magnetic yaw whenever both
             * are available. The bound transponder reports the actual
             * ground track of the aircraft we're flying in, which is
             * what a pilot cares about — the IMU only reports where
             * the kit happens to be pointing (could be the panel, a
             * pocket, a yoke clamp, etc.). IMU is the fallback when no
             * own-ship is bound or its velocity isn't fresh. The
             * imu_valid field in pk_pfd_status_t / pk_pfd_hsi_t is now
             * really "yaw_valid" — left renamed for the moment to
             * avoid churning two more headers; the consumers in
             * pfd_statusbar.c / pfd_hsi.c only use it as a "is the
             * yaw_deg good" gate. */
            float yaw_deg     = 0.0f;
            bool  yaw_valid   = false;
            if (own_valid && own.have_velocity) {
                yaw_deg   = (float)own.heading_deg;
                yaw_valid = true;
            } else if (have) {
                yaw_deg   = s.yaw_deg;
                yaw_valid = true;
            }

            /* Bank source priority — mirrors the yaw logic above:
             *   1) Derive from the bound aircraft's smoothed turn rate
             *      + ground speed (coordinated-turn formula). This is
             *      the actual aircraft's bank, irrespective of kit
             *      orientation.
             *   2) Fall back to IMU roll (the kit's tilt).
             * Pitch stays IMU-only — ADS-B carries no AoA, so we can
             * at best compute flight-path angle (atan(VS/GS)) which
             * isn't the same as aircraft pitch attitude.
             *
             * When the bank override fires we LEAVE imu.valid alone:
             * the attitude indicator gates on imu.valid for whether to
             * draw at all, and we still want to draw (with IMU pitch +
             * ADS-B bank) even if IMU itself is briefly stale, as long
             * as one or the other is fresh. */
            if (own_valid) {
                float bank_deg;
                if (pk_aircraft_derive_bank(
                        pk_ui_get_own_icao(), now_us,
                        (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL,
                        &bank_deg)) {
                    imu.roll_deg = bank_deg;
                    imu.valid    = true;
                }
            }

            pk_gps_state_t gps;
            pk_gps_get(&gps);

            pk_pfd_status_t stat = {
                .imu_valid      = yaw_valid,
                .yaw_deg        = yaw_deg,
                .aircraft_count = n_aircraft,
                .gps_have_fix   = gps.have_fix,
                .gps_sats       = (uint8_t)(gps.sats < 0 ? 0 : (gps.sats > 99 ? 99 : gps.sats)),
            };
            pk_pfd_hsi_t hsi = {
                .imu_valid = yaw_valid,
                .yaw_deg   = yaw_deg,
            };
            pk_pfd_alt_tape_t alt = {
                .valid       = own_valid && own.have_altitude,
                .altitude_ft = (own_valid && own.have_altitude)
                                   ? own.altitude_ft : 0,
            };

            /* Attitude fills the full panel as the screen background.
             * Statusbar / ALT tape / speed tape / HSI / VS draw on top
             * as opaque overlays — no need to pre-clear the frame. */
            pk_pfd_attitude_render(fb, &imu);
            pk_pfd_statusbar_render(fb, &stat);
            pk_pfd_alt_tape_render(fb, &alt);
            pk_pfd_speed_tape_t spd = {
                .valid           = own_valid && own.have_velocity,
                .ground_speed_kt = (own_valid && own.have_velocity)
                                       ? own.ground_speed_kt : 0,
            };
            pk_pfd_speed_tape_render(fb, &spd);
            pk_pfd_hsi_render(fb, &hsi);

            /* Three right-column info boxes below the ALT tape (y=168).
             * Each box: x=[256,320) = 64 px wide, 18 px tall, 2 px gap.
             *   Box 1 y[170,188]: BARO  — barometric altitude ft
             *   Box 2 y[190,208]: metric alt (ft × 0.3048) in metres
             *   Box 3 y[210,228]: VS    — vertical speed fpm
             * Source: BMP388 gas baro snapshot + own-ship ADS-B (VS). */
            {
                pk_baro_state_t baro;
                pk_baro_get(&baro);

                const uint16_t COL_BARO_LBL  = pk_rgb565(230, 200,  74); /* amber */
                const uint16_t COL_WHITE      = pk_rgb565(255, 255, 255);
                const uint16_t COL_BLUE       = pk_rgb565(150, 200, 255); /* light blue */
                const uint16_t COL_CYAN       = pk_rgb565( 70, 220, 250);
                const uint16_t COL_STALE      = pk_rgb565(100, 100, 100);
                char buf[16];

                /* ── Box 1: BARO altitude (ft) ─────────────────────── */
                pk_pfd_darken_rect(fb, 256, 170, PK_DISPLAY_W, 188, 160);
                pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             258, 173, "BARO", COL_BARO_LBL, 1);
                if (baro.valid) {
                    int balt = baro.alt_ft;
                    if (balt <  -9999) balt = -9999;
                    if (balt > 99999)  balt = 99999;
                    snprintf(buf, sizeof(buf), "%5d", balt);
                } else {
                    snprintf(buf, sizeof(buf), "   --");
                }
                pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             288, 173, buf, baro.valid ? COL_WHITE : COL_STALE, 1);

                /* ── Box 2: metric altitude (m) ─────────────────────── */
                pk_pfd_darken_rect(fb, 256, 190, PK_DISPLAY_W, 208, 160);
                pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             258, 193, "ALT", COL_BARO_LBL, 1);
                if (baro.valid) {
                    /* 修复4: 先钳 alt_ft(与 Box1 BARO 同范围),再换算 */
                    int balt_clamped = baro.alt_ft;
                    if (balt_clamped <  -9999) balt_clamped = -9999;
                    if (balt_clamped >  99999) balt_clamped = 99999;
                    /* 修复5: lroundf 四舍五入 */
                    int alt_m = (int)lroundf((float)balt_clamped * 0.3048f);
                    if (alt_m < -9999) alt_m = -9999;
                    if (alt_m > 99999) alt_m = 99999;
                    snprintf(buf, sizeof(buf), "%5d", alt_m);
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 288, 193, buf, COL_BLUE, 1);
                } else {
                    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 288, 193, "   --", COL_STALE, 1);
                }

                /* ── Box 3: VS (vertical speed, fpm) ────────────────── */
                pk_pfd_darken_rect(fb, 256, 210, PK_DISPLAY_W, 228, 160);
                pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             258, 213, "VS", COL_CYAN, 1);
                {
                    bool adsb_vs = own_valid && own.have_velocity && (own_src == PK_OWN_SRC_BOUND_ADSB);
                    if (adsb_vs) {
                        /* Priority 1: own-ship ADS-B vertical rate */
                        int vs = own.vert_rate_fpm;
                        if (vs >  9999) vs =  9999;
                        if (vs < -9999) vs = -9999;
                        snprintf(buf, sizeof(buf), "%+5d", vs);
                        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                     276, 213, buf, COL_WHITE, 1);
                    } else if (baro.valid) {
                        /* Priority 2: baro-derived VS (reference, amber) */
                        int vs = baro.vs_fpm;
                        if (vs >  9999) vs =  9999;
                        if (vs < -9999) vs = -9999;
                        snprintf(buf, sizeof(buf), "%+5d", vs);
                        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                     276, 213, buf, COL_BARO_LBL, 1);
                    } else {
                        /* Priority 3: no data */
                        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                     276, 213, "   --", COL_STALE, 1);
                    }
                }
            }

            /* Own-ship source badge — bottom-left x[0,78] y[210,232].
             * Three states:
             *   BOUND_ADSB → callsign (cyan) or ICAO hex (cyan)
             *   GPS        → "GPS" (white)
             *   NONE/stale → "--" (grey) */
#define PFD_SRC_BADGE_X1  78
#define PFD_SRC_BADGE_Y0  210
#define PFD_SRC_BADGE_Y1  232
            {
                /* Cyan matches project-wide COL_LABEL (70,220,250). */
                const uint16_t SRC_ADSB  = pk_rgb565( 70, 220, 250);
                const uint16_t SRC_GPS   = pk_rgb565(240, 240, 240);
                const uint16_t SRC_NONE  = pk_rgb565(100, 100, 100);
                char src_buf[8];
                uint16_t src_col;

                if (!own_valid || own_src == PK_OWN_SRC_NONE) {
                    src_buf[0] = '-'; src_buf[1] = '-'; src_buf[2] = '\0';
                    src_col = SRC_NONE;
                } else if (own_src == PK_OWN_SRC_BOUND_ADSB) {
                    bool used_callsign = false;
                    if (own.have_callsign) {
                        /* Copy at most 6 chars (72 px max in 78-px badge)
                         * then strip trailing spaces. */
                        int i;
                        for (i = 0; i < 6 && own.callsign[i]; i++)
                            src_buf[i] = own.callsign[i];
                        src_buf[i] = '\0';
                        while (i > 0 && src_buf[i - 1] == ' ')
                            src_buf[--i] = '\0';
                        if (src_buf[0] != '\0') used_callsign = true;
                    }
                    if (!used_callsign) {
                        snprintf(src_buf, sizeof(src_buf), "%06lX",
                                 (unsigned long)own.icao24);
                    }
                    src_col = SRC_ADSB;
                } else {
                    /* PK_OWN_SRC_GPS */
                    src_buf[0] = 'G'; src_buf[1] = 'P'; src_buf[2] = 'S';
                    src_buf[3] = '\0';
                    src_col = SRC_GPS;
                }

                pk_pfd_darken_rect(fb, 0, PFD_SRC_BADGE_Y0,
                                   PFD_SRC_BADGE_X1, PFD_SRC_BADGE_Y1, 128);
                /* Cockpit font, scale-2: 12 px per glyph.
                 * Left-pad within the 78-px box so text appears centred. */
                {
                    int txt_x = (PFD_SRC_BADGE_X1 - (int)strlen(src_buf) * 12) / 2;
                    if (txt_x < 2) txt_x = 2;
                    pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                         txt_x, PFD_SRC_BADGE_Y0 + 2,
                                         src_buf, src_col);
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
                                   : (mode == PK_UI_MODE_SETTINGS)   ? "SET"
                                   : (mode == PK_UI_MODE_ABOUT)      ? "ABOUT"
                                   : (mode == PK_UI_MODE_DIAG)       ? "DIAG"
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
