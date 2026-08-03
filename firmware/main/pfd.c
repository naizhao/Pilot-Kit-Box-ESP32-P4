/*
 * pfd.c — Primary Flight Display task + frame dispatcher.
 *
 * Renders the current UI mode into the logical 800×480 RGB565 framebuffer
 * at about 30 FPS. display.c rotates it into the native 480×800 ST7701
 * MIPI-DSI scan buffers.
 *
 * Top-level modes are PFD, traffic, ADS-B list, settings, about and
 * diagnostics; the calibration wizard is a full-screen overlay.
 *
 * Each PFD widget owns its screen region and reads from a small
 * per-frame POD assembled here (pk_pfd_imu_t, pk_pfd_status_t,
 * pk_pfd_hsi_t, plus the optional own-ship ADS-B snapshot).
 */

#include "pfd.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "battery.h"
#include "about_page.h"
#include "adsb_list.h"
#include "diag_page.h"
#include "aircraft_state.h"
#include "ble_gatt.h"
#include "gps.h"
#include "own_ship.h"
#include "cal_wizard.h"
#include "config_demo.h"
#include "display.h"
#include "imu_task.h"
#include "pfd_attitude.h"
#include "pfd_draw.h"
#include "pfd_hsi.h"
#include "pfd_hsi_traffic.h"
#include "lv_port.h"
#include "pfd_infobox.h"
#include "pk_ui_nav.h"
#include "pk_ui_nav_host.h"
#include "touch_gt911.h"
#include "soc_temp.h"
#include "pk_sdcard.h"
#include "pfd_statusbar.h"
#include "pfd_speed_tape.h"
#include "pfd_tape.h"
#include "baro.h"
#include "sdkconfig.h"
#include "apt_detail_page.h"
#include "keyboard_page.h"
#include "nav_grid_page.h"
#include "search_page.h"
#include "settings_page.h"
#include "traffic_page.h"
#include "map_page.h"
#include "ui_state.h"
#include "i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pfd";

/* --- Transient toast overlay ---------------------------------------- *
 * 每帧把 ui_state 里的 toast 状态同步给控件层。
 *
 * 此前是自己在 framebuffer 上画一个框（pk_pfd_fill_rect + 旧 CJK 位图字体），
 * 那样它与 PFD 同层，会被 dock、FAB 盖住——而 toast 恰恰是最该盖住别人的。
 * 现在交给 pk_ui_nav_toast()，画在控件层最前面。
 *
 * 过期判定仍在 pk_ui_toast_get() 里：它带时间戳且线程安全，按键中断里也能
 * 安全地 show。这里只做「有就显示、没有就收起」。 */
/*
 * 阶段 5b：闪烁告警（SD 写失败 / 降级）复用这条同步路径——闪烁的"灭"相位
 * 就是照旧调 pk_ui_nav_toast(NULL, ...) 把控件藏起来，"亮"相位再显示同一句
 * 文案。不新增控件、不碰 pk_ui_nav.c，闪烁纯粹靠这里每帧的显/隐切换实现。
 *
 * 不得让 toast 变回可点击：它不吃点击靠的是 pk_ui_nav.c 里
 * lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE)，与 touch_gt911.c 的
 * eaten 链无关——这里改闪烁逻辑时若误触发那处加回 CLICKABLE，会把底下
 * 页面的点击吞掉（SD 故障告警绝不能挡住飞行中的操作）。
 */
static void sync_toast(void)
{
    pk_tr_id_t id;
    bool is_error;
    if (pk_ui_toast_get(&id, &is_error) && pk_ui_toast_blink_visible()) {
        pk_ui_nav_toast(pk_i18n_text(id), is_error);
    } else {
        pk_ui_nav_toast(NULL, false);
    }
}

static void pfd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pfd_task running (G1000 landscape)");

    /* 渲染目标从裸 framebuffer 换成 LVGL 的背景 canvas。
     *
     * 各 pk_pfd_*_render() 的签名与实现完全不变——它们照旧往一块
     * PK_DISPLAY_W×H 的 RGB565 缓冲里写，只是这块缓冲现在归 LVGL 管，
     * 于是触摸控件（FAB / dock / Toast）能叠在 PFD 之上并与之混合。
     * 模拟器早已跑在这条路径上，两边至此一致。 */
    if (pk_lv_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "LVGL init failed — exiting");
        vTaskDelete(NULL);
    }
    uint16_t *fb = pk_lv_port_canvas_px();
    if (fb == NULL) {
        ESP_LOGE(TAG, "no canvas buffer — exiting");
        vTaskDelete(NULL);
    }
    /* 先有输入设备再建控件：nav 层那些 lv_indev_active() 取的是「当前正在
     * 上报的设备」，一个都没注册的话 dock 与 FAB 全是死的。 */
    (void)pk_touch_init();
    (void)pk_soc_temp_init();
    pk_ui_nav_init();
    /* 建完 FAB 才能把它摆回上次的落点。 */
    pk_ui_nav_host_init();

    /* Big stack-eaters live here as file-static so they don't blow the
     * task stack. Pinned to PSRAM (.ext_ram.bss) so they don't compete
     * with FreeRTOS / ESP-Hosted tasks for scarce DMA-capable internal
     * RAM during the early-boot constructor storm. */
    static EXT_RAM_BSS_ATTR aircraft_t scratch[AIRCRAFT_TABLE_CAPACITY];
    /* 帧率排查：本模块的 PERF/PERF2/PERF3 埋点（draw/lvgl/flush、各 PFD
     * 组件、PPA 与 vsync 等待）走 ESP_LOGD，CONFIG_LOG_MAXIMUM_LEVEL=4 已把
     * 它们编译进来，默认被 INFO 级别挡住。要看就在这里加一行
     * esp_log_level_set(TAG, ESP_LOG_DEBUG) 临时提级，别提交——每秒三行会
     * 把串口刷满，真出问题时反而看不见有用的日志。
     *
     * 2026-08-02 用它测过一轮，分解表与优化方向见
     * docs/ui_performance-zh_CN.md，下次动手前先读那份，别重复测。 */

    int64_t  fps_window_start_us = esp_timer_get_time();
    uint32_t frames_in_window = 0;
    int64_t  acc_draw_us = 0, acc_lvgl_us = 0;   /* 诊断：分段耗时 */
    int64_t  acc_att_us = 0, acc_bar_us = 0, acc_tape_us = 0, acc_hsi_us = 0;
    int64_t  last_tick_us = 0;      /* 上一次喂给 LVGL 的时间戳 */

    while (1) {
        TickType_t frame_start = xTaskGetTickCount();
        int64_t t_frame0 = esp_timer_get_time();

        pk_imu_sample_t s;
        bool have = pk_imu_sample_get(&s);
        pk_ui_cal_wizard_tick(have, have ? s.accuracy : 0);

        pk_ui_mode_t mode = pk_ui_get_mode();

        /*
         * 模态层排在 pk_ui_mode_t 之前。
         *
         * 键盘编辑器与搜索页都是「浮在某一页之上的整屏层」，不是模式循环里的
         * 一站（dock 塞不下第 8 个页签，导航也只有两层——见 search_page.h）。
         * 原先键盘的判定嵌在 case PK_UI_MODE_SETTINGS 里，等于把「它只可能
         * 从设置页打开」这个当时的事实写死进了分派；搜索页从地图页打开，
         * 那条 case 就再也覆盖不到。提到 switch 之前，与 touch_gt911.c 的
         * 分派次序对齐（两处次序一致是"看得见的就是点得中的"的前提）。
         *
         * 2026-08-02：次序本身收进了 pk_ui_modal_top()（apt_detail_page.h），
         * 两处分派共用同一个纯函数，不再靠注释互相提醒。次序 键盘 > 详情 >
         * 搜索 的依据与"关掉最上层就回到来时的地方"这条推论都写在那边。
         */
        const pk_ui_modal_t modal = pk_ui_modal_top(pk_nav_grid_page_active(),
                                                    pk_keyboard_page_active(),
                                                    pk_apt_detail_page_active(),
                                                    pk_search_page_active());
        /*
         * 导航网格不进下面这条 if/else 链，理由是它与另外三层的性质不同：
         * 键盘 / 详情 / 搜索都是整屏**不透明**重绘，画了它们就不必画底页；
         * 网格是**半透明覆盖层**，遮罩之下要留住底页的轮廓（nav_grid_page.c
         * 里 232 那条：地平线的明暗分界仍要隐约可辨）。而 pk_pfd_darken_rect()
         * 是就地把已有像素压暗，canvas 又是单块常驻缓冲（lv_port.c 的
         * pk_lv_port_canvas_px）——只画网格不画底页的话，同一批像素会被逐帧
         * 反复压暗，两三帧就全黑了。
         * 所以它走「底页照常画完，再叠上去」，叠加动作在 switch 之后。
         */
        if (modal == PK_UI_MODAL_KEYBOARD) {
            pk_keyboard_page_render(fb);
        } else if (modal == PK_UI_MODAL_DETAIL) {
            pk_apt_detail_page_render(fb);
        } else if (modal == PK_UI_MODAL_SEARCH) {
            pk_search_page_render(fb);
        } else switch (mode) {
        case PK_UI_MODE_CAL_WIZARD:
            pk_cal_wizard_render(fb);
            break;

        case PK_UI_MODE_ABOUT:
            pk_about_page_render(fb);
            break;

        case PK_UI_MODE_DIAG:
            pk_diag_page_render(fb);
            break;

        case PK_UI_MODE_ADSB_LIST:
            pk_adsb_list_render(fb);
            break;

        case PK_UI_MODE_SETTINGS:
            pk_settings_page_render(fb);
            break;

        case PK_UI_MODE_TRAFFIC:
            pk_traffic_page_render(fb);
            break;

        case PK_UI_MODE_MAP:
            pk_map_page_render(fb);
            break;

        case PK_UI_MODE_PFD:
        default: {
            int64_t now_us = esp_timer_get_time();

            pk_pfd_imu_t imu = {
                .valid     = have,
                .roll_deg  = have ? s.roll_deg  : 0.0f,
                .pitch_deg = have ? s.pitch_deg : 0.0f,
            };

            /* Same "fresh contact" 60s window the BLE GDL90 emitter uses. */
            size_t n_aircraft = aircraft_state_snapshot(
                scratch, sizeof(scratch) / sizeof(scratch[0]),
                now_us, AIRCRAFT_STALE_AGE_US);

            /* Own-ship: ALT/VS/GS sourced from the bound transponder's
             * ADS-B reports. pk_ui_get_own_icao() returns the runtime
             * binding (set via TARE short-press in ADS-B list mode)
             * or falls back to the compile-time CONFIG_PK_OWN_ICAO.
             * Stale window is PK_OWN_STALE_AGE_MS. */
            aircraft_t own = {0};
            pk_own_src_t own_src;
            bool own_valid = pk_own_ship_resolve(
                now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own, &own_src);

            static int64_t own_log = 0;
            if(now_us - own_log > 1000000){
                own_log = now_us;
                ESP_LOGD("pfd", "own src=%d valid=%d lat=%.5f lon=%.5f",
                         own_src, own_valid, own.lat, own.lon);
            }

            /* HDG 来源统一走 pk_own_heading_resolve（ADS-B>IMU>GPS track>无）——
             * PFD / traffic / list 共用同一优先级，不再各处内联。pk_pfd_status_t /
             * pk_pfd_hsi_t 的 imu_valid 字段其实是 "yaw_valid"(yaw_deg 是否可用)。 */
            float yaw_deg = 0.0f;
            bool  yaw_valid = pk_own_heading_resolve(
                own_valid, own_src, &own, have, have ? s.yaw_deg : 0.0f,
                &yaw_deg, NULL);

            /* Bank source priority — mirrors the yaw logic above:
             *   1) Derive from the bound aircraft's smoothed turn rate
             *      + ground speed (coordinated-turn formula). This is
             *      the actual aircraft's bank, irrespective of kit
             *      orientation.
             *   2) Fall back to IMU roll (the kit's tilt).
             * Pitch stays IMU-only — ADS-B carries no AoA, so we can
             * at best compute flight-path angle (atan(VS/GS)) which
             * isn't the same as aircraft pitch attitude.
             *
             * When the bank override fires we LEAVE imu.valid alone:
             * the attitude indicator gates on imu.valid for whether to
             * draw at all, and we still want to draw (with IMU pitch +
             * ADS-B bank) even if IMU itself is briefly stale, as long
             * as one or the other is fresh. */
            if (own_valid) {
                float bank_deg;
                if (pk_aircraft_derive_bank(
                        pk_ui_get_own_icao(), now_us,
                        (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL,
                        &bank_deg)) {
                    imu.roll_deg = bank_deg;
                    imu.valid    = true;
                }
            }

            /* 顶栏状态位。GPS/BLE/SD/电量/温度这一批"设备自身状态"字段与
             * 交通/地图/列表三页共用同一份取数逻辑，收在
             * pk_ui_topbar_status_collect（pfd_statusbar.c）里，四页只有
             * 这一处实现，不再各自抄一份容易分叉。
             *
             * rec 尚无数据源，pk_ui_topbar_status_collect 里留默认 false，
             * 顶栏的降级逻辑会自动不显示它 —— 详见 IMPLEMENTATION_PLAN.md 的
             * 「P2 · 顶栏状态位数据源接入」TODO(P2-1)：
             *   rec_active ← record_sink_file_stats() 加时间窗 */
            pk_pfd_status_t stat = {
                .imu_valid      = yaw_valid,
                .yaw_deg        = yaw_deg,
                .aircraft_count = n_aircraft,
            };
            pk_ui_topbar_status_collect(&stat);
            pk_pfd_hsi_t hsi = {
                .imu_valid = yaw_valid,
                .yaw_deg   = yaw_deg,
            };
            pk_pfd_alt_tape_t alt = {
                .valid       = own_valid && own.have_altitude,
                .altitude_ft = (own_valid && own.have_altitude)
                                   ? own.altitude_ft : 0,
            };

            /* Attitude fills the full panel as the screen background.
             * Statusbar / ALT tape / speed tape / HSI / VS draw on top
             * as opaque overlays — no need to pre-clear the frame. */
            int64_t tm0 = esp_timer_get_time();
            pk_pfd_attitude_render(fb, &imu);
            int64_t tm1 = esp_timer_get_time();
            pk_pfd_statusbar_render(fb, &stat);
            int64_t tm2 = esp_timer_get_time();
            pk_pfd_alt_tape_render(fb, &alt);
            pk_pfd_speed_tape_t spd = {
                .valid           = own_valid && own.have_velocity,
                .ground_speed_kt = (own_valid && own.have_velocity)
                                       ? own.ground_speed_kt : 0,
            };
            pk_pfd_speed_tape_render(fb, &spd);
            int64_t tm3 = esp_timer_get_time();
            pk_pfd_hsi_render(fb, &hsi);
            pk_pfd_hsi_traffic_render(fb);   /* HSI 半圆外圈叠加前方 traffic */
            int64_t tm4 = esp_timer_get_time();
            acc_att_us   += tm1 - tm0;
            acc_bar_us   += tm2 - tm1;
            acc_tape_us  += tm3 - tm2;
            acc_hsi_us   += tm4 - tm3;

            /* 右下三个数值框 + 左下本机来源徽标。绘制在 pfd_infobox.c，
             * 这里只负责把运行时状态整理成它要的数据。 */
            {
                pk_baro_state_t baro;
                pk_baro_get(&baro);

                bool adsb_vs = own_valid && own.have_velocity &&
                               (own_src == PK_OWN_SRC_BOUND_ADSB);
                pk_pfd_infobox_t ib = {
                    .baro_valid   = baro.valid,
                    .baro_alt_ft  = baro.alt_ft,
                    .alt_valid    = alt.valid,
                    .alt_ft       = alt.altitude_ft,
                    /* VS 优先取 ADS-B 自报（权威），否则退到 baro 微分（参考）。 */
                    .vs_valid     = adsb_vs || baro.valid,
                    .vs_fpm       = adsb_vs ? own.vert_rate_fpm : baro.vs_fpm,
                    .vs_from_adsb = adsb_vs,
                };
                pk_pfd_infobox_render(fb, &ib);
            }

            {
                /* ADS-B 降级提示：检测"绑定丢失"(BOUND_ADSB → 非绑定)的跳变，
                 * 之后 5s 闪烁红字 ADS-B LOST。覆盖"飞机 ADS-B/PFD 死机"场景
                 * ——提醒飞行员主显数据没了、已切到盒子自主传感器。
                 * 重新绑定即清除提示。 */
                static pk_own_src_t s_prev_src     = PK_OWN_SRC_NONE;
                static int64_t      s_adsb_lost_us = 0;
                pk_own_src_t cur_src = own_valid ? own_src : PK_OWN_SRC_NONE;
                if (s_prev_src == PK_OWN_SRC_BOUND_ADSB &&
                    cur_src    != PK_OWN_SRC_BOUND_ADSB) {
                    s_adsb_lost_us = now_us;          /* 刚丢失绑定 */
                }
                if (cur_src == PK_OWN_SRC_BOUND_ADSB) {
                    s_adsb_lost_us = 0;               /* 重新绑定 → 清提示 */
                }
                s_prev_src = cur_src;

                pk_pfd_leftbox_t lb = {
                    .speed_valid = spd.valid,
                    .kmh = (int)(spd.ground_speed_kt * 1.852f + 0.5f),
                    .adsb_lost_alert = (s_adsb_lost_us != 0) &&
                                       (now_us - s_adsb_lost_us < 5000000LL),
                    .alert_blink_on  = ((now_us / 400000) & 1) != 0,
                };

                /* 标签的降级链：呼号 → squawk → ICAO hex。依赖 aircraft_t，
                 * 故留在这里，pfd_infobox 只吃最终字符串。 */
                if (!own_valid || own_src == PK_OWN_SRC_NONE) {
                    lb.src = PK_PFD_SRC_NONE;
                    snprintf(lb.label, sizeof(lb.label), "--");
                } else if (own_src == PK_OWN_SRC_BOUND_ADSB) {
                    lb.src = PK_PFD_SRC_ADSB;
                    bool used = false;
                    if (own.have_callsign) {
                        int i;
                        for (i = 0; i < 7 && own.callsign[i]; i++)
                            lb.label[i] = own.callsign[i];
                        lb.label[i] = '\0';
                        while (i > 0 && lb.label[i - 1] == ' ')
                            lb.label[--i] = '\0';
                        if (lb.label[0] != '\0') used = true;
                    }
                    if (!used && own.have_squawk) {
                        /* squawk 比 ICAO hex 对飞行员更有意义 */
                        snprintf(lb.label, sizeof(lb.label), "%04d", own.squawk);
                        used = true;
                    }
                    if (!used) {
                        snprintf(lb.label, sizeof(lb.label), "%06lX",
                                 (unsigned long)own.icao24);
                    }
                } else {
                    lb.src = PK_PFD_SRC_GPS;
                    snprintf(lb.label, sizeof(lb.label), "GPS");
                }
                pk_pfd_leftbox_render(fb, &lb);
            }

            break;
        }
        }

        /* 全屏导航网格：压在一切之上的半透明层，底页刚在上面画完（见 modal
         * 那段注释）。放在 switch **之外**与演示模式标识同理——每一页都要能
         * 被它盖住，而上面那个 switch 的每一支都只知道自己那一页。 */
        if (modal == PK_UI_MODAL_NAVGRID) {
            pk_nav_grid_page_render(fb);
        }

        /* 瞬时提示叠加在任意页面之上(TARE 保存 / own 绑定·取消反馈)。 */
        sync_toast();

        /* 演示模式的常驻标识。放在这里——**switch 之外**——是它能覆盖每一页的
         * 原因：上面那个 switch 每一支都只画自己那一页，谁都不知道演示模式的
         * 存在；标识画在控件层且每帧无条件同步一次，就不存在"某一页忘了加"。
         * 每帧调一次而不是只在开关时调：切语言、进出键盘页、调平向导都会重排
         * 控件层，漏同步一次就是一段无标识的窗口。 */
        pk_ui_nav_set_demo(pk_demo_enabled());

        /* 交给 LVGL 合成并推屏：它会把 canvas 与其上的控件混合到 display
         * 缓冲，再经 lv_port 的 flush_cb 调 pk_display_flush_full()。
         * 直接调 flush_full 会跳过控件层，只推 PFD。 */
        int64_t t_draw_end = esp_timer_get_time();
        pk_lv_port_invalidate();
        /* 必须喂**真实**帧间隔。此前硬编码 33 ms，而真机一帧要 100 ms，于是
         * LVGL 眼中的时间只有真实的三分之一：180 ms 的 dock 动画实际跑满
         * 550 ms，调平长按 1 s 要按满 3 s，dock 的 5 s 自动收起变成 15 s。
         * 这类「感觉有点慢」的问题全是同一个根因。 */
        uint32_t elapsed_ms = 33;
        if (last_tick_us != 0) {
            int64_t d = (t_draw_end - last_tick_us) / 1000;
            /* 夹一下：首帧与调试断点会给出离谱的间隔，直接喂给 LVGL 会让
             * 动画一步跳到终点、定时器成批到期。 */
            elapsed_ms = (d < 1) ? 1 : (d > 500 ? 500 : (uint32_t)d);
        }
        last_tick_us = t_draw_end;
        pk_lv_port_tick(elapsed_ms);
        int64_t t_lvgl_end = esp_timer_get_time();
        acc_draw_us += t_draw_end - t_frame0;
        acc_lvgl_us += t_lvgl_end - t_draw_end;
        frames_in_window++;

        int64_t now = esp_timer_get_time();
        if (now - fps_window_start_us >= 1000000) {
            const char *mode_label = (mode == PK_UI_MODE_ADSB_LIST)  ? "LIST"
                                   : (mode == PK_UI_MODE_SETTINGS)   ? "SET"
                                   : (mode == PK_UI_MODE_ABOUT)      ? "ABOUT"
                                   : (mode == PK_UI_MODE_DIAG)       ? "DIAG"
                                   : (mode == PK_UI_MODE_CAL_WIZARD) ? "CAL"
                                   :                                   "PFD";
            {
                int64_t flush_us = 0; uint32_t flush_cnt = 0;
                uint32_t n = frames_in_window ? frames_in_window : 1;
                pk_lv_port_flush_stats(&flush_us, &flush_cnt);
                ESP_LOGD(TAG, "PERF2: att=%lld bar=%lld tape=%lld hsi=%lld us/frame",
                         (long long)(acc_att_us / n), (long long)(acc_bar_us / n),
                         (long long)(acc_tape_us / n), (long long)(acc_hsi_us / n));
                acc_att_us = acc_bar_us = acc_tape_us = acc_hsi_us = 0;
                {
                    int64_t ppa_us = 0, wait_us = 0; uint32_t pcnt = 0;
                    pk_display_flush_split(&ppa_us, &wait_us, &pcnt);
                    if (pcnt) {
                        ESP_LOGD(TAG, "PERF3: ppa=%lldus vsync_wait=%lldus per flush",
                                 (long long)(ppa_us / pcnt), (long long)(wait_us / pcnt));
                    }
                }
                ESP_LOGD(TAG, "PERF: draw=%lldus lvgl=%lldus (flush=%lldus x%lu) per frame",
                         (long long)(acc_draw_us / n),
                         (long long)(acc_lvgl_us / n),
                         (long long)(flush_cnt ? flush_us / flush_cnt : 0),
                         (unsigned long)flush_cnt);
                acc_draw_us = 0; acc_lvgl_us = 0;
            }
            /* 每秒一条 WARN。串口是排障时第一眼看的地方，"数据是假的"必须在
             * 这里也藏不住——屏上有徽标、手机上没数据、日志里有这条，三处互相
             * 印证，任何一条渠道单独看都能发现。 */
            if (pk_demo_enabled())
                ESP_LOGW(TAG, "DEMO MODE ACTIVE — attitude/GPS/baro/traffic "
                              "below are SIMULATED");
            ESP_LOGI(TAG, "%s %lu FPS  | roll=%+6.2f pitch=%+6.2f yaw=%6.2f"
                          "  imu_valid=%d",
                     mode_label,
                     (unsigned long)frames_in_window,
                     have ? s.roll_deg : 0.0f,
                     have ? s.pitch_deg : 0.0f,
                     have ? s.yaw_deg : 0.0f,
                     have);
            frames_in_window = 0;
            fps_window_start_us = now;
        }
        vTaskDelayUntil(&frame_start, pdMS_TO_TICKS(33));   /* 30 FPS target */
    }
}

esp_err_t pk_pfd_start(void)
{
    /* 6 KiB stack — generous because trig / floating-point ESP_LOGI
     * format strings can each chew 1 KiB on RISC-V. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        pfd_task, "pfd", 6 * 1024, NULL, 4, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
