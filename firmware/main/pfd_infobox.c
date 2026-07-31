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
#include "i18n.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"

#define COL_BARO   pk_rgb565(230, 200,  74)   /* amber：气压源，参考值 */
#define COL_WHITE  pk_rgb565(255, 255, 255)
#define COL_BLUE   pk_rgb565(150, 200, 255)
#define COL_CYAN   pk_rgb565( 70, 220, 250)
#define COL_GREY   pk_rgb565(180, 180, 180)
#define COL_STALE  pk_rgb565(100, 100, 100)

/* 2026-08-01：这里原有一份 `#else` 的 320 档版面（5×7 位图、字宽 6）。
 * PK_DISPLAY_W 已由 display.h 无条件钉死在 800，那条分支一行都编不到，
 * 随小屏兼容预览一并删除。要换屏得先改 display.h。 */
#define IB_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#define IB_GLYPH_W   PK_AA_M_W
#define IB_TEXT_DY   ((PFD_IB_ROW_H - PK_AA_M_H) / 2)

/*
 * ── 标签用词的边界 ───────────────────────────────────────────────
 *
 * **标准缩写一律不译，自造缩写一律改成能读懂的形式。**
 *
 * HDG / VS / GPS / ADS-B / KM/H / N-E-S-W 是航电通用缩写，中文飞行员本来就
 * 这么说，译成汉字反而要在脑子里再翻一道——它们保持英文，也就不进 i18n 表。
 * 而 OWN / B / ALT(米) 三个不是缩写而是**截断**：OWN 不是 own-ship 的任何一种
 * 标准写法（ICAO/FAA 都写全），B 是 BARO 砍掉三个字母，ALT 在这一格里指的是
 * 「米制高度」而不是 ALT 本身。三个都源自 320×240 那块小屏挤不下，换到
 * 800×480 之后宽度绰绰有余（宽度账见 i18n_catalog.py 的 PFD_IB_* 那一节），
 * 没有理由继续让人猜。
 *
 * 一行 = 半透明底 + 左侧标签 + 右对齐数值。
 *
 * 左右两块全都是这个形状，差别只在内容与配色，所以收成一个函数——
 * 各写一遍的话，将来调行高或内边距就得改好几处，必漏。
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
        /* strlen × cell_w 只在数值侧成立，因为数值恒是 ASCII（数字 + ft/m/
         * 呼号/GPS/--）。标签侧现在有汉字，一个字三字节，同样的算法会把
         * 「本机」量成 6 格——但标签是**左对齐**的，压根不测宽，所以这里不必
         * 改。哪天数值侧出现中文，这一行必须换成 pk_aa_text_width()。 */
        int vw = (int)strlen(value) * IB_GLYPH_W;
        IB_PUTS(fb, x1 - PFD_IB_PAD - vw, ty, value, value_col);
    }
}

/* ── 左块 ─────────────────────────────────────────────────────── */

/*
 * 左块只剩两行，但**底边仍与右块对齐**——两块信息框坐在半圆罗盘两侧、靠共同
 * 的底边读成一对，所以空出来的那一行留在**顶上**，让姿态背景透出来，而不是
 * 把两行往上顶。
 *
 * 删掉的是 MPH：英里/小时在中国空域零使用场景，却占着 200×30 px 一整行；
 * KM/H 保留且不译（SI 符号，中国空域与国产/苏系机型通用）。
 */
#define LB_ROW_KMH   1
#define LB_ROW_OWN   2

void pk_pfd_leftbox_render(uint16_t *fb, const pk_pfd_leftbox_t *d)
{
    char buf[16];

    if (d->speed_valid) {
        snprintf(buf, sizeof(buf), "%d", d->kmh);
        draw_row(fb, PFD_IB_LEFT_X0, LB_ROW_KMH, "KM/H", COL_CYAN,
                 buf, COL_WHITE);
    } else {
        draw_row(fb, PFD_IB_LEFT_X0, LB_ROW_KMH, "KM/H", COL_CYAN,
                 "--", COL_STALE);
    }

    /* 末行：本机数据来源。
     *
     * 绑定丢失时整行让给红色闪烁的告警——那是要抢注意力的，还挂着「本机」
     * 标签只会稀释它。 */
    if (d->adsb_lost_alert) {
        uint16_t c = d->alert_blink_on ? pk_rgb565(255, 40, 40)
                                       : pk_rgb565(110, 20, 20);
        draw_row(fb, PFD_IB_LEFT_X0, LB_ROW_OWN, "ADS-B LOST", c, NULL, c);
        return;
    }

    uint16_t col = (d->src == PK_PFD_SRC_ADSB) ? COL_CYAN
                 : (d->src == PK_PFD_SRC_GPS)  ? COL_WHITE
                                               : COL_STALE;
    draw_row(fb, PFD_IB_LEFT_X0, LB_ROW_OWN, pk_i18n_text(PK_TR_PFD_IB_OWN),
             COL_GREY, d->label, col);
}

/* ── 右块 ─────────────────────────────────────────────────────── */

void pk_pfd_infobox_render(uint16_t *fb, const pk_pfd_infobox_t *d)
{
    char buf[16];

    /* ── 行 1：气压高度 ──────────────────────────────────────
     * 原来只写一个 B。当年的理由是「BARO 四个字母会挤掉 5 位数值」，但那是
     * 320 px 屏的账：200 px 版面下 BARO(60) + "99999ft"(105) = 165 ≤ 192，
     * 算式根本不成立。琥珀色确实提示了气压源，但颜色是**第二层**信息，不能
     * 替代标签本身。 */
    if (d->baro_valid) {
        int v = d->baro_alt_ft;
        if (v < -9999) v = -9999;
        if (v > 99999) v = 99999;
        snprintf(buf, sizeof(buf), "%dft", v);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_row(fb, PFD_IB_RIGHT_X0, 0, pk_i18n_text(PK_TR_PFD_IB_BARO), COL_BARO,
             buf, d->baro_valid ? COL_WHITE : COL_STALE);

    /* ── 行 2：权威高度的米值 ────────────────────────────────
     * 换算的是 ADS-B 高度而非气压高度：增压舱内气压计不准，而右侧高度带
     * 显示的就是 ADS-B 值，两者必须同源，否则同一屏上两个高度对不上。
     *
     * 标签不能叫 ALT：上一行也是高度，两行一个叫 B 一个叫 ALT，读不出「这行
     * 是米」。中国空域用米制高度层，这一行对中国飞行员价值最高。 */
    if (d->alt_valid) {
        int m = (int)lroundf((float)d->alt_ft * 0.3048f);
        if (m < -9999) m = -9999;
        if (m > 99999) m = 99999;
        snprintf(buf, sizeof(buf), "%dm", m);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_row(fb, PFD_IB_RIGHT_X0, 1, pk_i18n_text(PK_TR_PFD_IB_ALT_M), COL_CYAN,
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
