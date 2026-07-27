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
#include "pfd_layout.h"
#include "pfd_aa_text.h"
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
#define HSI_TOP        PFD_HSI_TOP
#define HSI_BOT        PFD_HSI_BOT

/* Virtual center is *below* the panel so we only see the top half of
 * the rose — same G1000 trick we use for the bank arc. */
#define HSI_CX         PFD_HSI_CX
#define HSI_CY         PFD_HSI_CY
#define HSI_R          PFD_HSI_R

/* HDG numeric box — scale-2 digits (4 chars × 12 = 48 px wide × 14 tall).
 * Box interior 48×14 + 1 px border + small padding → 54×18.
 *
 * Y position: bottom stays at y=162 (same as the old scale-3 box's
 * bottom), top moves down to y=144. Keeps the gap from box-bottom to
 * the visible top of the rose (~y=175) consistent across the resize. */
#define HDGBOX_X0      PFD_HDGBOX_X0
#define HDGBOX_Y0      PFD_HDGBOX_Y0
#define HDGBOX_X1      PFD_HDGBOX_X1
#define HDGBOX_Y1      PFD_HDGBOX_Y1
#define HDGBOX_BG_ALPHA 150

/* ── 字号分档 ──────────────────────────────────────────────────
 *
 * 罗盘刻度数字与航向框数字原本都是 5×7 位图 scale-1（6 px 宽），换算到
 * 4.3″ 屏只有 0.7 mm，远低于 spec §2 的 18 px 硬下限，实际是读不出来的。
 *
 * 航向框取 M 而不是 XL：顶栏已有一份 HDG 读数，这里是重复信息，让它压过
 * 高度/空速会打乱主次。原框 120×40 是照 M 档尺寸开的，但里面填的还是
 * 48 px 宽的 cockpit 字，于是空出 72 px —— 框看着大得没道理，正是这个原因。 */
#if PK_DISPLAY_W >= 800
#  define ROSE_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_S)
#  define ROSE_LBL_W      PK_AA_S_W
#  define ROSE_LBL_H      PK_AA_S_H
#  define ROSE_LBL_INSET  26
#  define HDG_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#  define HDGBOX_PAD_X    5
#  define HDGBOX_PAD_Y    3
#else
#  define ROSE_PUTS(fb, x, y, s, col) \
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), 1)
#  define ROSE_LBL_W      6
#  define ROSE_LBL_H      6
#  define ROSE_LBL_INSET  15
#  define HDG_PUTS(fb, x, y, s, col) \
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col))
#  define HDGBOX_PAD_X    3
#  define HDGBOX_PAD_Y    2
#endif

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

        pk_pfd_draw_line_aa(fb, cx, cy, tx, ty, 1.3f, COL_TICK);

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
            /* 标签沿半径向内退 ROSE_LBL_INSET，再按自身尺寸回退半个宽高，
             * 使字形中心落在那一点上。 */
            int len = (int)strlen(lbl);
            int lx = (int)((float)HSI_CX +
                           (float)(HSI_R - ROSE_LBL_INSET) * cosf(rad))
                     - len * ROSE_LBL_W / 2;
            int ly = (int)((float)HSI_CY -
                           (float)(HSI_R - ROSE_LBL_INSET) * sinf(rad))
                     - ROSE_LBL_H / 2;
            ROSE_PUTS(fb, lx, ly, lbl, COL_LABEL);
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

    /* HDG box: dimmed translucent interior + 1 px white border +
     * scale-2 digits. The darkened pad keeps the heading readable
     * without fully blocking the attitude background. */
    pk_pfd_darken_rect(fb, HDGBOX_X0 + 1, HDGBOX_Y0 + 1,
                       HDGBOX_X1 - 1, HDGBOX_Y1 - 1,
                       HDGBOX_BG_ALPHA);
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
        HDG_PUTS(fb, HDGBOX_X0 + HDGBOX_PAD_X, HDGBOX_Y0 + HDGBOX_PAD_Y,
                 buf, COL_LABEL);
    } else {
        HDG_PUTS(fb, HDGBOX_X0 + HDGBOX_PAD_X, HDGBOX_Y0 + HDGBOX_PAD_Y,
                 "---~", COL_STALE);
    }
}
