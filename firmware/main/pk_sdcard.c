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
/* 累计挂载尝试次数。诊断页用它区分"没插卡"与"插了但挂不上"——两者在
 * 状态上都是 PK_SD_NO_CARD，但前者计数不动、后者每 3 s 涨一次。 */
static volatile uint32_t s_mount_attempts;
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

/*
 * 卡槽上电。拔卡时会被 sd_unmount_locked() 拆掉，重挂前再建一次。
 *
 * 为什么要拆了重建：热插拔插入后一直挂不上（2026-07-29 罩哥实测），而**开机
 * 挂载是成功的**（日志 "microSD mounted at /sdcard: SL32G 29.7 GB"）——同一
 * 套引脚和 LDO 配置，区别只在于开机那次卡是从**冷态**开始的。
 *
 * unmount 走的 esp_vfs_fat_sdcard_unmount() 只会调 host.deinit()，而这里的
 * deinit 是 dummy（ESP-Hosted 占着同一个 SDMMC 控制器，不能真 deinit，
 * 见 project_sd_slot0_hosted_workaround）。于是电源一直没断，新插入的卡跳不
 * 回 idle 状态，后续 CMD0/CMD8 自然谈不拢。
 *
 * 拆掉 LDO 句柄 = 给卡断电，重建 = 重新上电，等于把冷启动那条路再走一遍。
 */
static bool sd_power_on_locked(void)
{
    if (s_pwr_ctrl != NULL) return true;
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO chan %d power-on failed: %s",
                 SD_LDO_CHAN, esp_err_to_name(err));
        s_pwr_ctrl = NULL;
        return false;
    }
    return true;
}

static void sd_power_off_locked(void)
{
    if (s_pwr_ctrl == NULL) return;
    sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
    s_pwr_ctrl = NULL;
}

static bool sd_mount_locked(void)
{
    if (!sd_power_on_locked()) return false;

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
        .max_files              = 8,       /* 原 4：地图页要同时开多个 .pmtiles 包 */
        .allocation_unit_size   = 16 * 1024,
    };

    s_mount_attempts++;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        /* 每次都打，带尝试序号。
         *
         * 原先"只在错误码变化时打一条"是为了防 3 s 重试刷屏，代价是**热插拔
         * 失败时日志里一条都看不到**——2026-07-29 排查"插卡后一直 retry 挂不
         * 上"时，抓了 20 s 串口一行 SD 日志都没有，因为错误码从开机起就没变
         * 过。3 s 一条的频率完全可接受，可见性比省日志重要。 */
        ESP_LOGW(TAG, "mount attempt #%lu failed: %s",
                 (unsigned long)s_mount_attempts, esp_err_to_name(err));
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

/* 「卸载前静默」回调表。固定槽位够用：目前只有 tile_loader 与 aero_db
 * 两个 SD 消费方（record_sink_file 是上轮修复前就有的旧句柄，不在本轮
 * 回归范围）。注册通常发生在 init 期，但探测任务已在跑，写表要持锁。 */
#define SD_PRE_UNMOUNT_CB_MAX 4
static void (*s_pre_unmount_cb[SD_PRE_UNMOUNT_CB_MAX])(void);
static int s_pre_unmount_cb_n;

static void sd_unmount_locked(void)
{
    /*
     * 顺序要点：先翻状态，再让上层静默，最后才真正 unmount。
     *
     * 1) 状态先翻成 NO_CARD——aero 的分块加载每 64 KB 轮询一次
     *    pk_sdcard_is_mounted()、loader 取件前也查，翻早一步它们立即停发
     *    新 I/O。原先状态翻转在 unmount **之后**，上层看到的永远太晚。
     * 2) 依次调回调：各模块以自己的锁为栅栏，等在途 SD 读退出并 fclose
     *    自己的句柄。这一步之后系统里没有打开的 SD fd、没有在途 I/O，
     *    esp_vfs_fat_sdcard_unmount() 的无条件 free(fat_ctx) 才不会变成
     *    use-after-free（IDF 的 vfs_fat_close 即使卡已不在、f_close 报错
     *    也会无条件释放 FIL 槽与 fd，所以「卸载前 fclose」能可靠清空句柄）。
     */
    s_state = PK_SD_NO_CARD;
    s_total_bytes = 0;
    s_free_bytes  = 0;
    for (int i = 0; i < s_pre_unmount_cb_n; i++) {
        s_pre_unmount_cb[i]();
    }

    if (s_card != NULL) {
        /*
         * 必须检查返回值。
         *
         * 卡被物理拔出后，esp_vfs_fat_sdcard_unmount() 往往失败——它要访问
         * 已经不在的卡去 flush/close。失败时**VFS 挂载点不会被注销**，于是
         * 后面每一次 esp_vfs_fat_sdmmc_mount() 都直接返回
         * ESP_ERR_INVALID_STATE（"这个路径已经挂了"），怎么重试都没用。
         *
         * 实测就是这个：插卡后日志一路 "mount attempt #24..#29 failed:
         * ESP_ERR_INVALID_STATE"，而开机那次好好的——因为开机时挂载点本来
         * 就是干净的。
         *
         * 所以失败要兜底：直接注销路径，把 VFS 恢复到可再挂的状态。
         */
        esp_err_t uerr = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        if (uerr != ESP_OK) {
            ESP_LOGW(TAG, "unmount failed (%s) — force-unregistering %s",
                     esp_err_to_name(uerr), SD_MOUNT_POINT);
            esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
        }
        s_card = NULL;

        /*
         * 注销 slot——这是 dummy deinit 漏掉的那一半，也是热插拔挂不上的真因。
         *
         * host.deinit 被换成 dummy 是为了不去动 ESP-Hosted 共用的那个 SDMMC
         * **控制器**（project_sd_slot0_hosted_workaround）。但 slot 是控制器
         * 之下的一层：mount 时 esp_vfs_fat_sdmmc_sdcard_init() 会调
         * init_sdmmc_host() 注册 slot，而 dummy deinit 什么都不做，于是 slot
         * 一直停在"已注册"。下一次 mount 走到同一处就被
         * sd_host_sdmmc.c:156 的 `ESP_GOTO_ON_FALSE(slot_available, ...)`
         * 挡下，返回 ESP_ERR_INVALID_STATE——实测日志里那串
         * "mount attempt #77..#81 failed: ESP_ERR_INVALID_STATE" 就是它。
         *
         * sdmmc_host_deinit_slot() 只注销 slot、不碰控制器，正好是我们要的
         * 粒度：Hosted 的 SDIO 仍在 slot 1 上跑，互不影响。
         */
        esp_err_t derr = sdmmc_host_deinit_slot(SD_SLOT);
        if (derr != ESP_OK && derr != ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "deinit_slot(%d): %s", SD_SLOT, esp_err_to_name(derr));

        /* 断电，让下一张卡能从冷态开始。 */
        sd_power_off_locked();
    }
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
    /* 这几路在正常运行时会刷屏，故默认静音。但热插拔挂不上时它们正是唯一的
     * 线索来源（卡的 CID/CSD、命令超时、时钟协商都在这里报），所以留一个
     * 编译开关：排障时打开重烧一次即可，不必回头翻代码找是哪几个 tag。 */
#if CONFIG_PK_SDMMC_VERBOSE
    /* 开着它跑正常业务 = 自废武功：115200 波特下这些 DEBUG 行能占满串口，
     * ESP_LOG 在 TX 满时阻塞调用方，SD 驱动被自己的日志堵死——2026-08-01
     * 实测地图瓦片一次 28KB 读盘要 23 秒（94% 的串口流量是 SD_HOST）。
     * 排障完务必关回去，所以这里响一声，别再让它悄悄留在 sdkconfig 里。 */
    ESP_LOGW(TAG, "PK_SDMMC_VERBOSE 已开启：SD 吞吐会塌到几乎不可用，仅供排障！");
    esp_log_level_set("sdmmc_req", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_DEBUG);
    esp_log_level_set("SD_HOST", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_periph", ESP_LOG_DEBUG);
#else
    esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);
#endif

    /* SLOT_0 IO 供电：P4 片上 LDO 通道 4。失败则 SD 永远探测不到，
     * 打 ERROR 但不崩 —— 其余固件功能不受影响。 */
/* 上电交给 sd_power_on_locked()（mount 内部会调），这里不再单独建句柄——
     * 两处各建一次的话，热插拔断电后 init 那份就成了悬空引用。 */

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

void pk_sdcard_register_pre_unmount_cb(void (*cb)(void))
{
    if (cb == NULL) return;
    /* 探测任务可能正持锁跑卸载序列，写表必须与之互斥；调用方按约定在
     * pk_sdcard_init() 之后才注册，s_lock 此时必然已建。 */
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "register_pre_unmount_cb before init — dropped");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pre_unmount_cb_n < SD_PRE_UNMOUNT_CB_MAX) {
        s_pre_unmount_cb[s_pre_unmount_cb_n++] = cb;
    } else {
        ESP_LOGE(TAG, "pre-unmount cb table full (%d) — dropped",
                 SD_PRE_UNMOUNT_CB_MAX);
    }
    xSemaphoreGive(s_lock);
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
