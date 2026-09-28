/* sw3_button.c — 纯状态机在前，目标端 GPIO/显示/PMIC 胶水在 host guard 后。 */
#include "sw3_button.h"

#include <string.h>

static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t duration_ms)
{
    return (uint32_t)(now_ms - since_ms) >= duration_ms;
}

void pk_sw3_button_reset(pk_sw3_button_t *s, uint32_t now_ms)
{
    if (s == NULL) return;
    memset(s, 0, sizeof *s);
    s->boot_ms = now_ms;
}

pk_sw3_event_t pk_sw3_button_update(pk_sw3_button_t *s,
                                    bool pressed,
                                    uint32_t now_ms)
{
    if (s == NULL) return PK_SW3_EVENT_NONE;

    if (!s->raw_known) {
        s->raw_known = true;
        s->raw_pressed = pressed;
        s->raw_since_ms = now_ms;
    } else if (pressed != s->raw_pressed) {
        s->raw_pressed = pressed;
        s->raw_since_ms = now_ms;
    }

    pk_sw3_event_t events = PK_SW3_EVENT_NONE;

    /* 原始电平与稳定态不一致时正在消抖。特别地，看到原始松手后不能继续
     * 用“当前 now”跨长按阈值，否则 1.99s 的按压会因 30ms 松手消抖被误判
     * 成长按。最终分类使用原始松手开始的时刻 raw_since_ms。 */
    if (s->raw_pressed != s->stable_pressed &&
        elapsed(now_ms, s->raw_since_ms, PK_SW3_DEBOUNCE_MS)) {
        s->stable_pressed = s->raw_pressed;

        if (s->stable_pressed) {
            if (s->armed) {
                s->tracking_press = true;
                s->long_announced = false;
                /* 消抖只确认原始沿有效，不应把真实按下时刻推迟 30ms，更不能在
                 * 任务恰好停顿数秒时把那段实际按住时间全部吃掉。 */
                s->pressed_since_ms = s->raw_since_ms;
            }
        } else if (!s->armed) {
            /* 退出 shipping mode 的那一次按住/松手只负责解除启动保护。 */
            if (elapsed(now_ms, s->boot_ms, PK_SW3_STARTUP_GUARD_MS)) {
                s->armed = true;
            }
            s->tracking_press = false;
            s->long_announced = false;
        } else if (s->tracking_press) {
            const uint32_t held_ms =
                (uint32_t)(s->raw_since_ms - s->pressed_since_ms);
            if (held_ms >= PK_SW3_LONG_PRESS_MS) {
                if (!s->long_announced) events |= PK_SW3_EVENT_LONG_READY;
                events |= PK_SW3_EVENT_LONG_RELEASE;
            } else {
                events |= PK_SW3_EVENT_SHORT_PRESS;
            }
            s->tracking_press = false;
            s->long_announced = false;
        }
    }

    /* 上电保护不仅等时间，还必须见过一次稳定松手。用户按住 SW3 约 3s
     * 唤醒时，P4 可能在手仍未松开时已经启动；不等松手就 armed 会立刻把
     * 同一次按压解释成软件长按，再次关机。 */
    if (!s->armed && !s->stable_pressed && !s->raw_pressed &&
        elapsed(now_ms, s->boot_ms, PK_SW3_STARTUP_GUARD_MS)) {
        s->armed = true;
    }

    /* 松手消抖期间不再补发 LONG_READY，分类留到稳定松手分支用实际按住
     * 时长一次完成。正常轮询时则在阈值这一拍只发一次提示。 */
    if (s->armed && s->tracking_press && s->stable_pressed &&
        s->raw_pressed && !s->long_announced &&
        elapsed(now_ms, s->pressed_since_ms, PK_SW3_LONG_PRESS_MS)) {
        s->long_announced = true;
        events |= PK_SW3_EVENT_LONG_READY;
    }

    return events;
}

pk_sw3_policy_t pk_sw3_policy(pk_sw3_event_t events,
                              bool screen_awake,
                              bool external_power_present)
{
    pk_sw3_policy_t out = {0};
    const bool long_event =
        (events & (PK_SW3_EVENT_LONG_READY | PK_SW3_EVENT_LONG_RELEASE)) != 0;

    if (!long_event && (events & PK_SW3_EVENT_SHORT_PRESS) != 0) {
        out.set_screen = true;
        out.screen_awake = !screen_awake;
    }

    if ((events & PK_SW3_EVENT_LONG_READY) != 0) {
        out.show_release_to_poweroff = true;
        if (!screen_awake) {
            out.set_screen = true;
            out.screen_awake = true;
        }
    }

    if ((events & PK_SW3_EVENT_LONG_RELEASE) != 0) {
        if (external_power_present) {
            out.show_need_unplug = true;
        } else {
            out.request_shutdown = true;
        }
    }
    return out;
}

bool pk_sw3_runtime_ready(bool ui_ready, bool pfd_ready, bool power_ready)
{
    /* PMIC 暂不可读不能阻止短按息屏/亮屏；真正的关机路径由
     * power_sy6970_shutdown() 自己校验配置闸门与实时 REG07。保留这个
     * 输入是为了让 host 回归测试明确覆盖“PMIC unavailable”场景。 */
    (void)power_ready;
    return ui_ready && pfd_ready;
}

bool pk_sw3_external_power_present(bool snapshot_stale,
                                   bool snapshot_vbus_present)
{
    return !snapshot_stale && snapshot_vbus_present;
}

#ifndef PK_SW3_HOST_TEST

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "i18n_catalog.h"
#include "pfd.h"
#include "power_service.h"
#include "power_sy6970.h"
#include "ui_state.h"

#define SW3_SENSE_GPIO          GPIO_NUM_4
#define SW3_POLL_MS             10u
#define SW3_PROMPT_MIN_MS       300u
#define SW3_TASK_STACK          3072u
#define SW3_TASK_PRIORITY       3u

static const char *TAG = "sw3";
static TaskHandle_t s_task;

static bool external_power_present(void)
{
    const power_snapshot_t ps = power_service_snapshot();
    return pk_sw3_external_power_present(ps.stale, ps.vbus_present);
}

static bool runtime_ready(void)
{
    sy6970_diag_t diag;
    const bool power_ready = sy6970_diag_get(&diag) && diag.ready;
    return pk_sw3_runtime_ready(pk_ui_ready(), pk_pfd_ready(), power_ready);
}

static void sw3_task(void *arg)
{
    (void)arg;
    pk_sw3_button_t button;
    uint32_t prompt_ms = 0;
    pk_sw3_button_reset(&button, (uint32_t)(esp_timer_get_time() / 1000));

    ESP_LOGI(TAG, "GPIO4 SW3 sense task waiting for UI/PFD readiness");

    bool was_ready = false;
    bool raw_level_known = false;
    int last_raw_level = 1;
    for (;;) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (!runtime_ready()) {
            /* 未就绪期间每拍重置，任何启动长按都只能在第一次稳定松手后解除
             * 保护，绝不可能在无可见提示时落到 BATFET_DIS。 */
            pk_sw3_button_reset(&button, now_ms);
            was_ready = false;
            raw_level_known = false;
            vTaskDelay(pdMS_TO_TICKS(SW3_POLL_MS));
            continue;
        }
        if (!was_ready) {
            was_ready = true;
            pk_sw3_button_reset(&button, now_ms);
            ESP_LOGI(TAG,
                     "GPIO4 SW3 sense armed after readiness: debounce=%ums "
                     "long=%ums guard=%ums",
             PK_SW3_DEBOUNCE_MS, PK_SW3_LONG_PRESS_MS,
             PK_SW3_STARTUP_GUARD_MS);
        }
        const int raw_level = gpio_get_level(SW3_SENSE_GPIO);
        if (!raw_level_known || raw_level != last_raw_level) {
            ESP_LOGI(TAG, "GPIO4 raw=%d (%s)", raw_level,
                     raw_level == 0 ? "pressed" : "released");
            raw_level_known = true;
            last_raw_level = raw_level;
        }
        const bool pressed = raw_level == 0;
        const pk_sw3_event_t events =
            pk_sw3_button_update(&button, pressed, now_ms);

        if (events != PK_SW3_EVENT_NONE) {
            const pk_sw3_policy_t policy = pk_sw3_policy(
                events, pk_display_screen_awake(), external_power_present());

            if (policy.set_screen) {
                pk_display_screen_set_awake(policy.screen_awake);
                ESP_LOGI(TAG, "short/long action: screen %s",
                         policy.screen_awake ? "awake" : "blanked");
            }
            if (policy.show_release_to_poweroff) {
                prompt_ms = now_ms;
                pk_ui_toast_show(PK_TR_POWER_RELEASE_TO_OFF, false);
                ESP_LOGI(TAG, "long threshold reached; waiting for release");
            }
            if (policy.show_need_unplug) {
                pk_ui_toast_show(PK_TR_POWER_NEED_UNPLUG, true);
                ESP_LOGW(TAG, "power-off blocked: external power detected");
            }
            if (policy.request_shutdown) {
                /* 若任务曾长时间没有调度，READY 与 RELEASE 可能在同一拍补发；
                 * 至少保留 300ms，让“松开关机”提示确实能被人看见。 */
                const uint32_t shown_ms = (uint32_t)(now_ms - prompt_ms);
                if (shown_ms < SW3_PROMPT_MIN_MS) {
                    vTaskDelay(pdMS_TO_TICKS(SW3_PROMPT_MIN_MS - shown_ms));
                }
                ESP_LOGW(TAG, "long press released; requesting BATFET shutdown");
                (void)power_sy6970_shutdown();
                /* 仅电池成功时不会返回；返回说明关机未完成（例如 H1 下游供电
                 * 绕过 U19，P4 无法在动作前直接检测）。 */
                pk_ui_toast_show(PK_TR_POWER_OFF_FAILED, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SW3_POLL_MS));
    }
}

esp_err_t pk_sw3_button_start(void)
{
    if (s_task != NULL) return ESP_OK;

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SW3_SENSE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO4 config failed: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(sw3_task, "sw3", SW3_TASK_STACK, NULL,
                    SW3_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif
