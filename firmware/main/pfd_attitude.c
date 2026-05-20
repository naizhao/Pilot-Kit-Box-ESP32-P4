/*
 * pfd_attitude.c — attitude indicator (sky/ground horizon with vertical
 * gradient + pitch ladder + bank arc + chevron pointer + sky pointer +
 * yellow wing reticle).
 *
 * Geometry follows the G1000-style layout spec (§3): the attitude
 * region occupies x ∈ [50, 248), y ∈ [18, 138) — 198 × 120 pixels,
 * sitting between the top status bar and the bottom HSI region. All
 * rotations happen around the region's geometric center (149, 78).
 *
 * The bank arc is drawn with a virtual center placed *below* the
 * visible region (149, 200) so the visible portion of the arc curves
 * cleanly across the top of the attitude box. Same trick the old
 * portrait code used, just with smaller radius and re-centered.
 *
 * The sky/ground colors use a perpendicular-distance LUT for the
 * G1000 gradient — brightest at the horizon, darker further away —
 * so the band texture stays correct under bank.
 */

#include "pfd_attitude.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_attr.h"

#include "display.h"
#include "pfd_draw.h"
#include "pfd_font.h"

/* --- Layout constants ----------------------------------------------- *
 *
 * The attitude indicator fills the WHOLE panel below the top status
 * bar — Garmin G1000 treats it as the screen background, not a small
 * sub-region. Other widgets (statusbar / ALT tape / HSI / GS / VS)
 * draw OVER the attitude as opaque overlays.
 */
#define PFD_ATTITUDE_LEFT       0
#define PFD_ATTITUDE_RIGHT      PK_DISPLAY_W                   /* 320 */
#define PFD_ATTITUDE_TOP        18                              /* just below statusbar */
#define PFD_ATTITUDE_BOT        PK_DISPLAY_H                   /* 240 */
#define PFD_CX                  (PK_DISPLAY_W / 2)             /* 160 */
#define PFD_CY                  ((PFD_ATTITUDE_TOP + PFD_ATTITUDE_BOT) / 2)  /* 129 */
#define PFD_PIXELS_PER_DEG      3

/* Bank arc: virtual center placed below the visible region so the
 * arc curves cleanly across the top of the attitude indicator. With
 * R=100 and CY=130, the 0° tick lands at y=30 (just below the
 * statusbar) and ±60° ticks land at x=160±87 → arc spans ~174 px
 * wide ≈ 54% of the 320-px panel (Garmin G1000 keeps the bank
 * indicator inside the middle 1/3..3/5 of screen width). */
#define BANK_ARC_CX             160
#define BANK_ARC_CY             130
#define BANK_ARC_R              100

/* Gradient LUT size: max perpendicular distance from horizon we map
 * to distinct colors. Beyond this the gradient saturates at the "far"
 * end. 160 keeps the gradient ramp visible across the full screen. */
#define ATTITUDE_HEIGHT         160

/* --- Palette (spec §4, RGB565, panel byte order) ------------------- */
#define COL_HORIZON_LINE        pk_rgb565(255, 255, 255)
#define COL_RETICLE             pk_rgb565(255, 255,   0)   /* pure yellow */
#define COL_PITCH_LINE          pk_rgb565(255, 255, 255)
#define COL_BANK_TICK           pk_rgb565(255, 255, 255)
#define COL_BANK_POINTER        pk_rgb565(255, 255,   0)   /* pure yellow */
#define COL_SKY_POINTER         pk_rgb565(255, 255, 255)
#define COL_BANK_ARC            pk_rgb565(255, 255, 255)

/* --- Gradient LUTs (sky/ground), built once on first render -------- */

static EXT_RAM_BSS_ATTR uint16_t s_sky_grad[ATTITUDE_HEIGHT];
static EXT_RAM_BSS_ATTR uint16_t s_ground_grad[ATTITUDE_HEIGHT];
static bool s_grad_built = false;

static uint16_t blend_rgb565(uint8_t r0, uint8_t g0, uint8_t b0,
                             uint8_t r1, uint8_t g1, uint8_t b1,
                             int t)
{
    /* t in [0, 256]: 0 returns (r0,g0,b0), 256 returns (r1,g1,b1). */
    uint8_t r = (uint8_t)((r0 * (256 - t) + r1 * t) >> 8);
    uint8_t g = (uint8_t)((g0 * (256 - t) + g1 * t) >> 8);
    uint8_t b = (uint8_t)((b0 * (256 - t) + b1 * t) >> 8);
    return pk_rgb565(r, g, b);
}

static void build_gradient_luts(void)
{
    if (s_grad_built) return;
    for (int i = 0; i < ATTITUDE_HEIGHT; ++i) {
        int t = (i * 256) / (ATTITUDE_HEIGHT - 1);   /* 0 near, 256 far */
        s_sky_grad[i]    = blend_rgb565( 35, 145, 235,
                                         15,  70, 140, t);
        s_ground_grad[i] = blend_rgb565(170, 125,  80,
                                         95,  65,  35, t);
    }
    s_grad_built = true;
}

/* --- Sky / ground horizon with gradient ----------------------------- */

static void draw_horizon(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float rad      = roll_deg * (float)M_PI / 180.0f;
    const float slope    = -tanf(rad);
    const float cos_roll = cosf(rad);
    const float pitch_px = pitch_deg * PFD_PIXELS_PER_DEG;

    for (int y = PFD_ATTITUDE_TOP; y < PFD_ATTITUDE_BOT; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = PFD_ATTITUDE_LEFT; x < PFD_ATTITUDE_RIGHT; ++x) {
            float hy   = (float)PFD_CY + pitch_px +
                         slope * ((float)x - (float)PFD_CX);
            float dy   = (float)y - hy;
            float perp = fabsf(dy) * cos_roll;
            int idx = (int)perp;
            if (idx >= ATTITUDE_HEIGHT) idx = ATTITUDE_HEIGHT - 1;
            row[x] = (dy < 0.0f) ? s_sky_grad[idx] : s_ground_grad[idx];
        }
    }

    /* 2 px white horizon line along the rotated horizon, clipped to
     * the attitude region. */
    for (int x = PFD_ATTITUDE_LEFT; x < PFD_ATTITUDE_RIGHT; ++x) {
        float hy = (float)PFD_CY + pitch_px +
                   slope * ((float)x - (float)PFD_CX);
        int yi = (int)(hy + 0.5f);
        pk_pfd_put_pixel(fb, x, yi,     COL_HORIZON_LINE);
        pk_pfd_put_pixel(fb, x, yi - 1, COL_HORIZON_LINE);
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
        /* Mark half-widths sized for the full 320-wide attitude:
         * ±10° → 140 px wide, ±20° → 100 px, ±30° → 70 px. */
        int half_w = (abs_p == 10) ? 70 : (abs_p == 20 ? 50 : 35);
        int mark_y = PFD_CY + (int)((pitch_deg - (float)p) *
                                    PFD_PIXELS_PER_DEG + 0.5f);
        if (mark_y < PFD_ATTITUDE_TOP - 20 || mark_y > PFD_ATTITUDE_BOT + 20) {
            continue;
        }
        int lx, ly, rx, ry;
        rotate_about_center(cs, sn, PFD_CX - half_w, mark_y, &lx, &ly);
        rotate_about_center(cs, sn, PFD_CX + half_w, mark_y, &rx, &ry);
        pk_pfd_draw_line(fb, lx, ly, rx, ry, COL_PITCH_LINE);
        if (p < 0) {
            /* "Below the horizon" marks rendered with an extra half-
             * length overlay — closest we get to a dashed line without
             * background-aware erasing. */
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

/* --- Bank arc + chevron + sky pointer ------------------------------ */

static void place_on_arc(float angle_deg, int *x, int *y)
{
    float rad = angle_deg * (float)M_PI / 180.0f;
    *x = (int)((float)BANK_ARC_CX + (float)BANK_ARC_R * sinf(rad) + 0.5f);
    *y = (int)((float)BANK_ARC_CY - (float)BANK_ARC_R * cosf(rad) + 0.5f);
}

static const int8_t bank_ticks[] = { -60, -45, -30, -20, -10, 10, 20, 30, 45, 60 };

static void draw_bank_arc(uint16_t *fb, float roll_deg)
{
    /* White inner arc connecting all the tick bases — a smooth thin
     * curve from -60° to +60°. Sample one point per 0.5° (so adjacent
     * samples land roughly 1.5 px apart on a R=170 arc) and plot it
     * 2 px thick (radially) so the line stays continuous at low
     * resolutions. */
    for (int step = -120; step <= 120; ++step) {
        float a   = (float)step * 0.5f;
        float rad = a * (float)M_PI / 180.0f;
        int x_in  = (int)((float)BANK_ARC_CX +
                          (float)(BANK_ARC_R - 1) * sinf(rad) + 0.5f);
        int y_in  = (int)((float)BANK_ARC_CY -
                          (float)(BANK_ARC_R - 1) * cosf(rad) + 0.5f);
        int x_out = (int)((float)BANK_ARC_CX +
                          (float)BANK_ARC_R * sinf(rad) + 0.5f);
        int y_out = (int)((float)BANK_ARC_CY -
                          (float)BANK_ARC_R * cosf(rad) + 0.5f);
        pk_pfd_put_pixel(fb, x_in,  y_in,  COL_BANK_ARC);
        pk_pfd_put_pixel(fb, x_out, y_out, COL_BANK_ARC);
    }

    /* Tick marks: three-tier lengths so the visual hierarchy reads
     * cleanly — ±10° smallest, ±20° medium, ±30°/±45°/±60° longest.
     * Garmin uses similar graduated lengths. */
    for (size_t i = 0; i < sizeof(bank_ticks) / sizeof(bank_ticks[0]); ++i) {
        int angle = bank_ticks[i];
        int abs_a = angle < 0 ? -angle : angle;
        int tick_len;
        switch (abs_a) {
            case 10:           tick_len =  4; break;
            case 20:           tick_len =  6; break;
            default:           tick_len = 10; break;   /* 30, 45, 60 */
        }
        int x0, y0;
        place_on_arc((float)angle, &x0, &y0);
        float rad = (float)angle * (float)M_PI / 180.0f;
        int x1 = (int)((float)BANK_ARC_CX +
                       (float)(BANK_ARC_R + tick_len) * sinf(rad) + 0.5f);
        int y1 = (int)((float)BANK_ARC_CY -
                       (float)(BANK_ARC_R + tick_len) * cosf(rad) + 0.5f);
        pk_pfd_draw_line(fb, x0, y0, x1, y1, COL_BANK_TICK);
    }

    /* Sky pointer — fixed downward-pointing inverted white triangle
     * at the top center of the attitude region. Marks the 0° bank
     * reference; the chevron below indicates current bank against it. */
    pk_pfd_draw_triangle(fb,
                         PFD_CX,     PFD_ATTITUDE_TOP + 14,
                         PFD_CX - 6, PFD_ATTITUDE_TOP +  2,
                         PFD_CX + 6, PFD_ATTITUDE_TOP +  2,
                         COL_SKY_POINTER);

    /* Bank chevron — yellow filled triangle hanging *below* the arc
     * at the current roll angle. Tip touches the arc; base sits 12 px
     * further from BANK_ARC_CY (toward the bottom of the screen). */
    int tip_x, tip_y;
    place_on_arc(roll_deg, &tip_x, &tip_y);
    float rad = roll_deg * (float)M_PI / 180.0f;
    /* Vector pointing from arc point inward toward the virtual center —
     * that's the direction the chevron base sits. */
    float in_x = -sinf(rad);
    float in_y =  cosf(rad);
    int base_cx = (int)((float)tip_x + in_x * 12.0f + 0.5f);
    int base_cy = (int)((float)tip_y + in_y * 12.0f + 0.5f);
    /* Perpendicular to (in_x, in_y) gives the chevron base width
     * direction; 6 px half-width. */
    float perp_x = -in_y;
    float perp_y =  in_x;
    int b1x = (int)((float)base_cx + perp_x * 6.0f + 0.5f);
    int b1y = (int)((float)base_cy + perp_y * 6.0f + 0.5f);
    int b2x = (int)((float)base_cx - perp_x * 6.0f + 0.5f);
    int b2y = (int)((float)base_cy - perp_y * 6.0f + 0.5f);
    pk_pfd_draw_triangle(fb, tip_x, tip_y, b1x, b1y, b2x, b2y, COL_BANK_POINTER);
}

/* --- Reticle (fixed) ----------------------------------------------- */

static void draw_reticle(uint16_t *fb)
{
    const int cx = PFD_CX;
    const int cy = PFD_CY;
    pk_pfd_fill_rect(fb, cx - 30, cy - 1, cx -  8, cy + 2, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx +  8, cy - 1, cx + 30, cy + 2, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx -  1, cy - 1, cx +  2, cy + 9, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx -  2, cy - 2, cx +  3, cy + 3, COL_RETICLE);
}

/* --- Public entry --------------------------------------------------- */

void pk_pfd_attitude_render(uint16_t *fb, const pk_pfd_imu_t *imu)
{
    build_gradient_luts();

    float roll  = imu->valid ? imu->roll_deg  : 0.0f;
    float pitch = imu->valid ? imu->pitch_deg : 0.0f;

    draw_horizon(fb, roll, pitch);
    draw_pitch_ladder(fb, roll, pitch);
    draw_bank_arc(fb, roll);
    draw_reticle(fb);
}
