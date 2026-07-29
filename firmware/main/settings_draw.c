/*
 * settings_draw.c — 设置页 800×480 的**纯绘制**层（spec §5.4）。
 *
 * 为什么单独一个文件：settings_page.c 里那套格式化两步确认状态机要开
 * FreeRTOS 任务，于是整个文件在模拟器里编不过——而版面恰恰是最需要反复
 * 看效果的部分。把绘制拆出来之后它只依赖各配置模块的 getter，模拟器用桩
 * 喂数据就能一键出图，不必为了挪一个控件去烧一次固件。
 *
 * 分工：本文件只读状态、只画像素；所有写操作（改 QNH、切量程、触发格式化）
 * 仍在 settings_page.c。
 */
#include "settings_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config_ble.h"
#include "config_qnh.h"
#include "config_storage.h"
#include "config_traffic.h"
#include "display.h"
#include "i18n.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "pk_sdcard.h"
#include "record_sink.h"

/* ═══════════════════════════════════════════════════════════════════════
 * 设置页 800×480（spec §5.4）
 *
 * 8 行 × 64 px，控件高 38 px。左半是项名，右半是控件——控件右对齐到同一条
 * 竖线上，扫一眼就知道每项当前选的是哪个，不必逐行找控件在哪。
 *
 * 分段控件（segmented）而不是下拉或滑块：选项都是 2~4 个的离散值，分段把
 * 全部选项和当前选择同时摆出来，一次触摸直达目标；下拉要两次交互，滑块在
 * 离散值上又不好停准。
 * ═════════════════════════════════════════════════════════════════════ */


#define SET_ROW_H      64
#define SET_CTL_H      38
#define SET_PAD        20
#define SET_CTL_R      (PK_DISPLAY_W - 16 - 56 - 12)   /* 避开 FAB，同列表页 */
#define SET_ROWS_VIS   ((PK_DISPLAY_H - PFD_BAR_BOT) / SET_ROW_H)

#define SET_ROWS      9
#define SET_VIEW_H    (PK_DISPLAY_H - PFD_BAR_BOT)
#define SET_MAX_SCROLL  (SET_ROWS * SET_ROW_H > SET_VIEW_H \
                         ? SET_ROWS * SET_ROW_H - SET_VIEW_H : 0)

/* 滚动偏移(px)。8 行 × 64 = 512 > 可视的 432，第 8 行"格式化 SD"必须滚
 * 才看得到——spec §5.4 就是这么写的（"危险按钮（需滚动可见）"），把最危险
 * 的操作放在需要多一个动作才能够到的地方。 */
static int s_set_scroll;

/* 触摸手势：与列表页/诊断页同一套——按下只记起点，位移超阈值才算拖动，
 * 松手时没拖过才当点击。三页共用同一套判定，手感才一致。 */
static int  s_press_y, s_press_scroll;
static bool s_press_valid, s_moved;
#define SET_DRAG_SLOP  12

/* 一个分段控件：n 个选项，sel 为当前项。返回控件左缘，供命中判定复用。 */
static int draw_seg(uint16_t *fb, int y_mid, const char *const *opts, int n,
                    int sel, bool dim)
{
    const uint16_t SEG_OFF = pk_rgb565(28, 36, 48);
    const uint16_t SEG_ON  = pk_rgb565(0, 110, 200);
    const uint16_t SEG_TXT_ON = pk_rgb565(255, 255, 255);
    const uint16_t SEG_TXT_OFF= pk_rgb565(170, 182, 200);
    const uint16_t SEG_DIM    = pk_rgb565(90, 96, 108);

    /* 每段宽度按最长选项算，所有段等宽——不等宽的分段控件在余光里像是
     * "当前项被放大了"，会误以为那是可拖动的滑块。 */
    int maxlen = 1;
    for (int i = 0; i < n; ++i) {
        const int l = (int)strlen(opts[i]);
        if (l > maxlen) maxlen = l;
    }
    const int seg_w = maxlen * pk_aa_cell_w(PK_AA_S) + 20;
    const int total = seg_w * n;
    const int x0    = SET_CTL_R - total;
    const int y0    = y_mid - SET_CTL_H / 2;

    /* 整组先铺一个圆角底槽，再把选中段画成圆角胶囊叠上去——这是分段控件的
     * 标准形态（iOS/Material 都是），比"每段各画一个方块"干净得多：段之间
     * 没有缝，视觉上就是一条被分成几格的轨道，而不是几个独立按钮。 */
    const int radius = SET_CTL_H / 2;      /* 全圆角，扁控件用半高最自然 */
    pk_pfd_fill_round_rect(fb, x0, y0, SET_CTL_R, y0 + SET_CTL_H, radius, SEG_OFF);

    for (int i = 0; i < n; ++i) {
        const int sx = x0 + i * seg_w;
        const bool on = (i == sel);
        if (on && !dim)
            pk_pfd_fill_round_rect(fb, sx + 2, y0 + 2, sx + seg_w - 2,
                                   y0 + SET_CTL_H - 2, radius - 2, SEG_ON);
        const int tw = (int)strlen(opts[i]) * pk_aa_cell_w(PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   sx + (seg_w - 2 - tw) / 2, y0 + (SET_CTL_H - PK_AA_S_H) / 2,
                   opts[i], dim ? SEG_DIM : (on ? SEG_TXT_ON : SEG_TXT_OFF),
                   PK_AA_S);
    }
    return x0;
}

/* 步进器：− 值 +。值居中，两枚按钮等宽，与分段控件右缘对齐。 */
static void draw_stepper(uint16_t *fb, int y_mid, const char *val)
{
    const uint16_t STP_BTN = pk_rgb565(28, 36, 48);
    const uint16_t STP_TXT = pk_rgb565(235, 240, 248);
    const int btn_w = 44;
    const int val_w = 130;
    const int y0    = y_mid - SET_CTL_H / 2;
    const int x0    = SET_CTL_R - (btn_w * 2 + val_w);

    const int r = SET_CTL_H / 2;
    pk_pfd_fill_round_rect(fb, x0, y0, x0 + btn_w, y0 + SET_CTL_H, r, STP_BTN);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + btn_w / 2 - 5,
               y0 + (SET_CTL_H - PK_AA_M_H) / 2, "-", STP_TXT, PK_AA_M);

    const int vw = (int)strlen(val) * pk_aa_cell_w(PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + btn_w + (val_w - vw) / 2,
               y0 + (SET_CTL_H - PK_AA_S_H) / 2, val, STP_TXT, PK_AA_S);

    const int bx = x0 + btn_w + val_w;
    pk_pfd_fill_round_rect(fb, bx, y0, bx + btn_w, y0 + SET_CTL_H, r, STP_BTN);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, bx + btn_w / 2 - 5,
               y0 + (SET_CTL_H - PK_AA_M_H) / 2, "+", STP_TXT, PK_AA_M);
}

void pk_settings_page_render(uint16_t *fb)
{
    const uint16_t V2_BG   = pk_rgb565(7, 10, 16);
    const uint16_t V2_HDR  = pk_rgb565(235, 235, 235);
    const uint16_t V2_KEY  = pk_rgb565(215, 222, 232);
    const uint16_t V2_LINE = pk_rgb565(26, 33, 44);

#ifdef PK_SIM_BUILD
    { void pk_settings_sim_scroll(void); pk_settings_sim_scroll(); }
#endif
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, V2_BG);

    int row = 0;
    #define ROW_Y(i)  (PFD_BAR_BOT + (i) * SET_ROW_H - s_set_scroll + SET_ROW_H / 2)
    #define ROW_LABEL(i, text) do {                                            \
        const int _y = ROW_Y(i);                                               \
        if (_y > PFD_BAR_BOT - SET_ROW_H && _y < PK_DISPLAY_H + SET_ROW_H) {    \
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SET_PAD,                \
                       _y - PK_AA_M_H / 2, (text), V2_KEY, PK_AA_M);          \
            pk_pfd_fill_rect(fb, SET_PAD, _y + SET_ROW_H / 2 - 1,              \
                             SET_CTL_R, _y + SET_ROW_H / 2, V2_LINE);         \
        }                                                                      \
    } while (0)

    /* 1 语言 */
    { static const char *o[] = { "\u4e2d\u6587", "EN" };
      ROW_LABEL(row, "LANGUAGE");
      draw_seg(fb, ROW_Y(row), o, 2, pk_i18n_get_lang() == PK_LANG_ZH ? 0 : 1, false);
      row++; }

    /* 2 QNH —— 步进器：它是连续量，分段摆不下。 */
    { char v[16]; snprintf(v, sizeof(v), "%.2f hPa", (double)pk_qnh_get());
      ROW_LABEL(row, "QNH");
      draw_stepper(fb, ROW_Y(row), v);
      row++; }

    /* 3 地图朝向 */
    { static const char *o[] = { "HDG UP", "NORTH UP" };
      ROW_LABEL(row, "MAP ORIENT");
      draw_seg(fb, ROW_Y(row), o, 2,
               pk_map_orient_get() == PK_MAP_HEADING_UP ? 0 : 1, false);
      row++; }

    /* 4 雷达量程 —— 选项取自 pk_traffic_range_nm，不另抄一份数字。 */
    { static const char *o[] = { "2", "5", "10", "20" };
      ROW_LABEL(row, "RADAR RANGE NM");
      draw_seg(fb, ROW_Y(row), o, 4, pk_traffic_range_idx_get(), false);
      row++; }

    /* 5 屏幕亮度 */
    { static const char *o[] = { "LOW", "MID", "HIGH", "AUTO" };
      ROW_LABEL(row, "BRIGHTNESS");
      /* AUTO 置灰：没有环境光传感器，选了也无从自动。摆出来是因为 spec 列了
       * 它，灰掉是因为不能假装能用——留一个点了没反应的选项更糟。 */
      draw_seg(fb, ROW_Y(row), o, 4, pk_backlight_level_get(), false);
      row++; }

    /* 6 日间/夜间配色 */
    { static const char *o[] = { "DAY", "NIGHT" };
      ROW_LABEL(row, "COLOR SCHEME");
      draw_seg(fb, ROW_Y(row), o, 2, 0, true);   /* 尚未接入，整行置灰 */
      row++; }

    /* 7 记录存储 */
    { static const char *o[] = { "FLASH", "SD CARD" };
      ROW_LABEL(row, "LOG STORAGE");
      draw_seg(fb, ROW_Y(row), o, 2,
               pk_log_store_get() == PK_LOG_STORE_SD ? 1 : 0,
               !pk_sdcard_is_mounted());
      row++; }

    /* 8 蓝牙开关（P2-4）——放在格式化之前，让危险按钮独占最底下那一行。 */
    { static const char *o[] = { "OFF", "ON" };
      ROW_LABEL(row, "BLUETOOTH");
      draw_seg(fb, ROW_Y(row), o, 2, pk_ble_enabled_get() ? 1 : 0, false);
      /* 「重启后生效」必须写出来：BLE 起停牵扯 NimBLE 的卸载路径，更牵扯
       * hosted 握手要排在点屏之前那条硬约束，运行时切会打乱开机顺序。
       * 不写的话，点了没反应会被当成 bug。 */
      { const int _y = ROW_Y(row);
        if (_y > PFD_BAR_BOT - SET_ROW_H && _y < PK_DISPLAY_H + SET_ROW_H)
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                       SET_PAD + 11 * pk_aa_cell_w(PK_AA_M) + 12,
                       _y - PK_AA_XS_H / 2, "(restart)",
                       pk_rgb565(120, 130, 145), PK_AA_XS); }
      row++; }

    /* 9 格式化 SD —— 危险按钮，红底。文案跟着两步确认状态机走。 */
    { ROW_LABEL(row, "FORMAT SD");
      const int y_mid = ROW_Y(row);
      const int y0 = y_mid - SET_CTL_H / 2;
      const int w  = 200;
      const int x0 = SET_CTL_R - w;
      const bool armed = (pk_settings_format_state() == 1);
      const bool avail = pk_sdcard_is_mounted() && !record_sink_file_uses_sd();
      pk_pfd_fill_round_rect(fb, x0, y0, x0 + w, y0 + SET_CTL_H, SET_CTL_H / 2,
                       !avail  ? pk_rgb565(45, 45, 50)
                       : armed ? pk_rgb565(200, 40, 40)
                               : pk_rgb565(90, 30, 30));
      const char *txt = !pk_sdcard_is_mounted() ? "NO CARD"
                      : record_sink_file_uses_sd() ? "IN USE"
                      : armed ? "TAP AGAIN 5s" : "FORMAT";
      const int tw = (int)strlen(txt) * pk_aa_cell_w(PK_AA_S);
      pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + (w - tw) / 2,
                 y0 + (SET_CTL_H - PK_AA_S_H) / 2, txt,
                 avail ? pk_rgb565(255, 255, 255) : pk_rgb565(120, 124, 132),
                 PK_AA_S);
      row++; }

    /* 顶栏最后画，行从底下滑过。 */
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PFD_BAR_BOT, V2_BG);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SET_PAD,
               (PFD_BAR_BOT - PK_AA_M_H) / 2, "SETTINGS", V2_HDR, PK_AA_M);
    #undef ROW_Y
    #undef ROW_LABEL
}

/* ── 触摸 ──────────────────────────────────────────────────────────
 * 目前只做滚动。各控件的点击命中要等写操作那侧（改 QNH、切量程…）一起接，
 * 那些 setter 在 settings_page.c 里；渲染层先把滚动落地，否则第 8 行根本
 * 够不到。 */
bool pk_settings_page_touch(int x, int y)
{
    /* 右侧 FAB 那条必须放行，否则设置页就切不走了（列表页踩过这个坑）。 */
    s_press_valid = (y >= PFD_BAR_BOT && x < SET_CTL_R + 8);
    if (!s_press_valid) return false;
    s_press_y      = y;
    s_press_scroll = s_set_scroll;
    s_moved        = false;
    return true;
}

bool pk_settings_page_drag(int x, int y)
{
    if (!s_press_valid) return false;
    (void)x;
    const int dy = y - s_press_y;
    if (!s_moved && (dy > SET_DRAG_SLOP || dy < -SET_DRAG_SLOP)) s_moved = true;
    if (!s_moved) return true;

    int sy = s_press_scroll - dy;      /* 方向与手指一致 */
    if (sy < 0) sy = 0;
    if (sy > SET_MAX_SCROLL) sy = SET_MAX_SCROLL;
    s_set_scroll = sy;
    return true;
}

void pk_settings_page_touch_up(void)     { s_press_valid = false; s_moved = false; }
void pk_settings_page_touch_cancel(void) { s_press_valid = false; s_moved = false; }

#ifdef PK_SIM_BUILD
/* 截图用：PK_SIM_SET_SCROLL=<px> 直接把版面滚到指定位置。 */
#include <stdlib.h>
void pk_settings_sim_scroll(void)
{
    const char *e = getenv("PK_SIM_SET_SCROLL");
    if (e) {
        int v = atoi(e);
        s_set_scroll = v < 0 ? 0 : (v > SET_MAX_SCROLL ? SET_MAX_SCROLL : v);
    }
}
#endif
