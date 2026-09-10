/*
 * config_qnh.c — QNH(修正海压)可调值 + AUTO/MANUAL 模式,NVS 持久化。
 *
 * NVS namespace: "pk_qnh"  key: "qnh"(blob float) / "mode"(u8)
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
#define QNH_NVS_KEY_MODE   "mode"

#define QNH_DEFAULT_HPA    1013.25f
#define QNH_MIN_HPA         950.0f
#define QNH_MAX_HPA        1050.0f

/* volatile:baro_task 读/写(auto) / settings 写(manual);float 32-bit 对齐,
 * RISC-V 单指令读写原子 */
static volatile float s_qnh_hpa = QNH_DEFAULT_HPA;

/* 默认 AUTO:出厂/无 NVS 时气压高度自动贴合 GNSS,用户拨轮即转 MANUAL。 */
static volatile pk_qnh_mode_t s_mode = PK_QNH_MODE_AUTO;

/* 临界区锁:跨任务读写 s_qnh_hpa / s_mode 的原子性防御 */
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

pk_qnh_mode_t pk_qnh_mode_get(void)
{
    portENTER_CRITICAL(&s_qnh_mux);
    pk_qnh_mode_t m = s_mode;
    portEXIT_CRITICAL(&s_qnh_mux);
    return m;
}

/* 持久化一个 NVS 键(值 + 模式)。失败只告警,不影响 RAM 已生效的值。 */
static void persist(const char *key, const void *data, size_t len, const char *what)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(QNH_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs set %s failed (%s)", what, esp_err_to_name(err));
    }
}

void pk_qnh_set(float hpa)
{
    float clamped = clamp_qnh(hpa);

    portENTER_CRITICAL(&s_qnh_mux);
    s_qnh_hpa = clamped;
    s_mode    = PK_QNH_MODE_MANUAL;   /* 手动拨轮即接管,auto 不再覆盖 */
    portEXIT_CRITICAL(&s_qnh_mux);

    persist(QNH_NVS_KEY, &clamped, sizeof(float), "qnh");
    uint8_t m = (uint8_t)PK_QNH_MODE_MANUAL;
    persist(QNH_NVS_KEY_MODE, &m, sizeof(m), "mode");
    ESP_LOGI(TAG, "QNH -> %.2f hPa (manual, saved)", clamped);
}

void pk_qnh_set_auto(float hpa)
{
    float clamped = clamp_qnh(hpa);
    portENTER_CRITICAL(&s_qnh_mux);
    s_qnh_hpa = clamped;
    s_mode    = PK_QNH_MODE_AUTO;
    portEXIT_CRITICAL(&s_qnh_mux);
    /* 不写 NVS:auto 每拍都在动。模式本身在切到 AUTO 时由 pk_qnh_set_mode 持久化。 */
}

void pk_qnh_set_mode(pk_qnh_mode_t mode)
{
    portENTER_CRITICAL(&s_qnh_mux);
    s_mode = mode;
    portEXIT_CRITICAL(&s_qnh_mux);
    uint8_t m = (uint8_t)mode;
    persist(QNH_NVS_KEY_MODE, &m, sizeof(m), "mode");
    ESP_LOGI(TAG, "QNH mode -> %s", mode == PK_QNH_MODE_AUTO ? "AUTO" : "MANUAL");
}

void pk_qnh_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    esp_err_t err = nvs_open(QNH_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no QNH namespace, using default %.2f hPa / AUTO", QNH_DEFAULT_HPA);
        s_qnh_hpa = QNH_DEFAULT_HPA;
        s_mode    = PK_QNH_MODE_AUTO;
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s), using default %.2f hPa / AUTO",
                 esp_err_to_name(err), QNH_DEFAULT_HPA);
        s_qnh_hpa = QNH_DEFAULT_HPA;
        s_mode    = PK_QNH_MODE_AUTO;
        return;
    }

    float v = QNH_DEFAULT_HPA;
    size_t len = sizeof(float);
    err = nvs_get_blob(h, QNH_NVS_KEY, &v, &len);
    if (err == ESP_OK && len == sizeof(float) &&
        v >= QNH_MIN_HPA && v <= QNH_MAX_HPA) {
        s_qnh_hpa = v;
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "QNH blob bad (%s/len=%u), using default %.2f hPa",
                     esp_err_to_name(err), (unsigned)len, QNH_DEFAULT_HPA);
        }
        s_qnh_hpa = QNH_DEFAULT_HPA;
    }

    uint8_t m = (uint8_t)PK_QNH_MODE_AUTO;
    size_t mlen = sizeof(m);
    if (nvs_get_blob(h, QNH_NVS_KEY_MODE, &m, &mlen) == ESP_OK && mlen == sizeof(m) &&
        (m == (uint8_t)PK_QNH_MODE_AUTO || m == (uint8_t)PK_QNH_MODE_MANUAL)) {
        s_mode = (pk_qnh_mode_t)m;
    } else {
        s_mode = PK_QNH_MODE_AUTO;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "QNH loaded: %.2f hPa / %s", s_qnh_hpa,
             s_mode == PK_QNH_MODE_AUTO ? "AUTO" : "MANUAL");
}
