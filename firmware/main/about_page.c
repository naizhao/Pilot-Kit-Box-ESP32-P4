/*
 * about_page.c — 「关于」页。
 *
 * 定位（spec §5.6）：**纯静态身份**。logo / 版本 / 构建时间 / 依赖版本 /
 * 硬件型号 + 二维码，仅此而已。
 *
 * 所有 ✓ 状态灯、校准精度、实时数值一律归诊断页，**两页零重复**。旧版把
 * IMU 校准指示器放在这里，是错的——那是运行时状态，不是身份。
 *
 * 布局（800×480）
 * ---------------
 *   y=0    ┌──────────────┬───────────────────────────────────────┐
 *          │ 关于          │                                       │ 顶栏 48
 *   y=48   ├──────────────┼───────────────────────────────────────┤
 *          │              │ 版本    v0.9.3-4.3in                   │
 *          │   [ logo ]   │ 构建    Jul 28 2026 21:40:00           │
 *          │   128×128    │ ESP-IDF v6.0.1                         │
 *          │              │ LVGL    v9.5.0                         │
 *          │ PILOT KIT    │ 开发板  Waveshare P4 4.3               │
 *          │ BOX          │ 芯片    ESP32-P4 v1.3                  │
 *          │              │ 屏幕    ST7701 800x480        [ QR ]   │
 *   y=480  └──────────────┴───────────────────────────────────────┘
 *
 * 字号按 spec §2 的硬阶梯：标题 L(30) / 标签 M(26) / 数值 S(21)。低于 18 px
 * 一律禁止——旧版那套 8×8 汉字在这块屏上只有 1.0 mm，已永久废弃。
 */

#include "about_page.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

#include "display.h"
#include "i18n.h"
#include "logo_blob.h"
#include "pfd_layout.h"
#include "pfd_aa_text.h"
#include "pfd_aa_font.h"
#include "ui_state.h"

/* 模拟器不链接 LVGL 的版本头，给一份与 idf_component.yml 锁定值一致的兜底。 */
#ifndef LVGL_VERSION_MAJOR
#  define LVGL_VERSION_MAJOR 9
#  define LVGL_VERSION_MINOR 5
#  define LVGL_VERSION_PATCH 0
#endif

/* ── 布局 ────────────────────────────────────────────────────────
 * 左栏放身份（logo + 产品名），右栏放事实（键值）。分栏而不是一列到底，
 * 是因为这一页的主角是「这台设备是什么」，logo 该有它的分量；而 800 px 宽
 * 用单列排版会让每行右侧空掉一大片。 */
#define AB_HEADER_H      PFD_BAR_BOT      /* 与 PFD 状态栏等高，切页时不跳 */
#define AB_HEADER_PAD_X  24
#define AB_HEADER_PAD_Y   9

#define AB_LEFT_X        24
#define AB_LEFT_W       248
#define AB_LOGO_SIZE    128
#define AB_LOGO_Y        84
#define AB_NAME_Y       (AB_LOGO_Y + AB_LOGO_SIZE + 24)

#define AB_RIGHT_X      304               /* 右栏标签起点 */
#define AB_VALUE_X      452               /* 数值起点：容得下最长的标签 */
#define AB_ROW0_Y        64
#define AB_ROW_H         40               /* M 档 26 px + 14 行距，九行铺满 */

/* 二维码放左栏（身份区）下方，不放右下角：右下既会压住最后一行键值，也正好
 * 是 FAB 的位置。左栏 logo 与产品名之下本来就有空档，语义上也更贴——它同属
 * 「这台设备是什么」。 */
#define AB_QR_SIZE      104
#define AB_QR_X         (AB_LEFT_X + (AB_LEFT_W - AB_QR_SIZE) / 2)
#define AB_QR_Y         308

/* 混排的垂直对齐已经在 text.c 内部处理（CJK 相对拉丁 cell 下移半个差值），
 * 这里不要再叠加一次。 */

/* ── 配色 ──────────────────────────────────────────────────────── */
#define COL_BG           pk_rgb565( 12,  12,  16)
#define COL_HEADER       pk_rgb565(180, 235, 255)
#define COL_KEY          pk_rgb565(150, 170, 195)   /* 标签退一档，让数值出挑 */
#define COL_VAL          pk_rgb565(255, 255, 255)
#define COL_DIVIDER      pk_rgb565( 60,  70,  86)
#define COL_NAME         pk_rgb565(240, 245, 255)
#define COL_LOGO_PLATE   pk_rgb565(255, 255, 255)  /* 与图案白底同色 */
#define COL_LOGO_MARK    pk_rgb565( 64, 156, 255)
#define COL_QR_PLATE     pk_rgb565(238, 240, 245)
#define COL_QR_INK       pk_rgb565( 12,  12,  16)

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

/*
 * 一行「标签 数值」。
 *
 * 标签与数值走**同一份字体的同一档**（S）——中西文都由 gen_pfd_aa_font.py
 * 一次生成，CJK 的 cell 高度与拉丁一致，所以这里不需要任何垂直补偿。
 */
static void draw_row(uint16_t *fb, int row, pk_tr_id_t key_id, const char *val)
{
    const int y = AB_ROW0_Y + row * AB_ROW_H;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               AB_RIGHT_X, y, pk_i18n_text(key_id), COL_KEY, PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               AB_VALUE_X, y, val, COL_VAL, PK_AA_S);
}

/*
 * 画 logo：白色圆角底板 + 居中的完整图案。
 *
 * 关键是**留内边距**。上一版沿用了开机画面的 LOGO_SRC_CROP=24 去裁源图——
 * 那个值是为 80×80 的小尺寸调的（裁掉 SVG 导出的空白好让图案占满），放到
 * 128×128 上就把六边形外框整个切掉了。
 *
 * 图标该有的样子是：图案完整，四周留一圈内边距，再套圆角。所以这里不裁源图，
 * 而是把它整体缩进 AB_LOGO_PAD，空出来的部分由白色底板填充。
 */
#define AB_LOGO_PAD     12
#define AB_LOGO_RADIUS  (AB_LOGO_SIZE * 22 / 100)

/* 该像素是否落在圆角矩形内。只在四个角上做圆检测，其余直接通过。 */
static bool in_rounded_rect(int col, int row, int size, int r)
{
    int dx = 0, dy = 0;
    if (col < r)                 dx = r - col;
    else if (col >= size - r)    dx = col - (size - r - 1);
    if (row < r)                 dy = r - row;
    else if (row >= size - r)    dy = row - (size - r - 1);
    if (dx == 0 || dy == 0) return true;
    return dx * dx + dy * dy <= r * r;
}

static void draw_logo(uint16_t *fb, int x, int y, int size)
{
    int sw = 0, sh = 0;
    const uint16_t *src = pk_logo_bitmap(&sw, &sh);
    if (src == NULL || sw <= 0 || sh <= 0) return;

    const int inner = size - 2 * AB_LOGO_PAD;

    for (int row = 0; row < size; ++row) {
        const int yy = y + row;
        if (yy < 0 || yy >= PK_DISPLAY_H) continue;
        uint16_t *dst = fb + yy * PK_DISPLAY_W;

        for (int col = 0; col < size; ++col) {
            const int xx = x + col;
            if (xx < 0 || xx >= PK_DISPLAY_W) continue;
            if (!in_rounded_rect(col, row, size, AB_LOGO_RADIUS)) continue;

            const int ix = col - AB_LOGO_PAD;
            const int iy = row - AB_LOGO_PAD;
            if (ix < 0 || iy < 0 || ix >= inner || iy >= inner) {
                dst[xx] = COL_LOGO_PLATE;      /* 内边距：底板本色 */
                continue;
            }
            /* 整图缩放，不裁——外框也是图案的一部分。 */
            const uint16_t v = src[(iy * sh / inner) * sw + (ix * sw / inner)];
            /* blob 与 framebuffer 的字节序约定不同，见 display.h 的 pk_rgb565()。 */
            dst[xx] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

/*
 * 二维码占位。
 *
 * spec §5.6 要求本页带二维码（指向产品页 / 说明书）。目标 URL 尚未确定，
 * 先按最终尺寸画出静区与三个定位角，把版面占住——URL 定了用生成器产出真码
 * 替换即可，周围留白与位置都已按最终形态摆好。
 */
static void draw_qr_placeholder(uint16_t *fb, int x, int y, int size)
{
    fill_rect(fb, x, y, x + size, y + size, COL_QR_PLATE);

    const int m = 10;                  /* 静区 */
    const int e = (size - 2 * m) / 4;  /* 定位角边长 */
    const int corners[3][2] = {
        { x + m,            y + m },
        { x + size - m - e, y + m },
        { x + m,            y + size - m - e },
    };
    for (int i = 0; i < 3; ++i) {
        const int cx0 = corners[i][0], cy0 = corners[i][1];
        fill_rect(fb, cx0, cy0, cx0 + e, cy0 + e, COL_QR_INK);
        fill_rect(fb, cx0 + e / 4, cy0 + e / 4,
                  cx0 + e - e / 4, cy0 + e - e / 4, COL_QR_PLATE);
        fill_rect(fb, cx0 + e * 3 / 8, cy0 + e * 3 / 8,
                  cx0 + e * 5 / 8, cy0 + e * 5 / 8, COL_QR_INK);
    }
}

void pk_about_page_render(uint16_t *fb)
{
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* ── 顶栏 ───────────────────────────────────────────────── */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               AB_HEADER_PAD_X, AB_HEADER_PAD_Y - 6,
               pk_i18n_text(PK_TR_ABOUT_TITLE), COL_HEADER, PK_AA_M);
    fill_rect(fb, 0, AB_HEADER_H - 2, PK_DISPLAY_W, AB_HEADER_H, COL_DIVIDER);

    /* ── 左栏：身份 ─────────────────────────────────────────── */
    draw_logo(fb, AB_LEFT_X + (AB_LEFT_W - AB_LOGO_SIZE) / 2, AB_LOGO_Y,
              AB_LOGO_SIZE);
    {
        static const char kName[] = "PILOT KIT BOX";
        const int w = (int)(sizeof(kName) - 1) * pk_aa_cell_w(PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   AB_LEFT_X + (AB_LEFT_W - w) / 2, AB_NAME_Y,
                   kName, COL_NAME, PK_AA_S);
    }

    /* 分栏线 */
    fill_rect(fb, AB_RIGHT_X - 32, AB_HEADER_H + 16,
              AB_RIGHT_X - 31, PK_DISPLAY_H - 24, COL_DIVIDER);

    /* ── 右栏：事实 ─────────────────────────────────────────── */
    const esp_app_desc_t *app = esp_app_get_description();
    char tmp[48];
    int row = 0;

    draw_row(fb, row++, PK_TR_ABOUT_VERSION, app ? app->version : "?");

    {
        /* 构建时间只取到分。秒对「这台设备是什么」没有信息量，却要多占三个
         * 字宽——而右栏宽度是有限的（452 起到 776 止，约 18 个 S 档字符）。 */
        const char *t = app ? app->time : __TIME__;
        snprintf(tmp, sizeof(tmp), "%s %.5s", app ? app->date : __DATE__, t);
        draw_row(fb, row++, PK_TR_ABOUT_BUILD, tmp);
    }

    snprintf(tmp, sizeof(tmp), "v%d.%d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
    draw_row(fb, row++, PK_TR_ABOUT_IDF, tmp);

    /* LVGL 版本也属「依赖版本」，spec §5.6 点名要求。 */
    snprintf(tmp, sizeof(tmp), "v%d.%d.%d",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    draw_row(fb, row++, PK_TR_ABOUT_LVGL, tmp);

    draw_row(fb, row++, PK_TR_ABOUT_BOARD, "Waveshare P4 4.3");

    {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        snprintf(tmp, sizeof(tmp), "ESP32-P4 v%d.%d",
                 chip.revision / 100, chip.revision % 100);
        draw_row(fb, row++, PK_TR_ABOUT_CHIP, tmp);
    }

    draw_row(fb, row++, PK_TR_ABOUT_DISPLAY, "ST7701 800x480");

    /* 姿态与接收机也是「硬件型号」，spec §5.6 归类里有它们；字段本身沿用
     * 既有实现，不按 spec 的示意图删改。 */
    draw_row(fb, row++, PK_TR_ABOUT_IMU, "BNO085 I2C0 0x4A");
    draw_row(fb, row++, PK_TR_ABOUT_DONGLE, "RTL-SDR 2MS/s");

    /* ── 二维码 ─────────────────────────────────────────────── */
    draw_qr_placeholder(fb, AB_QR_X, AB_QR_Y, AB_QR_SIZE);
}
