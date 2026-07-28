/*
 * config_fab.c — FAB 落点，NVS 持久化。
 *
 * 结构照 config_traffic.c（同 namespace 两个 u8 key + volatile + portMUX +
 * ensure_nvs + get/set/load），那份又照的 config_qnh.c。同类配置走同一套写法，
 * 读一个就等于读懂全部。
 */
#include "config_fab.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "cfg_fab";

#define FAB_NVS_NAMESPACE   "pk_fab"
#define FAB_NVS_KEY_LEFT    "left"
#define FAB_NVS_KEY_YPCT    "ypct"

/* 默认右侧：右手持机时拇指自然落点。垂直默认 -1 = 「没存过」，交给导航层
 * 自己的 FAB_DEFAULT_Y（它按 tape 与信息框之间的空当算，比这里写死一个数
 * 更贴合布局；布局一改那个数会跟着变，这里写死的不会）。 */
#define FAB_DEFAULT_LEFT    false
#define FAB_YPCT_UNSET      (-1)

static volatile uint8_t s_left  = FAB_DEFAULT_LEFT;
static volatile int8_t  s_ypct  = FAB_YPCT_UNSET;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 幂等 NVS init，照 config_traffic.c:ensure_nvs */
static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

/* 两个 key 一次 open 写完：它们总是一起变的，分两次开句柄只是多一次
 * 提交，中间断电还会留下「换了边但高度是旧的」这种半截状态。 */
static void save_pos(uint8_t left, uint8_t ypct)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(FAB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, FAB_NVS_KEY_LEFT, left);
    if (err == ESP_OK) err = nvs_set_u8(h, FAB_NVS_KEY_YPCT, ypct);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save fab pos failed (%s)", esp_err_to_name(err));
    }
}

bool pk_fab_left(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t v = s_left;
    portEXIT_CRITICAL(&s_mux);
    return v != 0;
}

int pk_fab_y_pct(void)
{
    portENTER_CRITICAL(&s_mux);
    int8_t v = s_ypct;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void pk_fab_pos_set(bool left, int y_pct)
{
    if (y_pct < 0)   y_pct = 0;
    if (y_pct > 100) y_pct = 100;

    portENTER_CRITICAL(&s_mux);
    s_left = left ? 1 : 0;
    s_ypct = (int8_t)y_pct;
    portEXIT_CRITICAL(&s_mux);

    save_pos(left ? 1 : 0, (uint8_t)y_pct);
    ESP_LOGI(TAG, "fab pos -> %s %d%%", left ? "LEFT" : "RIGHT", y_pct);
}

void pk_config_fab_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(FAB_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no fab config (%s), using defaults", esp_err_to_name(err));
        s_left = FAB_DEFAULT_LEFT;
        s_ypct = FAB_YPCT_UNSET;
        return;
    }
    uint8_t l = FAB_DEFAULT_LEFT ? 1 : 0, y = 0;
    bool have_y = (nvs_get_u8(h, FAB_NVS_KEY_YPCT, &y) == ESP_OK);
    if (nvs_get_u8(h, FAB_NVS_KEY_LEFT, &l) != ESP_OK) l = FAB_DEFAULT_LEFT ? 1 : 0;
    nvs_close(h);

    s_left = (l != 0) ? 1 : 0;
    s_ypct = (have_y && y <= 100) ? (int8_t)y : FAB_YPCT_UNSET;
    ESP_LOGI(TAG, "fab pos: %s %s", s_left ? "LEFT" : "RIGHT",
             s_ypct < 0 ? "(default height)" : "restored");
}
