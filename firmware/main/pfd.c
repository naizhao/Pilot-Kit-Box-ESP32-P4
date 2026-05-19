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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#include "ui_state.h"

static const char *TAG = "pfd";

/* --- Layout constants (legacy widgets) ------------------------------ */
#define PFD_HEADING_TOP         240
#define PFD_HEADING_BOT         270
#define PFD_PANEL_TOP           270
#define PFD_PANEL_BOT           320

/* Heading tape pixels per degree (legacy widget). */
#define HDG_PX_PER_DEG          3

/* Legacy attitude center, still referenced by the heading-tape caret
 * positioning until phase D replaces the tape. */
#define PFD_CX                  (PK_DISPLAY_W / 2)

/* --- Palette (RGB565, panel byte order) — legacy widgets ----------- */
#define COL_PANEL_BG            pk_rgb565( 12,  12,  16)
#define COL_TAPE_BG             pk_rgb565( 20,  20,  28)
#define COL_TAPE_TICK           pk_rgb565(200, 200, 200)
#define COL_TAPE_LABEL          pk_rgb565( 80, 220, 240)
#define COL_TAPE_CARET          pk_rgb565(255, 215,   0)
#define COL_LABEL               pk_rgb565( 80, 220, 240)
#define COL_VALUE               pk_rgb565(240, 240, 240)
#define COL_ACCENT              pk_rgb565(255, 215,   0)

/* --- Heading tape ---------------------------------------------------- *
 *
 * Bottom strip showing ±40° of compass heading around the current yaw,
 * scrolling sideways as the airplane turns. Major ticks at every 10°
 * with three-digit labels every 30°. A yellow caret in the middle
 * points to the current heading.
 */

static void draw_heading_tape(uint16_t *fb, float yaw_deg)
{
    pk_pfd_fill_rect(fb, 0, PFD_HEADING_TOP, PK_DISPLAY_W, PFD_HEADING_BOT, COL_TAPE_BG);

    /* Each visible degree maps to HDG_PX_PER_DEG pixels. We need every
     * heading h such that |h - yaw_deg| (modulo 360) <= half-window. */
    const int half_window = PK_DISPLAY_W / (2 * HDG_PX_PER_DEG) + 5;

    int yaw_floor = (int)floorf(yaw_deg);
    for (int dh = -half_window; dh <= half_window; ++dh) {
        int hdg = ((yaw_floor + dh) % 360 + 360) % 360;
        int x = PFD_CX + (int)(((float)hdg - yaw_deg + (float)dh) * HDG_PX_PER_DEG / 1.0f + 0.5f);
        /* Simpler: position relative to the centre by dh degrees offset. */
        x = PFD_CX + dh * HDG_PX_PER_DEG -
            (int)((yaw_deg - (float)yaw_floor) * HDG_PX_PER_DEG + 0.5f);
        if (x < -10 || x > PK_DISPLAY_W + 10) continue;
        bool major = (hdg % 10) == 0;
        bool labelled = (hdg % 30) == 0;
        int tick_h = major ? 6 : 3;
        pk_pfd_fill_rect(fb, x, PFD_HEADING_TOP, x + 1,
                  PFD_HEADING_TOP + tick_h, COL_TAPE_TICK);
        if (labelled) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%03d", hdg);
            int w = (int)strlen(buf) * PK_FONT_CELL_W(1);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         x - w / 2, PFD_HEADING_TOP + 9, buf,
                         COL_TAPE_LABEL, 1);
        }
    }
    /* Centre caret pointing down to the heading exactly under it. */
    int cx = PFD_CX;
    pk_pfd_draw_triangle(fb,
                  cx,      PFD_HEADING_BOT - 1,
                  cx - 5,  PFD_HEADING_BOT - 8,
                  cx + 5,  PFD_HEADING_BOT - 8,
                  COL_TAPE_CARET);
}

/* --- Bottom panel: numeric readouts ---------------------------------- */

static void draw_panel_text(uint16_t *fb,
                            const pk_imu_sample_t *s, bool imu_valid,
                            size_t aircraft_count)
{
    pk_pfd_fill_rect(fb, 0, PFD_PANEL_TOP, PK_DISPLAY_W, PFD_PANEL_BOT, COL_PANEL_BG);

    char buf[16];

    /* Row 1: roll on the left, pitch on the right. Scale 2 = 10×14 px
     * characters, ~6 chars wide → ~72 px per readout. */
    const int row1_y = PFD_PANEL_TOP + 4;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, row1_y,
                 "R", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%+6.1f~",
             imu_valid ? s->roll_deg : 0.0f);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 28, row1_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 124, row1_y,
                 "P", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%+6.1f~",
             imu_valid ? s->pitch_deg : 0.0f);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 148, row1_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    /* Row 2: HDG and ADSB count. */
    const int row2_y = PFD_PANEL_TOP + 26;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, row2_y,
                 "HDG", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%03d~",
             imu_valid ? ((int)s->yaw_deg + 360) % 360 : 0);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 52, row2_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 124, row2_y,
                 "ADSB", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%2u", (unsigned)aircraft_count);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 188, row2_y,
                 buf, COL_ACCENT, 2);
}

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
            float yaw = have ? s.yaw_deg : 0.0f;

            /* Same "fresh contact" 60s window the BLE GDL90 emitter uses. */
            size_t n_aircraft = aircraft_state_snapshot(
                scratch, sizeof(scratch) / sizeof(scratch[0]),
                esp_timer_get_time(),
                AIRCRAFT_STALE_AGE_US);

            pk_pfd_attitude_render(fb, &imu);
            draw_heading_tape(fb, yaw);
            draw_panel_text(fb, &s, have, n_aircraft);
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
