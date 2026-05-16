/*
 * button_task.c — BTN1 (GPIO 26) polling + short/long press dispatch.
 *
 * State machine:
 *
 *   RELEASED ─[level→0]─→ PRESSING (start debounce timer)
 *
 *   PRESSING ─[level=1 within debounce]─→ RELEASED (bounce, ignore)
 *   PRESSING ─[level=0 past debounce]──→ HELD_SHORT
 *
 *   HELD_SHORT ─[level=1]──→ tare_yaw(), RELEASED
 *   HELD_SHORT ─[held ≥ LONG_PRESS_MS]→ full_reorient(), HELD_LONG
 *
 *   HELD_LONG ─[level=1]──→ RELEASED  (suppresses retrigger until lifted)
 */

#include "button_task.h"
#include "imu_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "btn";

#define BTN1_GPIO            26
#define BTN1_POLL_MS         20      /* 50 Hz polling */
#define BTN1_DEBOUNCE_US     (40 * 1000)
#define BTN1_LONG_PRESS_US   (3000 * 1000)

typedef enum {
    BTN_RELEASED,
    BTN_PRESSING,
    BTN_HELD_SHORT,
    BTN_HELD_LONG,
} btn_state_t;

static void button_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "BTN1 polling task running on GPIO%d", BTN1_GPIO);

    btn_state_t state = BTN_RELEASED;
    int64_t     down_us = 0;

    while (1) {
        int     level = gpio_get_level(BTN1_GPIO);   /* 0 = pressed (pull-up) */
        int64_t now   = esp_timer_get_time();

        switch (state) {
        case BTN_RELEASED:
            if (level == 0) {
                state   = BTN_PRESSING;
                down_us = now;
            }
            break;

        case BTN_PRESSING:
            if (level != 0) {
                /* Released within debounce window — bounce, drop. */
                state = BTN_RELEASED;
            } else if (now - down_us >= BTN1_DEBOUNCE_US) {
                state = BTN_HELD_SHORT;
            }
            break;

        case BTN_HELD_SHORT:
            if (level != 0) {
                /* Released before long threshold → SHORT press fires. */
                ESP_LOGI(TAG, "short press → tare yaw");
                (void)pk_imu_tare_yaw();
                state = BTN_RELEASED;
            } else if (now - down_us >= BTN1_LONG_PRESS_US) {
                /* Still down past 3 s → LONG press fires, latch state. */
                ESP_LOGI(TAG, "long press → full reorient + save DCD");
                (void)pk_imu_full_reorient();
                state = BTN_HELD_LONG;
            }
            break;

        case BTN_HELD_LONG:
            if (level != 0) state = BTN_RELEASED;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(BTN1_POLL_MS));
    }
}

esp_err_t pk_button_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN1_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        button_task, "btn", 3072, NULL, 3, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
