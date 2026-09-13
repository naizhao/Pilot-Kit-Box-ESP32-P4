#include "pk_clock_persist.h"

#include <stdint.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "pk_clock.h"

static const char *TAG = "clk_persist";

#define CLK_NVS_NAMESPACE  "pk_clock"
#define CLK_NVS_KEY        "last_ms"

/*
 * 写入周期。
 *
 * 这个值**不影响恢复精度**——恢复出来的时间误差由"关机了多久"主导，而那个
 * 量无从知道，写得再密也补不回来。它真正决定的是 flash 写入次数：10 分钟一次
 * 是一天 144 次、一年约 5 万次，对带磨损均衡的 NVS 是很轻的负载。
 *
 * 所以不要为了"更准"去调小它——收益是零，代价是 flash 寿命。
 */
#define CLK_PERSIST_INTERVAL_MS  (10 * 60 * 1000)

/* 首次写入不等满一个周期：开机几分钟内断电是常事（插拔、试机、换位置），
 * 等满 10 分钟等于那段时间白跑一次。30 s 足够 GPS 拿到 fix 或蓝牙连上。 */
#define CLK_PERSIST_FIRST_MS     (30 * 1000)

/* 任务栈：只做一次 nvs_open/set/commit，3 KB 足够（同类的 config_*.c 都在
 * 调用方上下文里做同样的事）。栈走堆分配，不占开机前那 65 KB 内部 .bss 预算。 */
#define CLK_PERSIST_STACK  3072

static bool read_back(int64_t *out_ms)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(CLK_NVS_NAMESPACE, NVS_READONLY, &h);
    if (e != ESP_OK) {
        /* 首次开机时命名空间还不存在，这是正常的，不是故障。 */
        ESP_LOGD(TAG, "nvs_open(ro): %s", esp_err_to_name(e));
        return false;
    }
    int64_t v = 0;
    e = nvs_get_i64(h, CLK_NVS_KEY, &v);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "nvs_get_i64: %s", esp_err_to_name(e));
        return false;
    }
    *out_ms = v;
    return true;
}

static void write_now(void)
{
    /* 没被任何来源校准过的时钟不值得存：那是开机至今的 uptime，存下去只会在
     * 下次开机被 pk_clock 的 2024 下限挡掉，白写一次 flash。 */
    if (!pk_clock_is_synced()) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t ms = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;

    nvs_handle_t h;
    esp_err_t e = nvs_open(CLK_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "nvs_open(rw): %s", esp_err_to_name(e)); return; }
    e = nvs_set_i64(h, CLK_NVS_KEY, ms);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "persist failed: %s", esp_err_to_name(e));
    else             ESP_LOGD(TAG, "persisted epoch_ms=%lld", (long long)ms);
}

static void persist_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CLK_PERSIST_FIRST_MS));
    write_now();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CLK_PERSIST_INTERVAL_MS));
        write_now();
    }
}

void pk_clock_persist_start(void)
{
    int64_t saved_ms = 0;
    if (read_back(&saved_ms)) {
        /* 交给 pk_clock 统一判定：2024 下限、可信度阶梯都在那一处，
         * 这里不自己复制一份判断（复制出来的那份迟早跟本体分叉）。 */
        if (pk_clock_apply_epoch_ms(saved_ms, "nvs")) {
            ESP_LOGI(TAG, "restored clock from NVS: epoch_ms=%lld "
                          "(偏早未知时长——关机期间无法计时，仅作兜底)",
                     (long long)saved_ms);
        } else {
            ESP_LOGW(TAG, "NVS value rejected by pk_clock: epoch_ms=%lld",
                     (long long)saved_ms);
        }
    } else {
        ESP_LOGI(TAG, "no saved clock in NVS (首次开机或已擦除)");
    }

    BaseType_t ok = xTaskCreate(persist_task, "clk_persist", CLK_PERSIST_STACK,
                                NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) ESP_LOGW(TAG, "persist task create failed — 时间将不再落盘");
}
