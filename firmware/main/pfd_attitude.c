/*
 * pfd_attitude.c — attitude indicator (sky/ground horizon + pitch
 * ladder + bank arc + yellow reticle).
 *
 * Extracted from pfd.c during phase B of the G1000 redesign. The
 * geometry constants and palette tokens remain identical to the
 * pre-split portrait code; phase C re-tunes them.
 */

#include "pfd_attitude.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "display.h"
#include "pfd_draw.h"
#include "pfd_font.h"

/* --- Layout constants ----------------------------------------------- */
#define PFD_ATTITUDE_TOP        0
#define PFD_ATTITUDE_BOT        240
#define PFD_CX                  (PK_DISPLAY_W / 2)
#define PFD_CY                  ((PFD_ATTITUDE_TOP + PFD_ATTITUDE_BOT) / 2)
#define PFD_PIXELS_PER_DEG      3

/* Bank-arc geometry: pretend the arc has a centre well below the panel
 * so its visible top arcs cleanly across the upper screen. */
#define BANK_ARC_CX             PFD_CX
#define BANK_ARC_CY             200
#define BANK_ARC_R              190

/* --- Palette (RGB565, panel byte order) ----------------------------- */
#define COL_SKY                 pk_rgb565( 30, 130, 230)
#define COL_GROUND              pk_rgb565(120,  85,  50)
#define COL_HORIZON             pk_rgb565(255, 255, 255)
#define COL_RETICLE             pk_rgb565(255, 215,   0)
#define COL_PITCH_LINE          pk_rgb565(255, 255, 255)
#define COL_BANK_TICK           pk_rgb565(255, 255, 255)
#define COL_BANK_POINTER        pk_rgb565(255, 215,   0)

/* --- Sky / ground horizon ------------------------------------------- */

static void draw_horizon(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float rad = roll_deg * (float)M_PI / 180.0f;
    const float slope = -tanf(rad);
    const float pitch_px = pitch_deg * PFD_PIXELS_PER_DEG;

    for (int y = PFD_ATTITUDE_TOP; y < PFD_ATTITUDE_BOT; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            float hy = (float)PFD_CY + pitch_px + slope * ((float)x - (float)PFD_CX);
            row[x] = ((float)y < hy) ? COL_SKY : COL_GROUND;
        }
    }
    for (int x = 0; x < PK_DISPLAY_W; ++x) {
        float hy = (float)PFD_CY + pitch_px + slope * ((float)x - (float)PFD_CX);
        int yi = (int)(hy + 0.5f);
        pk_pfd_put_pixel(fb, x, yi, COL_HORIZON);
        pk_pfd_put_pixel(fb, x, yi - 1, COL_HORIZON);
    }
}

/* --- Pitch ladder --------------------------------------------------- */

static const int8_t pitch_marks[] = {-30, -20, -10, 10, 20, 30};

static void rotate_about_center(float cs, float sn,
                                int x_in, int y_in,
                                int *x_out, int *y_out)
{
    float dx = (float)(x_in - PFD_CX);
    float dy = (float)(y_in - PFD_CY);
    *x_out = (int)(PFD_CX + cs * dx - sn * dy + 0.5f);
    *y_out = (int)(PFD_CY + sn * dx + cs * dy + 0.5f);
}

static void draw_pitch_ladder(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float rad = roll_deg * (float)M_PI / 180.0f;
    const float cs = cosf(rad);
    const float sn = sinf(rad);

    for (size_t i = 0; i < sizeof(pitch_marks) / sizeof(pitch_marks[0]); ++i) {
        int p = pitch_marks[i];
        int abs_p = p < 0 ? -p : p;
        int half_w = (abs_p == 10) ? 30 : (abs_p == 20 ? 20 : 14);
        int mark_y = PFD_CY + (int)((pitch_deg - (float)p) * PFD_PIXELS_PER_DEG + 0.5f);
        if (mark_y < PFD_ATTITUDE_TOP - 20 || mark_y > PFD_ATTITUDE_BOT + 20) continue;
        int lx, ly, rx, ry;
        rotate_about_center(cs, sn, PFD_CX - half_w, mark_y, &lx, &ly);
        rotate_about_center(cs, sn, PFD_CX + half_w, mark_y, &rx, &ry);
        pk_pfd_draw_line(fb, lx, ly, rx, ry, COL_PITCH_LINE);
        if (p < 0) {
            int mx = (lx + rx) / 2;
            int my = (ly + ry) / 2;
            pk_pfd_draw_line(fb, lx, ly, mx, my, COL_PITCH_LINE);
        }
        char label[4];
        snprintf(label, sizeof(label), "%d", abs_p);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     lx - 14, ly - 3, label, COL_PITCH_LINE, 1);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     rx + 2, ry - 3, label, COL_PITCH_LINE, 1);
    }
}

/* --- Bank arc ------------------------------------------------------- */

static void place_on_arc(float angle_deg, int *x, int *y)
{
    float rad = angle_deg * (float)M_PI / 180.0f;
    *x = (int)((float)BANK_ARC_CX + (float)BANK_ARC_R * sinf(rad) + 0.5f);
    *y = (int)((float)BANK_ARC_CY - (float)BANK_ARC_R * cosf(rad) + 0.5f);
}

static const int8_t bank_ticks[] = { -30, -20, -10, 0, 10, 20, 30 };

static void draw_bank_arc(uint16_t *fb, float roll_deg)
{
    for (size_t i = 0; i < sizeof(bank_ticks) / sizeof(bank_ticks[0]); ++i) {
        int angle = bank_ticks[i];
        int x0, y0, x1, y1;
        place_on_arc((float)angle, &x0, &y0);
        float rad = (float)angle * (float)M_PI / 180.0f;
        x1 = (int)((float)BANK_ARC_CX + (float)(BANK_ARC_R + 6) * sinf(rad) + 0.5f);
        y1 = (int)((float)BANK_ARC_CY - (float)(BANK_ARC_R + 6) * cosf(rad) + 0.5f);
        pk_pfd_draw_line(fb, x0, y0, x1, y1, COL_BANK_TICK);
        if (angle == 0) {
            int tx, ty;
            place_on_arc(0.0f, &tx, &ty);
            pk_pfd_draw_triangle(fb,
                                 tx,     ty + 1,
                                 tx - 4, ty - 7,
                                 tx + 4, ty - 7,
                                 COL_BANK_TICK);
        }
    }
    int px, py;
    place_on_arc(roll_deg, &px, &py);
    float rad = roll_deg * (float)M_PI / 180.0f;
    int ax = (int)((float)BANK_ARC_CX + (float)(BANK_ARC_R - 8) * sinf(rad) + 0.5f);
    int ay = (int)((float)BANK_ARC_CY - (float)(BANK_ARC_R - 8) * cosf(rad) + 0.5f);
    int bx = (int)(px + 5.0f * cosf(rad) + 0.5f);
    int by = (int)(py + 5.0f * sinf(rad) + 0.5f);
    int cx = (int)(px - 5.0f * cosf(rad) + 0.5f);
    int cy = (int)(py - 5.0f * sinf(rad) + 0.5f);
    pk_pfd_draw_triangle(fb, ax, ay, bx, by, cx, cy, COL_BANK_POINTER);
}

/* --- Reticle (fixed) ----------------------------------------------- */

static void draw_reticle(uint16_t *fb)
{
    const int cx = PFD_CX;
    const int cy = PFD_CY;
    pk_pfd_fill_rect(fb, cx - 30, cy - 1, cx - 8,  cy + 2, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx +  8, cy - 1, cx + 30, cy + 2, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx -  1, cy - 1, cx +  2, cy + 9, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx -  2, cy - 2, cx +  3, cy + 3, COL_RETICLE);
}

/* --- Public entry --------------------------------------------------- */

void pk_pfd_attitude_render(uint16_t *fb, const pk_pfd_imu_t *imu)
{
    float roll  = imu->valid ? imu->roll_deg  : 0.0f;
    float pitch = imu->valid ? imu->pitch_deg : 0.0f;
    draw_horizon(fb, roll, pitch);
    draw_pitch_ladder(fb, roll, pitch);
    draw_bank_arc(fb, roll);
    draw_reticle(fb);
}
