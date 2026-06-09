/*
 * config_traffic.c — 地图朝向 + 雷达量程，NVS 持久化。
 *
 * 结构照 config_qnh.c（volatile + portMUX + ensure_nvs + get/set/load），
 * NVS 类型用 u8 照 i18n.c（pk_i18n_set_lang 的 nvs_set_u8 范式）。
 */
#include "config_traffic.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cfg_tfc";

#define TFC_NVS_NAMESPACE   "pk_tfc"
#define TFC_NVS_KEY_ORIENT  "orient"
#define TFC_NVS_KEY_RANGE   "range"
#define RANGE_IDX_DEFAULT   1          /* 5 NM */

static const int RANGE_NM[4] = { 2, 5, 10, 20 };

static volatile uint8_t s_orient    = PK_MAP_HEADING_UP;
static volatile uint8_t s_range_idx = RANGE_IDX_DEFAULT;
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

static void save_u8(const char *key, uint8_t v)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(TFC_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed (%s)", key, esp_err_to_name(err));
    }
}

pk_map_orient_t pk_map_orient_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t v = s_orient;
    portEXIT_CRITICAL(&s_mux);
    return (pk_map_orient_t)v;
}

void pk_map_orient_set(pk_map_orient_t m)
{
    uint8_t v = (m == PK_MAP_NORTH_UP) ? PK_MAP_NORTH_UP : PK_MAP_HEADING_UP;
    portENTER_CRITICAL(&s_mux);
    s_orient = v;
    portEXIT_CRITICAL(&s_mux);
    save_u8(TFC_NVS_KEY_ORIENT, v);
    ESP_LOGI(TAG, "map orient -> %s", v ? "NORTH-UP" : "HDG-UP");
}

int pk_traffic_range_idx_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t v = s_range_idx;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void pk_traffic_range_idx_set(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    portENTER_CRITICAL(&s_mux);
    s_range_idx = (uint8_t)idx;
    portEXIT_CRITICAL(&s_mux);
    save_u8(TFC_NVS_KEY_RANGE, (uint8_t)idx);
    ESP_LOGI(TAG, "traffic range -> %d NM", RANGE_NM[idx]);
}

int pk_traffic_range_nm(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return RANGE_NM[idx];
}

void pk_config_traffic_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(TFC_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no traffic config (%s), using defaults", esp_err_to_name(err));
        s_orient    = PK_MAP_HEADING_UP;
        s_range_idx = RANGE_IDX_DEFAULT;
        return;
    }
    uint8_t o = PK_MAP_HEADING_UP, r = RANGE_IDX_DEFAULT;
    if (nvs_get_u8(h, TFC_NVS_KEY_ORIENT, &o) != ESP_OK) o = PK_MAP_HEADING_UP;
    if (nvs_get_u8(h, TFC_NVS_KEY_RANGE,  &r) != ESP_OK) r = RANGE_IDX_DEFAULT;
    nvs_close(h);

    s_orient    = (o == PK_MAP_NORTH_UP) ? PK_MAP_NORTH_UP : PK_MAP_HEADING_UP;
    s_range_idx = (r <= 3) ? r : RANGE_IDX_DEFAULT;
    ESP_LOGI(TAG, "traffic config: orient=%s range=%dNM",
             s_orient ? "N-UP" : "H-UP", RANGE_NM[s_range_idx]);
}
