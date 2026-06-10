/*
 * about_page.c — "About" screen renderer.
 *
 * Layout (320 × 240 landscape):
 *
 *   y =   0   ┌────────────────────────────────────────────────────┐
 *             │ ABOUT                  (cyan)                       │  header
 *   y =  20   ├────────────────────────────────────────────────────┤
 *             │ Project : PILOT KIT BOX                             │
 *             │ Version : v0.5.0-ce49d37 (version.txt + git sha)    │
 *             │ Build   : May 20 2026 14:30:00                      │
 *             │ ESP-IDF : v6.0.1                                    │
 *             │ Board   : Waveshare ESP32-P4-WIFI6                  │
 *             │ Chip rev: v1.3                                      │
 *             │ Display : TK024F3036 320×240 SPI 40MHz              │
 *             │ IMU     : BNO085 (I²C0 @ 0x4A)                      │
 *             │ Dongle  : RTL-SDR @ 2 MS/s                          │
 *   y = 170   │ IMU cal : ●●○ (acc 2/3)                             │
 *   y = 190   │ (Fusion converged.)                                 │
 *   y = 226   ├────────────────────────────────────────────────────┤
 *             │ MODE to cycle    UP/DOWN scroll                     │
 *   y = 240   └────────────────────────────────────────────────────┘
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
#include "i18n.h"
#include "text.h"
#include "ui_state.h"

/* Layout */
#define ABOUT_LEFT_PAD       6
#define ABOUT_HEADER_TITLE_Y 4
#define ABOUT_HEADER_UI_Y    6
#define ABOUT_BODY_Y         32
#define ABOUT_LINE_H         16
#define ABOUT_CAL_GAP        6
#define ABOUT_HINT_GAP       16
#define ABOUT_FOOTER_Y       224
#define ABOUT_COLON_X        84
#define ABOUT_VALUE_X        100

/* Palette */
#define COL_BG               pk_rgb565( 12,  12,  16)
#define COL_HEADER           pk_rgb565(180, 235, 255)
#define COL_KEY              pk_rgb565(180, 235, 255)
#define COL_VAL              pk_rgb565(255, 255, 255)
#define COL_DIM              pk_rgb565(255, 255, 255)
#define COL_DIVIDER          pk_rgb565( 90, 100, 120)
#define COL_CAL_DOT_OFF      pk_rgb565(105, 110, 115)
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

static void draw_kv(uint16_t *fb, int y, pk_tr_id_t key_id, const char *val)
{
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           ABOUT_LEFT_PAD, y, pk_i18n_text(key_id), COL_KEY);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           ABOUT_COLON_X, y, ":", COL_KEY);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           ABOUT_VALUE_X, y, val, COL_VAL);
}

static void draw_cal_indicator(uint16_t *fb, int y, uint8_t accuracy)
{
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           ABOUT_LEFT_PAD, y, pk_i18n_text(PK_TR_ABOUT_CAL),
                           COL_KEY);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           ABOUT_COLON_X, y, ":", COL_KEY);

    /* Three small filled circles representing accuracy 1, 2, 3.
     * We approximate a circle with a small filled square (cheap). */
    int dot_size = 10;
    int dot_gap  = 4;
    int dot_x    = ABOUT_VALUE_X;
    int dot_y    = y;

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
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           dot_x + 3 * (dot_size + dot_gap) + 8, y,
                           buf, COL_VAL);

    /* Hint line below */
    const char *hint;
    if (accuracy >= 2) {
        hint = pk_i18n_text(PK_TR_ABOUT_HINT_CONVERGED);
    } else if (accuracy >= 1) {
        hint = pk_i18n_text(PK_TR_ABOUT_HINT_CONVERGING);
    } else {
        hint = pk_i18n_text(PK_TR_ABOUT_HINT_FIG8);
    }
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           ABOUT_LEFT_PAD, y + ABOUT_HINT_GAP, hint, COL_DIM);
}

void pk_about_page_render(uint16_t *fb)
{
    pk_lang_t lang = pk_i18n_get_lang();

    /* Clear background — may be coming from any other view. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* Body — key/value rows */
    int y = ABOUT_BODY_Y - pk_ui_about_scroll_y();

    const esp_app_desc_t *app = esp_app_get_description();
    draw_kv(fb, y, PK_TR_ABOUT_PROJECT, app ? app->project_name : "pilot_kit_box");
    y += ABOUT_LINE_H;
    draw_kv(fb, y, PK_TR_ABOUT_VERSION, app ? app->version : "?");
    y += ABOUT_LINE_H;

    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%s %s",
             app ? app->date : __DATE__,
             app ? app->time : __TIME__);
    draw_kv(fb, y, PK_TR_ABOUT_BUILD, tmp);
    y += ABOUT_LINE_H;

    snprintf(tmp, sizeof(tmp), "v%d.%d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
    draw_kv(fb, y, PK_TR_ABOUT_IDF, tmp);
    y += ABOUT_LINE_H;

    draw_kv(fb, y, PK_TR_ABOUT_BOARD, "Waveshare ESP32-P4-WIFI6");
    y += ABOUT_LINE_H;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(tmp, sizeof(tmp), "v%d.%d",
             chip.revision / 100, chip.revision % 100);
    draw_kv(fb, y, PK_TR_ABOUT_CHIP, tmp);
    y += ABOUT_LINE_H;

    draw_kv(fb, y, PK_TR_ABOUT_DISPLAY, "TK024F3036 320x240 SPI");
    y += ABOUT_LINE_H;

    draw_kv(fb, y, PK_TR_ABOUT_IMU, "BNO085 (I2C0 0x4A)");
    y += ABOUT_LINE_H;

    draw_kv(fb, y, PK_TR_ABOUT_DONGLE, "RTL-SDR 2 MS/s @ 1090MHz");
    y += ABOUT_LINE_H;

    /* IMU calibration indicator */
    uint8_t acc = pk_ui_cal_wizard_last_accuracy();
    draw_cal_indicator(fb, y + ABOUT_CAL_GAP, acc);

    /* Header and footer are fixed overlays over the scrollable body. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, 26, COL_BG);
    if (lang == PK_LANG_ZH) {
        pk_text_puts_page_title(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                ABOUT_LEFT_PAD, ABOUT_HEADER_TITLE_Y,
                                pk_i18n_text(PK_TR_ABOUT_TITLE), COL_HEADER);
    } else {
        pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        ABOUT_LEFT_PAD, ABOUT_HEADER_UI_Y,
                        pk_i18n_text(PK_TR_ABOUT_TITLE), COL_HEADER);
    }
    fill_rect(fb, 0, 22, PK_DISPLAY_W, 24, COL_DIVIDER);

    fill_rect(fb, 0, ABOUT_FOOTER_Y - 3, PK_DISPLAY_W, PK_DISPLAY_H,
              COL_BG);
    fill_rect(fb, 0, ABOUT_FOOTER_Y - 2, PK_DISPLAY_W, ABOUT_FOOTER_Y - 1,
              COL_DIVIDER);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           ABOUT_LEFT_PAD, ABOUT_FOOTER_Y,
                           pk_i18n_text(PK_TR_ABOUT_FOOTER), COL_DIM);
}
