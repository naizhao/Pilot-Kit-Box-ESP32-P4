/*
 * pfd_speed_tape.c — Garmin G1000-style left-side ground-speed tape.
 *
 * Mirrors pfd_tape.c (ALT tape) geometry to the left edge:
 *   • Tape band  x ∈ [0, 64),  y ∈ [18, 168)  — 64 px wide, 150 px tall
 *   • Metric pad x ∈ [0, 64),  y ∈ [170, 208) — km/h + mph conversions
 *   • Right-edge 1 px cyan divider  (x = 63)   — mirrors ALT left-edge cyan
 *   • Ticks grow LEFTward from the cyan edge (中心侧) — 镜像 ALT 刻度方向
 *   • Labels left-justified at x = 2 (屏幕边),文字在左、刻度在右
 *   • Centre value box at y ∈ [STAPE_CY-10, STAPE_CY+10] — derived from tape centre
 *
 * Scale: 1 px = 2 kt.  Minor ticks every 5 kt (2.5 px → 1 px steps),
 * major ticks every 25 kt, labels every 50 kt.
 *
 * Stale handling: frame + ticks always draw; when valid=false the centre
 * box shows "---" in grey and tick labels are suppressed.
 */

#include "pfd_speed_tape.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_draw.h"
#include "pfd_font.h"

/* ── geometry ─────────────────────────────────────────────────────────── */
#define STAPE_X0     0
#define STAPE_X1    64
#define STAPE_TOP   18
#define STAPE_BOT  168      /* tape band bottom */
#define STAPE_CY   ((STAPE_TOP + STAPE_BOT) / 2)   /* 93 */

/* Metric pad sits immediately below the tape band */
#define METRIC_TOP  170
#define METRIC_BOT  208

/* 对齐 ALT 刻度密度(ALT minor 4px/major 20px):minor 10kt=5px、major 50kt=25px */
#define MINOR_KT    10
#define MAJOR_KT    50
#define LABEL_EVERY 50

/* Pixels per knot (1 px = 2 kt → 0.5 px/kt; use integer: 2 kt = 1 px) */
#define KT_PER_PX   2

/* Centre value box — derived from tape geometry centre (mirrors pfd_tape.c) */
#define BOX_X0   STAPE_X0
#define BOX_X1   STAPE_X1
#define BOX_Y0   (STAPE_CY - 10)   /* 83 */
#define BOX_Y1   (STAPE_CY + 10)   /* 103 */

/* ── colours ────────────────────────────────────────────────────────── */
#define COL_BG         pk_rgb565(  8,   8,  12)
#define COL_BORDER_R   pk_rgb565( 70, 220, 250)   /* cyan right edge */
#define COL_TICK       pk_rgb565(220, 220, 220)
#define COL_LABEL      pk_rgb565(240, 240, 240)
#define COL_BOX_BRDR   pk_rgb565(255, 255, 255)
#define COL_VALUE      pk_rgb565(255, 255, 255)
#define COL_STALE      pk_rgb565(100, 100, 100)
#define COL_METRIC     pk_rgb565(180, 180, 180)
#define COL_METRIC_ST  pk_rgb565( 80,  80,  80)

/* ── render ─────────────────────────────────────────────────────────── */
void pk_pfd_speed_tape_render(uint16_t *fb, const pk_pfd_speed_tape_t *s)
{
    /* Semi-transparent dark band over the attitude background. */
    pk_pfd_darken_rect(fb, STAPE_X0, STAPE_TOP, STAPE_X1, STAPE_BOT, 128);

    /* Right-edge 1 px cyan divider (mirrors ALT tape left-edge cyan). */
    pk_pfd_fill_rect(fb, STAPE_X1 - 1, STAPE_TOP, STAPE_X1, STAPE_BOT, COL_BORDER_R);

    /* Walk minor ticks across the visible window.
     * Tape spans (STAPE_BOT - STAPE_TOP) = 150 px at 2 kt/px = 300 kt total.
     * Show ±150 kt around centre; clamp minimum to 0. */
    int center_kt = s->valid ? s->ground_speed_kt : 60;
    if (center_kt < 0) center_kt = 0;
    int low  = ((center_kt - 150) / MINOR_KT) * MINOR_KT;
    int high = ((center_kt + 150) / MINOR_KT) * MINOR_KT;
    if (low < 0) low = 0;

    for (int kt = low; kt <= high; kt += MINOR_KT) {
        /* 1 px = 2 kt → pixel offset = (kt_delta) / KT_PER_PX */
        int y = STAPE_CY - ((kt - center_kt) / KT_PER_PX);
        if (y < STAPE_TOP || y >= STAPE_BOT) continue;

        bool major    = (kt % MAJOR_KT) == 0;
        int  tick_len = major ? 10 : 4;

        /* 镜像 ALT:刻度贴 cyan 边(中心侧 x63)朝左伸,文字留在左(屏幕边) */
        pk_pfd_fill_rect(fb,
                         STAPE_X1 - 1 - tick_len, y,
                         STAPE_X1 - 1, y + 1,
                         COL_TICK);

        if (s->valid && major && (kt % LABEL_EVERY) == 0 && kt > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", kt);
            /* Left-justify label: start at x=2, clear of the tick */
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         STAPE_X0 + 2, y - 3, buf, COL_LABEL, 1);
        }
    }

    /* ── Centre value box ─────────────────────────────────────────── */
    pk_pfd_fill_rect(fb, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, COL_BG);
    /* Border: top, bottom, left, right */
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X1,     BOX_Y0 + 1, COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y1 - 1, BOX_X1,     BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X0 + 1, BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X1 - 1, BOX_Y0,     BOX_X1,     BOX_Y1,     COL_BOX_BRDR);

    if (s->valid) {
        char buf[8];
        int gs = s->ground_speed_kt;
        if (gs < 0)   gs = 0;
        if (gs > 999) gs = 999;
        snprintf(buf, sizeof(buf), "%3d", gs);
        /* 3 glyphs × 12 px = 36 px; box is 64 px wide → 2 px border each
         * side leaves 60 px interior; centre at x = 1 + (60-36)/2 = 13 */
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             BOX_X0 + 13, BOX_Y0 + 3, buf, COL_VALUE);
    } else {
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             BOX_X0 + 13, BOX_Y0 + 3, "---", COL_STALE);
    }

    /* ── Metric conversion pad ────────────────────────────────────── */
    pk_pfd_darken_rect(fb, STAPE_X0, METRIC_TOP, STAPE_X1, METRIC_BOT, 128);

    if (s->valid) {
        int kmh = (int)(s->ground_speed_kt * 1.852f + 0.5f);
        int mph = (int)(s->ground_speed_kt * 1.15078f + 0.5f);
        char buf[16];

        /* Line 1: km/h */
        snprintf(buf, sizeof(buf), "%d km/h", kmh);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     STAPE_X0 + 2, METRIC_TOP + 3, buf, COL_METRIC, 1);

        /* Line 2: mph */
        snprintf(buf, sizeof(buf), "%d mph", mph);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     STAPE_X0 + 2, METRIC_TOP + 13, buf, COL_METRIC, 1);
    } else {
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     STAPE_X0 + 2, METRIC_TOP + 3,  "-- km/h", COL_METRIC_ST, 1);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     STAPE_X0 + 2, METRIC_TOP + 13, "-- mph",  COL_METRIC_ST, 1);
    }
}
