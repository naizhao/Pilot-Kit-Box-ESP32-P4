/*
 * cal_wizard.c — figure-8 motion prompt for BNO085 magnetometer
 * calibration.
 *
 * The figure-8 path is a Bernoulli lemniscate:
 *
 *     x(t) = a · cos(t) / (1 + sin²(t))
 *     y(t) = a · sin(t)·cos(t) / (1 + sin²(t))
 *
 * Static outline is drawn dim, with one bright animated dot tracing
 * the path at ~3 s/loop so the user has something to follow with
 * their hand.
 */

#include "cal_wizard.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "display.h"
#include "pfd_font.h"
#include "ui_state.h"

/* Layout */
#define WZ_FIG8_CX           (PK_DISPLAY_W / 2)
#define WZ_FIG8_CY           140
#define WZ_FIG8_R            70           /* horizontal half-extent of the
                                             figure-8 in pixels */
#define WZ_FIG8_VSCALE       2.0f         /* multiplier on y so the figure
                                             looks taller than its raw 1:1
                                             aspect ratio */
#define WZ_FIG8_OUTLINE_PTS  240          /* sample count for the dim outline */
#define WZ_FIG8_PERIOD_US    3000000      /* one full pass every 3 s */

#define WZ_INSTR_Y           220
#define WZ_BAR_Y             266
#define WZ_BAR_W             200
#define WZ_BAR_H             16
#define WZ_BAR_X             ((PK_DISPLAY_W - WZ_BAR_W) / 2)

/* Palette */
#define COL_BG               pk_rgb565( 12,  12,  16)
#define COL_HEADER           pk_rgb565( 80, 220, 240)
#define COL_OUTLINE          pk_rgb565( 80,  80,  90)
#define COL_DOT              pk_rgb565(255, 215,   0)
#define COL_INSTR            pk_rgb565(220, 220, 220)
#define COL_INSTR_DIM        pk_rgb565(140, 140, 140)
#define COL_BAR_BG           pk_rgb565( 40,  40,  50)
#define COL_BAR_LOW          pk_rgb565(255,  80,  40)
#define COL_BAR_MID          pk_rgb565(255, 200,  60)
#define COL_BAR_HIGH         pk_rgb565( 80, 220,  80)

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

static inline void put_pixel(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= PK_DISPLAY_W || y < 0 || y >= PK_DISPLAY_H) return;
    fb[y * PK_DISPLAY_W + x] = c;
}

static void fig8_point(float t, int *x, int *y)
{
    float sin_t = sinf(t);
    float cos_t = cosf(t);
    float denom = 1.0f + sin_t * sin_t;
    *x = (int)(WZ_FIG8_CX + WZ_FIG8_R * cos_t / denom + 0.5f);
    *y = (int)(WZ_FIG8_CY + WZ_FIG8_R * sin_t * cos_t / denom *
                WZ_FIG8_VSCALE + 0.5f);
}

static void draw_outline(uint16_t *fb)
{
    /* Dim outline: sample N points along t∈[0,2π) and plot each as a
     * 2×2 dot for visibility. We don't worry about pixel-perfect
     * antialiasing — the dot will draw over the path each frame. */
    for (int i = 0; i < WZ_FIG8_OUTLINE_PTS; ++i) {
        float t = (float)i * 2.0f * (float)M_PI / WZ_FIG8_OUTLINE_PTS;
        int x, y;
        fig8_point(t, &x, &y);
        put_pixel(fb, x,     y,     COL_OUTLINE);
        put_pixel(fb, x + 1, y,     COL_OUTLINE);
        put_pixel(fb, x,     y + 1, COL_OUTLINE);
    }
}

static void draw_animated_dot(uint16_t *fb)
{
    int64_t t_us = esp_timer_get_time();
    float phase = (float)((t_us % WZ_FIG8_PERIOD_US)) /
                  (float)WZ_FIG8_PERIOD_US;
    float t = phase * 2.0f * (float)M_PI;
    int x, y;
    fig8_point(t, &x, &y);
    /* A 5×5 filled square as the "dot" — chunky enough to track
     * easily from arm's length on the 2.4" panel. */
    fill_rect(fb, x - 2, y - 2, x + 3, y + 3, COL_DOT);
}

static void draw_progress_bar(uint16_t *fb, uint8_t accuracy)
{
    /* Bar background */
    fill_rect(fb, WZ_BAR_X, WZ_BAR_Y,
              WZ_BAR_X + WZ_BAR_W, WZ_BAR_Y + WZ_BAR_H, COL_BAR_BG);
    /* Filled fraction (acc / 3) — pick colour by current quality */
    uint16_t fill_col = (accuracy >= 2) ? COL_BAR_HIGH
                      : (accuracy >= 1) ? COL_BAR_MID
                      :                   COL_BAR_LOW;
    int filled_w = (WZ_BAR_W - 4) * (int)accuracy / 3;
    if (filled_w > 0) {
        fill_rect(fb, WZ_BAR_X + 2, WZ_BAR_Y + 2,
                  WZ_BAR_X + 2 + filled_w, WZ_BAR_Y + WZ_BAR_H - 2,
                  fill_col);
    }

    /* Numeric label below the bar */
    char buf[40];
    snprintf(buf, sizeof(buf), "Quality:  %u / 3", accuracy);
    int label_w = (int)strlen(buf) * PK_FONT_CELL_W(1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 (PK_DISPLAY_W - label_w) / 2, WZ_BAR_Y + WZ_BAR_H + 4,
                 buf, COL_INSTR, 1);
}

void pk_cal_wizard_render(uint16_t *fb)
{
    /* Solid clear — wizard owns the full screen. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* Header */
    const char *hdr = "COMPASS CAL";
    int hdr_w = (int)strlen(hdr) * PK_FONT_CELL_W(2);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 (PK_DISPLAY_W - hdr_w) / 2, 8, hdr, COL_HEADER, 2);

    /* Figure-8 outline + moving dot */
    draw_outline(fb);
    draw_animated_dot(fb);

    /* Instruction lines */
    const char *line1 = "Move device in a figure-8";
    const char *line2 = "rotating in all directions";
    int w1 = (int)strlen(line1) * PK_FONT_CELL_W(1);
    int w2 = (int)strlen(line2) * PK_FONT_CELL_W(1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 (PK_DISPLAY_W - w1) / 2, WZ_INSTR_Y,
                 line1, COL_INSTR, 1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 (PK_DISPLAY_W - w2) / 2, WZ_INSTR_Y + 12,
                 line2, COL_INSTR, 1);

    /* Progress bar driven by latest accuracy */
    uint8_t acc = pk_ui_cal_wizard_last_accuracy();
    draw_progress_bar(fb, acc);

    /* Footer hint */
    const char *foot = "Press MODE to skip";
    int fw = (int)strlen(foot) * PK_FONT_CELL_W(1);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 (PK_DISPLAY_W - fw) / 2, PK_DISPLAY_H - 14,
                 foot, COL_INSTR_DIM, 1);
}
