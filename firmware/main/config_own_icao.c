/*
 * config_own_icao.c — 本机 ICAO 绑定关系，NVS 持久化。
 *
 * 结构照 config_ac_category.c（volatile + ensure_nvs + get/set/load），
 * 差异：NVS 类型用 u32（own_icao 是 24-bit ICAO），不做范围钳制——
 * 读出来只判 ==0（无绑定）还是 !=0（有绑定）。
 */
#include "config_own_icao.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

#include "ui_state.h"   /* pk_ui_set_own_icao — load() 恢复绑定时调 */

static const char *TAG = "cfg_ownicao";

#define OWNICAO_NVS_NAMESPACE  "pk_ownicao"
#define OWNICAO_NVS_KEY        "icao"

/* 幂等 NVS init，照 config_ac_category.c / config_qnh.c / config_traffic.c */
static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

void pk_config_own_icao_set(uint32_t icao24)
{
    uint32_t v = icao24 & 0xFFFFFF;
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(OWNICAO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u32(h, OWNICAO_NVS_KEY, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed (%s)", OWNICAO_NVS_KEY, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "own ICAO persisted → %06lX", (unsigned long)v);
}

void pk_config_own_icao_clear(void)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(OWNICAO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u32(h, OWNICAO_NVS_KEY, 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear %s failed (%s)", OWNICAO_NVS_KEY, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "own ICAO cleared in NVS");
}

void pk_config_own_icao_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(OWNICAO_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no own ICAO config (%s), unbound", esp_err_to_name(err));
        return;
    }
    uint32_t v = 0;
    if (nvs_get_u32(h, OWNICAO_NVS_KEY, &v) != ESP_OK) v = 0;
    nvs_close(h);

    if (v != 0) {
        ESP_LOGI(TAG, "restoring own ICAO %06lX from NVS", (unsigned long)v);
        pk_ui_set_own_icao(v);
    } else {
        ESP_LOGI(TAG, "own ICAO not set in NVS, leaving unbound");
    }
}
