/*
 * config_qnh.c — QNH(修正海压)可调值 + NVS 持久化。
 *
 * NVS namespace: "pkbox"  key: "qnh"  类型: blob (float, 4 bytes)
 * 照 imu_task.c:719-748 的 blob 范式实现。
 */

#include "config_qnh.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "qnh";

#define QNH_NVS_NAMESPACE  "pk_qnh"
#define QNH_NVS_KEY        "qnh"

#define QNH_DEFAULT_HPA    1013.25f
#define QNH_MIN_HPA         950.0f
#define QNH_MAX_HPA        1050.0f

/* volatile:baro_task 读 / settings 写;float 32-bit 对齐,RISC-V 单指令读写原子 */
static volatile float s_qnh_hpa = QNH_DEFAULT_HPA;

/* 临界区锁:跨任务读写 s_qnh_hpa 的原子性防御 */
static portMUX_TYPE s_qnh_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── 内部:确保 NVS 已初始化(幂等;多次调用安全) ── */
static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    /* ESP_OK 或 ESP_ERR_INVALID_STATE(已初始化)均视为就绪,不做额外处理 */
}

/* ── 内部:钳制辅助 ── */
static float clamp_qnh(float v)
{
    if (v < QNH_MIN_HPA) return QNH_MIN_HPA;
    if (v > QNH_MAX_HPA) return QNH_MAX_HPA;
    return v;
}

/* ── 公共 API ── */

float pk_qnh_get(void)
{
    portENTER_CRITICAL(&s_qnh_mux);
    float v = s_qnh_hpa;
    portEXIT_CRITICAL(&s_qnh_mux);
    return v;
}

void pk_qnh_set(float hpa)
{
    float clamped = clamp_qnh(hpa);

    /* 临界区内只做内存写,NVS flash I/O 在临界区外 */
    portENTER_CRITICAL(&s_qnh_mux);
    s_qnh_hpa = clamped;
    portEXIT_CRITICAL(&s_qnh_mux);

    /* NVS 持久化:照 imu_task.c imu_nvs_save_tare 的 blob 范式 */
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(QNH_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    float v = clamped;
    err = nvs_set_blob(h, QNH_NVS_KEY, &v, sizeof(float));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_blob/commit failed (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "QNH -> %.2f hPa (saved)", clamped);
    }
}

void pk_qnh_load(void)
{
    /* 照 imu_task.c imu_nvs_load_tare 的 blob 范式 */
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(QNH_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no QNH namespace, using default %.2f hPa", QNH_DEFAULT_HPA);
        s_qnh_hpa = QNH_DEFAULT_HPA;
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s), using default %.2f hPa",
                 esp_err_to_name(err), QNH_DEFAULT_HPA);
        s_qnh_hpa = QNH_DEFAULT_HPA;
        return;
    }

    float v = QNH_DEFAULT_HPA;
    size_t len = sizeof(float);
    err = nvs_get_blob(h, QNH_NVS_KEY, &v, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no QNH key, using default %.2f hPa", QNH_DEFAULT_HPA);
        s_qnh_hpa = QNH_DEFAULT_HPA;
        return;
    }
    if (err != ESP_OK || len != sizeof(float)) {
        ESP_LOGW(TAG, "nvs_get_blob failed (%s), using default %.2f hPa",
                 esp_err_to_name(err), QNH_DEFAULT_HPA);
        s_qnh_hpa = QNH_DEFAULT_HPA;
        return;
    }

    /* 范围校验:损坏数据降级到默认值 */
    if (v < QNH_MIN_HPA || v > QNH_MAX_HPA) {
        ESP_LOGW(TAG, "QNH out of range (%.2f), using default %.2f hPa",
                 v, QNH_DEFAULT_HPA);
        s_qnh_hpa = QNH_DEFAULT_HPA;
        return;
    }

    s_qnh_hpa = v;
    ESP_LOGI(TAG, "QNH loaded: %.2f hPa", v);
}
