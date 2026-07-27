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
#include <string.h>

#include "esp_attr.h"

#include "display.h"
#include "pfd_layout.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_font.h"

/* --- Layout constants ----------------------------------------------- *
 *
 * The attitude indicator fills the WHOLE panel below the top status
 * bar — Garmin G1000 treats it as the screen background, not a small
 * sub-region. Other widgets (statusbar / ALT tape / HSI / GS / VS)
 * draw OVER the attitude as opaque overlays.
 */
#define PFD_ATTITUDE_LEFT       PFD_ATT_LEFT
#define PFD_ATTITUDE_RIGHT      PK_DISPLAY_W
/* 天地背景一直填到屏幕底，而不是止于姿态区下沿：下面的 HSI 罗盘是半透明的，
 * 底下没有天地就是一片未初始化的 framebuffer。
 * 注意这**不等于**几何中心的取法 —— PFD_CX/CY 来自 pfd_layout.h，按姿态区
 * 算；此前这里用 (18 + PK_DISPLAY_H)/2 把两件事混为一谈，导致地平线下沉。 */
#define PFD_ATTITUDE_TOP        PFD_ATT_TOP
#define PFD_ATTITUDE_BOT        PK_DISPLAY_H

/* Bank arc: virtual center placed below the visible region so the
 * arc curves cleanly across the top of the attitude indicator. With
 * R=100 and CY=130, the 0° tick lands at y=30 (just below the
 * statusbar) and ±60° ticks land at x=160±87 → arc spans ~174 px
 * wide ≈ 54% of the 320-px panel (Garmin G1000 keeps the bank
 * indicator inside the middle 1/3..3/5 of screen width). */
#define BANK_ARC_CX             PFD_BANK_ARC_CX
#define BANK_ARC_CY             PFD_BANK_ARC_CY
#define BANK_ARC_R              PFD_BANK_ARC_R

/* Gradient LUT size: max perpendicular distance from horizon we map
 * to distinct colors. Beyond this the gradient saturates at the "far"
 * end. 160 keeps the gradient ramp visible across the full screen. */
#define ATTITUDE_HEIGHT         PFD_ATT_H

/* --- Palette (spec §4, RGB565, panel byte order) ------------------- */
#define COL_HORIZON_LINE        pk_rgb565(255, 255, 255)
#define COL_RETICLE             pk_rgb565(255, 255,   0)   /* pure yellow */
#define COL_PITCH_LINE          pk_rgb565(255, 255, 255)
#define COL_BANK_TICK           pk_rgb565(255, 255, 255)
#define COL_BANK_POINTER        pk_rgb565(255, 255,   0)   /* pure yellow */
#define COL_SKY_POINTER         pk_rgb565(255, 255, 255)
#define COL_BANK_ARC            pk_rgb565(255, 255, 255)

/* ── 俯仰梯度尺寸 ──────────────────────────────────────────────
 *
 * 半宽原值 35/24/16 是照 320 屏姿态区（约 170 px 宽）定的。800 屏姿态区
 * 有 600 px，照搬会让梯度线短得像三道划痕，与 G1000 上「±10° 线约占姿态
 * 区三分之一」的观感差得远，故按区宽等比放大。
 *
 * 标签同样从 5×7 位图 scale-1（6 px）换成 S 档 —— 6 px 在 4.3″ 屏上只有
 * 0.7 mm，低于 spec §2 的 18 px 硬下限。 */
#if PK_DISPLAY_W >= 800
/* ±10° 线全宽约占姿态区宽度的 22%（与 320 上 70/320 的观感一致）：
 * 800 姿态区宽 600 → 132 px 全宽 → 半宽 66，其余两档按 35:24:16 同比。 */
#  define LADDER_W10        66
#  define LADDER_W20        45
#  define LADDER_W30        30
#  define LADDER_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_S)
#  define LADDER_LBL_W      PK_AA_S_W
#  define LADDER_LBL_H      PK_AA_S_H
#  define LADDER_LBL_GAP    8
/* 梯度线的下界是**航向框顶**，不是罗盘顶：框坐在罗盘正上方、姿态区下沿，
 * 是这一侧最先挡路的东西。画到框上只会两层叠字。 */
#  define LADDER_BOT        (PFD_HDGBOX_Y0 - 10)
/* 坡度刻度长度 / 天空指针 / chevron 同样按物理尺寸对齐 320（×1.3），
 * 而不是按面板像素等比（×2.5）—— 后者会让这些符号在新屏上大得突兀。 */
#  define BANK_TICK_S        5
#  define BANK_TICK_M        8
#  define BANK_TICK_L       13
#  define SKYPTR_H          16
#  define SKYPTR_HW          8
#  define CHEVRON_LEN       16
#  define CHEVRON_HW         8
/* 坡度弧比俯仰梯度更粗：它是姿态的**参考框架**，梯度是框架内的刻度，
 * 主次要一眼分得出。 */
#  define BANK_ARC_TH      2.8f
#else
#  define LADDER_W10        35
#  define LADDER_W20        24
#  define LADDER_W30        16
#  define LADDER_PUTS(fb, x, y, s, col) \
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), 1)
#  define LADDER_LBL_W      6
#  define LADDER_LBL_H      6
#  define LADDER_LBL_GAP    2
#  define LADDER_BOT        (PFD_ATTITUDE_BOT + 20)
#  define BANK_TICK_S        4
#  define BANK_TICK_M        6
#  define BANK_TICK_L       10
#  define SKYPTR_H          12
#  define SKYPTR_HW          6
#  define CHEVRON_LEN       12
#  define CHEVRON_HW         6
#  define BANK_ARC_TH      2.0f
#endif


/* --- Gradient LUTs (sky/ground), built once on first render -------- */

static EXT_RAM_BSS_ATTR uint16_t s_sky_grad[16][ATTITUDE_HEIGHT];
static EXT_RAM_BSS_ATTR uint16_t s_ground_grad[16][ATTITUDE_HEIGHT];
static bool s_grad_built = false;

static uint8_t blend_u8(uint8_t a, uint8_t b, int t)
{
    return (uint8_t)(((int)a * (256 - t) + (int)b * t) >> 8);
}

static void build_gradient_luts(void)
{
    if (s_grad_built) return;
    for (int cell = 0; cell < 16; ++cell) {
        int dx = cell & 3;
        int dy = cell >> 2;
        for (int i = 0; i < ATTITUDE_HEIGHT; ++i) {
            int t = (i * 256) / (ATTITUDE_HEIGHT - 1);   /* 0 near, 256 far */
            uint8_t sky_r = blend_u8( 35,  15, t);
            uint8_t sky_g = blend_u8(145,  70, t);
            uint8_t sky_b = blend_u8(235, 140, t);
            uint8_t gnd_r = blend_u8(170,  95, t);
            uint8_t gnd_g = blend_u8(125,  65, t);
            uint8_t gnd_b = blend_u8( 80,  35, t);
            s_sky_grad[cell][i] = pk_pfd_rgb565_dither(sky_r, sky_g, sky_b, dx, dy);
            s_ground_grad[cell][i] = pk_pfd_rgb565_dither(gnd_r, gnd_g, gnd_b, dx, dy);
        }
    }
    s_grad_built = true;
}

/* --- Sky / ground horizon with gradient ----------------------------- */

static void draw_horizon(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float rad      = roll_deg * (float)M_PI / 180.0f;
    const float slope    = -tanf(rad);
    const float cos_roll = fabsf(cosf(rad));
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
            int cell = ((y & 3) << 2) | (x & 3);
            row[x] = (dy < 0.0f) ? s_sky_grad[cell][idx] : s_ground_grad[cell][idx];

            float coverage = 1.5f - perp;  /* 2 px horizon line + 1 px AA fringe. */
            if (coverage > 0.0f) {
                uint8_t alpha = coverage >= 1.0f
                                    ? 255
                                    : (uint8_t)(coverage * 255.0f + 0.5f);
                pk_pfd_blend_pixel(fb, x, y, COL_HORIZON_LINE, alpha);
            }
        }
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
        /* Mark half-widths sized to sit inside the bank-arc footprint
         * (~170 px wide) without crowding the reticle: ±10° → 70 px
         * wide, ±20° → 48 px, ±30° → 32 px. */
        /* 三级视觉层次：离地平线越远，越短、越细、越淡。
         *
         * 中心附近是最常用的读数区，该最实；±30° 只是余光里的方位感，压下去
         * 反而让整个姿态区不那么吵。这也是与坡度弧拉开差别的手段——两者本来
         * 同为纯白 1.4 px，在弧与梯度交叠处根本分不出谁是谁。 */
        int   half_w = (abs_p == 10) ? LADDER_W10
                     : (abs_p == 20 ? LADDER_W20 : LADDER_W30);
        float line_th = (abs_p == 10) ? 1.7f : (abs_p == 20 ? 1.4f : 1.1f);
        /* 没有 alpha 通道，用明度模拟不透明度：深色天地背景上，压暗即变淡。 */
        uint16_t lcol = (abs_p == 10) ? pk_rgb565(255, 255, 255)
                      : (abs_p == 20) ? pk_rgb565(215, 218, 225)
                                      : pk_rgb565(175, 180, 190);
        int mark_y = PFD_CY + (int)((pitch_deg - (float)p) *
                                    PFD_PIXELS_PER_DEG + 0.5f);
        /* 上界按**标签**而不是线来判：状态栏是不透明的，线越界只是被盖住，
         * 标签越界却会露出半个字。留半个 cell 高，字完整才画。 */
        if (mark_y - LADDER_LBL_H / 2 < PFD_ATTITUDE_TOP || mark_y > LADDER_BOT) {
            continue;
        }
        int lx, ly, rx, ry;
        rotate_about_center(cs, sn, PFD_CX - half_w, mark_y, &lx, &ly);
        rotate_about_center(cs, sn, PFD_CX + half_w, mark_y, &rx, &ry);
        pk_pfd_draw_line_aa(fb, (float)lx, (float)ly, (float)rx, (float)ry,
                            line_th, lcol);
        if (p < 0) {
            /* "Below the horizon" marks rendered with an extra half-
             * length overlay — closest we get to a dashed line without
             * background-aware erasing. */
            int mx = (lx + rx) / 2;
            int my = (ly + ry) / 2;
            pk_pfd_draw_line_aa(fb, (float)lx, (float)ly, (float)mx, (float)my,
                                line_th, lcol);
        }
        /* 数字贴在梯度线两端外侧，垂直中心与线对齐。 */
        char label[4];
        snprintf(label, sizeof(label), "%d", abs_p);
        int lw = (int)strlen(label) * LADDER_LBL_W;
        LADDER_PUTS(fb, lx - lw - LADDER_LBL_GAP, ly - LADDER_LBL_H / 2,
                    label, lcol);
        LADDER_PUTS(fb, rx + LADDER_LBL_GAP, ry - LADDER_LBL_H / 2,
                    label, lcol);
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
    pk_pfd_draw_arc_aa(fb, (float)BANK_ARC_CX, (float)BANK_ARC_CY,
                       (float)BANK_ARC_R - 0.5f,
                       -60.0f, 60.0f, BANK_ARC_TH, COL_BANK_ARC);

    /* Tick marks: three-tier lengths so the visual hierarchy reads
     * cleanly — ±10° smallest, ±20° medium, ±30°/±45°/±60° longest.
     * Garmin uses similar graduated lengths. */
    for (size_t i = 0; i < sizeof(bank_ticks) / sizeof(bank_ticks[0]); ++i) {
        int angle = bank_ticks[i];
        int abs_a = angle < 0 ? -angle : angle;
        /* 长度与粗细同向变化，读起来才是一个层级而不是两套信息。 */
        int   tick_len;
        float tick_th;
        switch (abs_a) {
            case 10:  tick_len = BANK_TICK_S; tick_th = 1.4f; break;
            case 20:  tick_len = BANK_TICK_M; tick_th = 1.7f; break;
            default:  tick_len = BANK_TICK_L; tick_th = 2.1f; break;  /* 30/45/60 */
        }
        int x0, y0;
        place_on_arc((float)angle, &x0, &y0);
        float rad = (float)angle * (float)M_PI / 180.0f;
        int x1 = (int)((float)BANK_ARC_CX +
                       (float)(BANK_ARC_R + tick_len) * sinf(rad) + 0.5f);
        int y1 = (int)((float)BANK_ARC_CY -
                       (float)(BANK_ARC_R + tick_len) * cosf(rad) + 0.5f);
        pk_pfd_draw_line_aa(fb, (float)x0, (float)y0, (float)x1, (float)y1,
                            tick_th, COL_BANK_TICK);
    }

    /* Sky pointer — fixed downward-pointing inverted white triangle
     * at the top center of the attitude region. Marks the 0° bank
     * reference; the chevron below indicates current bank against it. */
    pk_pfd_draw_triangle(fb,
                         PFD_CX,             PFD_ATTITUDE_TOP + 2 + SKYPTR_H,
                         PFD_CX - SKYPTR_HW, PFD_ATTITUDE_TOP + 2,
                         PFD_CX + SKYPTR_HW, PFD_ATTITUDE_TOP + 2,
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
    int base_cx = (int)((float)tip_x + in_x * (float)CHEVRON_LEN + 0.5f);
    int base_cy = (int)((float)tip_y + in_y * (float)CHEVRON_LEN + 0.5f);
    /* Perpendicular to (in_x, in_y) gives the chevron base width
     * direction; 6 px half-width. */
    float perp_x = -in_y;
    float perp_y =  in_x;
    int b1x = (int)((float)base_cx + perp_x * (float)CHEVRON_HW + 0.5f);
    int b1y = (int)((float)base_cy + perp_y * (float)CHEVRON_HW + 0.5f);
    int b2x = (int)((float)base_cx - perp_x * (float)CHEVRON_HW + 0.5f);
    int b2y = (int)((float)base_cy - perp_y * (float)CHEVRON_HW + 0.5f);
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
