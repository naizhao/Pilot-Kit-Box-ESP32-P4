/*
 * pfd_infobox.c — 见 pfd_infobox.h。
 *
 * 几何全部来自 pfd_layout.h，不再是散落在 pfd_task 里的 256 / 170 / 318。
 */

#include "pfd_infobox.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_font.h"
#include "pfd_layout.h"

#define COL_BARO   pk_rgb565(230, 200,  74)   /* amber：气压源，参考值 */
#define COL_WHITE  pk_rgb565(255, 255, 255)
#define COL_BLUE   pk_rgb565(150, 200, 255)
#define COL_CYAN   pk_rgb565( 70, 220, 250)
#define COL_STALE  pk_rgb565(100, 100, 100)

#if PK_DISPLAY_W >= 800
#  define IB_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_S)
#  define IB_GLYPH_W   PK_AA_S_W
#  define IB_TEXT_DY   ((PFD_IB_ROW_H - PK_AA_S_H) / 2)
#  define BADGE_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_S)
#  define BADGE_GLYPH_W  PK_AA_S_W
#  define BADGE_TEXT_DY  ((PFD_BADGE_H - PK_AA_S_H) / 2)
#else
#  define IB_PUTS(fb, x, y, s, col) \
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), 1)
#  define IB_GLYPH_W   6
#  define IB_TEXT_DY   3
#  define BADGE_PUTS(fb, x, y, s, col) \
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col))
#  define BADGE_GLYPH_W  12
#  define BADGE_TEXT_DY  3
#endif

/* 一行 = 半透明底 + 左侧标签 + 右对齐数值。三行的结构完全相同，
 * 差别只在标签、数值串和颜色，故收成一个函数。 */
static void draw_row(uint16_t *fb, int row,
                     const char *label, uint16_t label_col,
                     const char *value, uint16_t value_col)
{
    const int y0 = PFD_IB_TOP + row * (PFD_IB_ROW_H + PFD_IB_ROW_GAP);
    pk_pfd_darken_rect(fb, PFD_IB_X0, y0, PK_DISPLAY_W, y0 + PFD_IB_ROW_H, 160);

    const int ty = y0 + IB_TEXT_DY;
    IB_PUTS(fb, PFD_IB_X0 + PFD_IB_PAD, ty, label, label_col);

    int vw = (int)strlen(value) * IB_GLYPH_W;
    IB_PUTS(fb, PK_DISPLAY_W - PFD_IB_PAD - vw, ty, value, value_col);
}

void pk_pfd_infobox_render(uint16_t *fb, const pk_pfd_infobox_t *d)
{
    char buf[16];

    /* ── 行 1：气压高度 ──────────────────────────────────────
     * 标签只用单字母 B —— 琥珀色已经说明了它是气压源，把 "BARO" 四个字母
     * 铺开会挤掉 5 位数值的位置。 */
    if (d->baro_valid) {
        int v = d->baro_alt_ft;
        if (v < -9999) v = -9999;
        if (v > 99999) v = 99999;
        snprintf(buf, sizeof(buf), "%dft", v);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_row(fb, 0, "B", COL_BARO, buf, d->baro_valid ? COL_WHITE : COL_STALE);

    /* ── 行 2：权威高度的米值 ────────────────────────────────
     * 换算的是 ADS-B 高度而非气压高度：增压舱内气压计不准，而右侧高度带
     * 显示的就是 ADS-B 值，两者必须同源，否则同一屏上两个高度对不上。 */
    if (d->alt_valid) {
        int m = (int)lroundf((float)d->alt_ft * 0.3048f);
        if (m < -9999) m = -9999;
        if (m > 99999) m = 99999;
        snprintf(buf, sizeof(buf), "%dm", m);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_row(fb, 1, "ALT", COL_CYAN, buf, d->alt_valid ? COL_BLUE : COL_STALE);

    /* ── 行 3：升降率 ────────────────────────────────────── */
    uint16_t vs_col;
    if (d->vs_valid) {
        int v = d->vs_fpm;
        if (v >  9999) v =  9999;
        if (v < -9999) v = -9999;
        snprintf(buf, sizeof(buf), "%+d", v);
        vs_col = d->vs_from_adsb ? COL_WHITE : COL_BARO;
    } else {
        snprintf(buf, sizeof(buf), "--");
        vs_col = COL_STALE;
    }
    draw_row(fb, 2, "VS", COL_CYAN, buf, vs_col);
}

void pk_pfd_srcbadge_render(uint16_t *fb, const pk_pfd_srcbadge_t *d)
{
    pk_pfd_darken_rect(fb, 0, PFD_BADGE_Y0, PFD_BADGE_W, PFD_BADGE_Y1, 128);

    if (d->adsb_lost_alert) {
        uint16_t c = d->alert_blink_on ? pk_rgb565(255, 40, 40)
                                       : pk_rgb565(110, 20, 20);
        BADGE_PUTS(fb, PFD_BADGE_PAD, PFD_BADGE_Y0 + BADGE_TEXT_DY,
                   "ADS-B LOST", c);
        return;
    }

    uint16_t col = (d->src == PK_PFD_SRC_ADSB) ? COL_CYAN
                 : (d->src == PK_PFD_SRC_GPS)  ? pk_rgb565(240, 240, 240)
                                               : COL_STALE;
    int tw = (int)strlen(d->label) * BADGE_GLYPH_W;
    int tx = (PFD_BADGE_W - tw) / 2;          /* 框内居中 */
    if (tx < PFD_BADGE_PAD) tx = PFD_BADGE_PAD;
    BADGE_PUTS(fb, tx, PFD_BADGE_Y0 + BADGE_TEXT_DY, d->label, col);
}
