/*
 * config_diaglog.c — 见 config_diaglog.h。
 *
 * 结构照 config_antenna.c（volatile + portMUX + ensure_nvs + get/set/load）。
 * 文件清单集中在这里而不是散在各记录器里：以后再加记录器，只要往
 * DIAG_FILES 里加一行，"关闭即删除"和"清除"两条路径自动覆盖到它——
 * 靠每个模块自己记得实现删除，迟早漏掉一个，而漏掉的那个正是用户以为
 * 已经删了的。
 */
#include "config_diaglog.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pk_sdcard.h"

static const char *TAG = "diaglog";

#define DIAG_NVS_NAMESPACE  "pk_diag"
#define DIAG_NVS_KEY_ON     "enabled"

/* 所有会落到用户 SD 卡上的诊断文件。新增记录器时加在这里。 */
static const char *const DIAG_FILES[] = {
    "/sdcard/rf1090.csv",     /* 1090 接收链统计（rf1090_log） */
    "/sdcard/p4.log",         /* P4 自己的全部 ESP_LOG（pk_syslog） */
    "/sdcard/p4.old.log",     /* 同上，轮转保留的那一份 */
    "/sdcard/power.csv",      /* 电池长期记录（power_log） */
};
#define DIAG_FILE_N (sizeof DIAG_FILES / sizeof DIAG_FILES[0])

static volatile bool s_enabled;            /* 默认关闭，见头文件 */
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool pk_diaglog_enabled(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool v = s_enabled;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void pk_diaglog_clear_files(void)
{
    if (!pk_sdcard_io_acquire()) return;
    for (size_t i = 0; i < DIAG_FILE_N; i++) {
        if (unlink(DIAG_FILES[i]) == 0)
            ESP_LOGI(TAG, "已删除 %s", DIAG_FILES[i]);
    }
    pk_sdcard_io_release();
}

long pk_diaglog_size_bytes(void)
{
    if (!pk_sdcard_io_acquire()) return 0;
    long total = 0;
    struct stat st;
    for (size_t i = 0; i < DIAG_FILE_N; i++)
        if (stat(DIAG_FILES[i], &st) == 0) total += (long)st.st_size;
    pk_sdcard_io_release();
    return total;
}

void pk_diaglog_set(bool on)
{
    portENTER_CRITICAL(&s_mux);
    s_enabled = on;
    portEXIT_CRITICAL(&s_mux);

    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(DIAG_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, DIAG_NVS_KEY_ON, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }

    /* 关闭即删除。留着"开关是关的、数据还在卡上"这种状态，等于让用户以为
     * 自己清掉了而其实没有——那比不给开关更糟。 */
    if (!on) pk_diaglog_clear_files();

    ESP_LOGI(TAG, "诊断记录 -> %s%s", on ? "开" : "关",
             on ? "" : "（已删除已记录的文件）");
}

void pk_config_diaglog_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    uint8_t v = 0;                      /* 读不到 = 关，见头文件"默认关闭" */
    if (nvs_open(DIAG_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, DIAG_NVS_KEY_ON, &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    s_enabled = (v != 0);
    ESP_LOGI(TAG, "诊断记录: %s", s_enabled ? "开" : "关（默认）");
}
