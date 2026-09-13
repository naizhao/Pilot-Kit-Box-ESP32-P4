/*
 * config_antenna.c — 见 config_antenna.h。
 *
 * 结构照 config_demo.c（volatile + portMUX + ensure_nvs + get/set/load），
 * NVS 类型 u8、两个 key 一个 namespace。与那边的实质差别只有一点：set 之后
 * 还要把配置推给 RP2040，因为真正接天线的开关在它那一侧。
 */
#include "config_antenna.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "adsb_link.h"
#include "adsb_link_task.h"

static const char *TAG = "cfg_ant";

#define ANT_NVS_NAMESPACE  "pk_ant"
#define ANT_NVS_KEY_1090   "ant1090"
#define ANT_NVS_KEY_GNSS   "antgnss"
#define ANT_NVS_KEY_THRESH "th1090"

/* 默认 0/0 = 开机安全默认（板载 IFA / 外接 J2），与 rf_safety 的上电向量
 * 同一个状态——见头文件里为什么这条对齐是必需的。 */
static volatile uint8_t s_ant_1090;
static volatile uint8_t s_ant_gnss;
/* 门限按**协议编码值**存（0=用固件默认，1..100=10..1000‰），不存 permille：
 * 存原始值就得在 NVS、payload 两处各维护一次标度换算，迟早漂。 */
static volatile uint8_t s_thresh_val;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 幂等 NVS init，照 config_ble.c:ensure_nvs */
static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

pk_ant_1090_t pk_ant_1090_get(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t v = s_ant_1090;
    portEXIT_CRITICAL(&s_mux);
    return v ? PK_ANT_1090_EXTERNAL : PK_ANT_1090_ONBOARD;
}

pk_ant_gnss_t pk_ant_gnss_get(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t v = s_ant_gnss;
    portEXIT_CRITICAL(&s_mux);
    return v ? PK_ANT_GNSS_ONBOARD : PK_ANT_GNSS_EXTERNAL;
}

static void save_u8(const char *key, uint8_t v)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(ANT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "save %s failed (%s)", key, esp_err_to_name(err));
}

void pk_ant_1090_set(pk_ant_1090_t sel)
{
    const uint8_t v = (sel == PK_ANT_1090_EXTERNAL) ? 1 : 0;
    portENTER_CRITICAL(&s_mux);
    s_ant_1090 = v;
    portEXIT_CRITICAL(&s_mux);
    save_u8(ANT_NVS_KEY_1090, v);
    ESP_LOGI(TAG, "1090 antenna -> %s", v ? "EXTERNAL J6" : "ONBOARD IFA");
    pk_adsb_link_push_config();
}

void pk_ant_gnss_set(pk_ant_gnss_t sel)
{
    const uint8_t v = (sel == PK_ANT_GNSS_ONBOARD) ? 1 : 0;
    portENTER_CRITICAL(&s_mux);
    s_ant_gnss = v;
    portEXIT_CRITICAL(&s_mux);
    save_u8(ANT_NVS_KEY_GNSS, v);
    ESP_LOGI(TAG, "GNSS antenna -> %s", v ? "ONBOARD patch J8" : "EXTERNAL J2");
    pk_adsb_link_push_config();
}

int pk_thresh_1090_get(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t v = s_thresh_val;
    portEXIT_CRITICAL(&s_mux);
    const int pml = adsb_link_threshold_from_val(v);
    return pml < 0 ? 0 : pml;        /* 存坏了就退回"用固件默认" */
}

void pk_thresh_1090_set(int permille)
{
    const uint8_t v = (permille <= 0) ? 0
                                      : adsb_link_threshold_to_val(permille);
    portENTER_CRITICAL(&s_mux);
    s_thresh_val = v;
    portEXIT_CRITICAL(&s_mux);
    save_u8(ANT_NVS_KEY_THRESH, v);
    ESP_LOGI(TAG, "1090 threshold -> %d permille%s",
             v ? v * 10 : 0, v ? "" : " (RP2040 固件默认)");
    pk_adsb_link_push_config();
}

void pk_config_antenna_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(ANT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_ant_1090 = 0; s_ant_gnss = 0;      /* 没存过 → 开机安全默认 */
        return;
    }
    uint8_t a = 0, g = 0, t = 0;
    if (nvs_get_u8(h, ANT_NVS_KEY_1090, &a) != ESP_OK) a = 0;
    if (nvs_get_u8(h, ANT_NVS_KEY_GNSS, &g) != ESP_OK) g = 0;
    if (nvs_get_u8(h, ANT_NVS_KEY_THRESH, &t) != ESP_OK) t = 0;
    nvs_close(h);
    s_ant_1090 = a ? 1 : 0;
    s_ant_gnss = g ? 1 : 0;
    /* 存坏的值不要带进运行期：越界一律退回 0（用固件默认），否则会把一个
     * 无效门限一路下发过去，而门限错了的症状是"一个包都收不到"。 */
    s_thresh_val = (adsb_link_threshold_from_val(t) < 0) ? 0 : t;
    ESP_LOGI(TAG, "antenna: 1090=%s gnss=%s thresh=%d permille",
             s_ant_1090 ? "EXTERNAL J6" : "ONBOARD IFA",
             s_ant_gnss ? "ONBOARD patch J8" : "EXTERNAL J2",
             s_thresh_val * 10);
}

size_t pk_antenna_build_config_payload(uint8_t *out, size_t cap)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t th = s_thresh_val;
    portEXIT_CRITICAL(&s_mux);
    /* 门限跟着天线一起走这条路径，握手时才会被一并重推——RP2040 不持久化
     * 配置，漏掉它的话调好的门限活不过对面一次重启。 */
    const adsb_link_cfg_item_t items[3] = {
        { ADSB_LINK_CFG_ANT_1090,  (uint8_t)pk_ant_1090_get() },
        { ADSB_LINK_CFG_ANT_GNSS,  (uint8_t)pk_ant_gnss_get() },
        { ADSB_LINK_CFG_THRESHOLD, th },
    };
    return adsb_link_config_encode(out, cap, items, 3);
}
