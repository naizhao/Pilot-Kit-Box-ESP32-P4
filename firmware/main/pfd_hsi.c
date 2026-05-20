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

#define HDGBOX_X0      123
#define HDGBOX_Y0      138
#define HDGBOX_X1      197
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

        /* Labels only for the four cardinal directions — dropping the
         * intermediate "3"/"6"/"9"/"12"/"15"/etc. numeric labels keeps
         * the rose visually lighter. The font is already scale-1; we
         * compensate by labelling sparsely. */
        if (hdg == 0 || hdg == 90 || hdg == 180 || hdg == 270) {
            const char *lbl = (hdg == 0)   ? "N"
                            : (hdg == 90)  ? "E"
                            : (hdg == 180) ? "S"
                            :                "W";
            int lx = (int)((float)HSI_CX +
                           (float)(HSI_R - 15) * cosf(rad)) - 2;
            int ly = (int)((float)HSI_CY -
                           (float)(HSI_R - 15) * sinf(rad)) - 3;
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         lx, ly, lbl, COL_LABEL, 1);
        }
    }

    /* Magenta course line — a long vertical bar running through the
     * aircraft icon, from just below the HDG box to near the bottom
     * of the rose. Garmin uses this to indicate the current selected
     * course; we draw it fixed (always pointing "up" relative to the
     * rotating rose) until a real course source exists. */
    pk_pfd_fill_rect(fb,
                     HSI_CX - 1, HSI_TOP + 26,
                     HSI_CX + 2, HSI_BOT  -  4,
                     COL_HDG_BUG);
    /* Arrow head at the top of the course line. */
    pk_pfd_draw_triangle(fb,
                         HSI_CX,     HSI_TOP + 20,
                         HSI_CX - 5, HSI_TOP + 28,
                         HSI_CX + 5, HSI_TOP + 28,
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
        /* Interior 72 × 22 px fits 4 glyphs scale 3 (72 × 21) with
         * 0.5 px vertical breathing room (use y = HDGBOX_Y0 + 1). */
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     HDGBOX_X0 + 1, HDGBOX_Y0 + 1, buf, COL_LABEL, 3);
    } else {
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     HDGBOX_X0 + 1, HDGBOX_Y0 + 1, "---~", COL_STALE, 3);
    }
}
