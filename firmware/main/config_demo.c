/*
 * config_demo.c — 见 config_demo.h。
 *
 * 结构照 config_ble.c（volatile + portMUX + ensure_nvs + get/set/load），
 * NVS 类型 u8。唯一的实质差别是**默认值取 0**，以及 set 里那条 WARN 级日志：
 * 这是个危险状态，开启动作本身就该在串口上留痕。
 */
#include "config_demo.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg_demo";

#define DEMO_NVS_NAMESPACE  "pk_demo"
#define DEMO_NVS_KEY        "demo_on"

static volatile uint8_t s_on;          /* 默认关——见头文件第 1 条约束 */
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

bool pk_demo_enabled(void)
{
    /* 每帧、每个数据源 getter 都会问它，所以这里不能有 NVS 或阻塞操作。
     * 单字节读用临界区而不是原子读，是为了与 set 的写配对——portMUX 在 P4
     * 上是自旋锁，开销约几十个周期，相对每帧几十毫秒的渲染可以忽略。 */
    portENTER_CRITICAL(&s_mux);
    const uint8_t v = s_on;
    portEXIT_CRITICAL(&s_mux);
    return v != 0;
}

void pk_demo_set_enabled(bool on)
{
    const uint8_t v = on ? 1 : 0;
    portENTER_CRITICAL(&s_mux);
    s_on = v;
    portEXIT_CRITICAL(&s_mux);

    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(DEMO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, DEMO_NVS_KEY, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "save failed (%s)", esp_err_to_name(err));

    /* 开启走 WARN、关闭走 INFO：排障时第一件事是 grep 日志里的 W，演示模式
     * 开着而用户以为在看真数据，是最值得一眼看到的一种"配置问题"。 */
    if (v) ESP_LOGW(TAG, "DEMO MODE ENABLED — all flight data is SIMULATED, "
                         "GDL90 traffic output is suppressed");
    else   ESP_LOGI(TAG, "demo mode disabled — real sensors restored");
}

void pk_config_demo_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(DEMO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_on = 0;                       /* 没存过 → 关 */
        return;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, DEMO_NVS_KEY, &v) != ESP_OK) v = 0;
    nvs_close(h);
    s_on = v ? 1 : 0;
    if (s_on) {
        ESP_LOGW(TAG, "booting with DEMO MODE still ENABLED (persisted) — "
                      "all flight data is SIMULATED");
    }
}
