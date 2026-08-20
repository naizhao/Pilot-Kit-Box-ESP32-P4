/*
 * about_page.c — 「关于」页。
 *
 * 定位（spec §5.6）：**纯静态身份**。logo / 版本 / 构建时间 / 依赖版本 /
 * 硬件型号 + 网址，仅此而已。
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
/* 标题的纵向位置改用 PK_UI_TITLE_Y（pfd_layout.h），本页不再自算——原来那个
 * 手调的 PAD_Y-6 是为 L 档配的，换成 M 档后只会把字顶在栏子上沿。
 * 横向同理走 PK_UI_PAD_L：本页原来自留一份 24，比 diag/list 的 16 多缩 8 px，
 * 在切页时看得出来（评审指出 about 标题左边多了个空格）。 */

#define AB_LEFT_X        PK_UI_PAD_L
#define AB_LEFT_W       248
#define AB_LOGO_SIZE    176   /* 176-2*12 略大于源图，见 draw_logo */
#define AB_LOGO_Y        76
#define AB_NAME_Y       (AB_LOGO_Y + AB_LOGO_SIZE + 20)

#define AB_RIGHT_X      304               /* 右栏标签起点 */
#define AB_VALUE_X      452               /* 数值起点：容得下最长的标签 */
#define AB_ROW0_Y        64
#define AB_ROW_H         40               /* M 档 26 px + 14 行距，九行铺满 */
#define AB_ROWS           9               /* 右栏行数，见 pk_about_page_render */

/* 网址跟在产品名之下，同属左栏的身份区。 */
#define AB_URL_Y        (AB_NAME_Y + 44)

/* 混排的垂直对齐归渲染器管（pfd_aa_text.c 的 pk_aa_puts：中西文来自同一份
 * AA 字体，按 cell 顶端对齐），这里不要再叠加一次偏移。
 * 原文写的是 text.c —— 那套 8 px 位图 CJK 字体已随最后一个调用者一并删除。 */

/* ── 配色 ──────────────────────────────────────────────────────── */
#define COL_BG           pk_rgb565( 12,  12,  16)
/* 标题色见 PK_UI_TITLE_COL（pfd_layout.h）：本页原来的淡蓝与其余四页的白不
 * 是一回事，而淡蓝在本页另有主人——COL_URL 那条可点链接。 */
#define COL_KEY          pk_rgb565(150, 170, 195)   /* 标签退一档，让数值出挑 */
#define COL_VAL          pk_rgb565(255, 255, 255)
#define COL_DIVIDER      pk_rgb565( 60,  70,  86)
#define COL_NAME         pk_rgb565(240, 245, 255)
#define COL_LOGO_PLATE   pk_rgb565(255, 255, 255)  /* 与图案白底同色 */
#define COL_URL          pk_rgb565(110, 180, 240)

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
/* 数值区的可用宽度：从数值起点到屏幕右缘，留一个右边距。 */
#define AB_VALUE_W      (PK_DISPLAY_W - AB_VALUE_X - 24)

/*
 * 挑一个装得下的档位。
 *
 * 值的长度不受我们控制——version 是 git describe 的产物
 * （"0.9.3-4.3in-127-g1a2b3c4d-dirty" 这种），构建串与型号也都可能变长。
 * 与其截断（把最有信息量的尾部 sha 切掉），不如降档显示：18 → 14 → 12。
 * 三档都覆盖到 0x7F，降下去仍是完整可读的文本。
 */
static pk_aa_size_t fit_size(const char *s, int avail)
{
    static const pk_aa_size_t kLadder[] = { PK_AA_M, PK_AA_S, PK_AA_XS };
    const int n = (int)strlen(s);
    for (size_t i = 0; i < sizeof(kLadder) / sizeof(kLadder[0]); ++i) {
        if (n * pk_aa_cell_w(kLadder[i]) <= avail) return kLadder[i];
    }
    return PK_AA_XS;
}

/* ── 滚动 ────────────────────────────────────────────────────────
 *
 * 判定规则与 adsb_list / diag / settings 三页**一模一样**：按下只记起点，
 * 位移超过 AB_DRAG_SLOP 才算拖动，松手时没拖过才算点击。四页手感必须一致——
 * 同一台设备上"要按多久才算拖"如果各页不同，手指是学不会的。
 *
 * 这一页当前 9 行、内容高约 362 px，装得进 432 px 的视口，所以平时 max=0、
 * 一动不动。滚动是为**内容长起来之后**准备的：版本号在正式产物里是
 * "0.9.3-4.3in-127-g1a2b3c4d-dirty" 这种 git describe 串，右栏再加两行
 * （许可证、序列号…）就会超屏。真到那天再补交互，就又是一次「点不到最后一行」
 * 的现场事故。 */
static int s_scroll_y;      /* 滚动偏移(px)，0 = 顶 */
static int s_content_h;     /* 上一帧实际画出的内容高（含顶栏以下全部） */

#define AB_VIEW_H     (PK_DISPLAY_H - AB_HEADER_H)
#define AB_DRAG_SLOP  12

/* 滚到底的最大偏移。内容装得下时恒为 0。 */
static int ab_max_scroll(void)
{
    const int over = s_content_h - AB_VIEW_H;
    return over > 0 ? over : 0;
}

static void draw_row(uint16_t *fb, int row, pk_tr_id_t key_id, const char *val)
{
    const int y = AB_ROW0_Y + row * AB_ROW_H - s_scroll_y;
    /* 滚出视口的行直接不画：省一趟 blit，也免得字压到顶栏上（顶栏虽然最后补画
     * 会盖住，但那是靠巧合，不该依赖）。 */
    if (y + pk_aa_cell_h(PK_UI_ITEM_SIZE) < AB_HEADER_H || y >= PK_DISPLAY_H) return;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               AB_RIGHT_X, y, pk_i18n_text(key_id), COL_KEY, PK_UI_ITEM_SIZE);

    /* 降档后字更矮，往下挪半个差值，与标签保持同一条视觉中线。 */
    const pk_aa_size_t vs = fit_size(val, AB_VALUE_W);
    const int dy = (pk_aa_cell_h(PK_UI_ITEM_SIZE) - pk_aa_cell_h(vs)) / 2;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               AB_VALUE_X, y + dy, val, COL_VAL, vs);
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
/* 内边距取 8：内容区正好 176-16 = 160 = 源图尺寸，**1:1 不缩放**。
 * 缩放会让量角器那圈细刻度断成虚线（最近邻采样直接丢像素）——开机画面上
 * 实测过，反馈是「四周有点破」。 */
#define AB_LOGO_PAD     8
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

void pk_about_page_render(uint16_t *fb)
{
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /*
     * 内容高按两栏的**实际底边**取大值，不写死一个数。
     *
     * 写死的话，以后右栏加一行、logo 换个尺寸，滚动范围就悄悄错了，而错法是
     * "最后一行永远够不到"——这种 bug 在真机上极难归因。放在画之前算，是因为
     * 触摸的夹取要用它，而触摸可能在本帧画完之前就来了。
     */
    {
        const int right_bot = AB_ROW0_Y + AB_ROWS * AB_ROW_H;
        const int left_bot  = AB_URL_Y + pk_aa_cell_h(PK_AA_M);
        const int bot = (right_bot > left_bot) ? right_bot : left_bot;
        s_content_h = bot - AB_HEADER_H + 16;      /* 底部留一口气 */
        /* 内容缩短（切语言、少一行）而 s_scroll_y 还停在旧底部时，这一夹把它
         * 拉回来，否则整页看起来空掉一截。 */
        const int max_y = ab_max_scroll();
        if (s_scroll_y > max_y) s_scroll_y = max_y;
    }
#ifdef PK_SIM_BUILD
    { void pk_about_sim_scroll(void); pk_about_sim_scroll(); }
#endif

    /* ── 左栏：身份 ─────────────────────────────────────────── */
    draw_logo(fb, AB_LEFT_X + (AB_LEFT_W - AB_LOGO_SIZE) / 2,
              AB_LOGO_Y - s_scroll_y, AB_LOGO_SIZE);
    {
        static const char kName[] = "PILOT KIT BOX";
        const int w = (int)(sizeof(kName) - 1) * pk_aa_cell_w(PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   AB_LEFT_X + (AB_LEFT_W - w) / 2, AB_NAME_Y - s_scroll_y,
                   kName, COL_NAME, PK_AA_M);
    }

    /* 分栏线：**不跟着滚**。它是版面的骨架而不是内容，跟着走反而会让人以为
     * 整页在平移。上下端各留一点收口。 */
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

    /* ── 网址 ───────────────────────────────────────────────
     * spec §5.6 原本要求二维码。先用文字网址顶上：二维码要占 100 px 见方
     * 才扫得动，而这一页真正的主角是身份与版本；等 URL 与落地页定稿再换回
     * 图形码不迟。 */
    {
        static const char kUrl[] = "https://air.club";
        const int w = (int)(sizeof(kUrl) - 1) * pk_aa_cell_w(PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   AB_LEFT_X + (AB_LEFT_W - w) / 2, AB_URL_Y - s_scroll_y,
                   kUrl, COL_URL, PK_AA_M);
    }

    /* 行数与 AB_ROWS 对不上就地兜住：滚动范围是拿 AB_ROWS 算的，加了一行却
     * 忘了改常量，症状是"最后一行永远滚不到"，而现场根本看不出是常量的锅。 */
    if (row != AB_ROWS) s_content_h += (row - AB_ROWS) * AB_ROW_H;

    /* 滚动条：贴右缘，只在内容超出一屏时出现。样式照 diag_page.c。 */
    if (s_content_h > AB_VIEW_H) {
        const int tx = PK_DISPLAY_W - 6;
        const int bar_h = AB_VIEW_H * AB_VIEW_H / s_content_h;
        const int bar_y = AB_HEADER_H + s_scroll_y * AB_VIEW_H / s_content_h;
        fill_rect(fb, tx, AB_HEADER_H, tx + 3, PK_DISPLAY_H,
                  pk_rgb565(30, 38, 50));
        fill_rect(fb, tx, bar_y, tx + 3, bar_y + bar_h,
                  pk_rgb565(120, 135, 155));
    }

    /* ── 顶栏最后画：内容从它底下滑过去，而不是压在它上面 ───────── */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, AB_HEADER_H - 2, COL_BG);
    /* 标题的字号/颜色/垂直位置由 pfd_layout.h 统一给：这一页曾经是全设备唯一
     * 用 L 档的，比其余四页大整整一档。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               AB_LEFT_X, PK_UI_TITLE_Y,
               pk_i18n_text(PK_TR_ABOUT_TITLE), PK_UI_TITLE_COL,
               PK_UI_TITLE_SIZE);
    fill_rect(fb, 0, AB_HEADER_H - 2, PK_DISPLAY_W, AB_HEADER_H, COL_DIVIDER);
}

/* ── 触摸：拖动滚动 ────────────────────────────────────────────────
 *
 * 与 adsb_list / diag / settings 同一套判定，连阈值都用同一个数：按下只记
 * 起点，位移超 12 px 才算拖动，松手时没拖过才算点击。本页目前没有可点的
 * 东西（网址还不是链接），但规则先立住——等网址变成可点的那天，判定不必
 * 再写一遍，也不会写出第四种手感。
 *
 * 与 touch_gt911.c 的约定同其余三页：返回 true 表示这一下被本页消费。
 */
static int  s_press_x, s_press_y, s_press_scroll;
static bool s_press_valid, s_moved;

bool pk_about_page_touch(int x, int y)
{
    /* 右侧 FAB 那条竖带必须放行，否则关于页就切不走了——列表页正是栽在这里
     * （整个数据区都当命中区，dock 页签的坐标先被页面吃掉）。 */
    s_press_valid = (y >= AB_HEADER_H && x < PK_DISPLAY_W - 80);
    if (!s_press_valid) return false;
    s_press_x      = x;
    s_press_y      = y;
    s_press_scroll = s_scroll_y;
    s_moved        = false;
    return true;
}

bool pk_about_page_drag(int x, int y)
{
    if (!s_press_valid) return false;
    (void)x;
    const int dy = y - s_press_y;
    if (!s_moved && (dy > AB_DRAG_SLOP || dy < -AB_DRAG_SLOP)) s_moved = true;
    if (!s_moved) return true;

    int sy = s_press_scroll - dy;      /* 方向与手指一致：手指上滑，内容上走 */
    const int max_y = ab_max_scroll();
    if (sy < 0)     sy = 0;
    if (sy > max_y) sy = max_y;
    s_scroll_y = sy;
    return true;
}

void pk_about_page_touch_up(void)
{
    s_press_valid = false;
    s_moved       = false;
}

#ifdef PK_SIM_BUILD
/*
 * 截图用的两个旋钮。
 *
 * PK_SIM_ABOUT_PAD=<px>
 *   在内容底部虚加一段高度。今天这一页只有 9 行、内容 392 px，装得进 432 px
 *   的视口，于是滚动范围恒为 0、一张截图也拍不出来。**不能因为暂时用不上就
 *   不验证**——那正是"等真用上了才发现是坏的"那条老路。这个旋钮只改高度、
 *   不伪造任何内容，滚上去露出的就是真实的空白。
 *
 * PK_SIM_ABOUT_SCROLL=<px>
 *   把版面滚到指定位置。刻意走**真实的按下-拖动**两步而不是直接写
 *   s_scroll_y——要验证的正是那条判定链（12 px 阈值、方向、上下夹取），
 *   绕过去就等于没验证。
 */
#include <stdlib.h>
void pk_about_sim_scroll(void)
{
    const char *pad = getenv("PK_SIM_ABOUT_PAD");
    if (pad) s_content_h += atoi(pad);

    const char *e = getenv("PK_SIM_ABOUT_SCROLL");
    if (!e) return;
    const int px = atoi(e);
    if (!pk_about_page_touch(200, 300)) return;
    pk_about_page_drag(200, 300 - px);
    pk_about_page_touch_up();
}
#endif
