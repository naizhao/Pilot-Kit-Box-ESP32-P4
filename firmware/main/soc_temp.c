/*
 * soc_temp.c — ESP32-P4 片内温度传感器 → 顶栏过热告警。
 *
 * 为什么不能用气压计那个温度
 * --------------------------
 * baro_task.c 读的是 BMP388 的**环境**温度（那颗芯片贴在扩展板上，测的是
 * 座舱空气）。这里要判断的是「设备自己在过热」，两者在暴晒的座舱里可以差
 * 二三十度——环境 45 ℃ 而 SoC 结温 80 ℃ 是常态。用环境温度当过热判据，
 * 该报警的时候不会报。
 *
 * 阈值是**代理指标**，需要真机热测标定
 * ------------------------------------
 * docs/ux/box-4.3-ux-spec.md §335 记的是「目标屏工作温度上限 70 ℃」，而本
 * 传感器测的是 SoC 结温，两者不是一个位置：结温总是高于屏温，高多少取决于
 * 外壳、风道与当时的负载，只能实测。
 *
 * 所以这里不假装能算出屏温，而是把结温当作「整机热状态」的代理量，默认阈值
 * 取 85 ℃ 触发 / 78 ℃ 解除：
 *   - 85 ℃ 对 ESP32-P4 自身仍在安全区（远低于结温上限），不会误报成故障；
 *   - 但机内能到 85 ℃，说明散热已经明显吃紧，屏幕大概率正在逼近它的 70 ℃。
 * 真机做过暴晒/满载热测之后，用 CONFIG_PK_SOC_TEMP_WARN_C 调到实测对应值。
 *
 * 7 ℃ 的滞回是为了别在阈值上抖：顶栏那一格出现又消失，比一直不出现更糟——
 * 会训练飞行员忽略它。
 */
#include "soc_temp.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "soc_temp";

#define TEMP_WARN_ON_C    CONFIG_PK_SOC_TEMP_WARN_C
#define TEMP_WARN_OFF_C   (CONFIG_PK_SOC_TEMP_WARN_C - 7)

/* 采样间隔。温度是慢变量，1 Hz 远快于它的变化率；渲染每帧都问一次，靠这个
 * 间隔挡住，不让 30 fps 的绘制把传感器读成热点。 */
#define TEMP_SAMPLE_US    (1000 * 1000)

static temperature_sensor_handle_t s_ts;
static int64_t s_last_us;
static float   s_last_c;
static bool    s_warn;

esp_err_t pk_soc_temp_init(void)
{
    /*
     * 量程必须**完整落在某一个硬件档位内**，不能自己拼一个跨档的区间——
     * 驱动是逐档比对 range_min/range_max 包含关系的（temperature_sensor.c:67），
     * 匹配不上就报 "Out of testing range"。P4 的档位表见
     * esp_hal_ana_conv/esp32p4/temperature_sensor_periph.c：
     *
     *     [ 50, 125] ±3     [ 20, 100] ±2     [-10,  80] ±1
     *     [-30,  50] ±2     [-40,  20] ±3
     *
     * 取 [20, 100]：告警点 85 ℃ 落在里面（[-10,80] 那档精度更高但读不到
     * 告警点，等于白装），误差 ±2 ℃ 对「热不热」这个判断绰绰有余。
     *
     * 低于 20 ℃ 读数不可信，但那只会出现在冷启动最初几秒——芯片一跑起来
     * 结温就上到 40 ℃ 以上，而那几秒里不可能过热，读不准无害。
     *
     * 初版写的 (-10, 110) 跨了档，真机上直接 install failed: INVALID_ARG。
     */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);

    esp_err_t err = temperature_sensor_install(&cfg, &s_ts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = temperature_sensor_enable(s_ts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable failed: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_ts);
        s_ts = NULL;
        return err;
    }
    ESP_LOGI(TAG, "SoC temp sensor up, warn at %d C (clears at %d C)",
             TEMP_WARN_ON_C, TEMP_WARN_OFF_C);
    return ESP_OK;
}

bool pk_soc_temp_get(int *temp_c)
{
    if (s_ts == NULL) return false;

    int64_t now = esp_timer_get_time();
    if (s_last_us == 0 || now - s_last_us >= TEMP_SAMPLE_US) {
        float c;
        if (temperature_sensor_get_celsius(s_ts, &c) == ESP_OK) {
            s_last_c  = c;
            s_last_us = now;

            /* 滞回：升过 ON 才点亮，降到 OFF 以下才熄灭。 */
            if (!s_warn && c >= (float)TEMP_WARN_ON_C) {
                s_warn = true;
                ESP_LOGW(TAG, "over temperature: %.1f C", c);
            } else if (s_warn && c <= (float)TEMP_WARN_OFF_C) {
                s_warn = false;
                ESP_LOGI(TAG, "temperature back to normal: %.1f C", c);
            }
        }
    }

    if (temp_c) {
        /* 四舍五入而不是截断：79.6 ℃ 显示成 79 会让人以为还差得远。 */
        *temp_c = (int)(s_last_c >= 0 ? s_last_c + 0.5f : s_last_c - 0.5f);
    }
    return s_warn;
}
