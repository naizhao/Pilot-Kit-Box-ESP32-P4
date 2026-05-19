/*
 * pfd.c — Phase 4c v2 Primary Flight Display.
 *
 * Renders a recognisable aviation-style PFD into the 240×320 ST7789
 * framebuffer at 30 FPS. The horizon math is identical to v1; v2
 * adds the elements a pilot actually looks at:
 *
 *   ┌──────────────────────────────┐  y =   0
 *   │     bank arc + pointer       │ 35 px
 *   ├──────────────────────────────┤  y =  35
 *   │   pitch ladder over horizon  │
 *   │      + center reticle        │ 205 px
 *   ├──────────────────────────────┤  y = 240
 *   │      heading tape            │ 30 px
 *   ├──────────────────────────────┤  y = 270
 *   │   R:+12.3°   P:-5.4°         │
 *   │   HDG: 087°  ADSB: 12        │ 50 px
 *   └──────────────────────────────┘  y = 320
 *
 * The sky/ground horizon fills the 0..240 band; bank arc + pitch
 * ladder draw on top of it (over the sky). Heading tape and number
 * panel are dark, so we explicitly fill_rect those regions every frame.
 *
 * Phase 4d (deferred) will:
 *   - swap the synchronous pk_display_flush_full() for an async GDMA
 *     pipeline so render+flush can overlap
 *   - hoist common 16-bit fills into a fast memset16 (or use esp_lcd's
 *     trans_done callback to flip double-buffers)
 *   - chase 60 FPS
 *
 * The math here intentionally avoids floating-point trig in the inner
 * pixel loops — the horizon fill samples a precomputed slope, and the
 * pitch-ladder lines pre-rotate their endpoints with one sin/cos per
 * line then run Bresenham.
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
#include "pfd_legacy.h"
#include "ui_state.h"

static const char *TAG = "pfd";

/* --- Render task ----------------------------------------------------- */

static void pfd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pfd_task running (Phase 4c v2: ladder + arc + tape + text)");

    uint16_t *fb = pk_display_framebuffer();
    if (fb == NULL) {
        ESP_LOGE(TAG, "no framebuffer — exiting");
        vTaskDelete(NULL);
    }

    /* Big stack-eaters live here as file-static so they don't blow the
     * task stack. Pinned to PSRAM (.ext_ram.bss) so they don't compete
     * with FreeRTOS / ESP-Hosted tasks for scarce DMA-capable
     * internal RAM during the early-boot constructor storm. */
    static EXT_RAM_BSS_ATTR aircraft_t scratch[AIRCRAFT_TABLE_CAPACITY];
    int64_t  fps_window_start_us = esp_timer_get_time();
    uint32_t frames_in_window = 0;

    while (1) {
        TickType_t frame_start = xTaskGetTickCount();

        /* Sample the IMU once per frame. We pass the accuracy into
         * the UI's calibration-wizard watchdog so the wizard auto-
         * enters / auto-exits based on fusion convergence, then read
         * the (possibly just-flipped) UI mode for dispatch. */
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
            pk_pfd_imu_t imu = {
                .valid     = have,
                .roll_deg  = have ? s.roll_deg  : 0.0f,
                .pitch_deg = have ? s.pitch_deg : 0.0f,
            };

            /* Same "fresh contact" 60s window the BLE GDL90 emitter uses. */
            size_t n_aircraft = aircraft_state_snapshot(
                scratch, sizeof(scratch) / sizeof(scratch[0]),
                esp_timer_get_time(),
                AIRCRAFT_STALE_AGE_US);

            pk_pfd_attitude_render(fb, &imu);
            pk_pfd_legacy_render(fb, &s, have, n_aircraft);
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
     * format strings can each chew 1 KiB on RISC-V, and the dashboard
     * line at the bottom of pfd_task uses both. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        pfd_task, "pfd", 6 * 1024, NULL, 4, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
