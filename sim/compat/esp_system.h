/* esp_system.h 桩 —— 诊断页只用 esp_reset_reason()。
 * 模拟器恒报 POWERON：那是最常见的真实值，也不会把 SYS 卡染成琥珀。 */
#pragma once

typedef enum {
    ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
    ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB,
    ESP_RST_JTAG,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
