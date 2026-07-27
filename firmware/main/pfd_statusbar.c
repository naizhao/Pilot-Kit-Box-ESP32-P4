/*
 * pfd_statusbar.c — G1000-style top status strip (18 px tall, full
 * panel width). Left: cyan "HDG" + green numeric heading from yaw.
 * Right: cyan "ADSB" + green count from aircraft_state_snapshot().
 *
 * Rendered every frame from the per-frame pk_pfd_status_t the PFD task
 * builds. Stale (no IMU) HDG renders as grey "---°".
 */

#include "pfd_statusbar.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_layout.h"
#include "pfd_draw.h"
#include "pfd_font.h"

#define STATUSBAR_TOP   0
#define STATUSBAR_BOT  PFD_BAR_BOT

#define COL_BG     pk_rgb565(  8,   8,  12)
#define COL_LABEL  pk_rgb565( 70, 220, 250)
#define COL_GREEN  pk_rgb565(  0, 220,  60)
#define COL_STALE  pk_rgb565(100, 100, 100)
#define COL_RED    pk_rgb565(255,  80,  60)

/* Centre of the unused mid-span x[90,232). */
#define MID_CENTRE_X (PFD_CX + 1)

void pk_pfd_statusbar_render(uint16_t *fb, const pk_pfd_status_t *s)
{
    pk_pfd_fill_rect(fb, 0, STATUSBAR_TOP, PK_DISPLAY_W, STATUSBAR_BOT, COL_BG);

    char buf[12];

    /* Left: "HDG" (cyan, scale 2 = 36 px) + " NNN°" (4 glyphs scale 2
     * = 48 px → ends at x≈90). */
    pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         6, 1, "HDG", COL_LABEL);
    if (s->imu_valid) {
        int hdg = ((int)s->yaw_deg + 360) % 360;
        snprintf(buf, sizeof(buf), "%03d~", hdg);
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             46, 1, buf, COL_GREEN);
    } else {
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                             46, 1, "---~", COL_STALE);
    }

    /* Centre x[90,232): GPS reception status.
     * Each cockpit glyph is 12 px wide (scale-2 fixed advance).
     * We build the string, compute its pixel width, then start it
     * so it is horizontally centred in the 142-px gap. */
    {
        uint16_t  gps_col;
        if (s->gps_have_fix) {
            snprintf(buf, sizeof(buf), "GPS (%u)", (unsigned)s->gps_sats);
            gps_col = COL_GREEN;
        } else {
            snprintf(buf, sizeof(buf), "NO GPS");
            gps_col = COL_RED;
        }
        /* strlen * 12 px per cockpit glyph, centred on x=161 */
        {
            int len = (int)strlen(buf);
            int gps_x = MID_CENTRE_X - (len * 12) / 2;
            pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                 gps_x, 1, buf, gps_col);
        }
    }

    /* Right: "ADSB NN" — right-justified-ish. "ADSB" = 4 glyphs × 12 =
     * 48 px at scale 2; we place it so the trailing NN sits flush
     * with x=312 leaving a 2-px right margin on the 320-wide panel. */
    pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         232, 1, "ADSB", COL_LABEL);
    snprintf(buf, sizeof(buf), "%2u", (unsigned)s->aircraft_count);
    pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         288, 1, buf, COL_GREEN);
}
