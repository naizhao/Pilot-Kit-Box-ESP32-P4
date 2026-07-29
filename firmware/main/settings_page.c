/*
 * settings_page.c — language/settings screen (多行可选, Task 9 扩展).
 *
 * 行布局:
 *   行 0: Language  <EN/中文>
 *   行 1: QNH       <1013.25 hPa>
 *   行 2: MAP       <HDG UP / NORTH UP>
 *   行 3: RANGE     <2/5/10/20 NM>
 *   行 4: LOG       <FLASH / MICROSD>      — ADS-B 日志存储位置(重启生效)
 *   行 5: FORMAT SD <两步确认格式化>
 *
 * 选中行用高亮边条(COL_ROW_EDGE_SEL)区分未选中行(COL_ROW_EDGE)。
 * 光标状态由 s_sel_row 维护,pk_settings_cursor_next() 切换。
 */

#include "settings_page.h"

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
#include "i18n.h"
#include "pfd_draw.h"
#include "text.h"
#include "config_qnh.h"
#include "config_storage.h"
#include "config_traffic.h"
#include "pk_sdcard.h"
#include "record_sink.h"

#define COL_BG              pk_rgb565( 12,  12,  16)
#define COL_HEADER          pk_rgb565(180, 235, 255)
#define COL_ROW             pk_rgb565( 30,  36,  44)
#define COL_ROW_SEL         pk_rgb565( 20,  44,  60)   /* 选中行背景(略深蓝) */
#define COL_ROW_EDGE        pk_rgb565( 60,  80,  90)   /* 未选中边条(暗) */
#define COL_ROW_EDGE_SEL    pk_rgb565( 80, 220, 240)   /* 选中边条(亮青) */
#define COL_KEY             pk_rgb565(180, 235, 255)
#define COL_VAL             pk_rgb565(255, 255, 255)
#define COL_WARN            pk_rgb565(255, 180,  80)   /* 格式化确认/进行中 */
#define COL_DIM             pk_rgb565(255, 255, 255)
#define COL_DIVIDER         pk_rgb565( 60,  60,  70)

#define SETTINGS_HEADER_TITLE_Y  4
#define SETTINGS_HEADER_UI_Y     6
#define SETTINGS_ROW_TOP        48
/* 6 行布局: 24px 行高 + 4px 间隔 = 28px 步距,48 + 6×28 = 216,footer 不挤。
 * (原 4 行时代是 38px 行高,6 行装不下 240px 高的屏。) */
#define SETTINGS_ROW_H          24
#define SETTINGS_ROW_GAP         4
#define SETTINGS_ROW_COUNT       6

static const char *TAG = "settings";

/* 当前选中行:0=Language 1=QNH 2=MAP 3=RANGE 4=LOG 5=FORMAT SD */
static volatile int s_sel_row = 0;

/* ── FORMAT SD 两步确认状态机 ──
 * IDLE -(按键,有卡)-> ARM -(5s 内再按)-> BUSY -> DONE/FAIL -(3s)-> IDLE
 * ARM 超时 5s 自动回 IDLE;无卡/日志正占用 SD 时给短暂提示。 */
typedef enum {
    FMT_IDLE = 0,
    FMT_ARM,
    FMT_BUSY,
    FMT_DONE,
    FMT_FAIL,
    FMT_INUSE,    /* 日志后端正写 SD,拒绝格式化 */
} fmt_state_t;

#define FMT_ARM_TIMEOUT_US   (5 * 1000 * 1000)
#define FMT_MSG_HOLD_US      (3 * 1000 * 1000)

static volatile fmt_state_t s_fmt_state = FMT_IDLE;
static volatile int64_t     s_fmt_since;     /* 进入当前状态的时刻 */

/* 状态超时衰减(渲染/按键路径都调,幂等) */
static void fmt_decay(void)
{
    int64_t now = esp_timer_get_time();
    fmt_state_t st = s_fmt_state;
    if (st == FMT_ARM && now - s_fmt_since > FMT_ARM_TIMEOUT_US) {
        s_fmt_state = FMT_IDLE;
    } else if ((st == FMT_DONE || st == FMT_FAIL || st == FMT_INUSE) &&
               now - s_fmt_since > FMT_MSG_HOLD_US) {
        s_fmt_state = FMT_IDLE;
    }
}

/* 一次性格式化任务 — pk_sdcard_format() 阻塞数秒,不能在按键回调里跑 */
static void fmt_task(void *arg)
{
    (void)arg;
    esp_err_t err = pk_sdcard_format();
    s_fmt_state = (err == ESP_OK) ? FMT_DONE : FMT_FAIL;
    s_fmt_since = esp_timer_get_time();
    vTaskDelete(NULL);
}

/* 两步确认状态机的当前态，给渲染层用（1 = 已 ARM，等第二次点击）。
 * 直接暴露枚举会把内部状态定义泄出去，这里只回答渲染真正关心的那一件事。 */
int pk_settings_format_state(void)
{
    fmt_decay();
    return (s_fmt_state == FMT_ARM) ? 1 : 0;
}

void pk_settings_format_action(void)
{
    fmt_decay();
    switch (s_fmt_state) {
    case FMT_IDLE:
        if (!pk_sdcard_is_mounted()) {
            /* 无卡:状态机不进 ARM,渲染端直接显示 NO CARD,无需提示态 */
            return;
        }
        if (record_sink_file_uses_sd()) {
            /* 当前日志正写这张卡,格式化会毁掉打开中的文件 */
            ESP_LOGW(TAG, "format refused: log sink is writing to microSD");
            s_fmt_state = FMT_INUSE;
            s_fmt_since = esp_timer_get_time();
            return;
        }
        s_fmt_state = FMT_ARM;
        s_fmt_since = esp_timer_get_time();
        break;
    case FMT_ARM:
        s_fmt_state = FMT_BUSY;
        s_fmt_since = esp_timer_get_time();
        if (xTaskCreate(fmt_task, "sd_format", 4096, NULL, 3, NULL) != pdTRUE) {
            ESP_LOGE(TAG, "xTaskCreate(sd_format) failed");
            s_fmt_state = FMT_FAIL;
            s_fmt_since = esp_timer_get_time();
        }
        break;
    default:
        /* BUSY/DONE/FAIL/INUSE — 忽略按键 */
        break;
    }
}

/* ── 光标控制 ── */

void pk_settings_cursor_next(void)
{
    s_sel_row = (s_sel_row + 1) % SETTINGS_ROW_COUNT;
}

int pk_settings_cursor_row(void)
{
    return s_sel_row;
}

/* ── 渲染单行 ── */

static void render_row_col(uint16_t *fb, int row_idx,
                           const char *key_str, const char *val_str,
                           uint16_t val_col)
{
    int row_y     = SETTINGS_ROW_TOP + row_idx * (SETTINGS_ROW_H + SETTINGS_ROW_GAP);
    /* UI 字体高 12px,在 24px 行内垂直居中:顶 = row_y + (ROW_H-12)/2 = +6 */
    int text_y_ui = row_y + (SETTINGS_ROW_H - 12) / 2;

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

    /* 统一用平滑 UI 字体(含完整 ASCII + i18n 中文)。
     * 原来中文界面走 page_title 时,ASCII(QNH/MAP/RANGE/HDG UP/NM…) 退化成
     * 5×7 点阵放大字,与平滑中文混排显得破糙;settings 是密集混排 UI 页,
     * 正是 pk_text_puts_ui 的设计场景。两种语言现在都走同一平滑路径。 */
    pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                    22, text_y_ui, key_str, COL_KEY);
    pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                    182, text_y_ui, val_str, val_col);
}

static void render_row(uint16_t *fb, int row_idx,
                       const char *key_str, const char *val_str,
                       pk_lang_t lang)
{
    (void)lang;
    render_row_col(fb, row_idx, key_str, val_str, COL_VAL);
}

/* ── 主渲染入口 ── */

/* 旧的 320×240 逐行视图。8 项版面（spec §5.4）改用下面的 draw_settings_v2，
 * 这段留着做参照——格式化那套两步确认状态机仍在用它验证过的时序。 */
__attribute__((unused))
static void settings_render_legacy(uint16_t *fb)
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

    /* 行 4: LOG 日志存储位置(NVS 即存,后端重启生效) */
    {
        bool want_sd  = (pk_log_store_get() == PK_LOG_STORE_SD);
        bool on_sd    = record_sink_file_uses_sd();
        const char *v = want_sd ? (on_sd ? "MICROSD" : "MICROSD (REBOOT)")
                                : (on_sd ? "FLASH (REBOOT)"   : "FLASH");
        render_row(fb, 4, "LOG", v, lang);
    }

    /* 行 5: FORMAT SD 两步确认 */
    {
        fmt_decay();
        const char *v;
        uint16_t    c = COL_VAL;
        switch (s_fmt_state) {
        case FMT_ARM:   v = "AGAIN TO CONFIRM"; c = COL_WARN; break;
        case FMT_BUSY:  v = "FORMATTING...";    c = COL_WARN; break;
        case FMT_DONE:  v = "DONE";                           break;
        case FMT_FAIL:  v = "FAILED";           c = COL_WARN; break;
        case FMT_INUSE: v = "IN USE BY LOG";    c = COL_WARN; break;
        default:
            v = pk_sdcard_is_mounted() ? "PRESS UP/DOWN" : "NO CARD";
            break;
        }
        render_row_col(fb, 5, "FORMAT SD", v, c);
    }

    /* 底部分隔线 + footer */
    pk_pfd_fill_rect(fb, 0, PK_DISPLAY_H - 18, PK_DISPLAY_W, PK_DISPLAY_H - 17,
                     COL_DIVIDER);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           6, PK_DISPLAY_H - 16,
                           pk_i18n_text(PK_TR_SETTINGS_FOOTER), COL_DIM);
}

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

#include "pfd_aa_text.h"
#include "pfd_aa_font.h"
#include "pfd_layout.h"

#define SET_ROW_H      64
#define SET_CTL_H      38
#define SET_PAD        20
#define SET_CTL_R      (PK_DISPLAY_W - 16 - 56 - 12)   /* 避开 FAB，同列表页 */
#define SET_ROWS_VIS   ((PK_DISPLAY_H - PFD_BAR_BOT) / SET_ROW_H)

static int s_set_scroll;      /* 滚动偏移(px) */

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

    for (int i = 0; i < n; ++i) {
        const int sx = x0 + i * seg_w;
        const bool on = (i == sel);
        pk_pfd_fill_rect(fb, sx, y0, sx + seg_w - 2, y0 + SET_CTL_H,
                         dim ? SEG_OFF : (on ? SEG_ON : SEG_OFF));
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

    pk_pfd_fill_rect(fb, x0, y0, x0 + btn_w, y0 + SET_CTL_H, STP_BTN);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + btn_w / 2 - 5,
               y0 + (SET_CTL_H - PK_AA_M_H) / 2, "-", STP_TXT, PK_AA_M);

    const int vw = (int)strlen(val) * pk_aa_cell_w(PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + btn_w + (val_w - vw) / 2,
               y0 + (SET_CTL_H - PK_AA_S_H) / 2, val, STP_TXT, PK_AA_S);

    const int bx = x0 + btn_w + val_w;
    pk_pfd_fill_rect(fb, bx, y0, bx + btn_w, y0 + SET_CTL_H, STP_BTN);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, bx + btn_w / 2 - 5,
               y0 + (SET_CTL_H - PK_AA_M_H) / 2, "+", STP_TXT, PK_AA_M);
}

void pk_settings_page_render(uint16_t *fb)
{
    const uint16_t V2_BG   = pk_rgb565(7, 10, 16);
    const uint16_t V2_HDR  = pk_rgb565(235, 235, 235);
    const uint16_t V2_KEY  = pk_rgb565(215, 222, 232);
    const uint16_t V2_LINE = pk_rgb565(26, 33, 44);

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

    /* 8 格式化 SD —— 危险按钮，红底。文案跟着两步确认状态机走。 */
    { ROW_LABEL(row, "FORMAT SD");
      const int y_mid = ROW_Y(row);
      const int y0 = y_mid - SET_CTL_H / 2;
      const int w  = 200;
      const int x0 = SET_CTL_R - w;
      const bool armed = (pk_settings_format_state() == 1);
      const bool avail = pk_sdcard_is_mounted() && !record_sink_file_uses_sd();
      pk_pfd_fill_rect(fb, x0, y0, x0 + w, y0 + SET_CTL_H,
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
