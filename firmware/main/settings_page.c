/*
 * settings_page.c — 设置页的**状态与写操作**（渲染在 settings_draw.c）。
 *
 * 本文件只保留光标位置、FORMAT SD 两步确认状态机，以及 pk_settings_apply()
 * 这个「把一次控件操作落到 NVS」的入口。绘制早已整体搬到 settings_draw.c
 * （它拥有几何与命中判定），这里一个像素都不画。
 *
 * 2026-07-30：删掉了 320×240 时代的逐行渲染器 settings_render_legacy() 与它
 * 专属的 render_row/render_row_col、配色与行距常量。它自 8 项版面（spec §5.4）
 * 上线起就带着 __attribute__((unused)) 挂在这里「留作参照」，一年没有调用者；
 * 硬件已换成 4.3″ 800×480 触摸屏，不会再退回 2.4″ 逐行版面，留着只是占 app
 * 分区（当时余量只剩 10%）。
 */

#include "settings_page.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display.h"
#include "i18n.h"
#include "config_ble.h"
#include "config_qnh.h"
#include "config_storage.h"
#include "config_traffic.h"
#include "pk_sdcard.h"
#include "record_sink.h"

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
               * display.c 的 s_bl_step_duty[]。只有 LOW/MID/HIGH 三档：
               * 板上没有环境光传感器，AUTO 那一格已经从控件里去掉了。 */
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
