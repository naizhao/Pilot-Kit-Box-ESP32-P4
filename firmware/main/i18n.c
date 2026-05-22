/*
 * i18n.c — active language state and persistence.
 */

#include "i18n.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "i18n";

#define I18N_NVS_NAMESPACE  "pk_ui"
#define I18N_NVS_KEY_LANG   "lang"

static pk_lang_t s_lang = PK_LANG_EN;

static bool lang_valid(pk_lang_t lang)
{
    return lang >= 0 && lang < PK_LANG_COUNT;
}

static esp_err_t ensure_nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s), wiping and retrying",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_ERR_INVALID_STATE) return ESP_OK;
    return err;
}

esp_err_t pk_i18n_init(void)
{
    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s), language persistence disabled",
                 esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(I18N_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no persisted language, defaulting to English");
        s_lang = PK_LANG_EN;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open language namespace failed (%s), defaulting to English",
                 esp_err_to_name(err));
        s_lang = PK_LANG_EN;
        return err;
    }

    uint8_t raw = 0;
    err = nvs_get_u8(h, I18N_NVS_KEY_LANG, &raw);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no persisted language key, defaulting to English");
        s_lang = PK_LANG_EN;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load language failed (%s), defaulting to English",
                 esp_err_to_name(err));
        s_lang = PK_LANG_EN;
        return err;
    }
    s_lang = lang_valid((pk_lang_t)raw) ? (pk_lang_t)raw : PK_LANG_EN;
    ESP_LOGI(TAG, "language ready: %s", pk_i18n_lang_name(s_lang));
    return ESP_OK;
}

pk_lang_t pk_i18n_get_lang(void)
{
    return s_lang;
}

esp_err_t pk_i18n_set_lang(pk_lang_t lang)
{
    if (!lang_valid(lang)) return ESP_ERR_INVALID_ARG;
    s_lang = lang;

    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    err = nvs_open(I18N_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, I18N_NVS_KEY_LANG, (uint8_t)lang);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "language -> %s (%s)",
             pk_i18n_lang_name(lang), esp_err_to_name(err));
    return err;
}

esp_err_t pk_i18n_toggle_lang(void)
{
    pk_lang_t next = (s_lang == PK_LANG_EN) ? PK_LANG_ZH : PK_LANG_EN;
    return pk_i18n_set_lang(next);
}

const char *pk_i18n_text(pk_tr_id_t id)
{
    return pk_i18n_catalog_text(s_lang, id);
}

const char *pk_i18n_lang_name(pk_lang_t lang)
{
    switch (lang) {
    case PK_LANG_EN: return pk_i18n_catalog_text(s_lang, PK_TR_LANG_ENGLISH);
    case PK_LANG_ZH: return pk_i18n_catalog_text(s_lang, PK_TR_LANG_CHINESE);
    default:         return "?";
    }
}
