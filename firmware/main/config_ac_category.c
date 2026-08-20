/*
 * config_ac_category.c — 机型分类，NVS 持久化。
 *
 * 结构照 config_traffic.c（volatile + portMUX + ensure_nvs + get/set/load），
 * NVS 类型用 u8 照 i18n.c（pk_i18n_set_lang 的 nvs_set_u8 范式）。
 */
#include "config_ac_category.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cfg_accat";

#define ACCAT_NVS_NAMESPACE  "pk_accat"
#define ACCAT_NVS_KEY_CAT    "cat"

/* 评审拍板：默认轻型活塞（本产品用户主流），设计文档「机型分类阈值」节。 */
#define ACCAT_DEFAULT  PK_AC_CAT_PISTON_LIGHT

static volatile uint8_t s_cat = (uint8_t)ACCAT_DEFAULT;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 幂等 NVS init，照 config_qnh.c:ensure_nvs / config_traffic.c:ensure_nvs */
static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

/* 落在 [GLIDER_ULTRALIGHT, JET_TRANSPORT] 区间内才认——0(unknown)与越界值都
 * 钳到默认档，不能让一个坏掉的 NVS 值把相位状态机的阈值表越界索引。 */
static pk_ac_category_t clamp_cat(uint8_t v)
{
    if (v < PK_AC_CAT_GLIDER_ULTRALIGHT || v >= PK_AC_CAT_COUNT)
        return ACCAT_DEFAULT;
    return (pk_ac_category_t)v;
}

pk_ac_category_t pk_ac_category_get(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t v = s_cat;
    portEXIT_CRITICAL(&s_mux);
    return (pk_ac_category_t)v;
}

void pk_ac_category_set(pk_ac_category_t cat)
{
    uint8_t v = (uint8_t)clamp_cat((uint8_t)cat);
    portENTER_CRITICAL(&s_mux);
    s_cat = v;
    portEXIT_CRITICAL(&s_mux);

    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(ACCAT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, ACCAT_NVS_KEY_CAT, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed (%s)", ACCAT_NVS_KEY_CAT, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "ac category -> %u", (unsigned)v);
}

void pk_config_ac_category_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(ACCAT_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no ac category config (%s), using default", esp_err_to_name(err));
        s_cat = (uint8_t)ACCAT_DEFAULT;
        return;
    }
    uint8_t v = (uint8_t)ACCAT_DEFAULT;
    if (nvs_get_u8(h, ACCAT_NVS_KEY_CAT, &v) != ESP_OK) v = (uint8_t)ACCAT_DEFAULT;
    nvs_close(h);

    s_cat = (uint8_t)clamp_cat(v);
    ESP_LOGI(TAG, "ac category: %u", (unsigned)s_cat);
}
