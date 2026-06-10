/*
 * settings_page.c — language/settings screen (多行可选, Task 9 扩展).
 *
 * 行布局:
 *   行 0: Language  <EN/中文>
 *   行 1: QNH       <1013.25 hPa>
 *
 * 选中行用高亮边条(COL_ROW_EDGE_SEL)区分未选中行(COL_ROW_EDGE)。
 * 光标状态由 s_sel_row 维护,pk_settings_cursor_next() 切换。
 */

#include "settings_page.h"

#include <stdint.h>
#include <stdio.h>

#include "display.h"
#include "i18n.h"
#include "pfd_draw.h"
#include "text.h"
#include "config_qnh.h"
#include "config_traffic.h"

#define COL_BG              pk_rgb565( 12,  12,  16)
#define COL_HEADER          pk_rgb565(180, 235, 255)
#define COL_ROW             pk_rgb565( 30,  36,  44)
#define COL_ROW_SEL         pk_rgb565( 20,  44,  60)   /* 选中行背景(略深蓝) */
#define COL_ROW_EDGE        pk_rgb565( 60,  80,  90)   /* 未选中边条(暗) */
#define COL_ROW_EDGE_SEL    pk_rgb565( 80, 220, 240)   /* 选中边条(亮青) */
#define COL_KEY             pk_rgb565(180, 235, 255)
#define COL_VAL             pk_rgb565(255, 255, 255)
#define COL_DIM             pk_rgb565(255, 255, 255)
#define COL_DIVIDER         pk_rgb565( 60,  60,  70)

#define SETTINGS_HEADER_TITLE_Y  4
#define SETTINGS_HEADER_UI_Y     6
#define SETTINGS_ROW_TOP        48
#define SETTINGS_ROW_H          38
#define SETTINGS_ROW_GAP         4   /* 两行之间的间隔 */

/* 当前选中行:0=Language 1=QNH 2=MAP(朝向) 3=RANGE(量程) */
static volatile int s_sel_row = 0;

/* ── 光标控制 ── */

void pk_settings_cursor_next(void)
{
    s_sel_row = (s_sel_row + 1) % 4;
}

int pk_settings_cursor_row(void)
{
    return s_sel_row;
}

/* ── 渲染单行 ── */

static void render_row(uint16_t *fb, int row_idx,
                       const char *key_str, const char *val_str,
                       pk_lang_t lang)
{
    int row_y     = SETTINGS_ROW_TOP + row_idx * (SETTINGS_ROW_H + SETTINGS_ROW_GAP);
    int text_y    = row_y + (SETTINGS_ROW_H / 2) + 3;   /* 垂直居中偏移 */
    int text_y_ui = text_y + 1;

    bool selected = (row_idx == s_sel_row);
    uint16_t col_bg   = selected ? COL_ROW_SEL  : COL_ROW;
    uint16_t col_edge = selected ? COL_ROW_EDGE_SEL : COL_ROW_EDGE;

    /* 行背景 */
    pk_pfd_fill_rect(fb, 10, row_y,
                     PK_DISPLAY_W - 10, row_y + SETTINGS_ROW_H,
                     col_bg);
    /* 左侧彩色边条 */
    pk_pfd_fill_rect(fb, 10, row_y,
                     13, row_y + SETTINGS_ROW_H,
                     col_edge);

    /* 文字渲染:中文用 page_title 字体,英文用 ui 字体 */
    if (lang == PK_LANG_ZH) {
        pk_text_puts_page_title(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                22, text_y, key_str, COL_KEY);
        pk_text_puts_page_title(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                182, text_y, val_str, COL_VAL);
    } else {
        pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        22, text_y_ui, key_str, COL_KEY);
        pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        182, text_y_ui, val_str, COL_VAL);
    }
}

/* ── 主渲染入口 ── */

void pk_settings_page_render(uint16_t *fb)
{
    pk_lang_t lang = pk_i18n_get_lang();

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* 标题 */
    if (lang == PK_LANG_ZH) {
        pk_text_puts_page_title(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                6, SETTINGS_HEADER_TITLE_Y,
                                pk_i18n_text(PK_TR_SETTINGS_TITLE),
                                COL_HEADER);
    } else {
        pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        6, SETTINGS_HEADER_UI_Y,
                        pk_i18n_text(PK_TR_SETTINGS_TITLE),
                        COL_HEADER);
    }
    pk_pfd_fill_rect(fb, 0, 24, PK_DISPLAY_W, 26, COL_DIVIDER);

    /* 行 0: Language */
    render_row(fb, 0,
               pk_i18n_text(PK_TR_SETTINGS_LANGUAGE),
               pk_i18n_lang_name(lang),
               lang);

    /* 行 1: QNH */
    char qnh_buf[20];
    snprintf(qnh_buf, sizeof(qnh_buf), "%.2f hPa", pk_qnh_get());
    render_row(fb, 1, "QNH", qnh_buf, lang);

    /* 行 2: MAP 地图朝向 */
    render_row(fb, 2, "MAP",
               pk_map_orient_get() == PK_MAP_NORTH_UP ? "NORTH UP" : "HDG UP",
               lang);

    /* 行 3: RANGE 雷达量程 */
    char range_buf[16];
    snprintf(range_buf, sizeof(range_buf), "%d NM",
             pk_traffic_range_nm(pk_traffic_range_idx_get()));
    render_row(fb, 3, "RANGE", range_buf, lang);

    /* 底部分隔线 + footer */
    pk_pfd_fill_rect(fb, 0, PK_DISPLAY_H - 18, PK_DISPLAY_W, PK_DISPLAY_H - 17,
                     COL_DIVIDER);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           6, PK_DISPLAY_H - 16,
                           pk_i18n_text(PK_TR_SETTINGS_FOOTER), COL_DIM);
}
