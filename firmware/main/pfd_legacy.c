/*
 * pfd_legacy.c — transitional home for the heading-tape + numeric
 * text-panel widgets that pre-dated the G1000 redesign. Phase D of
 * the redesign deletes this file once pfd_statusbar + pfd_hsi land.
 *
 * Kept here in isolation so the deletion in phase D is one commit
 * removing this file + its CMakeLists entry, no cross-cutting edits
 * inside pfd.c.
 */

#include "pfd_legacy.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_draw.h"
#include "pfd_font.h"

/* --- Layout constants ----------------------------------------------- */
#define PFD_HEADING_TOP        240
#define PFD_HEADING_BOT        270
#define PFD_PANEL_TOP          270
#define PFD_PANEL_BOT          320

#define PFD_CX                 (PK_DISPLAY_W / 2)
#define HDG_PX_PER_DEG         3

/* --- Palette -------------------------------------------------------- */
#define COL_PANEL_BG           pk_rgb565( 12,  12,  16)
#define COL_TAPE_BG            pk_rgb565( 20,  20,  28)
#define COL_TAPE_TICK          pk_rgb565(200, 200, 200)
#define COL_TAPE_LABEL         pk_rgb565( 80, 220, 240)
#define COL_TAPE_CARET         pk_rgb565(255, 215,   0)
#define COL_LABEL              pk_rgb565( 80, 220, 240)
#define COL_VALUE              pk_rgb565(240, 240, 240)
#define COL_ACCENT             pk_rgb565(255, 215,   0)

/* --- Heading tape --------------------------------------------------- */

static void draw_heading_tape(uint16_t *fb, float yaw_deg)
{
    pk_pfd_fill_rect(fb, 0, PFD_HEADING_TOP, PK_DISPLAY_W, PFD_HEADING_BOT,
                     COL_TAPE_BG);

    const int half_window = PK_DISPLAY_W / (2 * HDG_PX_PER_DEG) + 5;

    int yaw_floor = (int)floorf(yaw_deg);
    for (int dh = -half_window; dh <= half_window; ++dh) {
        int hdg = ((yaw_floor + dh) % 360 + 360) % 360;
        int x = PFD_CX + dh * HDG_PX_PER_DEG -
                (int)((yaw_deg - (float)yaw_floor) * HDG_PX_PER_DEG + 0.5f);
        if (x < -10 || x > PK_DISPLAY_W + 10) continue;
        bool major = (hdg % 10) == 0;
        bool labelled = (hdg % 30) == 0;
        int tick_h = major ? 6 : 3;
        pk_pfd_fill_rect(fb, x, PFD_HEADING_TOP, x + 1,
                         PFD_HEADING_TOP + tick_h, COL_TAPE_TICK);
        if (labelled) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%03d", hdg);
            int w = (int)strlen(buf) * PK_FONT_CELL_W(1);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         x - w / 2, PFD_HEADING_TOP + 9, buf,
                         COL_TAPE_LABEL, 1);
        }
    }
    int cx = PFD_CX;
    pk_pfd_draw_triangle(fb,
                         cx,      PFD_HEADING_BOT - 1,
                         cx - 5,  PFD_HEADING_BOT - 8,
                         cx + 5,  PFD_HEADING_BOT - 8,
                         COL_TAPE_CARET);
}

/* --- Bottom numeric panel ------------------------------------------ */

static void draw_panel_text(uint16_t *fb,
                            const pk_imu_sample_t *s, bool imu_valid,
                            size_t aircraft_count)
{
    pk_pfd_fill_rect(fb, 0, PFD_PANEL_TOP, PK_DISPLAY_W, PFD_PANEL_BOT,
                     COL_PANEL_BG);

    char buf[16];

    const int row1_y = PFD_PANEL_TOP + 4;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, row1_y,
                 "R", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%+6.1f~", imu_valid ? s->roll_deg : 0.0f);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 28, row1_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 124, row1_y,
                 "P", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%+6.1f~", imu_valid ? s->pitch_deg : 0.0f);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 148, row1_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    const int row2_y = PFD_PANEL_TOP + 26;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, row2_y,
                 "HDG", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%03d~",
             imu_valid ? ((int)s->yaw_deg + 360) % 360 : 0);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 52, row2_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 124, row2_y,
                 "ADSB", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%2u", (unsigned)aircraft_count);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 188, row2_y,
                 buf, COL_ACCENT, 2);
}

/* --- Public entry --------------------------------------------------- */

void pk_pfd_legacy_render(uint16_t *fb,
                          const pk_imu_sample_t *s, bool imu_valid,
                          size_t aircraft_count)
{
    float yaw = imu_valid ? s->yaw_deg : 0.0f;
    draw_heading_tape(fb, yaw);
    draw_panel_text(fb, s, imu_valid, aircraft_count);
}
