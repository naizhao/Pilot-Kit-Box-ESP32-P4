/*
 * pk_sdcard.c — 板载 microSD (TF1, SDMMC 4-bit) 挂载/探测/格式化。
 *
 * 设计:
 *   - 单一 mutex 串行化 mount/unmount/format，探测任务与 UI 触发的
 *     格式化不会互踩。
 *   - 板上无 CD 脚（docs/hardware/board_pinout.md: "No card-detect pin
 *     to a P4 GPIO — detect via mount-retry"）：
 *       未挂载 → 每 3 s 重试 esp_vfs_fat_sdmmc_mount；
 *       已挂载 → 每 2 s sdmmc_get_status 探活，失败即视为拔卡卸载。
 *   - 无卡时 sdmmc 驱动层会刷错误日志，init 时把相关 TAG 静音，
 *     状态变化由本模块自己打一条简洁 INFO。
 */

#include "pk_sdcard.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "pk_sd";

#define SD_MOUNT_POINT   "/sdcard"
/* TF1 slot 引脚，docs/hardware/board_pinout.md:139-151 */
#define SD_PIN_CLK       43
#define SD_PIN_CMD       44
#define SD_PIN_D0        39
#define SD_PIN_D1        40
#define SD_PIN_D2        41
#define SD_PIN_D3        42

/* GPIO39-44 是 P4 SDMMC SLOT_0 的专用引脚（UHS-I capable）。SLOT_0 的
 * IO 电源 VDD_IO_5 由 P4 片上 LDO 通道 4 (LDO_VO4) 供给 —— 必须经
 * sd_pwr_ctrl_new_on_chip_ldo 上电，否则数据线无电、任何卡都探测不到。
 * Waveshare 官方例程 (esp32-p4-platform/examples/esp-idf/09_sdmmc) 同配。 */
#define SD_SLOT          SDMMC_HOST_SLOT_0
#define SD_LDO_CHAN      4

#define SD_PROBE_PERIOD_MS    3000   /* 未挂载时重试间隔 */
#define SD_ALIVE_PERIOD_MS    2000   /* 已挂载时探活间隔 */
#define SD_TASK_STACK         4096

static sdmmc_card_t          *s_card;
static volatile pk_sd_state_t s_state = PK_SD_NO_CARD;
static SemaphoreHandle_t      s_lock;

/* 容量缓存 — 由挂载点/探测任务刷新；pk_sdcard_info 只读缓存，
 * 这样诊断页每帧调用不会触发 FAT 扫描 I/O。 */
static volatile uint64_t s_total_bytes;
static volatile uint64_t s_free_bytes;

static void sd_refresh_info_locked(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_state == PK_SD_MOUNTED &&
        esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        s_total_bytes = total;
        s_free_bytes  = free_b;
    } else {
        s_total_bytes = 0;
        s_free_bytes  = 0;
    }
}

/* --- mount / unmount（调用方须持锁） ----------------------------------- */

/* SLOT_0 IO 电源（LDO_VO4）— init 时创建一次，常驻 */
static sd_pwr_ctrl_handle_t s_pwr_ctrl;

/* IDF ≥6.0 的 SDMMC 控制器只能被 init 一次（IDF issue #16233），而本板
 * ESP-Hosted 走 SDIO（C6 在 Slot 1）已经 init 过了 —— 再调 sdmmc_host_init
 * 会报 "no available sd host controller"。照抄 esp_hosted 官方示例
 * examples/host_sdcard_with_hosted 的 workaround：把 init/deinit 换成
 * 空函数，复用 hosted 持有的控制器，TF 卡只占 Slot 0。 */
static esp_err_t sdmmc_host_init_dummy(void)   { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }

static bool sd_mount_locked(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot   = SD_SLOT;
    host.init   = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
    host.pwr_ctrl_handle = s_pwr_ctrl;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   /* Waveshare 官方示例同配 */
    slot.width = 4;
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.d1    = SD_PIN_D1;
    slot.d2    = SD_PIN_D2;
    slot.d3    = SD_PIN_D3;
    /* 板上数据线/CMD 已有外部上拉；内部上拉按 Waveshare 官方示例叠加 */

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* 格式化只走用户显式操作 */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        /* 只在错误码变化时打一条，避免 3s 重试刷屏 */
        static esp_err_t s_last_err = ESP_OK;
        if (err != s_last_err) {
            ESP_LOGW(TAG, "mount attempt failed: %s", esp_err_to_name(err));
            s_last_err = err;
        }
        s_card = NULL;
        return false;
    }

    s_state = PK_SD_MOUNTED;
    sd_refresh_info_locked();
    ESP_LOGI(TAG, "microSD mounted at %s: %s %.1f GB",
             SD_MOUNT_POINT, s_card->cid.name,
             ((double)s_card->csd.capacity * s_card->csd.sector_size)
                 / (1024.0 * 1024.0 * 1024.0));
    return true;
}

static void sd_unmount_locked(void)
{
    if (s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    s_state = PK_SD_NO_CARD;
    s_total_bytes = 0;
    s_free_bytes  = 0;
}

/* --- 后台插拔探测 ------------------------------------------------------- */

static void sd_detect_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_state == PK_SD_MOUNTED) {
            vTaskDelay(pdMS_TO_TICKS(SD_ALIVE_PERIOD_MS));
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_state == PK_SD_MOUNTED && s_card != NULL) {
                if (sdmmc_get_status(s_card) != ESP_OK) {
                    ESP_LOGW(TAG, "microSD removed — unmounting");
                    sd_unmount_locked();
                } else {
                    sd_refresh_info_locked();
                }
            }
            xSemaphoreGive(s_lock);
        } else if (s_state == PK_SD_NO_CARD) {
            vTaskDelay(pdMS_TO_TICKS(SD_PROBE_PERIOD_MS));
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_state == PK_SD_NO_CARD) {
                (void)sd_mount_locked();
            }
            xSemaphoreGive(s_lock);
        } else {
            /* PK_SD_FORMATTING — 格式化期间不打扰 */
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* --- 公共 API ------------------------------------------------------------ */

void pk_sdcard_init(void)
{
    if (s_lock != NULL) return;   /* 幂等 */
    s_lock = xSemaphoreCreateMutex();

    /* 无卡时 sdmmc 协议层每轮重试都会刷 E/W 日志，静音掉；
     * 状态变化由本模块自己报。 */
    esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);

    /* SLOT_0 IO 供电：P4 片上 LDO 通道 4。失败则 SD 永远探测不到，
     * 打 ERROR 但不崩 —— 其余固件功能不受影响。 */
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN };
    esp_err_t perr = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "LDO%d power ctrl init failed: %s — microSD disabled",
                 SD_LDO_CHAN, esp_err_to_name(perr));
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!sd_mount_locked()) {
        ESP_LOGI(TAG, "no microSD card at boot (will keep probing)");
    }
    xSemaphoreGive(s_lock);

    xTaskCreatePinnedToCore(sd_detect_task, "sd_detect",
                            SD_TASK_STACK, NULL, 2, NULL, 0);
}

pk_sd_state_t pk_sdcard_state(void)
{
    return s_state;
}

bool pk_sdcard_is_mounted(void)
{
    return s_state == PK_SD_MOUNTED;
}

const char *pk_sdcard_mount_point(void)
{
    return SD_MOUNT_POINT;
}

bool pk_sdcard_info(uint64_t *out_total, uint64_t *out_free)
{
    /* 只读缓存（探测任务每 2s 刷新），渲染路径调用零 I/O。 */
    if (s_state != PK_SD_MOUNTED || s_total_bytes == 0) return false;
    if (out_total) *out_total = s_total_bytes;
    if (out_free)  *out_free  = s_free_bytes;
    return true;
}

esp_err_t pk_sdcard_format(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != PK_SD_MOUNTED || s_card == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_state = PK_SD_FORMATTING;
    ESP_LOGI(TAG, "formatting microSD (FAT32)…");
    esp_err_t err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_state = PK_SD_MOUNTED;
        sd_refresh_info_locked();
        ESP_LOGI(TAG, "format done");
    } else {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        sd_unmount_locked();   /* 状态未知，卸掉让探测任务重挂 */
    }
    xSemaphoreGive(s_lock);
    return err;
}
