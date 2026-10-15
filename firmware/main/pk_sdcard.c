/*
 * pk_sdcard.c — 板载 microSD (TF1, SDMMC 4-bit) 挂载/探测/格式化。
 *
 * 设计:
 *   - 单一 mutex 串行化 mount/unmount/format，探测任务与 UI 触发的
 *     格式化不会互踩。
 *   - 板上无 CD 脚（docs/hardware/board_pinout.md: "No card-detect pin
 *     to a P4 GPIO — detect via mount-retry"）：
 *       未挂载 → 重试 esp_vfs_fat_sdmmc_mount，头 3 次每 3 s，连续失败
 *                起退避 ×1.5 封顶 12 s；最多连续 8 次（含 boot），
 *                预算耗尽完全停探直至重启（见
 *                sd_probe_period_for_streak 与 SD_PROBE_* 常量）；
 *       已挂载 → 每 2 s sdmmc_get_status 探活；真实块传输失败立即关闭
 *                新 I/O，探测任务安全卸载并隔离至重启。
 *   - 无卡时 sdmmc 驱动层会刷错误日志，init 时把相关 TAG 静音，
 *     状态变化由本模块自己打一条简洁 INFO。
 */

#include "pk_sdcard.h"
#include "pk_sd_transaction_safety.h"

#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "soc/soc_caps.h"
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
#define SD_PIN_PWR_EN    45  /* 载板 P 沟道 MOSFET gate，低有效 */

/* GPIO39-44 是 P4 SDMMC SLOT_0 的专用引脚（UHS-I capable）。SLOT_0 的
 * IO 电源 VDD_IO_5 由 P4 片上 LDO 通道 4 (LDO_VO4) 供给 —— 必须经
 * sd_pwr_ctrl_new_on_chip_ldo 上电，否则数据线无电、任何卡都探测不到。
 * Waveshare 官方例程 (esp32-p4-platform/examples/esp-idf/09_sdmmc) 同配。 */
#define SD_SLOT          SDMMC_HOST_SLOT_0
#define SD_LDO_CHAN      4

#define SD_PROBE_PERIOD_MS    3000   /* 未挂载时重试间隔（退避基数） */
#define SD_ALIVE_PERIOD_MS    2000   /* 已挂载时探活间隔 */
/* 无 CD 脚，init 失败无法区分无卡/原卡/替换卡。头 3 次失败保持
 * 3 s，之后每次 ×1.5 封顶 12 s；最多连续 8 次初始化尝试（含 boot）。
 * 预算耗尽后停止上电/挂载，晚插卡或换卡必须重启；成功挂载清零，正常
 * 拔卡后允许新一轮预算。GPIO/LDO 准备失败同样计入这 8 次。 */
#define SD_PROBE_BACKOFF_MAX_MS  12000
#define SD_INIT_ATTEMPT_LIMIT        8
#define SD_TASK_STACK         4096

static sdmmc_card_t          *s_card;
static volatile pk_sd_state_t s_state = PK_SD_NO_CARD;
/* 累计挂载尝试次数。诊断页用它区分"没插卡"与"插了但挂不上"——两者在
 * 状态上都是 PK_SD_NO_CARD，但前者计数不动、后者每轮探测涨一次
 * （节奏随退避从 3 s 放慢到 12 s，预算耗尽后不再增长）。 */
static volatile uint32_t s_mount_attempts;
/* 成功挂载次数（挂载代数），拔插检测用。见 pk_sdcard.h 的说明。 */
static volatile uint32_t s_mount_generation;
/* --- init 失败有界重试（探测任务/写者私有） -------------------------------
 * s_mount_fail_streak 在上电前消耗预算，成功挂载清零，只在 s_lock 内写
 * （boot 尝试发生在探测任务创建
 * 之前）；探测循环在锁外读 s_probe_period_ms 决定下一次延时——读和写来自
 * 同一个探测任务，无竞争。 */
static uint32_t s_mount_fail_streak;
static uint32_t s_probe_period_ms = SD_PROBE_PERIOD_MS;
/* Protected by s_io_mux for read-only UI callers on either core. */
static bool s_probe_exhausted;
/* 「卡在位，但文件系统挂不上」。契约见 pk_sdcard.h。 */
static volatile bool     s_media_error;
static SemaphoreHandle_t      s_lock;
/* Admission/count is independent of the lifecycle mutex. A consumer finishing
 * I/O must be able to release while format/unmount owns s_lock and waits. */
static portMUX_TYPE s_io_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_io_users;

static void sd_set_state(pk_sd_state_t state)
{
    portENTER_CRITICAL(&s_io_mux);
    s_state = state;
    portEXIT_CRITICAL(&s_io_mux);
}

static void sd_wait_io_idle(void)
{
    for (;;) {
        portENTER_CRITICAL(&s_io_mux);
        bool idle = s_io_users == 0;
        portEXIT_CRITICAL(&s_io_mux);
        if (idle) return;
        vTaskDelay(1);
    }
}

/* 容量缓存 — 由挂载点/探测任务刷新；pk_sdcard_info 只读缓存，
 * 这样诊断页每帧调用不会触发 FAT 扫描 I/O。 */
static volatile uint64_t s_total_bytes;
static volatile uint64_t s_free_bytes;

static void sd_set_info(uint64_t total, uint64_t free_b)
{
    portENTER_CRITICAL(&s_io_mux);
    s_total_bytes = total;
    s_free_bytes = free_b;
    portEXIT_CRITICAL(&s_io_mux);
}

static void sd_refresh_info_locked(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_state == PK_SD_MOUNTED) {
        /* INVALID_STATE does not set errno; do not mistake an old timeout for
         * this query's result. The detector keeps retrying every two seconds. */
        errno = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
            sd_set_info(total, free_b);
            return;
        }
        /* FAT lock contention is transient. Keep only this mount's last good
         * observation; unmount/format/new mount invalidate it below. */
        if (errno == ETIMEDOUT) return;
    }
    sd_set_info(0, 0);
}

/* --- mount / unmount（调用方须持锁） ----------------------------------- */

/*
 * SLOT_0 IO 电源（LDO_VO4）必须常驻。
 *
 * ESP32-P4 数据手册把 GPIO39..48 都列在 VDD_IO_5 域；微雪实际原理图把
 * ESP_LDO_VO4 接到这个域和 SD 上拉，而 TF1 的 VDD 是 ESP_3V3 经 GPIO45
 * 控制的低有效 PMOS。扩展板又把 BK4819 SDATA 接 GPIO47、PCM1808 DOUT
 * 接 GPIO48。因此删除这个句柄不是“只给卡断电”，而是会同时切断两路
 * AIRBAND 输入缓冲。句柄从首次 mount 起常驻，卡本体的 POR 只切 GPIO45。
 */
static sd_pwr_ctrl_handle_t s_pwr_ctrl;
static bool s_card_power_gpio_ready;

/* IDF 6.x 的 SDMMC 控制器只能初始化一次。本板已验证的
 * ESP-Hosted 拓扑是 C6 占 slot 1、microSD 占 slot 0；Hosted 先创建
 * 共享控制器。这与仓库内 esp_hosted/host_sdcard_with_hosted 官方示例
 * 的 workaround 相同。若日后改为非 SDIO transport，编译期条件会恢复
 * SDMMC_HOST_DEFAULT() 的标准 init/deinit，不会吞掉必需的控制器初始化。 */
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define SD_HOST_SHARED_WITH_HOSTED 1
static esp_err_t sdmmc_host_init_dummy(void)   { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }
#else
#define SD_HOST_SHARED_WITH_HOSTED 0
#endif

static bool sd_power_on_locked(void)
{
    if (s_pwr_ctrl == NULL) {
        sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "VDD_IO_5 LDO chan %d enable failed: %s",
                     SD_LDO_CHAN, esp_err_to_name(err));
            s_pwr_ctrl = NULL;
            return false;
        }
    }
    if (!s_card_power_gpio_ready) {
        const gpio_config_t power_io = {
            .pin_bit_mask = 1ULL << SD_PIN_PWR_EN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&power_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "card power GPIO%d init failed: %s",
                     SD_PIN_PWR_EN, esp_err_to_name(err));
            return false;
        }
        s_card_power_gpio_ready = true;
    }
    gpio_set_level(SD_PIN_PWR_EN, 0);
    /* PMOS 导通后给 TF1 本体完成电源爬升与 POR；VDD_IO_5 始终保持。 */
    vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

/* The SDK alignment callback only tests address/length divisibility. P4's
 * ordinary heap also contains RTC/scratch memory which SDMMC cannot access.
 * Reject the entire non-DMA range so native sdmmc_{read,write}_sectors uses
 * its MALLOC_CAP_DMA bounce buffer instead of starting an invalid transfer. */
static bool sd_dma_buffer_alignment(int slot, const void *buf, size_t size)
{
    if (slot != SD_SLOT) return sdmmc_host_check_buffer_alignment(slot, buf, size);
    if (buf == NULL || size == 0) return false;
    uintptr_t first = (uintptr_t)buf;
    if (size - 1 > UINTPTR_MAX - first) return false;
    const void *last = (const void *)(first + size - 1);
    bool dma_range = esp_ptr_dma_capable(buf) && esp_ptr_dma_capable(last);
#if SOC_SDMMC_PSRAM_DMA_CAPABLE
    dma_range = dma_range || (esp_ptr_dma_ext_capable(buf) && esp_ptr_dma_ext_capable(last));
#endif
    return dma_range && sdmmc_host_check_buffer_alignment(slot, buf, size);
}

static void sd_power_off_locked(void)
{
    if (!s_card_power_gpio_ready) return;
    gpio_set_level(SD_PIN_PWR_EN, 1);
    /* 给卡电源去耦放电；下一次探测再拉低并等待 POR。 */
    vTaskDelay(pdMS_TO_TICKS(5));
}

/* 连续挂载失败 streak 次后的下一次探测间隔：头 3 次失败保持 3 s，
 * 从第 4 次连续失败起每次 ×1.5，封顶 12 s。整数 ×1.5 用 *3/2；
 * ms 达到封顶即提前返回，循环最多 4 步，不退化成忙等。
 * 该间隔只调度探测任务；预算耗尽后 sd_mount_locked 不再触碰卡。 */
static uint32_t sd_probe_period_for_streak(uint32_t streak)
{
    uint32_t ms = SD_PROBE_PERIOD_MS;
    for (uint32_t i = 3; i < streak; i++) {
        ms = ms * 3 / 2;
        if (ms >= SD_PROBE_BACKOFF_MAX_MS) return SD_PROBE_BACKOFF_MAX_MS;
    }
    return ms;
}

static void sd_mount_failed_locked(void)
{
    uint32_t next_probe_ms = sd_probe_period_for_streak(s_mount_fail_streak);
    if (s_mount_fail_streak >= SD_INIT_ATTEMPT_LIMIT) {
        portENTER_CRITICAL(&s_io_mux);
        s_probe_exhausted = true;
        portEXIT_CRITICAL(&s_io_mux);
        ESP_LOGW(TAG, "microSD init failed %lu times — probing stopped until reboot",
                 (unsigned long)s_mount_fail_streak);
    } else if (next_probe_ms != s_probe_period_ms) {
        ESP_LOGI(TAG, "mount failed %lu times in a row — probing every %lu ms",
                 (unsigned long)s_mount_fail_streak, (unsigned long)next_probe_ms);
    }
    s_probe_period_ms = next_probe_ms;
}

static bool sd_mount_locked(void)
{
    /* A failed data transaction is not proof that a different card is now
     * inserted. No CD pin: do not automatically resume writes until reboot. */
    if (pk_sd_transaction_fault() != ESP_OK) return false;
    if (s_mount_fail_streak >= SD_INIT_ATTEMPT_LIMIT) return false;
    /* Count before any power preparation: GPIO/LDO failures and unarmed
     * initialization errors must not bypass the finite retry budget. */
    s_mount_fail_streak++;
    sd_set_info(0, 0);
    if (!sd_power_on_locked()) {
        sd_mount_failed_locked();
        return false;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot   = SD_SLOT;
    host.check_buffer_alignment = sd_dma_buffer_alignment;
#if SD_HOST_SHARED_WITH_HOSTED
    host.init   = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
#endif
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
        .max_files              = 16,      /* 原 8：ADS-B/本机落盘（pk_rec_store）峰值
                                             * 句柄预算算到 10（地图包上限 16 那条更紧），
                                             * 详见 ADS-B 数据持久化设计
                                             * （内部文档）「文件句柄预算」节 */
        .allocation_unit_size   = 16 * 1024,
    };

    s_mount_attempts++;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        /* 每次都打，带尝试序号。
         *
         * 原先"只在错误码变化时打一条"是为了防重试刷屏，代价是**热插拔
         * 失败时日志里一条都看不到**——2026-07-29 排查"插卡后一直 retry 挂不
         * 上"时，抓了 20 s 串口一行 SD 日志都没有，因为错误码从开机起就没变
         * 过。每轮一条（3 s 起随退避最长 12 s，最多 8 次）的频率完全可接受，可见性比
         * 省日志重要。 */
        ESP_LOGW(TAG, "mount attempt #%lu failed: %s",
                 (unsigned long)s_mount_attempts, esp_err_to_name(err));
        s_card = NULL;

        /*
         * 板上没有 CD 脚，"没插卡"和"插了卡但读不出"在状态上都是 PK_SD_NO_CARD。
         * 唯一能把两者分开的线索是错误码的**来源层**：
         * esp_vfs_fat_sdmmc_mount() 先走 esp_vfs_fat_sdmmc_sdcard_init()（卡层
         * 协商，卡不在/接触不良 → ESP_ERR_TIMEOUT / ESP_ERR_NOT_FOUND），协商
         * 成功后才走 mount_to_vfs_fat()，而它只在 f_mount 失败时返回 ESP_FAIL。
         * 所以 ESP_FAIL 等价于「卡确实在位、CMD0/CMD8 都谈成了，但分区上不是
         * 能挂的 FAT」——值得单独提示用户去格式化，而不是笼统地说"没有卡"。
         *
         * 只在这里赋值、不做累加：探测任务每轮重试一次（3 s 起随退避放慢），
         * 卡不动的话每轮结论相同，边沿判定交给上层（pk_tile_loader 的
         * handle_sd_transition）。
         */
        s_media_error = (err == ESP_FAIL);

#if SD_HOST_SHARED_WITH_HOSTED
        /* VFS 失败路径只会调 dummy deinit；显式注销本轮 slot 0，
         * 避免下次热插拔重试因 slot 泄漏而永久 INVALID_STATE。 */
        esp_err_t derr = sdmmc_host_deinit_slot(SD_SLOT);
        if (derr != ESP_OK && derr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "post-fail deinit_slot(%d): %s",
                     SD_SLOT, esp_err_to_name(derr));
        }
#endif

        /*
         * 失败后切 TF1 的 3V3 PMOS——否则插槽在两次重试之间一直带电，而
         * **带电插入的卡不会执行上电复位（POR）**。
         *
         * 2026-08-01 实测：开机无卡→插入，识别成功；随后拔出（此时卡正在被
         * pk_aero 读取，状态机停在中途）→ 再插回，就一直
         * "mount attempt #7..#16 failed: ESP_ERR_TIMEOUT"，且间隔精确 5.01 s
         * （3 s 轮询 + 2.01 s 固定超时）= 卡对 CMD0 完全无响应，不是在协商。
         * 两次插入的唯一差别就是卡有没有"通电运行中被拔走"这段历史：干净的
         * 卡带电插入也能认，被中途拔走的卡则必须靠断电走一遍 POR 才能回到
         * idle。
         *
         * 旧实现错误地删除 LDO_VO4 句柄；那只会切 VDD_IO_5（GPIO39..48），
         * 不会切微雪原理图中的卡 VDD，且会破坏 GPIO47/48 上的 AIRBAND。
         * 这里明确只切 GPIO45 控制的卡电源 PMOS。
         */
        sd_power_off_locked();
        sd_mount_failed_locked();
        return false;
    }

    s_media_error = false;  /* 挂载成功，清除此前的挂载错误结论 */
    s_mount_fail_streak = 0;        /* 成功挂载必须清零退避（§4-2） */
    s_probe_period_ms = SD_PROBE_PERIOD_MS;
    portENTER_CRITICAL(&s_io_mux);
    s_probe_exhausted = false;
    portEXIT_CRITICAL(&s_io_mux);
    s_mount_generation++;   /* 契约见 pk_sdcard.h：只增不减的「卡换过了」凭据 */
    pk_sd_transaction_arm();
    sd_set_state(PK_SD_MOUNTED);
    sd_refresh_info_locked();
    ESP_LOGI(TAG, "microSD mounted at %s: %s %.1f GB",
             SD_MOUNT_POINT, s_card->cid.name,
             ((double)s_card->csd.capacity * s_card->csd.sector_size)
                 / (1024.0 * 1024.0 * 1024.0));
    return true;
}

/* 常驻句柄的「卸载前静默」回调表；短开短关消费者使用 I/O lease。
 * 注册通常发生在 init 期，但探测任务已在跑，写表要持锁。 */
/* 4 → 8：地图/航空库/航空DB/日志四家已把 4 槽占满，ADS-B 落盘(pk_rec_store)
 * 再来就注册不上了。曾试过挂在 record_sink_file 的回调里转调，但那个注册是
 * `if (s_on_sdcard)` 有条件的、而日志后端默认是 flash——默认配置下转调根本
 * 不会发生，拔卡即 use-after-free。扩槽位是唯一干净的解法。 */
#define SD_PRE_UNMOUNT_CB_MAX 8
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
    sd_set_state(PK_SD_NO_CARD);
    sd_set_info(0, 0);
    /* One-shot consumers close their own handles before release. Drain these
     * before taking any persistent consumer's module lock in its callback. */
    sd_wait_io_idle();
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

#if SD_HOST_SHARED_WITH_HOSTED
        /* unmount 保留 Hosted 的共享控制器，只释放 microSD slot 0。 */
        esp_err_t derr = sdmmc_host_deinit_slot(SD_SLOT);
        if (derr != ESP_OK && derr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "deinit_slot(%d): %s", SD_SLOT, esp_err_to_name(derr));
        }
#endif

        /* 只给 TF1 本体断电，让下一张卡从冷态开始；VDD_IO_5 保持。 */
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
                esp_err_t data_error = pk_sd_transaction_fault();
                if (data_error != ESP_OK) {
                    s_media_error = true;
                    ESP_LOGE(TAG, "microSD data transaction failed (%s) — isolating until reboot",
                             esp_err_to_name(data_error));
                    sd_unmount_locked();
                } else if (sdmmc_get_status(s_card) != ESP_OK) {
                    ESP_LOGW(TAG, "microSD removed — unmounting");
                    sd_unmount_locked();
                } else {
                    sd_refresh_info_locked();
                }
            }
            xSemaphoreGive(s_lock);
        } else if (s_state == PK_SD_NO_CARD) {
            /* 间隔在 mount 尝试前按当前退避取值；写它的只有本任务
             * （及任务创建前的 boot 尝试），锁外读是安全的。 */
            vTaskDelay(pdMS_TO_TICKS(s_probe_period_ms));
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_state == PK_SD_NO_CARD && pk_sd_transaction_fault() == ESP_OK) {
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
    esp_log_level_set("sdmmc_req", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_periph", ESP_LOG_DEBUG);
    /* SD_HOST 同时承载 Hosted slot 1 和 microSD slot 0；DEBUG 会记录
     * 每笔 SDMMC 传输，每秒上百行会淹掉串口并拖慢 PFD。留在
     * WARN，真正的 slot/控制器错误仍会报出。 */
    esp_log_level_set("SD_HOST", ESP_LOG_WARN);
#else
    esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);
#endif
    /* vfs_fat_sdmmc 不跟随上面的开关静音：它的 CHECK_EXECUTE_RESULT 宏用
     * ESP_LOGE 报「挂载失败在哪一步」（sdcard_init = slot/控制器层，
     * mount_initialized = VFS 层），是热插拔排障唯一能区分这两层的线索。
     * 之前设成 NONE，等于把最关键的一句话堵死，出事只能重烧诊断固件。
     * 留在 ERROR：正常运行不吭声，失败时每次重试多一行，频率与本模块自己的
     * 挂载失败日志同节奏。 */
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_ERROR);

    /* SLOT_0 / GPIO39..48 IO 供电由首次 sd_power_on_locked() 建立后
     * 常驻；TF1 本体的上/下电由 GPIO45 单独控制。 */

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!sd_mount_locked()) {
        ESP_LOGI(TAG, "microSD unavailable at boot (up to %d init attempts)",
                 SD_INIT_ATTEMPT_LIMIT);
    }
    xSemaphoreGive(s_lock);

    xTaskCreatePinnedToCore(sd_detect_task, "sd_detect",
                            SD_TASK_STACK, NULL, 2, NULL, 0);
}

pk_sd_state_t pk_sdcard_state(void)
{
    portENTER_CRITICAL(&s_io_mux);
    pk_sd_state_t state = s_state;
    if (state == PK_SD_MOUNTED && pk_sd_transaction_fault() != ESP_OK) {
        state = PK_SD_NO_CARD;
    }
    portEXIT_CRITICAL(&s_io_mux);
    return state;
}

bool pk_sdcard_is_mounted(void)
{
    return pk_sdcard_state() == PK_SD_MOUNTED;
}

bool pk_sdcard_io_acquire(void)
{
    portENTER_CRITICAL(&s_io_mux);
    bool admitted = s_state == PK_SD_MOUNTED;
    admitted = admitted && pk_sd_transaction_fault() == ESP_OK;
    if (admitted) ++s_io_users;
    portEXIT_CRITICAL(&s_io_mux);
    return admitted;
}

void pk_sdcard_io_release(void)
{
    portENTER_CRITICAL(&s_io_mux);
    configASSERT(s_io_users > 0);
    --s_io_users;
    portEXIT_CRITICAL(&s_io_mux);
}

uint32_t pk_sdcard_mount_generation(void)
{
    return s_mount_generation;
}

bool pk_sdcard_media_error(void)
{
    return s_media_error || pk_sd_transaction_fault() != ESP_OK;
}

bool pk_sdcard_probe_exhausted(void)
{
    portENTER_CRITICAL(&s_io_mux);
    bool exhausted = s_probe_exhausted;
    portEXIT_CRITICAL(&s_io_mux);
    return exhausted;
}

const char *pk_sdcard_mount_point(void)
{
    return SD_MOUNT_POINT;
}

bool pk_sdcard_info(uint64_t *out_total, uint64_t *out_free)
{
    /* 只读缓存（探测任务每 2s 刷新），渲染路径调用零 I/O。 */
    portENTER_CRITICAL(&s_io_mux);
    bool known = s_state == PK_SD_MOUNTED &&
                 pk_sd_transaction_fault() == ESP_OK && s_total_bytes != 0;
    if (known) {
        if (out_total) *out_total = s_total_bytes;
        if (out_free) *out_free = s_free_bytes;
    }
    portEXIT_CRITICAL(&s_io_mux);
    return known;
}

bool pk_sdcard_register_pre_unmount_cb(void (*cb)(void))
{
    if (cb == NULL) return false;
    /* 探测任务可能正持锁跑卸载序列，写表必须与之互斥；调用方按约定在
     * pk_sdcard_init() 之后才注册，s_lock 此时必然已建。 */
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "register_pre_unmount_cb before init — dropped");
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool registered = false;
    for (int i = 0; i < s_pre_unmount_cb_n; ++i) {
        if (s_pre_unmount_cb[i] == cb) {
            xSemaphoreGive(s_lock);
            return true;
        }
    }
    if (s_pre_unmount_cb_n < SD_PRE_UNMOUNT_CB_MAX) {
        s_pre_unmount_cb[s_pre_unmount_cb_n++] = cb;
        registered = true;
    } else {
        ESP_LOGE(TAG, "pre-unmount cb table full (%d) — dropped",
                 SD_PRE_UNMOUNT_CB_MAX);
    }
    xSemaphoreGive(s_lock);
    return registered;
}

esp_err_t pk_sdcard_format(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != PK_SD_MOUNTED || s_card == NULL || pk_sd_transaction_fault() != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    sd_set_state(PK_SD_FORMATTING);
    sd_set_info(0, 0);
    sd_wait_io_idle();
    /* Same lifetime barrier as removal: all consumers see not-mounted before
     * their callbacks run, and every open fd/in-flight I/O is gone before FAT
     * is reformatted. Callbacks must not acquire this module's lock. */
    for (int i = 0; i < s_pre_unmount_cb_n; ++i) s_pre_unmount_cb[i]();
    ESP_LOGI(TAG, "formatting microSD (FAT32)…");
    esp_err_t err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
    if (err == ESP_OK && pk_sd_transaction_fault() != ESP_OK) {
        err = pk_sd_transaction_fault();
    }
    if (err == ESP_OK) {
        s_mount_generation++;
        sd_set_state(PK_SD_MOUNTED);
        sd_refresh_info_locked();
        ESP_LOGI(TAG, "format done");
    } else {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        sd_unmount_locked();   /* 状态未知，卸掉让探测任务重挂 */
    }
    xSemaphoreGive(s_lock);
    return err;
}
