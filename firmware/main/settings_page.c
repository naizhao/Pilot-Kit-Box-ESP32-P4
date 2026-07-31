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
#include "config_ble.h"
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

/*
 * 执行一次设置变更。row 是设置页的行号，v 的含义随控件而定：
 * 分段=段序号，步进器=±1，按钮=0。
 *
 * 命中判定在 settings_draw.c（它拥有几何），写操作在这里——那边是纯绘制，
 * 不该碰 NVS，更不该起格式化任务。
 */
void pk_settings_apply(int row, int v)
{
    switch (row) {
    case 0:   /* 语言 */
        pk_i18n_set_lang(v == 0 ? PK_LANG_ZH : PK_LANG_EN);
        break;

    case 1:   /* QNH ±0.01 hPa —— 与航空习惯一致（拨轮一格 0.01）。
               * 长按连调等手势层做出来再说，现在一下一格。 */
        pk_qnh_set(pk_qnh_get() + (v > 0 ? 0.01f : -0.01f));
        break;

    case 2:   /* 地图朝向 */
        pk_map_orient_set(v == 0 ? PK_MAP_HEADING_UP : PK_MAP_NORTH_UP);
        break;

    case 3:   /* 雷达量程 */
        pk_traffic_range_idx_set(v);
        break;

    case 4:   /* 屏幕亮度。传的是段序号，不是占空比——档位到亮度值的映射在
               * display.c 的 s_bl_step_duty[]。AUTO(=3) 暂不可用：没有环境光
               * 传感器，选了也无从自动，step_set 会忽略它、保持原档。 */
        pk_backlight_step_set((uint8_t)v);
        break;

    case 5:   /* 日间/夜间配色 —— 尚未接入，整行置灰，点击无动作 */
        break;

    case 6:   /* 记录存储。无卡时选 SD 没意义，直接忽略——渲染那边也是置灰的，
               * 两处判据要一致，否则会出现"看着灰的却点得动"。 */
        if (pk_sdcard_is_mounted() || v == 0)
            pk_log_store_set(v == 1 ? PK_LOG_STORE_SD : PK_LOG_STORE_FLASH);
        break;

    case 7:   /* 蓝牙开关（下次开机生效，行尾已标 restart） */
        pk_ble_enabled_set(v == 1);
        break;

    case 8:   /* 格式化 SD —— 复用两步确认状态机，第一次 ARM、第二次才真格式化 */
        pk_settings_format_action();
        break;

    default:
        break;
    }
}
