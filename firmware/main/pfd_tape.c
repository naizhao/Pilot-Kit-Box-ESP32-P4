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

#define TAPE_X0   256       /* tape band: 64 px wide (was 72) */
#define TAPE_X1   320
#define TAPE_TOP   18
#define TAPE_BOT  168       /* shortened to y=168 to make room for the
                               three info boxes (BARO / metric / VS)
                               below the tape. TAPE_CY shifts from 113
                               to 93 automatically. */
#define TAPE_CY   ((TAPE_TOP + TAPE_BOT) / 2)   /* 93 */

#define MINOR_FT      20
#define MAJOR_FT     100
#define LABEL_EVERY  200

/* Center value box — scale-2 digits (12 px wide × 14 tall), 5 chars
 * for altitudes up to 99999 ft. Interior 60 px wide + 1 px border each
 * side + 2 px horizontal padding → 64 wide. Aligned flush with the
 * tape band (no bleed into the attitude indicator). */
#define BOX_X0   256
#define BOX_X1   320
#define BOX_Y0   (TAPE_CY - 10)   /* 103 */
#define BOX_Y1   (TAPE_CY + 10)   /* 123 */

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

    /* Walk minor ticks across the visible window: tape spans
     * (TAPE_BOT - TAPE_TOP) px at 5 ft/px = 950 ft, so ±475 ft around
     * center. Round up to 500 for clean tick layout. */
    int center_ft = a->valid ? a->altitude_ft : 5000;
    int low  = ((center_ft - 500) / MINOR_FT) * MINOR_FT;
    int high = ((center_ft + 500) / MINOR_FT) * MINOR_FT;
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

    /* Center value box — fill, border, then digits. Sits flush with
     * the tape band (no bleed) at scale-2 font so 5-digit altitudes
     * (up to 99999 ft) fit cleanly inside the 64 px box. */
    pk_pfd_fill_rect(fb, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, COL_BG);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X1,     BOX_Y0 + 1, COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y1 - 1, BOX_X1,     BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X0 + 1, BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X1 - 1, BOX_Y0,     BOX_X1,     BOX_Y1,     COL_BOX_BRDR);

    if (a->valid) {
        char buf[8];
        int alt = a->altitude_ft;
        if (alt < 0)     alt = 0;
        if (alt > 99999) alt = 99999;
        snprintf(buf, sizeof(buf), "%5d", alt);
        /* 5 glyphs scale 2 = 60 px wide; box interior 60 px; 2 px
         * horizontal padding (1 border + 1 visual breathing room);
         * 3 px top padding centers the 14-px digit cells in the 20-
         * px box interior. */
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             BOX_X0 + 2, BOX_Y0 + 3, buf, COL_VALUE);
    } else {
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             BOX_X0 + 2, BOX_Y0 + 3, "-----", COL_STALE);
    }
}
