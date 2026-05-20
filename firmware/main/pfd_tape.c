/*
 * pfd_tape.c — Garmin G1000-style right-side altitude tape.
 *
 * Spec §3 geometry: 72 px wide tape band on the right of the attitude
 * region, with the center-value digit box (74 × 24) bleeding 2 px
 * left of the tape so it overlaps the attitude indicator like the
 * real G1000.
 *
 * Scale: 1 px = 5 ft. Minor ticks every 20 ft (4 px apart), major
 * ticks every 100 ft (20 px apart), labels every 200 ft.
 *
 * Stale handling: when `valid` is false we still draw the frame +
 * ticks (so the widget silhouette persists) but the center box shows
 * "----" in grey and tick labels are suppressed.
 */

#include "pfd_tape.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_draw.h"
#include "pfd_font.h"

#define TAPE_X0   248
#define TAPE_X1   320
#define TAPE_TOP   18
#define TAPE_BOT  138
#define TAPE_CY    78

#define MINOR_FT      20
#define MAJOR_FT     100
#define LABEL_EVERY  200

#define BOX_X0   246
#define BOX_X1   320
#define BOX_Y0    66
#define BOX_Y1    90

#define COL_BG         pk_rgb565(  8,   8,  12)
#define COL_BORDER_L   pk_rgb565( 70, 220, 250)   /* cyan left edge */
#define COL_TICK       pk_rgb565(220, 220, 220)
#define COL_LABEL      pk_rgb565(240, 240, 240)
#define COL_BOX_BRDR   pk_rgb565(255, 255, 255)
#define COL_VALUE      pk_rgb565(255, 255, 255)
#define COL_STALE      pk_rgb565(100, 100, 100)

void pk_pfd_alt_tape_render(uint16_t *fb, const pk_pfd_alt_tape_t *a)
{
    /* Semi-transparent dark band over the attitude background — pilot
     * still sees sky/ground through the tape but with enough contrast
     * for the ticks + labels. 50% darken (alpha=128). The 1 px cyan
     * left edge stays fully opaque as the visual divider. */
    pk_pfd_darken_rect(fb, TAPE_X0, TAPE_TOP, TAPE_X1, TAPE_BOT, 128);
    pk_pfd_fill_rect(fb, TAPE_X0, TAPE_TOP, TAPE_X0 + 1, TAPE_BOT, COL_BORDER_L);

    /* Walk minor ticks across the visible window: +- 300 ft around
     * center, snapped to a multiple of MINOR_FT. */
    int center_ft = a->valid ? a->altitude_ft : 5000;
    int low  = ((center_ft - 300) / MINOR_FT) * MINOR_FT;
    int high = ((center_ft + 300) / MINOR_FT) * MINOR_FT;
    for (int ft = low; ft <= high; ft += MINOR_FT) {
        /* 1 px = 5 ft → multiply ft-delta by 0.2 to get pixel offset. */
        int y = TAPE_CY - ((ft - center_ft) / 5);
        if (y < TAPE_TOP || y >= TAPE_BOT) continue;
        bool major = (ft % MAJOR_FT) == 0;
        int tick_len = major ? 10 : 4;
        pk_pfd_fill_rect(fb,
                         TAPE_X0 + 2, y,
                         TAPE_X0 + 2 + tick_len, y + 1,
                         COL_TICK);
        if (a->valid && major && (ft % LABEL_EVERY) == 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", ft);
            /* Right-justify the label inside the tape band, with the
             * digits sitting just left of the right panel edge. */
            int w = (int)strlen(buf) * 6;
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         TAPE_X1 - w - 2, y - 3, buf, COL_LABEL, 1);
        }
    }

    /* Center value box — fill, border, then digits. The box deliberately
     * bleeds 2 px left of the tape band so the rectangle straddles the
     * tape edge like the real G1000 ALT box. */
    pk_pfd_fill_rect(fb, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, COL_BG);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X1,     BOX_Y0 + 1, COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y1 - 1, BOX_X1,     BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X0 + 1, BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X1 - 1, BOX_Y0,     BOX_X1,     BOX_Y1,     COL_BOX_BRDR);

    if (a->valid) {
        char buf[8];
        int alt = a->altitude_ft;
        if (alt < 0)    alt = 0;
        if (alt > 9999) alt = 9999;
        snprintf(buf, sizeof(buf), "%4d", alt);
        /* 4 glyphs scale 3 = 72 px wide; box interior is 72 px;
         * 1 px left margin to clear the white border. */
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     BOX_X0 + 1, BOX_Y0 + 1, buf, COL_VALUE, 3);
    } else {
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     BOX_X0 + 1, BOX_Y0 + 1, "----", COL_STALE, 3);
    }
}
