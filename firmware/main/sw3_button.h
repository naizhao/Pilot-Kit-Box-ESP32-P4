/*
 * sw3_button.h — SW3 开机态按键状态机与 ESP32-P4 GPIO4 适配层。
 *
 * SW3 的主路径始终是 SW3.1→CHG_QON→SY6970 /QON、SW3.2→GND；本模块
 * 只读取经隔离二极管送到 J1-14/J3-14/GPIO4 的副本，不参与关机态唤醒。
 * 纯状态机不碰 GPIO/显示/PMIC，host 时间线测试可独立验证短长按互斥。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef PK_SW3_HOST_TEST
#include "esp_err.h"
#endif

#define PK_SW3_DEBOUNCE_MS       30u
#define PK_SW3_LONG_PRESS_MS   2000u
#define PK_SW3_STARTUP_GUARD_MS 2000u

typedef uint32_t pk_sw3_event_t;
enum {
    PK_SW3_EVENT_NONE         = 0,
    PK_SW3_EVENT_SHORT_PRESS  = 1u << 0,
    /* 长按阈值到达：用于先点亮屏幕并提示“松开关机”。 */
    PK_SW3_EVENT_LONG_READY   = 1u << 1,
    /* 长按后的稳定松手：此时 /QON 已恢复高电平，才允许写 BATFET_DIS。 */
    PK_SW3_EVENT_LONG_RELEASE = 1u << 2,
};

typedef struct {
    uint32_t boot_ms;
    uint32_t raw_since_ms;
    uint32_t pressed_since_ms;
    bool raw_known;
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    bool tracking_press;
    bool long_announced;
} pk_sw3_button_t;

void pk_sw3_button_reset(pk_sw3_button_t *s, uint32_t now_ms);

/* 喂入当前原始电平（pressed=true 表示 GPIO 低）。返回事件位图。
 * uint32_t 无符号差值使毫秒时基回绕仍保持正确。 */
pk_sw3_event_t pk_sw3_button_update(pk_sw3_button_t *s,
                                    bool pressed,
                                    uint32_t now_ms);

typedef struct {
    bool set_screen;
    bool screen_awake;
    bool show_release_to_poweroff;
    bool show_need_unplug;
    bool request_shutdown;
} pk_sw3_policy_t;

/* 把纯按键事件映射为外部可观察动作；不直接调用显示或 PMIC。 */
pk_sw3_policy_t pk_sw3_policy(pk_sw3_event_t events,
                              bool screen_awake,
                              bool external_power_present);

/* 目标胶水的两条纯判据也留在 host 可测层：UI/PFD 就绪后即可接受按键，
 * PMIC 暂不可用不能屏蔽短按交互；真正关机仍由 power_sy6970_shutdown()
 * 独立 fail-closed。过期电源快照不能永久把“已拔 USB”误判成“仍在位”。 */
bool pk_sw3_runtime_ready(bool ui_ready, bool pfd_ready, bool power_ready);
bool pk_sw3_external_power_present(bool snapshot_stale,
                                   bool snapshot_vbus_present);

#ifndef PK_SW3_HOST_TEST
/* 配置 GPIO4 内部上拉并创建 10ms 轮询任务。幂等。 */
esp_err_t pk_sw3_button_start(void);
#endif
