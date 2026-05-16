/*
 * about_page.c — "About" screen renderer.
 *
 * Layout (240 × 320 portrait):
 *
 *   y =   0   ┌─────────────────────────────────────┐
 *             │ ABOUT                  (cyan)       │  header
 *   y =  20   ├─────────────────────────────────────┤
 *             │ Project : PILOT KIT BOX             │
 *             │ Version : abb7989-dirty             │
 *             │ Build   : May 16 2026 16:09:12      │
 *             │ ESP-IDF : v6.0.1                    │
 *             │ Board   : Waveshare ESP32-P4-WIFI6  │
 *             │ Chip rev: v1.3                      │
 *             │ Display : TK024F3036 240×320 @40MHz │
 *             │ IMU     : BNO085 (I²C0 @ 0x4A)      │
 *             │ Dongle  : RTL-SDR @ 2 MS/s          │
 *   y = ...   │                                     │
 *             ├─────────────────────────────────────┤
 *             │ IMU calibration: ●●○  (acc 2/3)     │
 *             │ (mag fusion converged — TARE-long-  │
 *             │  press to persist)                  │
 *   y = 320   └─────────────────────────────────────┘
 *
 * Pure pixel pushing — no I/O.
 */

#include "about_page.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#include "display.h"
#include "pfd_font.h"
#include "ui_state.h"

/* Layout */
#define ABOUT_LEFT_PAD       6
#define ABOUT_HEADER_Y       4
#define ABOUT_BODY_Y         24
#define ABOUT_LINE_H         14
#define ABOUT_CAL_Y          240
#define ABOUT_HINT_Y         260

/* Palette */
#define COL_BG               pk_rgb565( 12,  12,  16)
#define COL_HEADER           pk_rgb565( 80, 220, 240)
#define COL_KEY              pk_rgb565(160, 160, 160)
#define COL_VAL              pk_rgb565(240, 240, 240)
#define COL_DIM              pk_rgb565(120, 120, 120)
#define COL_DIVIDER          pk_rgb565( 60,  60,  70)
#define COL_CAL_DOT_OFF      pk_rgb565( 60,  60,  60)
#define COL_CAL_LOW          pk_rgb565(255,  80,  40)   /* red    — acc 0 */
#define COL_CAL_MID          pk_rgb565(255, 200,  60)   /* yellow — acc 1 */
#define COL_CAL_HIGH         pk_rgb565( 80, 220,  80)   /* green  — acc 2..3 */

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

static void draw_kv(uint16_t *fb, int y, const char *key, const char *val)
{
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ABOUT_LEFT_PAD, y, key, COL_KEY, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ABOUT_LEFT_PAD + 60, y, val, COL_VAL, 1);
}

static void draw_cal_indicator(uint16_t *fb, uint8_t accuracy)
{
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ABOUT_LEFT_PAD, ABOUT_CAL_Y, "IMU cal :",
                 COL_KEY, 1);

    /* Three small filled circles representing accuracy 1, 2, 3.
     * We approximate a circle with a small filled square (cheap). */
    int dot_size = 9;
    int dot_gap  = 3;
    int dot_x    = ABOUT_LEFT_PAD + 60;
    int dot_y    = ABOUT_CAL_Y - 1;

    for (int i = 0; i < 3; ++i) {
        uint16_t col;
        if (accuracy > (uint8_t)i) {
            col = (accuracy >= 3) ? COL_CAL_HIGH
                : (accuracy >= 2) ? COL_CAL_HIGH
                : (accuracy >= 1) ? COL_CAL_MID
                :                   COL_CAL_LOW;
        } else {
            col = COL_CAL_DOT_OFF;
        }
        int x0 = dot_x + i * (dot_size + dot_gap);
        fill_rect(fb, x0, dot_y, x0 + dot_size, dot_y + dot_size, col);
    }

    /* Numeric "(acc N/3)" suffix */
    char buf[16];
    snprintf(buf, sizeof(buf), "(acc %u/3)", accuracy);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 dot_x + 3 * (dot_size + dot_gap) + 6, ABOUT_CAL_Y,
                 buf, COL_VAL, 1);

    /* Hint line below */
    const char *hint;
    if (accuracy >= 2) {
        hint = "Fusion converged.";
    } else if (accuracy >= 1) {
        hint = "Converging - keep moving";
    } else {
        hint = "Move in figure-8 to calibrate";
    }
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ABOUT_LEFT_PAD, ABOUT_HINT_Y, hint, COL_DIM, 1);
}

void pk_about_page_render(uint16_t *fb)
{
    /* Clear background — may be coming from any other view. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* Header */
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ABOUT_LEFT_PAD, ABOUT_HEADER_Y, "ABOUT", COL_HEADER, 2);
    fill_rect(fb, 0, 20, PK_DISPLAY_W, 22, COL_DIVIDER);

    /* Body — key/value rows */
    int y = ABOUT_BODY_Y + 4;

    const esp_app_desc_t *app = esp_app_get_description();
    draw_kv(fb, y, "Project :", app ? app->project_name : "pilot_kit_box");
    y += ABOUT_LINE_H;
    draw_kv(fb, y, "Version :", app ? app->version : "?");
    y += ABOUT_LINE_H;

    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%s %s",
             app ? app->date : __DATE__,
             app ? app->time : __TIME__);
    draw_kv(fb, y, "Build   :", tmp);
    y += ABOUT_LINE_H;

    snprintf(tmp, sizeof(tmp), "v%d.%d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
    draw_kv(fb, y, "ESP-IDF :", tmp);
    y += ABOUT_LINE_H;

    draw_kv(fb, y, "Board   :", "Waveshare ESP32-P4-WIFI6");
    y += ABOUT_LINE_H;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(tmp, sizeof(tmp), "v%d.%d",
             chip.revision / 100, chip.revision % 100);
    draw_kv(fb, y, "Chip rev:", tmp);
    y += ABOUT_LINE_H;

    draw_kv(fb, y, "Display :", "TK024F3036 240x320 SPI 40MHz");
    y += ABOUT_LINE_H;

    draw_kv(fb, y, "IMU     :", "BNO085 (I2C0 0x4A)");
    y += ABOUT_LINE_H;

    draw_kv(fb, y, "Dongle  :", "RTL-SDR 2 MS/s @ 1090MHz");
    y += ABOUT_LINE_H;

    /* IMU calibration indicator */
    uint8_t acc = pk_ui_cal_wizard_last_accuracy();
    draw_cal_indicator(fb, acc);

    /* Footer divider */
    fill_rect(fb, 0, PK_DISPLAY_H - 14, PK_DISPLAY_W, PK_DISPLAY_H - 13,
              COL_DIVIDER);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ABOUT_LEFT_PAD, PK_DISPLAY_H - 10,
                 "MODE  to cycle    UP/DOWN  scroll",
                 COL_DIM, 1);
}
