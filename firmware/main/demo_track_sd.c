/*
 * demo_track_sd.c —— 见 demo_track_sd.h。
 *
 * 本文件只做三件事：等卡、挑文件、把 demo_gpx.c 的结果发布给 demo_track.c。
 * 所有解析与清洗逻辑都在 demo_gpx.c 里（那份不依赖 ESP-IDF，好让 host 单测
 * 与 Python 对拍都能直接编）。这里一行数值计算都不许有，否则就又多出一个
 * "只有真机上跑得到"的分支。
 */

#include "demo_track_sd.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "demo_gpx.h"
#include "demo_track.h"
#include "pk_sdcard.h"

static const char *TAG = "pk_demo_sd";

/* 等挂载：每 2 s 一次，最多 60 s。
 *
 * 上界不是"以防万一"：加载任务成功与否都要退出，不能变成一个为了演示功能
 * 常驻的任务。60 s 足够覆盖"开机后才想起来把卡插上"，超过就是用户根本没打算
 * 用自定义轨迹——那时内置轨迹早已在播，什么都不缺。 */
#define WAIT_POLL_MS      2000
#define WAIT_TOTAL_MS    60000

/* PSRAM 分配钩子。轨迹表要常驻，原始点数组峰值 300+ KB，都不该占内部 RAM。 */
static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    /* PSRAM 要不到就退内部 RAM：小文件（几十点的降落片段）几 KB 而已，
     * 为它直接放弃自定义轨迹不值当。大文件在内部 RAM 上会失败，那时才回落
     * 内置轨迹，也是对的。 */
    return p ? p : malloc(n);
}

static void psram_free(void *p) { heap_caps_free(p); }

static const pk_demo_gpx_alloc_t PSRAM_HOOKS = {
    .alloc = psram_alloc,
    .release = psram_free,
};

static void demo_sd_task(void *arg)
{
    (void)arg;

    int waited = 0;
    while (!pk_sdcard_is_mounted()) {
        if (waited >= WAIT_TOTAL_MS) {
            ESP_LOGI(TAG, "no SD after %d s — using built-in demo track",
                     WAIT_TOTAL_MS / 1000);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
        waited += WAIT_POLL_MS;
    }

    static char names[PK_DEMO_GPX_LIST_MAX][PK_DEMO_GPX_NAME_MAX];
    const int n = pk_demo_gpx_list_dir(PK_DEMO_SD_DIR, names, PK_DEMO_GPX_LIST_MAX);
    if (n <= 0) {
        /* 目录不存在与目录空着在这里没必要分开——用户要做的事都是"往
         * /sdcard/demo 里拷一个 .gpx"，日志把路径写全就够了。 */
        ESP_LOGI(TAG, "no *.gpx in %s — using built-in demo track", PK_DEMO_SD_DIR);
        vTaskDelete(NULL);
        return;
    }
    for (int i = 0; i < n; ++i) {
        ESP_LOGI(TAG, "found %s/%s%s", PK_DEMO_SD_DIR, names[i],
                 (i == 0) ? "  <= will play" : "");
    }
    if (n > 1) {
        ESP_LOGW(TAG, "%d GPX files present — playing the first by name order; "
                      "rename to control which one", n);
    }

    char path[sizeof(PK_DEMO_SD_DIR) + 1 + PK_DEMO_GPX_NAME_MAX];
    snprintf(path, sizeof(path), "%s/%s", PK_DEMO_SD_DIR, names[0]);

    const int64_t t0 = esp_timer_get_time();
    pk_demo_gpx_result_t res;
    const char *err = NULL;
    if (!pk_demo_gpx_load_file(path, 0, &PSRAM_HOOKS, &res, &err)) {
        ESP_LOGW(TAG, "parse %s failed: %s — falling back to built-in demo track",
                 path, err ? err : "?");
        vTaskDelete(NULL);
        return;
    }
    const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;

    if (res.truncated) {
        ESP_LOGW(TAG, "%s truncated at %u raw points — tail of the flight ignored",
                 names[0], (unsigned)PK_DEMO_GPX_MAX_RAW_DEFAULT);
    }

    /* 来源结构体也走 PSRAM 并且**永不释放**：发布之后随时可能被回放任务读到，
     * 没有任何时刻能证明"已经没人在读了"。一次 12 B 的永久占用，比引入引用
     * 计数或 RCU 便宜得多。 */
    pk_demo_track_src_t *src =
        (pk_demo_track_src_t *)psram_alloc(sizeof(pk_demo_track_src_t));
    if (!src) {
        ESP_LOGW(TAG, "out of memory publishing track — using built-in");
        psram_free(res.pts);
        vTaskDelete(NULL);
        return;
    }
    src->pts   = res.pts;
    src->n     = res.n;
    src->dur_s = res.dur_s;
    pk_demo_track_use(src);        /* 先填满再发布，见 demo_track.h 的并发说明 */

    ESP_LOGI(TAG, "demo track := %s  (%u raw -> %u pts, %u B, %u s real, %lld ms)",
             names[0], (unsigned)res.raw_n, (unsigned)res.n,
             (unsigned)(res.n * sizeof(pk_demo_track_pt_t)),
             (unsigned)res.dur_s, (long long)dt_ms);

    vTaskDelete(NULL);
}

void pk_demo_track_sd_init(void)
{
    /* 栈 5 KB：解析用的 7 KB 缓冲在堆上（demo_gpx.c 的 scan_buf_t），栈上只有
     * strtod/sscanf 的帧。优先级 1（比 UI 与采样都低）——它是开机后的一次性
     * 后台活，抢不到 CPU 就晚一点切轨迹，没有任何东西等着它。 */
    const BaseType_t ok = xTaskCreatePinnedToCore(
        demo_sd_task, "demo_sd", 5120, NULL, 1, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "task create failed — using built-in demo track");
    }
}
