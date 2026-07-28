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
#define COL_GREY   pk_rgb565(180, 180, 180)
#define COL_STALE  pk_rgb565(100, 100, 100)

#if PK_DISPLAY_W >= 800
#  define IB_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#  define IB_GLYPH_W   PK_AA_M_W
#  define IB_TEXT_DY   ((PFD_IB_ROW_H - PK_AA_M_H) / 2)
#else
#  define IB_PUTS(fb, x, y, s, col) \
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), 1)
#  define IB_GLYPH_W   6
#  define IB_TEXT_DY   3
#endif

/*
 * 一行 = 半透明底 + 左侧标签 + 右对齐数值。
 *
 * 左右两块、六行全都是这个形状，差别只在内容与配色，所以收成一个函数——
 * 六处各写一遍的话，将来调行高或内边距就得改六处，必漏。
 */
static void draw_row(uint16_t *fb, int x0, int row,
                     const char *label, uint16_t label_col,
                     const char *value, uint16_t value_col)
{
    const int y0 = PFD_IB_TOP + row * (PFD_IB_ROW_H + PFD_IB_ROW_GAP);
    const int x1 = x0 + PFD_IB_W;
    pk_pfd_darken_rect(fb, x0, y0, x1, y0 + PFD_IB_ROW_H, 160);

    const int ty = y0 + IB_TEXT_DY;
    if (label && label[0]) IB_PUTS(fb, x0 + PFD_IB_PAD, ty, label, label_col);

    if (value && value[0]) {
        int vw = (int)strlen(value) * IB_GLYPH_W;
        IB_PUTS(fb, x1 - PFD_IB_PAD - vw, ty, value, value_col);
    }
}

/* ── 左块 ─────────────────────────────────────────────────────── */

void pk_pfd_leftbox_render(uint16_t *fb, const pk_pfd_leftbox_t *d)
{
    char buf[16];

    if (d->speed_valid) {
        snprintf(buf, sizeof(buf), "%d", d->kmh);
        draw_row(fb, PFD_IB_LEFT_X0, 0, "KM/H", COL_CYAN, buf, COL_WHITE);
        snprintf(buf, sizeof(buf), "%d", d->mph);
        draw_row(fb, PFD_IB_LEFT_X0, 1, "MPH", COL_CYAN, buf, COL_WHITE);
    } else {
        draw_row(fb, PFD_IB_LEFT_X0, 0, "KM/H", COL_CYAN, "--", COL_STALE);
        draw_row(fb, PFD_IB_LEFT_X0, 1, "MPH", COL_CYAN, "--", COL_STALE);
    }

    /* 第三行：本机数据来源。
     *
     * 绑定丢失时整行让给红色闪烁的告警——那是要抢注意力的，还挂着「OWN」
     * 标签只会稀释它。 */
    if (d->adsb_lost_alert) {
        uint16_t c = d->alert_blink_on ? pk_rgb565(255, 40, 40)
                                       : pk_rgb565(110, 20, 20);
        draw_row(fb, PFD_IB_LEFT_X0, 2, "ADS-B LOST", c, NULL, c);
        return;
    }

    uint16_t col = (d->src == PK_PFD_SRC_ADSB) ? COL_CYAN
                 : (d->src == PK_PFD_SRC_GPS)  ? COL_WHITE
                                               : COL_STALE;
    draw_row(fb, PFD_IB_LEFT_X0, 2, "OWN", COL_GREY, d->label, col);
}

/* ── 右块 ─────────────────────────────────────────────────────── */

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
    draw_row(fb, PFD_IB_RIGHT_X0, 0, "B", COL_BARO,
             buf, d->baro_valid ? COL_WHITE : COL_STALE);

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
    draw_row(fb, PFD_IB_RIGHT_X0, 1, "ALT", COL_CYAN,
             buf, d->alt_valid ? COL_BLUE : COL_STALE);

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
    draw_row(fb, PFD_IB_RIGHT_X0, 2, "VS", COL_CYAN, buf, vs_col);
}
