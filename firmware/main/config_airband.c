#include "config_airband.h"
#include "airband_frequency_input.h"
#include <string.h>
#ifndef PK_AIRBAND_CONFIG_HOST_TEST
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "cfg_airband";
static portMUX_TYPE s_config_mux = portMUX_INITIALIZER_UNLOCKED;
static pk_airband_config_t s_config;
static bool s_dirty;
static uint32_t s_revision, s_changed_ms;
static const pk_airband_config_t defaults = {
    .version=1, .channel_khz=121500, .carrier_hz=121500000,
    .squelch_dbm_x2=-180, .hysteresis_db_x2=12,
    .volume=35, .squelch_enabled=1,
};

void pk_airband_config_load(void)
{
    pk_airband_config_t c = defaults;
    nvs_handle_t h;
    esp_err_t err = nvs_open("pk_airband", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(c);
        err = nvs_get_blob(h,"settings",&c,&len);
        nvs_close(h);
        if (err != ESP_OK || len != sizeof(c) || c.version != 1) c = defaults;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG,"NVS load failed: %s; defaults",esp_err_to_name(err));
    }
    uint32_t hz=0;
    if (!pk_airband_channel_to_carrier_hz(c.channel_khz,&hz) || hz != c.carrier_hz) {
        c.channel_khz=defaults.channel_khz; c.carrier_hz=defaults.carrier_hz;
    }
    if (c.volume > 100) c.volume=defaults.volume;
    if (c.muted > 1) c.muted=defaults.muted;
    if (c.squelch_enabled > 1) c.squelch_enabled=defaults.squelch_enabled;
    if (c.squelch_dbm_x2 < -280 || c.squelch_dbm_x2 > -40) c.squelch_dbm_x2=defaults.squelch_dbm_x2;
    if (c.hysteresis_db_x2 > 80) c.hysteresis_db_x2=defaults.hysteresis_db_x2;
    c.reserved=0;
    portENTER_CRITICAL(&s_config_mux);
    s_config=c; s_dirty=false; s_revision++;
    portEXIT_CRITICAL(&s_config_mux);
}

pk_airband_config_t pk_airband_config_get(void)
{
    portENTER_CRITICAL(&s_config_mux);
    pk_airband_config_t c=s_config;
    portEXIT_CRITICAL(&s_config_mux);
    return c;
}

static void changed(void)
{
    s_dirty=true; s_revision++;
    s_changed_ms=(uint32_t)(esp_timer_get_time()/1000);
}

void pk_airband_config_tuned(uint32_t channel_khz,uint32_t carrier_hz)
{
    uint32_t hz;
    if (!pk_airband_channel_to_carrier_hz(channel_khz,&hz) || hz != carrier_hz) return;
    portENTER_CRITICAL(&s_config_mux);
    if (s_config.channel_khz != channel_khz || s_config.carrier_hz != carrier_hz) {
        s_config.channel_khz=channel_khz; s_config.carrier_hz=carrier_hz; changed();
    }
    portEXIT_CRITICAL(&s_config_mux);
}

void pk_airband_config_volume(uint8_t volume)
{
    if (volume > 100) volume=100;
    portENTER_CRITICAL(&s_config_mux);
    if (s_config.volume != volume) { s_config.volume=volume; changed(); }
    portEXIT_CRITICAL(&s_config_mux);
}
void pk_airband_config_muted(bool muted)
{
    portENTER_CRITICAL(&s_config_mux);
    if (s_config.muted != muted) { s_config.muted=muted; changed(); }
    portEXIT_CRITICAL(&s_config_mux);
}
void pk_airband_config_squelch(bool enabled,int16_t threshold,uint16_t hysteresis)
{
    if (threshold < -280 || threshold > -40 || hysteresis > 80) return;
    portENTER_CRITICAL(&s_config_mux);
    if (s_config.squelch_enabled != enabled || s_config.squelch_dbm_x2 != threshold ||
        s_config.hysteresis_db_x2 != hysteresis) {
        s_config.squelch_enabled=enabled; s_config.squelch_dbm_x2=threshold;
        s_config.hysteresis_db_x2=hysteresis; changed();
    }
    portEXIT_CRITICAL(&s_config_mux);
}

void pk_airband_config_poll(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_config_mux);
    if (!s_dirty || (uint32_t)(now_ms-s_changed_ms) < 2000u) {
        portEXIT_CRITICAL(&s_config_mux);
        return;
    }
    pk_airband_config_t c=s_config;
    uint32_t revision=s_revision;
    portEXIT_CRITICAL(&s_config_mux);
    nvs_handle_t h;
    esp_err_t err=nvs_open("pk_airband",NVS_READWRITE,&h);
    if (err == ESP_OK) {
        err=nvs_set_blob(h,"settings",&c,sizeof(c));
        if (err == ESP_OK) err=nvs_commit(h);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_config_mux);
    if (revision == s_revision) {
        if (err == ESP_OK) s_dirty=false;
        else s_changed_ms=now_ms;
    }
    portEXIT_CRITICAL(&s_config_mux);
    if (err != ESP_OK) ESP_LOGW(TAG,"NVS save failed: %s; retry pending",esp_err_to_name(err));
}

#ifndef PK_AIRBAND_CONFIG_HOST_TEST
static void config_task(void *arg)
{
    (void)arg;
    for (;;) {
        pk_airband_config_poll((uint32_t)(esp_timer_get_time()/1000));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif
bool pk_airband_config_start(void)
{
#ifndef PK_AIRBAND_CONFIG_HOST_TEST
    return xTaskCreate(config_task,"airband_nvs",3072,NULL,1,NULL) == pdPASS;
#else
    return true;
#endif
}
