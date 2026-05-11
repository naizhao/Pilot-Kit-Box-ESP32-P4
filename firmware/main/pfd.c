/*
 * pfd.c — minimal artificial horizon render task.
 *
 * The math we need for the horizon is small enough that I'm not pulling
 * in any rendering library: each output pixel just asks "am I above or
 * below the horizon line?" and picks a colour. The horizon line in
 * screen space is:
 *
 *     y - cy = tan(roll) * (x - cx) + (pitch_offset_px)
 *
 * where pitch_offset_px = pitch_deg * PIXELS_PER_DEGREE. We solve that
 * once per scanline and fill the row in two halves.
 *
 * Drawing target: display.c's PSRAM framebuffer, 240(W)×320(H) RGB565.
 */

#include "pfd.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "aircraft_state.h"
#include "display.h"
#include "imu_task.h"

static const char *TAG = "pfd";

/* --- Layout --------------------------------------------------------- */
/* Top 240 px: attitude indicator. Center reticle on (W/2, 120). */
#define PFD_ATTITUDE_H        240
#define PFD_PIXELS_PER_DEG    3      /* 30° pitch ≈ 90 px above/below cy */
#define PFD_BAR_Y0            (PK_DISPLAY_H - 40)   /* 280..320 px = status bar */

/* RGB565 palette (big-endian for the panel). Helpers in display.h. */
#define COL_SKY               pk_rgb565( 30, 130, 230)   /* daylight blue */
#define COL_GROUND            pk_rgb565(120,  85,  50)   /* warm brown */
#define COL_HORIZON           pk_rgb565(255, 255, 255)   /* clean white line */
#define COL_RETICLE           pk_rgb565(255, 215,   0)   /* aviation yellow */
#define COL_BAR_BG            pk_rgb565( 20,  20,  20)
#define COL_BAR_TICK          pk_rgb565( 80, 230,  80)
#define COL_BAR_LABEL_LO      pk_rgb565(180, 180, 180)
#define COL_BAR_LABEL_HI      pk_rgb565( 80, 230,  80)

/* --- Drawing helpers ------------------------------------------------- */

static inline void put_pixel(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= PK_DISPLAY_W || y < 0 || y >= PK_DISPLAY_H) return;
    fb[y * PK_DISPLAY_W + x] = c;
}

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y0 < 0) y0 = 0;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

/* --- Attitude indicator --------------------------------------------- */

static void draw_horizon(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float cx = PK_DISPLAY_W * 0.5f;
    const float cy = PFD_ATTITUDE_H * 0.5f;
    /* Slope of the horizon line in screen-y per screen-x.
     * tan(-roll) because we rotate the world the opposite way the
     * aircraft does, so a right-wing-down roll tips the horizon
     * down-on-the-right in the cockpit view. */
    const float rad = roll_deg * (float)M_PI / 180.0f;
    const float slope = -tanf(rad);
    /* Pitch offset: nose-up pushes the horizon DOWN on screen. */
    const float pitch_px = pitch_deg * PFD_PIXELS_PER_DEG;

    for (int y = 0; y < PFD_ATTITUDE_H; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            /* horizon screen-y at this x */
            float hy = cy + pitch_px + slope * ((float)x - cx);
            row[x] = ((float)y < hy) ? COL_SKY : COL_GROUND;
        }
    }

    /* Trace the horizon line itself (1 px white) to give the eye a
     * sharp edge — the per-pixel test alone leaves a slight stairstep. */
    for (int x = 0; x < PK_DISPLAY_W; ++x) {
        float hy = cy + pitch_px + slope * ((float)x - cx);
        int yi = (int)(hy + 0.5f);
        put_pixel(fb, x, yi, COL_HORIZON);
        put_pixel(fb, x, yi - 1, COL_HORIZON);
    }
}

/* --- Center reticle (the "aircraft" symbol that stays fixed) -------- */

static void draw_reticle(uint16_t *fb)
{
    const int cx = PK_DISPLAY_W / 2;
    const int cy = PFD_ATTITUDE_H / 2;

    /* Inverted-T crosshair, 3 px thick. Classic flight-deck shape:
     *
     *       │
     *    ───┴───
     *
     */
    fill_rect(fb, cx - 30, cy - 1, cx - 8,  cy + 2, COL_RETICLE);  /* left bar */
    fill_rect(fb, cx +  8, cy - 1, cx + 30, cy + 2, COL_RETICLE);  /* right bar */
    fill_rect(fb, cx -  1, cy - 1, cx +  2, cy + 9, COL_RETICLE);  /* center stem */
    /* small dot in centre */
    fill_rect(fb, cx - 2, cy - 2, cx + 3, cy + 3, COL_RETICLE);
}

/* --- ADS-B aircraft count bar -------------------------------------- *
 *
 * Renders one tile per fresh aircraft in the bottom 40-px band:
 *   - left section (0..40 px wide): a column header label area, kept
 *     dark for the v2 text to land in
 *   - right section: tiles, 8 px wide, 8 px tall, 2 px gap, wrapping
 *     across multiple rows if the count exceeds the bar width
 */

static void draw_aircraft_bar(uint16_t *fb, size_t aircraft_count)
{
    fill_rect(fb, 0, PFD_BAR_Y0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BAR_BG);

    /* Header rectangle (label "ADSB" goes here in 4c v2). */
    fill_rect(fb, 0, PFD_BAR_Y0, 48, PK_DISPLAY_H, COL_BAR_BG);
    /* Faint divider line. */
    fill_rect(fb, 48, PFD_BAR_Y0 + 4, 49, PK_DISPLAY_H - 4, COL_BAR_LABEL_LO);

    const int tile_w = 8, tile_h = 8, gap = 2;
    const int area_x = 56;
    int x = area_x;
    int y = PFD_BAR_Y0 + 4;
    /* Clamp to a sane upper bound so a bug never blows the buffer. */
    if (aircraft_count > 64) aircraft_count = 64;
    for (size_t i = 0; i < aircraft_count; ++i) {
        fill_rect(fb, x, y, x + tile_w, y + tile_h, COL_BAR_TICK);
        x += tile_w + gap;
        if (x + tile_w > PK_DISPLAY_W) {
            x = area_x;
            y += tile_h + gap;
            if (y + tile_h > PK_DISPLAY_H) break;
        }
    }
}

/* --- Render task ---------------------------------------------------- */

static void pfd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pfd_task running (target 30 FPS, single-buffer flush)");

    uint16_t *fb = pk_display_framebuffer();
    if (fb == NULL) {
        ESP_LOGE(TAG, "no framebuffer — exiting");
        vTaskDelete(NULL);
    }

    aircraft_t scratch[AIRCRAFT_TABLE_CAPACITY];
    int64_t  fps_window_start_us = esp_timer_get_time();
    uint32_t frames_in_window = 0;

    while (1) {
        TickType_t frame_start = xTaskGetTickCount();

        pk_imu_sample_t s;
        bool have = pk_imu_sample_get(&s);
        float roll  = have ? s.roll_deg  : 0.0f;
        float pitch = have ? s.pitch_deg : 0.0f;

        size_t n_aircraft = aircraft_state_snapshot(
            scratch, sizeof(scratch) / sizeof(scratch[0]),
            esp_timer_get_time());

        draw_horizon(fb, roll, pitch);
        draw_reticle(fb);
        draw_aircraft_bar(fb, n_aircraft);

        pk_display_flush_full();
        frames_in_window++;

        int64_t now = esp_timer_get_time();
        if (now - fps_window_start_us >= 1000000) {
            ESP_LOGI(TAG, "PFD %lu FPS  | roll=%+6.2f pitch=%+6.2f yaw=%6.2f"
                          "  imu_valid=%d  aircraft=%u",
                     (unsigned long)frames_in_window,
                     roll, pitch, have ? s.yaw_deg : 0.0f,
                     have, (unsigned)n_aircraft);
            frames_in_window = 0;
            fps_window_start_us = now;
        }

        /* Aim for ~33 ms / frame = 30 FPS. If render+flush already
         * blew the budget, fall through and immediately start the
         * next frame. */
        vTaskDelayUntil(&frame_start, pdMS_TO_TICKS(33));
    }
}

esp_err_t pk_pfd_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        pfd_task, "pfd", 4096, NULL, 4, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
