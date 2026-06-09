/*
 * diag_page.c — "Diagnostics" screen renderer.
 *
 * Layout (320 × 240 landscape):
 *
 *   y =   0   ┌────────────────────────────────────────────────────┐
 *             │ DIAGNOSTICS             (cyan)                      │  header
 *   y =  24   ├────────────────────────────────────────────────────┤
 *             │ (sensors wiring up...)                              │  placeholder
 *             │                                                     │
 *   y = 240   └────────────────────────────────────────────────────┘
 *
 * Hardware status rows are added in Task 5 — this file is the skeleton
 * only (title bar + placeholder body).
 *
 * Pure pixel pushing — no I/O.
 */

#include "diag_page.h"

#include <stdint.h>

#include "display.h"
#include "text.h"

/* Layout */
#define DIAG_LEFT_PAD        6
#define DIAG_HEADER_UI_Y     6
#define DIAG_BODY_Y         32
#define DIAG_LINE_H         16

/* Palette — matches about_page.c palette for visual consistency */
#define COL_BG               pk_rgb565( 12,  12,  16)
#define COL_HEADER           pk_rgb565(180, 235, 255)
#define COL_DIM              pk_rgb565(140, 145, 155)
#define COL_DIVIDER          pk_rgb565( 90, 100, 120)

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

void pk_diag_page_render(uint16_t *fb)
{
    /* Clear background — may be coming from any other view. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* Header title */
    pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                    DIAG_LEFT_PAD, DIAG_HEADER_UI_Y,
                    "DIAGNOSTICS", COL_HEADER);

    /* Divider line under header */
    fill_rect(fb, 0, 22, PK_DISPLAY_W, 24, COL_DIVIDER);

    /* Placeholder body — Task 5 will replace this with real hardware rows */
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           DIAG_LEFT_PAD, DIAG_BODY_Y,
                           "(sensors wiring up...)", COL_DIM);
}
