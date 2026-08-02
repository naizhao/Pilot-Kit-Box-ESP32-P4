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
/* 成功挂载次数（挂载代数），拔插检测用。见 pk_sdcard.h 的说明。 */
static volatile uint32_t s_mount_generation;
/* 「卡在位，但文件系统挂不上」。契约见 pk_sdcard.h。 */
static volatile bool     s_media_error;
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
    /* 上电后给卡一段稳定时间再发命令。sd_pwr_ctrl_new_on_chip_ldo 里
     * esp_ldo_acquire_channel 只是把 LDO 使能位打开就返回（ref_cnt 0→1 时
     * ldo_ll_enable(true)），轨压建立和卡自身的上电复位都还没走完；SD 规范
     * 要求上电到首条命令之间留出电源爬升时间。20 ms 相对 3 s 的重试周期
     * 可以忽略，却能让"插回来的卡"稳稳走完 POR。 */
    vTaskDelay(pdMS_TO_TICKS(20));
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
        .max_files              = 16,      /* 原 8：ADS-B/本机落盘（pk_rec_store）峰值
                                             * 句柄预算算到 10（地图包上限 16 那条更紧），
                                             * 详见 docs/internal/2026-08-02-adsb-data-
                                             * persistence-design-zh_CN.md「文件句柄预算」 */
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

        /*
         * 板上没有 CD 脚，"没插卡"和"插了卡但读不出"在状态上都是 PK_SD_NO_CARD。
         * 唯一能把两者分开的线索是错误码的**来源层**：
         * esp_vfs_fat_sdmmc_mount() 先走 esp_vfs_fat_sdmmc_sdcard_init()（卡层
         * 协商，卡不在/接触不良 → ESP_ERR_TIMEOUT / ESP_ERR_NOT_FOUND），协商
         * 成功后才走 mount_to_vfs_fat()，而它只在 f_mount 失败时返回 ESP_FAIL。
         * 所以 ESP_FAIL 等价于「卡确实在位、CMD0/CMD8 都谈成了，但分区上不是
         * 能挂的 FAT」——值得单独提示用户去格式化，而不是笼统地说"没有卡"。
         *
         * 只在这里赋值、不做累加：探测任务每 3 s 重试一次，卡不动的话每轮结论
         * 相同，边沿判定交给上层（pk_tile_loader 的 handle_sd_transition）。
         */
        s_media_error = (err == ESP_FAIL);

        /*
         * 补上 IDF 在挂载失败路径上漏掉的 slot 注销——热插拔「插回去也认不出，
         * 只能重启」的真因。
         *
         * esp_vfs_fat_sdmmc_sdcard_init()（fatfs/vfs/vfs_fat_sdmmc.c:230-266）
         * 的顺序是：host_config->init() → sdmmc_host_init_slot()（**slot 在这里
         * 就注册好了**）→ sdmmc_card_init()（探卡协商）。无卡时前两步都成功、
         * 第三步失败，而它的 cleanup 只有一句 call_host_deinit()。IDF 自己在
         * 那段代码上留了注释直说这事没做完：
         *     "If this failed …, slot deinit needs to called()
         *      … though slot deinit not implemented yet."
         * 官方路径下 host.deinit 是真的 sdmmc_host_deinit()，它会顺手把所有
         * slot 一起拆掉，所以缺陷被掩盖；而本板的 deinit 是 dummy（不能真拆
         * ESP-Hosted 共用的控制器，见上面 sdmmc_host_deinit_dummy 的说明），
         * 于是**每一次无卡的挂载重试都会泄漏一个已注册的 slot**。
         *
         * 后果链：拔卡后探测任务 3 s 试一次 → 第一次无卡重试就泄漏 slot →
         * 下一次 sdmmc_host_init_slot 被 sd_host_sdmmc.c:156 的
         * `slot_available` 挡下返回 ESP_ERR_INVALID_STATE → 此后卡插回来也
         * 一样，只有重启能清。实测日志里那串连续的
         * "mount attempt #27..#36 failed: ESP_ERR_INVALID_STATE" 就是它。
         *
         * 放在失败分支里无条件调一次，两种失败都被覆盖且幂等：
         *   - 卡协商失败 → 注销刚注册的 slot，不留泄漏；
         *   - slot 已被泄漏占着（init_slot 就返回 INVALID_STATE）→ 这次注销把
         *     它清掉，下一轮重试即可自愈，不必重启。
         * INVALID_STATE 是「本来就没注册」的正常回报，不算错。
         */
        esp_err_t derr = sdmmc_host_deinit_slot(SD_SLOT);
        if (derr != ESP_OK && derr != ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "post-fail deinit_slot(%d): %s",
                     SD_SLOT, esp_err_to_name(derr));

        /*
         * 失败后也要断电——否则插槽在两次重试之间一直带电，而**带电插入的卡
         * 不会执行上电复位（POR）**。
         *
         * 2026-08-01 实测：开机无卡→插入，识别成功；随后拔出（此时卡正在被
         * pk_aero 读取，状态机停在中途）→ 再插回，就一直
         * "mount attempt #7..#16 failed: ESP_ERR_TIMEOUT"，且间隔精确 5.01 s
         * （3 s 轮询 + 2.01 s 固定超时）= 卡对 CMD0 完全无响应，不是在协商。
         * 两次插入的唯一差别就是卡有没有"通电运行中被拔走"这段历史：干净的
         * 卡带电插入也能认，被中途拔走的卡则必须靠断电走一遍 POR 才能回到
         * idle。
         *
         * cd0ddc9 当初就是为这件事在 unmount 路径加的断电，但漏了这条挂载
         * 失败路径：拔卡后第一次重试上电，之后 s_pwr_ctrl 非空，
         * sd_power_on_locked() 直接短路返回，于是插槽长期带电，用户插回来的
         * 那一下正好落在带电窗口里。这里补齐后，插槽只在每次重试的短暂
         * 尝试期间带电，其余时间断电。
         */
        sd_power_off_locked();
        return false;
    }

    s_state = PK_SD_MOUNTED;
    s_media_error = false;  /* 挂上了，之前那张读不出的卡已经被换掉 */
    s_mount_generation++;   /* 契约见 pk_sdcard.h：只增不减的「卡换过了」凭据 */
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
    esp_log_level_set("sdmmc_req", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_periph", ESP_LOG_DEBUG);
    /* SD_HOST 是**共享** tag：ESP-Hosted 的 SDIO（slot 1）与本卡（slot 0）
     * 都用它，而 hosted 每笔传输都打一条 DEBUG。开到 DEBUG 会以每秒上百行
     * 淹掉串口——2026-08-01 实测一次 4 分钟抓包里 SD_HOST 占了 16765/17391
     * 行，把要看的 SD 事件全挤掉，PFD 也从 12 FPS 掉到 9。留在 WARN：
     * 真正的 slot/控制器错误照样报，hosted 的流水账不进来。 */
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
     * 留在 ERROR：正常运行不吭声，失败时每次重试多一行,频率与本模块自己的
     * 3 s 一条相当。 */
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_ERROR);

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

uint32_t pk_sdcard_mount_generation(void)
{
    return s_mount_generation;
}

bool pk_sdcard_media_error(void)
{
    return s_media_error;
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
