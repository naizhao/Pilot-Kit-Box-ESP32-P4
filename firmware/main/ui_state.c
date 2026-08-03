/*
 * ui_state.c — implementation of the UI mode + list cursor.
 *
 * Single static mutex protects mode and selection. Operations are
 * trivially short (load/store one int), so contention is negligible
 * and we don't bother with atomic primitives.
 */

#include "ui_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "ui";

#define UI_LIST_PENDING_DELTA_MAX  999   /* upper saturation on the
                                            pending scroll delta — no
                                            aircraft list will ever
                                            come close, this just
                                            prevents int wrap-around
                                            if someone holds UP/DOWN
                                            forever before the next
                                            renderer tick consumes the
                                            buffered presses */
#define UI_ABOUT_SCROLL_STEP_PX     24
#define UI_ABOUT_SCROLL_MAX_PX      40
#define UI_DIAG_SCROLL_STEP_PX      24
#define UI_DIAG_SCROLL_MAX_PX      300   /* 诊断页比 about 长(GPS 多行 + 每星座一行 SNR 柱状图) */

/* Calibration-wizard auto-trigger thresholds. The 10 s enter window
 * is long enough that we don't bother the user with the wizard
 * during a brief acc=0 dip on first boot before fusion converges
 * normally; the 3 s exit window with acc≥2 confirms fusion has
 * really converged before we dismiss. */
#define UI_CAL_WIZARD_ENTER_MS        10000
#define UI_CAL_WIZARD_EXIT_MS          3000
#define UI_CAL_WIZARD_EXIT_ACCURACY    2

#define UI_TOAST_DURATION_US      (1500 * 1000)  /* toast 在屏幕上停留 1.5s */

static SemaphoreHandle_t s_lock;
static pk_ui_mode_t      s_mode               = PK_UI_MODE_PFD;

/* List selection is tracked by ICAO, not row index. s_list_selected_icao
 * is the aircraft the user last had highlighted (0 = "no commitment
 * yet"); s_list_pending_delta is the un-applied scroll-button intent,
 * cleared by pk_ui_list_resolve_row(). Tracking by ICAO keeps the
 * highlight stuck to the same aircraft even when the snapshot row
 * order shifts (aircraft enters/leaves the trailing-60s window). */
static uint32_t          s_list_selected_icao;
static int               s_list_pending_delta;
static int               s_about_scroll_y;
static int               s_diag_scroll_y;

/* Traffic 雷达页的独立选中(按 ICAO)。与列表选中分开,避免互相污染,更
 * 关键是避免:本机被雷达页从目标列表排除后,列表版 resolve 永远找不到选中
 * ICAO 而 fallback 到 row 0,导致白色高亮/详情每帧跳到"最近那架"。
 * 0 = 当前无选中。 */
static uint32_t          s_tfc_selected_icao;

/* Runtime own-ship binding. s_own_icao_set distinguishes "user
 * explicitly bound something" (even if to 0) from "never set, use
 * Kconfig default". RAM-only — wiped on every reboot. */
static uint32_t          s_own_icao_runtime;
static bool              s_own_icao_set;

/* Calibration wizard state. acc_first_low_us = first time we saw
 * acc=0 in the current "low streak"; acc_first_high_us = first time
 * we saw acc≥2 in the current "high streak". 0 = no current streak. */
static int64_t           s_cal_acc_first_low_us;
static int64_t           s_cal_acc_first_high_us;
static uint8_t           s_cal_last_accuracy;

/*
 * 自动弹出校准页的"闸门"。true = 本次开机内不再自动进入。
 *
 * 为什么需要它
 * ------------
 * 下面的 tick 只要看到 acc=0 连续 10 s 就把 s_mode 强拽成 CAL_WIZARD，进入
 * 后计时器清零、acc 仍是 0 就重新计时。于是用户**无论用什么方式离开**（点
 * 页内的「稍后再说」、或 FAB → 导航网格切到别的页），10 s 后都会被原样拽
 * 回来。真机实测：室内磁环境 + 刚重装的 IMU，acc 连续 40 s 都是 0。旧版的
 * 退路是物理 MODE 键，4.3″ 板上那个键已经没有了。
 *
 * 抑制到什么时候：**直到磁力计精度真的上过 UI_CAL_WIZARD_EXIT_ACCURACY**
 * （见下面 tick 里那句 s_cal_auto_suppressed = false）。
 *
 * 为什么不是"抑制 N 分钟"
 *   N 到期时磁环境多半没变（用户还在同一间屋里），于是再弹一次、再被关掉，
 *   只是把骚扰的周期拉长，循环并没有断。用户按下"稍后再说"表达的是"我知道
 *   没校准，现在不想弄"，这个意图不该被一个计时器推翻。
 *
 * 为什么重新武装的条件是"acc 曾经 ≥2"而不是"重启"
 *   acc 上过 2 说明设备确实完成过一次校准；此后再掉回 0 是**新的一次**退化
 *   （换了环境、靠近了磁干扰源），那时候提示是有信息量的，不是重复骚扰。
 *
 * 为什么是 RAM-only（不落 NVS）
 *   开机时用户正处在"准备飞行"的场景，提醒一次是合理的；把"不想校准"写进
 *   NVS 会让一台从此再也不提示的盒子看起来像坏了，而排查线索只有一条藏在
 *   NVS 里的布尔量。
 *
 * 注意：这只挡**自动进入**。手动进入（导航网格里的入口）与自动退出
 * （acc≥2 持续 3 s 回 PFD）的逻辑一个字都没动。
 */
static bool              s_cal_auto_suppressed;

/* Transient toast. s_toast_until_us == 0 (or now past it) → no toast.
 * s_toast_blink_times>0 时按 400 ms 一拍闪烁（阶段 5b，见 ui_state.h
 * pk_ui_toast_show_blink 的注释）；s_toast_start_us 是闪烁相位的起点。 */
static pk_tr_id_t        s_toast_id;
static bool              s_toast_is_error;
static int64_t           s_toast_until_us;
static int               s_toast_blink_times;
static int64_t           s_toast_start_us;

#define UI_TOAST_BLINK_HALF_US   (400 * 1000)   /* 400 ms 一拍 */

esp_err_t pk_ui_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ui_state ready (default mode: PFD)");
    return ESP_OK;
}

pk_ui_mode_t pk_ui_get_mode(void)
{
    if (s_lock == NULL) return PK_UI_MODE_PFD;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pk_ui_mode_t m = s_mode;
    xSemaphoreGive(s_lock);
    return m;
}

static const char *mode_name(pk_ui_mode_t m)
{
    switch (m) {
    case PK_UI_MODE_PFD:         return "PFD";
    case PK_UI_MODE_TRAFFIC:     return "TRAFFIC";
    case PK_UI_MODE_MAP:         return "MAP";
    case PK_UI_MODE_ADSB_LIST:   return "ADSB_LIST";
    case PK_UI_MODE_SETTINGS:    return "SETTINGS";
    case PK_UI_MODE_ABOUT:       return "ABOUT";
    case PK_UI_MODE_DIAG:        return "DIAG";
    case PK_UI_MODE_CAL_WIZARD:  return "CAL_WIZARD";
    default:                     return "?";
    }
}

void pk_ui_toggle_mode(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* User-visible cycle: PFD → ADSB_LIST → SETTINGS → ABOUT → PFD …
     * The CAL_WIZARD mode is outside the cycle: pressing MODE while
     * in it returns to PFD and dismisses the wizard (the auto-
     * trigger state machine will re-arm next time accuracy drops). */
    switch (s_mode) {
    case PK_UI_MODE_PFD:         s_mode = PK_UI_MODE_TRAFFIC;   break;
    case PK_UI_MODE_TRAFFIC:     s_mode = PK_UI_MODE_MAP;       break;
    case PK_UI_MODE_MAP:         s_mode = PK_UI_MODE_ADSB_LIST; break;
    case PK_UI_MODE_ADSB_LIST:   s_mode = PK_UI_MODE_SETTINGS;  break;
    case PK_UI_MODE_SETTINGS:    s_mode = PK_UI_MODE_ABOUT;
                                  s_about_scroll_y = 0;          break;
    case PK_UI_MODE_ABOUT:       s_mode = PK_UI_MODE_DIAG;
                                  s_diag_scroll_y = 0;          break;
    case PK_UI_MODE_DIAG:        s_mode = PK_UI_MODE_PFD;       break;
    case PK_UI_MODE_CAL_WIZARD:  s_mode = PK_UI_MODE_PFD;
                                 /* 手动离开校准页 = 用户明确表示"现在不想
                                  * 校准"，关掉自动重弹的闸门，理由见
                                  * s_cal_auto_suppressed 的注释。 */
                                 s_cal_auto_suppressed = true;
                                 s_cal_acc_first_low_us = 0;    break;
    default:                     s_mode = PK_UI_MODE_PFD;       break;
    }
    pk_ui_mode_t new_mode = s_mode;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mode → %s", mode_name(new_mode));
}

void pk_ui_set_mode(pk_ui_mode_t mode)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 从校准页**主动切走**（导航网格里选了别的页）同样算"用户不想校准"。
     * 不这么做的话，用户用 FAB 切出去 10 s 后又被拽回来——罩哥真机上遇到的
     * 正是这一条。判据刻意收紧成"离开时正好在校准页"：只要写成"任何一次
     * set_mode 都抑制"，用户开机后随便切一次页，这一整轮开机就再也不会提示
     * 校准了。 */
    if (s_mode == PK_UI_MODE_CAL_WIZARD && mode != PK_UI_MODE_CAL_WIZARD) {
        s_cal_auto_suppressed  = true;
        s_cal_acc_first_low_us = 0;
    }
    s_mode = mode;
    /* 进入 About/Diag 时复位各自滚动位置 —— 与 toggle 路径行为一致。 */
    if (mode == PK_UI_MODE_ABOUT) s_about_scroll_y = 0;
    if (mode == PK_UI_MODE_DIAG)  s_diag_scroll_y  = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mode → %s (direct)", mode_name(mode));
}

/*
 * 用户点了校准页上的「稍后再说」。
 *
 * 回 PFD 而不是"回到被拽走之前那一页"：自动退出（acc≥2）走的就是 PFD，物理
 * MODE 键那条老路径（pk_ui_toggle_mode 的 CAL_WIZARD 分支）也是 PFD。为一个
 * 次要动作单独记一份"来时的页"，三条退路就会有两种落点，而这一页本来就是
 * 从任意页面被强行拽进来的——落回主界面反而是最不容易让人迷路的选择。
 */
void pk_ui_cal_wizard_dismiss(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode                 = PK_UI_MODE_PFD;
    s_cal_auto_suppressed  = true;
    s_cal_acc_first_low_us = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mode → PFD (user dismissed cal wizard; auto-enter "
                  "suppressed until acc≥%u is seen again)",
             UI_CAL_WIZARD_EXIT_ACCURACY);
}

/*
 * 用户从设置页那一行「罗盘校准」主动进来。
 *
 * 为什么不让设置页直接调 pk_ui_set_mode(PK_UI_MODE_CAL_WIZARD)
 * ---------------------------------------------------------
 * 那样只切页，不动闸门。而 s_cal_auto_suppressed 一旦被「稍后再说」/切走
 * 那两条路径置上，就要等磁力计精度真的上到 UI_CAL_WIZARD_EXIT_ACCURACY 才会
 * 复位（见该变量的注释）——恰恰是"还没校准好"的时候它一直关着。用户此刻的
 * 动作说明他改主意了，闸门必须跟着重新武装：不然他在这一页没转够就退出去，
 * 本次开机内既不会自动提醒、也不会有第二次提示。
 *
 * 闸门的开关一律留在本文件：s_cal_auto_suppressed 是私有状态，让设置页去改
 * 就得把它导出去，"谁在什么时候动过闸门"就散进各个页面了。页面只表达意图。
 *
 * 两个计时器也一并清零，各有各的原因：
 *   - s_cal_acc_first_low_us：进来之后 tick 的自动进入分支本来就不该再触发
 *     （已经在这一页了），清零与自动进入路径的做法一致。
 *   - s_cal_acc_first_high_us：**不清零会让这一页当场闪一下就跑掉**。设备
 *     若已经校准好（acc≥2 持续了几分钟），tick 的自动退出分支下一拍就满足
 *     "acc≥2 超过 3 s"，用户刚点开就被弹回 PFD。清零后至少有 3 s 的窗口；
 *     3 s 后仍然自动退回是**有意保留**的——精度已经够了，这一页没事可做，
 *     而真正需要校准（acc<2）时自动退出根本不会触发，页面会一直等着他。
 */
void pk_ui_cal_wizard_enter(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode                  = PK_UI_MODE_CAL_WIZARD;
    s_cal_auto_suppressed   = false;
    s_cal_acc_first_low_us  = 0;
    s_cal_acc_first_high_us = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mode → CAL_WIZARD (user opened it from settings; "
                  "auto-enter re-armed)");
}

void pk_ui_cal_wizard_tick(bool valid, uint8_t accuracy)
{
    if (s_lock == NULL) return;

    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cal_last_accuracy = valid ? accuracy : 0;

    if (valid && accuracy == 0) {
        if (s_cal_acc_first_low_us == 0) s_cal_acc_first_low_us = now;
        s_cal_acc_first_high_us = 0;
    } else if (valid && accuracy >= UI_CAL_WIZARD_EXIT_ACCURACY) {
        if (s_cal_acc_first_high_us == 0) s_cal_acc_first_high_us = now;
        s_cal_acc_first_low_us = 0;
        /* 精度真的上来过 → 重新武装自动弹出。之后再掉回 0 是**新的一次**
         * 退化（换了环境/受了磁干扰），那时候提示是有信息量的。 */
        s_cal_auto_suppressed = false;
    } else {
        /* acc=1 or invalid: don't progress either timer, but
         * don't reset them either — fusion is in transit. */
    }

    /* Auto-enter wizard from any non-wizard mode if acc has been
     * stuck at 0 for the enter window. s_cal_auto_suppressed 是用户手动
     * 关过之后的闸门——没有它，「稍后再说」按下去 10 s 就白按了。 */
    if (s_mode != PK_UI_MODE_CAL_WIZARD &&
        !s_cal_auto_suppressed &&
        s_cal_acc_first_low_us != 0 &&
        (now - s_cal_acc_first_low_us) / 1000 >= UI_CAL_WIZARD_ENTER_MS) {
        s_mode = PK_UI_MODE_CAL_WIZARD;
        s_cal_acc_first_low_us = 0;       /* reset so we don't re-trigger immediately */
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "mode → CAL_WIZARD (auto: acc=0 for >%dms — "
                       "device needs figure-8 motion)",
                 UI_CAL_WIZARD_ENTER_MS);
        return;
    }

    /* Auto-exit wizard back to PFD when acc has been ≥2 for the
     * exit window. */
    if (s_mode == PK_UI_MODE_CAL_WIZARD &&
        s_cal_acc_first_high_us != 0 &&
        (now - s_cal_acc_first_high_us) / 1000 >= UI_CAL_WIZARD_EXIT_MS) {
        s_mode = PK_UI_MODE_PFD;
        s_cal_acc_first_high_us = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "mode → PFD (auto: acc≥%u for >%dms — "
                       "fusion converged, dismissing wizard)",
                 UI_CAL_WIZARD_EXIT_ACCURACY, UI_CAL_WIZARD_EXIT_MS);
        return;
    }

    xSemaphoreGive(s_lock);
}

uint8_t pk_ui_cal_wizard_last_accuracy(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t a = s_cal_last_accuracy;
    xSemaphoreGive(s_lock);
    return a;
}

void pk_ui_list_scroll(int delta)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_list_pending_delta + delta;
    if (v < -UI_LIST_PENDING_DELTA_MAX) v = -UI_LIST_PENDING_DELTA_MAX;
    if (v >  UI_LIST_PENDING_DELTA_MAX) v =  UI_LIST_PENDING_DELTA_MAX;
    s_list_pending_delta = v;
    xSemaphoreGive(s_lock);
}

void pk_ui_about_scroll(int delta)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_about_scroll_y + delta * UI_ABOUT_SCROLL_STEP_PX;
    if (v < 0) v = 0;
    if (v > UI_ABOUT_SCROLL_MAX_PX) v = UI_ABOUT_SCROLL_MAX_PX;
    s_about_scroll_y = v;
    xSemaphoreGive(s_lock);
}

int pk_ui_about_scroll_y(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_about_scroll_y;
    xSemaphoreGive(s_lock);
    return v;
}

void pk_ui_diag_scroll(int delta)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_diag_scroll_y + delta * UI_DIAG_SCROLL_STEP_PX;
    if (v < 0) v = 0;
    if (v > UI_DIAG_SCROLL_MAX_PX) v = UI_DIAG_SCROLL_MAX_PX;
    s_diag_scroll_y = v;
    xSemaphoreGive(s_lock);
}

int pk_ui_diag_scroll_y(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_diag_scroll_y;
    xSemaphoreGive(s_lock);
    return v;
}

int pk_ui_list_resolve_row(const uint32_t *icaos, size_t n)
{
    if (s_lock == NULL || icaos == NULL || n == 0) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Find row currently occupied by the previously-selected ICAO. */
    int cur_row = -1;
    if (s_list_selected_icao != 0) {
        for (size_t i = 0; i < n; ++i) {
            if (icaos[i] == s_list_selected_icao) {
                cur_row = (int)i;
                break;
            }
        }
    }
    /* No prior selection, or the aircraft expired out of the snapshot:
     * anchor at row 0. */
    if (cur_row < 0) cur_row = 0;

    int new_row = cur_row + s_list_pending_delta;
    s_list_pending_delta = 0;
    if (new_row < 0)         new_row = 0;
    if (new_row >= (int)n)   new_row = (int)n - 1;

    s_list_selected_icao = icaos[new_row];

    xSemaphoreGive(s_lock);
    return new_row;
}

int pk_ui_traffic_resolve(const uint32_t *icaos, size_t n)
{
    if (s_lock == NULL) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* 当前选中 ICAO 在列表中的行(本机已被调用方排除,可能找不到)。 */
    int cur = -1;
    if (s_tfc_selected_icao != 0 && icaos != NULL) {
        for (size_t i = 0; i < n; ++i) {
            if (icaos[i] == s_tfc_selected_icao) { cur = (int)i; break; }
        }
    }

    int delta = s_list_pending_delta;
    s_list_pending_delta = 0;

    /* 关键:没选中 且 没滚动操作 → 维持"无选中",绝不 fallback 到 row 0
     * (这正是列表版 resolve 在雷达页随机跳的根因)。 */
    if ((cur < 0 && delta == 0) || n == 0) {
        s_tfc_selected_icao = 0;
        xSemaphoreGive(s_lock);
        return -1;
    }

    int nr = (cur < 0) ? 0 : cur + delta;   /* 首次/旧选中已失 → 锚 row 0 */
    if (nr < 0)        nr = 0;
    if (nr >= (int)n)  nr = (int)n - 1;
    s_tfc_selected_icao = icaos[nr];

    xSemaphoreGive(s_lock);
    return nr;
}

uint32_t pk_ui_list_get_selected_icao(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = s_list_selected_icao;
    xSemaphoreGive(s_lock);
    return v;
}

void pk_ui_set_own_icao(uint32_t icao24)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_own_icao_runtime = icao24 & 0xFFFFFF;
    s_own_icao_set     = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "own ICAO bound at runtime → %06lX",
             (unsigned long)(icao24 & 0xFFFFFF));
}

uint32_t pk_ui_get_own_icao(void)
{
    if (s_lock == NULL) return (uint32_t)CONFIG_PK_OWN_ICAO;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = s_own_icao_set ? s_own_icao_runtime
                                : (uint32_t)CONFIG_PK_OWN_ICAO;
    xSemaphoreGive(s_lock);
    return v;
}

void pk_ui_clear_own_icao(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_own_icao_runtime = 0;
    s_own_icao_set     = true;   /* 显式置位 → getter 返回 0 而非 Kconfig 默认 */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "own ICAO cleared at runtime");
}

/* 两个公开入口共用的实现；blink_times<=0 折成 0（"不闪"）。 */
static void toast_show_impl(pk_tr_id_t id, bool is_error, int blink_times)
{
    if (s_lock == NULL) return;
    int64_t now = esp_timer_get_time();
    int64_t duration_us = (blink_times > 0)
                         ? (int64_t)blink_times * 2 * UI_TOAST_BLINK_HALF_US
                         : UI_TOAST_DURATION_US;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_toast_id          = id;
    s_toast_is_error    = is_error;
    s_toast_blink_times = (blink_times > 0) ? blink_times : 0;
    s_toast_start_us    = now;
    s_toast_until_us    = now + duration_us;
    xSemaphoreGive(s_lock);
}

void pk_ui_toast_show(pk_tr_id_t id, bool is_error)
{
    toast_show_impl(id, is_error, 0);
}

void pk_ui_toast_show_blink(pk_tr_id_t id, bool is_error, int blink_times)
{
    toast_show_impl(id, is_error, blink_times);
}

bool pk_ui_toast_get(pk_tr_id_t *out_id, bool *out_error)
{
    if (s_lock == NULL) return false;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = (s_toast_until_us != 0 && now < s_toast_until_us);
    if (active) {
        if (out_id)    *out_id    = s_toast_id;
        if (out_error) *out_error = s_toast_is_error;
    }
    xSemaphoreGive(s_lock);
    return active;
}

bool pk_ui_toast_blink_visible(void)
{
    if (s_lock == NULL) return true;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool visible = true;
    if (s_toast_blink_times > 0) {
        int64_t elapsed = now - s_toast_start_us;
        if (elapsed < 0) elapsed = 0;
        int64_t phase = elapsed / UI_TOAST_BLINK_HALF_US;
        visible = (phase % 2) == 0;   /* 偶数拍=亮，奇数拍=灭 */
    }
    xSemaphoreGive(s_lock);
    return visible;
}
