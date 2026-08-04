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

#include "pk_cal_advisor.h"   /* 校准提示的全部判定都在那边，这里只切页 */
#include "config_own_icao.h"  /* own ICAO 绑定落盘 */

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

/* 校准向导渲染用的精度条（cal_wizard.c:282 / settings_draw.c:431 是消费方）。
 * 只是把最近一次 tick 看到的 accuracy 存下来，不参与任何判定。 */
static uint8_t           s_cal_last_accuracy;

/*
 * 「要不要提示校准」的全部判定状态。三段判据（重新武装滞回 / 飞行相位门控 /
 * 磁干扰识别）与每个阈值的依据都写在 pk_cal_advisor.c，本文件只负责按它的
 * 结论切页——判定和切页搅在一起正是骚扰循环的成因（见 pk_cal_advisor.h 文件
 * 头列的三条实测依据）。
 *
 * 为什么闸门不是"抑制 N 分钟"
 *   N 到期时磁环境多半没变（用户还在同一间屋里），于是再弹一次、再被关掉，
 *   只是把骚扰的周期拉长，循环并没有断。用户按下"稍后再说"表达的是"我知道
 *   没校准，现在不想弄"，这个意图不该被一个计时器推翻。所以解除闸门的条件
 *   是"精度真的连续好起来"（advisor 的 PK_CAL_REARM_MS）。
 *
 * 为什么是 RAM-only（不落 NVS）
 *   开机时用户正处在"准备飞行"的场景，提醒一次是合理的；把"不想校准"写进
 *   NVS 会让一台从此再也不提示的盒子看起来像坏了，而排查线索只有一条藏在
 *   NVS 里的布尔量。
 *
 * 与 s_mode 共用 s_lock：advisor 自己无静态变量、不加锁，本文件是它唯一的
 * 持有者，所有读写都在锁内完成。
 */
static pk_cal_advisor_t  s_cal_advisor;

/* 最近一次 tick 的建议等级，供 pk_ui_cal_hint_active() 读（阶段 C3 的状态栏
 * 图标）。存结论而不是每次现算：advice 是**电平**，重算要带上当前时刻，而
 * 状态栏取数的那一拍与 tick 不是同一拍。 */
static pk_cal_advice_t   s_cal_advice;

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
    /* 静态存储的零值恰好就是 advisor 的正确初值，这句因此是冗余的——留着是
     * 因为"初值由谁定"该有一个看得见的答案，而不是靠读者自己去确认
     * pk_cal_advisor_reset() 只是一句 memset(0)。 */
    pk_cal_advisor_reset(&s_cal_advisor);
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

/* advisor 的时基是 uint32 毫秒，esp_timer 给的是 int64 微秒。
 * 先在 int64 上整除再截断——反过来（先截 32 位再除 1000）会在开机 71 分钟
 * 后把时刻算错。截断本身是有意的：advisor 的时间比较全是无符号差值，
 * 49.7 天回绕照样正确（见 pk_cal_advisor.h 文件头与测试 SC8）。 */
static uint32_t cal_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
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
                                  * s_cal_advisor 的注释。 */
                                 pk_cal_advisor_dismiss(&s_cal_advisor,
                                                        cal_now_ms());  break;
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
        pk_cal_advisor_dismiss(&s_cal_advisor, cal_now_ms());
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
    uint32_t now_ms = cal_now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = PK_UI_MODE_PFD;
    pk_cal_advisor_dismiss(&s_cal_advisor, now_ms);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mode → PFD (user dismissed cal wizard; auto-enter "
                  "suppressed until acc≥%u holds for >%ums)",
             (unsigned)PK_CAL_EXIT_ACCURACY, (unsigned)PK_CAL_REARM_MS);
}

/*
 * 用户从设置页那一行「罗盘校准」主动进来。
 *
 * 为什么不让设置页直接调 pk_ui_set_mode(PK_UI_MODE_CAL_WIZARD)
 * ---------------------------------------------------------
 * 那样只切页，不动闸门。而闸门（advisor 的 suppressed）一旦被「稍后再说」/
 * 切走那两条路径置上，就要等磁力计精度真的**连续**好起来才会复位（见
 * pk_cal_advisor.h 的 PK_CAL_REARM_MS）——恰恰是"还没校准好"的时候它一直
 * 关着。用户此刻的动作说明他改主意了，闸门必须跟着重新武装：不然他在这一页
 * 没转够就退出去，本次开机内既不会自动提醒、也不会有第二次提示。
 *
 * 闸门的开关一律留在本文件：s_cal_advisor 是私有状态，让设置页去改就得把它
 * 导出去，"谁在什么时候动过闸门"就散进各个页面了。页面只表达意图。
 *
 * pk_cal_advisor_user_open() 顺手把两条连续段计时器一并清零（各有各的原因，
 * 见 advisor 那边的注释）。
 *
 * D3（2026-08-04）：user_open() 现在还置位 user_opened 标志，使得
 * should_exit_wizard() 恒返回 false——**用户主动进入的页面永不自动退出**。
 *
 *   推翻的老结论：上一版注释写「3 s 后仍然自动退回是有意保留的——精度已经
 *   够了，这一页没事可做」。这条被真机反馈推翻了：用户从设置页点进校准页，
 *   页面显示 3/3，几秒后自己跑回 PFD——用户原话「没给我校准的机会」。用户主动
 *   点进来就是要校准（重新校准 / 验证 / 画 8 字顶精度都正当），系统凭一个 acc
 *   读数替他判定「没事可做」然后收走页面是自作聪明。用户的显式动作必须压过
 *   系统的自动判断。保留这段历史是为了说明为什么当初设计了自动退出（老前提
 *   是合理的），以及是哪条真机反馈推翻了它。
 *
 *   只对自动弹出的页面维持原行为：acc 稳定达标 EXIT_MS 后自动退回 PFD。
 *   两种进入方式的区分靠 user_opened 标志（advisor 内部维护，dismiss / 自动
 *   进入路径都会清位）。
 */
void pk_ui_cal_wizard_enter(void)
{
    if (s_lock == NULL) return;
    uint32_t now_ms = cal_now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = PK_UI_MODE_CAL_WIZARD;
    pk_cal_advisor_user_open(&s_cal_advisor, now_ms);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mode → CAL_WIZARD (user opened it from settings; "
                  "auto-enter re-armed)");
}

void pk_ui_cal_wizard_tick(bool valid, uint8_t accuracy, pk_flight_phase_t phase,
                           uint8_t vib_level)
{
    if (s_lock == NULL) return;

    uint32_t now_ms = cal_now_ms();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cal_last_accuracy = valid ? accuracy : 0;

    pk_cal_advice_t advice = pk_cal_advisor_update(&s_cal_advisor, now_ms,
                                                   valid, accuracy, phase,
                                                   vib_level);
    s_cal_advice = advice;

    /*
     * IMU 断流（valid=false）时只是不抢页面，其余照常。
     *
     * 为什么挡在这一层而不是改状态机：advisor 的低精度累计走的是墙钟差值，
     * valid=false 的样本既不推进也不清零计时器（这是从旧实现原样搬过去的
     * 行为——"这一拍没取到数"不等于"磁环境变了"）。于是一个在断流之前就已
     * 起算的低精度连续段，会在断流期间照样走满 ENTER 窗口给出 WIZARD。
     * 这不是判定错：advisor 回答的是"该不该提示校准"，而"要不要为此换页"
     * 是页面层的事——一块读不到姿态的板子弹出一页让人画 8 字毫无意义，
     * 进度条也只会停在 0。让状态机去分辨这件事就得把 valid 的语义从"这一拍
     * 没取到数"扩成"IMU 是不是活着"，那需要另一条超时判据，纯属为一行胶水
     * 加一个状态。
     */
    bool take_page = (advice == PK_CAL_ADVICE_WIZARD) && valid;

    if (take_page && s_mode != PK_UI_MODE_CAL_WIZARD) {
        s_mode = PK_UI_MODE_CAL_WIZARD;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "mode → CAL_WIZARD (auto: acc=0 for >%ums, parked and "
                       "no magnetic jamming — device needs figure-8 motion)",
                 (unsigned)PK_CAL_ENTER_MS);
        return;
    }

    if (s_mode == PK_UI_MODE_CAL_WIZARD &&
        pk_cal_advisor_should_exit_wizard(&s_cal_advisor)) {
        s_mode = PK_UI_MODE_PFD;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "mode → PFD (auto: acc≥%u for >%ums — fusion converged, "
                       "dismissing wizard)",
                 (unsigned)PK_CAL_EXIT_ACCURACY, (unsigned)PK_CAL_EXIT_MS);
        return;
    }

    xSemaphoreGive(s_lock);
}

bool pk_ui_cal_hint_active(void)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = (s_cal_advice == PK_CAL_ADVICE_HINT);
    xSemaphoreGive(s_lock);
    return v;
}

bool pk_ui_cal_jammed(void)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = pk_cal_advisor_is_jammed(&s_cal_advisor);
    xSemaphoreGive(s_lock);
    return v;
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
    /* NVS 写操作放在释放 mutex 之后——照 config_ac_category.c 的范式
     * （portEXIT_CRITICAL 之后才碰 nvs），避免在持锁期间做阻塞 IO。 */
    pk_config_own_icao_set(icao24 & 0xFFFFFF);
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
    pk_config_own_icao_clear();   /* 解绑也落盘，释放 mutex 后调 */
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
