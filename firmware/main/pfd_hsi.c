/*
 * pfd_hsi.c — Garmin G1000-style HSI half-rose at the bottom of the
 * PFD. The rose rotates with yaw so the current heading is always at
 * the top of the visible arc. A boxed scale-3 numeric heading sits
 * above the rose center; a small white aircraft triangle marks the
 * rose center.
 */

#include "pfd_hsi.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_draw.h"
#include "pfd_font.h"

/* --- Layout ---------------------------------------------------------- *
 *
 * The HSI is fully transparent over the attitude background: the rose
 * ticks, cardinal labels, aircraft icon, and HDG box border draw
 * directly onto the sky/ground. Garmin G1000 keeps this region
 * unobstructed so the pilot still has a horizon reference around the
 * compass rose.
 */
#define HSI_TOP        138
#define HSI_BOT        240

/* Virtual center is *below* the panel so we only see the top half of
 * the rose — same G1000 trick we use for the bank arc. */
#define HSI_CX         160
#define HSI_CY         240
#define HSI_R           65

/* HDG numeric box — scale-2 digits (4 chars × 12 = 48 px wide × 14 tall).
 * Box interior 48×14 + 1 px border + small padding → 54×18.
 *
 * Y position: bottom stays at y=162 (same as the old scale-3 box's
 * bottom), top moves down to y=144. Keeps the gap from box-bottom to
 * the visible top of the rose (~y=175) consistent across the resize. */
#define HDGBOX_X0      133
#define HDGBOX_Y0      144
#define HDGBOX_X1      187
#define HDGBOX_Y1      162

/* Aircraft symbol sits near the bottom of the visible rose, slightly
 * above the panel's bottom edge — the Garmin convention is to put it
 * at the lower-middle of the half-rose, not the rose's mathematical
 * center (which is off-screen below). */
#define AIRCRAFT_Y     (HSI_BOT - 22)   /* y = 218 */

/* --- Palette ------------------------------------------------------- */
#define COL_TICK       pk_rgb565(220, 220, 220)
#define COL_LABEL      pk_rgb565(240, 240, 240)
#define COL_BORDER     pk_rgb565(255, 255, 255)
#define COL_AIRCRAFT   pk_rgb565(255, 255, 255)
#define COL_HDG_BUG    pk_rgb565(255,   0, 255)   /* magenta — Garmin course / heading bug */
#define COL_STALE      pk_rgb565(100, 100, 100)

void pk_pfd_hsi_render(uint16_t *fb, const pk_pfd_hsi_t *h)
{
    /* Fully transparent — the rose ticks, cardinal labels, aircraft
     * icon, and HDG box draw directly over the attitude indicator.
     * Garmin keeps the bottom compass area unobstructed so the pilot
     * still sees the sky/ground reference around the rose. */

    float yaw = h->imu_valid ? h->yaw_deg : 0.0f;

    /* Enumerate integer headings in 5° steps, project each onto the
     * rose by its offset from the current yaw. The visible top of the
     * arc corresponds to yaw itself. */
    for (int hdg = 0; hdg < 360; hdg += 5) {
        float delta = (float)hdg - yaw;
        while (delta >  180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        if (delta < -95.0f || delta > 95.0f) continue;

        /* Rose angle = 90° (top) minus delta. */
        float rose_deg = 90.0f - delta;
        float rad      = rose_deg * (float)M_PI / 180.0f;
        float cx = (float)HSI_CX + (float)HSI_R * cosf(rad);
        float cy = (float)HSI_CY - (float)HSI_R * sinf(rad);

        bool major30 = (hdg % 30) == 0;
        int tick_len = major30 ? 10 : 5;
        float tx = (float)HSI_CX + (float)(HSI_R - tick_len) * cosf(rad);
        float ty = (float)HSI_CY - (float)(HSI_R - tick_len) * sinf(rad);

        pk_pfd_draw_line(fb, (int)cx, (int)cy, (int)tx, (int)ty, COL_TICK);

        if (major30) {
            const char *lbl;
            char numbuf[4];
            switch (hdg) {
                case   0: lbl = "N"; break;
                case  90: lbl = "E"; break;
                case 180: lbl = "S"; break;
                case 270: lbl = "W"; break;
                default:
                    /* Garmin convention: label numeric headings by
                     * tens-of-degrees (30→"3", 120→"12", etc.). */
                    snprintf(numbuf, sizeof(numbuf), "%d", hdg / 10);
                    lbl = numbuf;
                    break;
            }
            int len   = (int)strlen(lbl);
            int lx = (int)((float)HSI_CX +
                           (float)(HSI_R - 15) * cosf(rad)) - len * 3;
            int ly = (int)((float)HSI_CY -
                           (float)(HSI_R - 15) * sinf(rad)) - 3;
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         lx, ly, lbl, COL_LABEL, 1);
        }
    }

    /* Magenta course line — a vertical bar running through the
     * aircraft icon. Tip sits 6 px below the HDG box bottom so the
     * arrow has clearance and doesn't poke into the framed digits;
     * line extends down to within 4 px of the panel edge. Garmin
     * uses this to indicate the current selected course; we draw
     * it fixed (always pointing "up" relative to the rotating rose)
     * until a real course source exists. */
    const int arrow_tip_y  = HDGBOX_Y1 + 6;             /* y = 168 */
    const int arrow_base_y = arrow_tip_y + 8;           /* y = 176, 8-px arrow */
    pk_pfd_fill_rect(fb,
                     HSI_CX - 1, arrow_base_y - 2,
                     HSI_CX + 2, HSI_BOT      - 4,
                     COL_HDG_BUG);
    pk_pfd_draw_triangle(fb,
                         HSI_CX,     arrow_tip_y,
                         HSI_CX - 5, arrow_base_y,
                         HSI_CX + 5, arrow_base_y,
                         COL_HDG_BUG);

    /* Aircraft silhouette — small fuselage + wings + tail, centered
     * on (HSI_CX, AIRCRAFT_Y). Drawn AFTER the magenta line so the
     * white icon overlays the line at the aircraft body. */
    {
        int cx = HSI_CX;
        int cy = AIRCRAFT_Y;
        /* Fuselage — 2 px wide vertical bar, 10 px tall, with a small
         * point at the nose. */
        pk_pfd_fill_rect(fb, cx - 1, cy - 5, cx + 2, cy + 5, COL_AIRCRAFT);
        pk_pfd_draw_triangle(fb,
                             cx,     cy - 7,
                             cx - 1, cy - 5,
                             cx + 2, cy - 5,
                             COL_AIRCRAFT);
        /* Wings — horizontal bar, 16 px wide, 2 px tall, slightly
         * forward of center. */
        pk_pfd_fill_rect(fb, cx - 8, cy - 1, cx + 9, cy + 1, COL_AIRCRAFT);
        /* Tail — short horizontal at the back. */
        pk_pfd_fill_rect(fb, cx - 4, cy + 4, cx + 5, cy + 6, COL_AIRCRAFT);
    }

    /* HDG box: transparent interior + 1 px white border + scale-3
     * digits. The interior shows the attitude background; the white
     * border anchors the box visually. */
    pk_pfd_fill_rect(fb, HDGBOX_X0,     HDGBOX_Y0,     HDGBOX_X1,     HDGBOX_Y0 + 1, COL_BORDER);
    pk_pfd_fill_rect(fb, HDGBOX_X0,     HDGBOX_Y1 - 1, HDGBOX_X1,     HDGBOX_Y1,     COL_BORDER);
    pk_pfd_fill_rect(fb, HDGBOX_X0,     HDGBOX_Y0,     HDGBOX_X0 + 1, HDGBOX_Y1,     COL_BORDER);
    pk_pfd_fill_rect(fb, HDGBOX_X1 - 1, HDGBOX_Y0,     HDGBOX_X1,     HDGBOX_Y1,     COL_BORDER);

    if (h->imu_valid) {
        char buf[8];
        int hdg = ((int)yaw + 360) % 360;
        snprintf(buf, sizeof(buf), "%03d~", hdg);
        /* 4 glyphs scale 2 = 48 × 14; box interior 52 × 16; center
         * with 3 px left padding + 2 px top padding. */
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     HDGBOX_X0 + 3, HDGBOX_Y0 + 2, buf, COL_LABEL, 2);
    } else {
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     HDGBOX_X0 + 3, HDGBOX_Y0 + 2, "---~", COL_STALE, 2);
    }
}
