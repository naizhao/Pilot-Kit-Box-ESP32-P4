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
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_font.h"

#define STATUSBAR_TOP   0
#define STATUSBAR_BOT  PFD_BAR_BOT

#define COL_BG     pk_rgb565(  8,   8,  12)
#define COL_LABEL  pk_rgb565( 70, 220, 250)
#define COL_GREEN  pk_rgb565(  0, 220,  60)
#define COL_STALE  pk_rgb565(100, 100, 100)
#define COL_RED    pk_rgb565(255,  80,  60)

/* 状态栏水平中线（GPS 段以此居中）。 */
#define MID_CENTRE_X (PFD_CX + 1)

/* 文字渲染器按分辨率取舍：
 *
 *   320×240 —— 沿用 cockpit 字形。它是为航电读数生成的 12×16 子集，
 *              无灰阶毛边、无 TTF hinting 伪影，小屏上比缩放位图清晰
 *              得多；代价是渲染器写死 scale-2（见 pfd_font.h 注释）。
 *
 *   800×480 —— cockpit 的 scale-2 换算下来只有 1.64 mm，低于规格
 *              §2 规定的 2.1 mm 硬下限，故改用可缩放位图取 scale 3
 *              （2.46 mm ≈ S 级）。大字号下位图本就不需要抗锯齿修饰。
 *
 * 两者的每字形步进不同（cockpit 固定 12 px，位图为 6×scale），布局
 * 计算一律走 BAR_GLYPH_W，不再出现魔数。 */
#if PK_DISPLAY_W >= 800
/* 大屏走 TTF 派生的抗锯齿字形（B612 Mono，见 pfd_aa_text.h）。
 * 位图字体整数倍放大会变成方块像素，在 217 PPI 上无法接受。 */
#  define BAR_GLYPH_W   PK_AA_S_W
#  define BAR_PUTS(fb, x, y, str, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (str), (col), PK_AA_S)
#else
#  define BAR_GLYPH_W   12
#  define BAR_PUTS(fb, x, y, str, col) \
        pk_font_puts_cockpit(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (str), (col))
#endif

/* 定宽字体下字符串的像素宽度。 */
#define BAR_TEXT_W(n)   ((n) * BAR_GLYPH_W)

void pk_pfd_statusbar_render(uint16_t *fb, const pk_pfd_status_t *s)
{
    pk_pfd_fill_rect(fb, 0, STATUSBAR_TOP, PK_DISPLAY_W, STATUSBAR_BOT, COL_BG);

    char      buf[12];
    const int ty = PFD_BAR_TEXT_Y;

    /* ── 左段：HDG + 航向，左对齐 ───────────────────────────── */
    {
        int x = PFD_BAR_MARGIN_L;
        BAR_PUTS(fb, x, ty, "HDG", COL_LABEL);
        x += BAR_TEXT_W(3) + PFD_BAR_GAP_LABEL;

        if (s->imu_valid) {
            int hdg = ((int)s->yaw_deg + 360) % 360;
            snprintf(buf, sizeof(buf), "%03d~", hdg);
            BAR_PUTS(fb, x, ty, buf, COL_GREEN);
        } else {
            BAR_PUTS(fb, x, ty, "---~", COL_STALE);
        }
    }

    /* ── 中段：GPS 接收状态，以屏幕中线居中 ─────────────────── */
    {
        uint16_t gps_col;
        if (s->gps_have_fix) {
            snprintf(buf, sizeof(buf), "GPS (%u)", (unsigned)s->gps_sats);
            gps_col = COL_GREEN;
        } else {
            snprintf(buf, sizeof(buf), "NO GPS");
            gps_col = COL_RED;
        }
        int gps_x = MID_CENTRE_X - BAR_TEXT_W((int)strlen(buf)) / 2;
        BAR_PUTS(fb, gps_x, ty, buf, gps_col);
    }

    /* ── 右段：ADSB + 目标计数，右对齐 ──────────────────────
     * 先按计数宽度反推其起点，再据此反推标签起点。原实现把两者的 x
     * 写死为 232 / 288，那是 320 宽面板算出来的，换屏即失效。 */
    {
        snprintf(buf, sizeof(buf), "%2u", (unsigned)s->aircraft_count);
        int cnt_w   = BAR_TEXT_W((int)strlen(buf));
        int cnt_x   = PK_DISPLAY_W - PFD_BAR_MARGIN_R - cnt_w;
        int label_x = cnt_x - PFD_BAR_GAP_WORD - BAR_TEXT_W(4);   /* "ADSB" */

        BAR_PUTS(fb, label_x, ty, "ADSB", COL_LABEL);
        BAR_PUTS(fb, cnt_x,   ty, buf,    COL_GREEN);
    }
}
