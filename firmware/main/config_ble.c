/*
 * config_ble.c — BLE 开关，NVS 持久化。
 *
 * 结构照 config_storage.c（volatile + portMUX + ensure_nvs + get/set/load），
 * NVS 类型 u8。默认**开**：BLE 是这台设备的主要输出通道（GDL90 给手机 App），
 * 默认关掉会让人以为设备坏了。
 */
#include "config_ble.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg_ble";

#define BLE_NVS_NAMESPACE  "pk_ble"
#define BLE_NVS_KEY        "ble_on"

static volatile uint8_t s_on = 1;      /* 默认开 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 幂等 NVS init，照 config_qnh.c:ensure_nvs */
static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool pk_ble_enabled_get(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t v = s_on;
    portEXIT_CRITICAL(&s_mux);
    return v != 0;
}

void pk_ble_enabled_set(bool on)
{
    const uint8_t v = on ? 1 : 0;
    portENTER_CRITICAL(&s_mux);
    s_on = v;
    portEXIT_CRITICAL(&s_mux);

    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(BLE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, BLE_NVS_KEY, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "save failed (%s)", esp_err_to_name(err));

    ESP_LOGI(TAG, "BLE -> %s (takes effect next boot)", v ? "on" : "off");
}

void pk_config_ble_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(BLE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_on = 1;                       /* 没存过 → 默认开 */
        ESP_LOGI(TAG, "no BLE setting stored, defaulting to on");
        return;
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, BLE_NVS_KEY, &v) != ESP_OK) v = 1;
    nvs_close(h);
    s_on = v ? 1 : 0;
    ESP_LOGI(TAG, "BLE setting: %s", s_on ? "on" : "off");
}
