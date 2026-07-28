/*
 * pfd.c — Primary Flight Display task + frame dispatcher.
 *
 * Renders one of four UI modes into the 320×240 ST7789 framebuffer at
 * 30 FPS:
 *
 *   PFD         — G1000-style attitude + statusbar + HSI + ALT tape +
 *                 GS / VS readouts.
 *   CAL_WIZARD  — IMU compass-calibration figure-8 prompt.
 *   ABOUT       — system info page.
 *   ADSB_LIST   — live ADS-B contacts table + detail pane.
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

#include "about_page.h"
#include "adsb_list.h"
#include "diag_page.h"
#include "aircraft_state.h"
#include "ble_gatt.h"
#include "gps.h"
#include "own_ship.h"
#include "cal_wizard.h"
#include "display.h"
#include "imu_task.h"
#include "pfd_attitude.h"
#include "pfd_draw.h"
#include "pfd_font.h"
#include "pfd_hsi.h"
#include "pfd_hsi_traffic.h"
#include "lv_port.h"
#include "pfd_infobox.h"
#include "pk_ui_nav.h"
#include "pk_ui_nav_host.h"
#include "touch_gt911.h"
#include "soc_temp.h"
#include "pfd_statusbar.h"
#include "pfd_speed_tape.h"
#include "pfd_tape.h"
#include "baro.h"
#include "sdkconfig.h"
#include "settings_page.h"
#include "traffic_page.h"
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
static void sync_toast(void)
{
    pk_tr_id_t id;
    bool is_error;
    if (pk_ui_toast_get(&id, &is_error)) {
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
    int64_t  fps_window_start_us = esp_timer_get_time();
    uint32_t frames_in_window = 0;

    while (1) {
        TickType_t frame_start = xTaskGetTickCount();

        pk_imu_sample_t s;
        bool have = pk_imu_sample_get(&s);
        pk_ui_cal_wizard_tick(have, have ? s.accuracy : 0);

        pk_ui_mode_t mode = pk_ui_get_mode();

        switch (mode) {
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

            pk_gps_state_t gps;
            pk_gps_get(&gps);

            /* 顶栏状态位。rec / batt / temp 三项尚无数据源，留默认 false，
             * 顶栏的降级逻辑会自动不显示它们 —— 详见 IMPLEMENTATION_PLAN.md
             * 的「P2 · 顶栏状态位数据源接入」：
             *   TODO(P2-1): rec_active   ← record_sink_file_stats() 加时间窗
             *   TODO(P2-2): batt_*       ← 需要 ADC 分压 + 充电检测脚，硬件未定
             *
             * temp（P2-3）已接：soc_temp.c 读片内结温，1 Hz 采样 + 滞回。
             * 注意不能改用 baro 的温度——那是 BMP388 测的座舱环境温度，与
             * 「设备自己在过热」是两回事，暴晒时能差二三十度。*/
            pk_pfd_status_t stat = {
                .imu_valid      = yaw_valid,
                .yaw_deg        = yaw_deg,
                .aircraft_count = n_aircraft,
                .gps_have_fix   = gps.have_fix,
                .gps_sats       = (uint8_t)(gps.sats < 0 ? 0 : (gps.sats > 99 ? 99 : gps.sats)),
                .ble_connected  = ble_gatt_is_connected(),
                /* 顶栏动效（充电动画）的相位基准，必须是单调时钟而非帧计数。 */
                .uptime_ms      = (uint32_t)(now_us / 1000),
            };
            stat.temp_warn = pk_soc_temp_get(&stat.temp_c);
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
            pk_pfd_attitude_render(fb, &imu);
            pk_pfd_statusbar_render(fb, &stat);
            pk_pfd_alt_tape_render(fb, &alt);
            pk_pfd_speed_tape_t spd = {
                .valid           = own_valid && own.have_velocity,
                .ground_speed_kt = (own_valid && own.have_velocity)
                                       ? own.ground_speed_kt : 0,
            };
            pk_pfd_speed_tape_render(fb, &spd);
            pk_pfd_hsi_render(fb, &hsi);
            pk_pfd_hsi_traffic_render(fb);   /* HSI 半圆外圈叠加前方 traffic */

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
                    .mph = (int)(spd.ground_speed_kt * 1.15078f + 0.5f),
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

        /* 瞬时提示叠加在任意页面之上(TARE 保存 / own 绑定·取消反馈)。 */
        sync_toast();

        /* 交给 LVGL 合成并推屏：它会把 canvas 与其上的控件混合到 display
         * 缓冲，再经 lv_port 的 flush_cb 调 pk_display_flush_full()。
         * 直接调 flush_full 会跳过控件层，只推 PFD。 */
        pk_lv_port_invalidate();
        pk_lv_port_tick(33);
        frames_in_window++;

        int64_t now = esp_timer_get_time();
        if (now - fps_window_start_us >= 1000000) {
            const char *mode_label = (mode == PK_UI_MODE_ADSB_LIST)  ? "LIST"
                                   : (mode == PK_UI_MODE_SETTINGS)   ? "SET"
                                   : (mode == PK_UI_MODE_ABOUT)      ? "ABOUT"
                                   : (mode == PK_UI_MODE_DIAG)       ? "DIAG"
                                   : (mode == PK_UI_MODE_CAL_WIZARD) ? "CAL"
                                   :                                   "PFD";
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
