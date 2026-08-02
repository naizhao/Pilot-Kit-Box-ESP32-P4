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

#include "ble_gatt.h"
#include "display.h"
#include "i18n.h"
#include "config_ble.h"
#include "config_demo.h"
#include "config_devname.h"
#include "config_qnh.h"
#include "config_storage.h"
#include "config_traffic.h"
#include "keyboard_page.h"
#include "pk_sdcard.h"
#include "record_sink.h"
#include "ui_state.h"      /* pk_ui_toast_show —— 演示模式开关的即时反馈 */

/*
 * 行数必须与 settings_draw.c 的 SET_ROWS 一致——那边才是版面的真源。
 * 这里原来写着 6，是 320×240 时代六项版面的遗留：4.3″ 上早已是 9 行（现在
 * 10 行），于是 cursor_next() 转到第 6 行就绕回去了，后面几项按键根本选不到。
 * 触摸上线后这条路径没人走，问题才一直没被发现。
 */
#define SETTINGS_ROW_COUNT       11

/* 键盘编辑器会把 max_len **静默**夹到自己的缓冲上限（keyboard_page.c 的
 * pk_keyboard_page_open）。两个上限一旦反过来，症状是「屏上敲得满、确定之后
 * 名字短了两个字符」——用户只会觉得设备把输入弄坏了。钉成编译期断言。 */
_Static_assert(PK_DEVNAME_MAX_LEN <= PK_KBD_TEXT_MAX,
               "设备名上限超过了键盘编辑器的缓冲，输入会被静默截断");

static const char *TAG = "settings";

/* 当前选中行。行号即 pk_settings_apply() 的 case 序号，见那边的注释；
 * 具体是哪一行由 settings_draw.c 的渲染顺序决定，不在这里另抄一份清单——
 * 抄的那份不会跟着版面变（上一版就停在「5=FORMAT SD」，实际早已是 8）。 */
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
 * 键盘编辑器按下「确定」——设备名那一行的收尾。
 *
 * 编辑器本身不知道自己在编什么（它是个通用的受限 ASCII 输入框），落 NVS 与
 * 重开广播都归设置页。两步顺序不能反：先存，再让 ble_gatt 按存好的值重拼名字。
 *
 * 2026-08-02：由全局弱符号 pk_keyboard_page_on_commit 改成随 open 传进去的
 * 函数指针。原因见 keyboard_page.h——搜索页也要用这块键盘，而链接期只容得下
 * 一个强符号，再来一个调用者就会互相顶掉。
 */
static void devname_commit(const char *text)
{
    pk_devname_set(text);
    /* 改名不必重启整机——广播停掉重开就行（与 BLE 总开关那行不同，那个受
     * hosted 握手必须排在点屏之前那条硬约束限制，只能下次开机生效）。 */
    pk_ble_device_name_apply();
    ESP_LOGI(TAG, "device name committed: \"%s\"", pk_ble_device_name());
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

    case 8:   /* 设备名（P2-5）—— 不在这里改值，弹出受限 ASCII 编辑器。
               * 传进去的是**用户串**（NVS 里那一条），不是屏上显示的广播名：
               * 没设过名字时屏上显示的是出厂默认 "Pilot Kit Box-AABBCC"，
               * 把那一串塞进输入框等于让用户在别人的默认名上改，而且 MAC
               * 后缀会就此进了正文——用户设了名之后广播名里本来是没有它的。 */
        { char cur[PK_DEVNAME_BUF_SIZE];
          pk_devname_get(cur, sizeof(cur));
          pk_keyboard_page_open(pk_i18n_text(PK_TR_SETTINGS_DEVNAME), cur,
                                PK_DEVNAME_MAX_LEN, devname_commit, NULL); }
        break;

    case 9:   /* 演示模式（安全件，见 config_demo.h）。
               *
               * 立即生效，不像蓝牙那行要等重启：各数据源 getter 每次调用都重新
               * 问一遍 pk_demo_enabled()，没有缓存，所以"退出演示模式"能立刻恢复
               * 真实传感器——这是安全要求，不是实现上的顺手。
               *
               * 两个方向都弹 toast：开启时要让人知道自己刚做了什么，关闭时要让
               * 人确认真的关掉了（不给反馈的话，屏上那枚徽标消失会被当成 UI
               * 抽风而不是操作生效）。 */
        pk_demo_set_enabled(v == 1);
        pk_ui_toast_show(v == 1 ? PK_TR_TOAST_DEMO_ON : PK_TR_TOAST_DEMO_OFF,
                         v == 1);
        break;

    case 10:  /* 格式化 SD —— 复用两步确认状态机，第一次 ARM、第二次才真格式化 */
        pk_settings_format_action();
        break;

    default:
        break;
    }
}
