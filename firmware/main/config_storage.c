/*
 * config_storage.c — ADS-B 日志存储位置，NVS 持久化。
 *
 * 结构照 config_traffic.c（volatile + portMUX + ensure_nvs + get/set/load），
 * NVS 类型用 u8。
 */
#include "config_storage.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cfg_stor";

#define STOR_NVS_NAMESPACE  "pk_stor"
#define STOR_NVS_KEY        "log_store"

static volatile uint8_t s_store = PK_LOG_STORE_FLASH;
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

pk_log_store_t pk_log_store_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t v = s_store;
    portEXIT_CRITICAL(&s_mux);
    return (pk_log_store_t)v;
}

void pk_log_store_set(pk_log_store_t s)
{
    uint8_t v = (s == PK_LOG_STORE_SD) ? PK_LOG_STORE_SD : PK_LOG_STORE_FLASH;
    portENTER_CRITICAL(&s_mux);
    s_store = v;
    portEXIT_CRITICAL(&s_mux);

    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(STOR_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, STOR_NVS_KEY, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed (%s)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "log store -> %s (takes effect next boot)",
             v ? "microSD" : "flash");
}

void pk_config_storage_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(STOR_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no storage config (%s), using flash", esp_err_to_name(err));
        s_store = PK_LOG_STORE_FLASH;
        return;
    }
    uint8_t v = PK_LOG_STORE_FLASH;
    if (nvs_get_u8(h, STOR_NVS_KEY, &v) != ESP_OK) v = PK_LOG_STORE_FLASH;
    nvs_close(h);

    s_store = (v == PK_LOG_STORE_SD) ? PK_LOG_STORE_SD : PK_LOG_STORE_FLASH;
    ESP_LOGI(TAG, "log store: %s", s_store ? "microSD" : "flash");
}
