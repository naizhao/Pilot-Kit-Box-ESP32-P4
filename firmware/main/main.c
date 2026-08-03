/*
 * main.c — Pilot Kit Box (ESP32-P4) application boot strap.
 *
 *   1. Allocate the shared IQ ring buffer.
 *   2. Spawn usb_host_lib_task on CPU 0 (USB stack lifecycle pump).
 *   3. Wait until the USB host library is installed.
 *   4. Spawn sdr_task on CPU 1 (RTL-SDR control + async IQ producer).
 *   5. Spawn dsp_task on CPU 1 (consumer + decoder + 1 Hz meter).
 *   6. Bring up storage sinks, LCD, IMU, UI state, PFD, and BLE.
 *
 * app_main returns; the three tasks own the rest of the runtime.
 */

#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#include "pilot_kit.h"
#include "aircraft_db.h"
#include "aircraft_state.h"
#include "gps.h"
#include "ble_gatt.h"
#include "boot_splash.h"
#include "config_qnh.h"
#include "config_storage.h"
#include "config_traffic.h"
#include "config_ac_category.h"
#include "pk_aero_db.h"
#include "pk_win.h"
#include "pk_aero_layer.h"
#include "apt_detail_page.h"
#include "nav_grid_page.h"
#include "search_page.h"
#include "pk_sdcard.h"
#include "pk_rec_store.h"
#include "pk_rec_ingest.h"
#include "pk_own_sampler.h"
#include "pk_rec_selftest.h"
#include "pk_tile_loader.h"
#include "display.h"
#include "imu_task.h"
#include "baro.h"
#include "battery.h"
#include "config_ble.h"
#include "config_demo.h"
#include "config_devname.h"
#include "i18n.h"
#include "pfd.h"
#include "record_sink.h"
#include "settings_page.h"
#include "ui_state.h"

static const char *TAG = "pilot_kit";

/* 开机把几条关键词条的 ID 和实际文案打到串口，用来验证「ID 指向的还是那句话」。
 * 平时关着（编译期整段消失）；怀疑文案错位时改成 1 重烧一次即可。
 * 背景见 firmware/scripts/i18n_ids.json 顶部的事故说明。 */
#define PK_I18N_ID_SELFTEST 0

RingbufHandle_t g_iq_ringbuf = NULL;

/*
 * USB 排障日志开关（2026-08-03 起临时默认打开）。
 *
 * 为什么需要：dongle 接到载板 USB-A（DP/DM 走 J3 排针 27/25）之后不再枚举，
 * 而 VBUS 实测 4.88 V、dongle 发烫说明供电与上电都正常。默认 INFO 级别下
 * USB 栈**一句话都不打**——「根端口有没有上电」「有没有看到 D+ 上拉」这些
 * 判据全在 DEBUG 级，于是现象只剩 sdr_task 那句干等的 waiting，无从下手。
 *
 * 打开后能区分三种情况（这正是要拿的实证）：
 *   - 连 `HUB: Root port powered` 都没有  → 根端口没起来，问题在主机侧；
 *   - 有 powered、没有 connection            → 主机没看到设备 D+ 上拉，
 *                                              问题在 DP/DM 走线或极性；
 *   - 有 connection、ENUM 报错               → 枚举失败，多半是信号完整性
 *                                              （J3 排针飞线跑 480 Mbps）。
 *
 * 代价是每次插拔多几十行日志，没有设备时几乎不刷屏。
 *
 * 2026-08-03 已收工，改回 0。当时靠它拿到的判据是「root port active、
 * 0 enumerated device、HUB 全程无 power-on 失败」——据此排除了主机侧，
 * 把问题定位到载板那段接线。下次 dongle 又不认，第一件事就是改回 1。
 */
#define PK_USB_DIAG_VERBOSE   0

static void usb_diag_enable_logs(void)
{
#if PK_USB_DIAG_VERBOSE
    /* TAG 取自 managed_components/espressif__usb/src/ 各文件里的 *_TAG 常量，
     * 以及 esp_hw_support/usb_phy/usb_phy.c 的 USBPHY_TAG。 */
    esp_log_level_set("usb_phy",  ESP_LOG_DEBUG);   /* PHY 选型/初始化 */
    esp_log_level_set("HCD DWC",  ESP_LOG_DEBUG);   /* 控制器与端口状态机 */
    esp_log_level_set("HUB",      ESP_LOG_DEBUG);   /* 根端口上电/连接检测 */
    esp_log_level_set("ENUM",     ESP_LOG_DEBUG);   /* 枚举各阶段 */
    esp_log_level_set("USBH",     ESP_LOG_DEBUG);
    esp_log_level_set("USB HOST", ESP_LOG_DEBUG);
    ESP_LOGW(TAG, "USB diagnostic logging ENABLED (PK_USB_DIAG_VERBOSE=1) "
                  "— set it back to 0 once the dongle enumerates");
#endif
}

void usb_host_lib_task(void *arg)
{
    usb_diag_enable_logs();

    ESP_LOGI(TAG, "Installing USB host stack on peripheral_map=0x%x "
                  "(BIT0 = peripheral 0 = High-Speed / UTMI, see pilot_kit.h)",
             (unsigned)PK_USB_PERIPHERAL_MAP);

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = PK_USB_PERIPHERAL_MAP,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "USB host stack installed");

    /* Wake app_main so it can spawn the SDR task that depends on the stack
     * being up. */
    xTaskNotifyGive((TaskHandle_t)arg);

    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "USB host: no clients registered");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGW(TAG, "USB host: all devices freed");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Pilot Kit Box (ESP32-P4) boot");

    /* Log wakeup causes + GPIO wake status. MODE sleep uses
     * esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(), so a
     * successful MODE wake sets the ESP_SLEEP_WAKEUP_GPIO bit and
     * should include GPIO5 in gpio_status. */
    uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    uint64_t gpio_status = esp_sleep_get_gpio_wakeup_status();
    ESP_LOGI(TAG, "boot wakeup_causes=0x%lx  gpio_status=0x%llx",
             (unsigned long)wake_causes, (unsigned long long)gpio_status);

    ESP_LOGI(TAG, "Free internal heap at boot: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    g_iq_ringbuf = xRingbufferCreate(PK_IQ_RINGBUF_SIZE_BYTES,
                                     RINGBUF_TYPE_BYTEBUF);
    assert(g_iq_ringbuf != NULL && "IQ ring buffer alloc failed");
    ESP_LOGI(TAG, "IQ ring buffer ready: %u B (BYTEBUF)",
             (unsigned)PK_IQ_RINGBUF_SIZE_BYTES);

    TaskHandle_t lib_task_hdl = NULL;
    BaseType_t ok = xTaskCreatePinnedToCore(
        usb_host_lib_task, "usb_lib", 4096,
        xTaskGetCurrentTaskHandle(), 5, &lib_task_hdl, 0);
    assert(ok == pdTRUE);

    /* Block until USB host stack is installed (usb_host_lib_task notifies). */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "USB host stack online — spawning SDR + DSP tasks");

    /* Initialise the per-aircraft fusion table before any sink can write
     * into it. */
    aircraft_state_init();
    pk_gps_start();   /* GT-U8 GPS on UART1 */

    /* Bring the ADS-B record sinks up before the producer starts. The
     * file sink mounts LittleFS and may take ~50 ms on first boot (it
     * formats the partition automatically). The UART sink is always
     * available; the BLE sink is a thin wrapper that null-guards
     * everything until ble_gatt_init() succeeds later — safe to
     * register even if BLE never comes up. */
    /* microSD 探测 + 日志存储位置设置必须先于 file sink 创建：
     * record_sink_file_create() 据此决定写 flash LittleFS 还是 /sdcard。 */
    pk_config_storage_load();
    pk_batt_init();
    pk_sdcard_init();
    /* ADS-B / 本机数据落盘的 session 目录管理，须晚于 pk_sdcard_init()。
     * 阶段 3a：只建目录/开文件，不接数据源（dsp_task / own_ship / 相位
     * 状态机是 3b 的事）。pre-unmount 静默复用 record_sink_file.c 的
     * sd_close_log_cb 转调，不额外占 pk_sdcard 的回调槽位。 */
    pk_rec_store_init();
    /* traffic.trk 生产端的非阻塞入队 + 独立写任务，须晚于 pk_rec_store_init()
     * （写任务要调 pk_rec_store_append_traffic_record()）、早于 dsp_task
     * 起跑（下面 sdr_task/dsp_task 创建之前）——dsp_task 的 Mode-S 解码热
     * 路径调 pk_rec_ingest_position/identity()，队列必须已经建好，否则
     * enqueue_or_drop() 会因 s_queue==NULL 直接丢数据（见 pk_rec_ingest.h）。 */
    pk_rec_ingest_init();
    /* 机型分类须先于 own_sampler_start()：采样任务第一拍就调
     * pk_flight_phase_reset() 用它算振动地板初值，若晚于 start() 才 load，
     * 开机头几秒会用编译期默认值而不是用户在设置页选的档位。 */
    pk_config_ac_category_load();
    /* 本机 1 Hz 航迹采样（own.trk 生产者），阶段 3b。不要求 GPS/IMU/baro
     * 已就绪——它们分别在下面才 start（GPS 已在 aircraft_state_init() 之后
     * 起了，IMU/baro 还要再等一两百行），采样器每 tick best-effort 取值。 */
    pk_own_sampler_start();
    pk_tile_loader_init();   /* 地图页瓦片加载任务，须晚于 pk_sdcard_init() */
    /* SD 航空数据库懒加载：只创建后台任务、零 IO（开机不加载是定案）。
     * 须晚于 pk_sdcard_init()——任务靠 pk_sdcard_is_mounted() 决定何时
     * 开始分块加载 /sdcard/aero/pk_aero.bin。 */
    pk_aero_db_init();
    /* SD 机型库（ICAO24 → 型号/注册号）懒加载，同样只建任务、零 IO。
     * 须晚于 pk_sdcard_init()；任务自带 12 s 静默期，避开 pk_aero 的加载
     * 窗口，不与它抢 SD 带宽。未就绪时查询返回 NULL，UI 显示 ICAO24。 */
    pk_aircraft_db_init();
    /* 以本机为中心的滚动窗口（W1 骨架，设计见
     * docs/internal/2026-08-03-window-based-data-architecture-zh_CN.md）。
     * 与 pk_aero_db 全量加载**并存**：窗口另开一个只读句柄按格区间读，
     * 老路径一个字节没动，UI 侧本轮也还没切过来。只创建后台任务、零 IO，
     * 须晚于 pk_sdcard_init()。PK_WIN_ENABLE=0 时本调用是空函数。 */
    pk_win_init();
    /* 地图页的航空数据叠加层：只创建后台快照任务，等地图页第一次渲染
     * 报出视图才开始查（pk_aero_layer.h）。须晚于 pk_aero_db_init()。 */
    pk_aero_layer_init();
    /* 搜索页的后台查询任务 + 从 NVS 读回最近搜索。同样只建任务、零 IO，
     * 须晚于 pk_aero_db_init()（它是那边的消费者）。 */
    pk_search_page_init();
    /* 全屏导航网格（点 FAB 打开的主菜单）。它没有后台任务也没有 NVS，init()
     * 只是把「开着没 / 在第几页 / 亮度 pop 开着没」摆回初值——静态量零初始化
     * 本来就与它写的一致，但生命周期得走全，不留「这一个例外不用 init」。 */
    pk_nav_grid_page_init();
    /* 机场详情页**没有**初始化：它没有后台任务也没有 NVS，打开那一刻同步
     * 取数就够（89 条记录读全是 µs 级，见 apt_detail_page.h）。这里只挂一个
     * 默认关闭的自检钩子——PK_APT_DETAIL_SMOKE=0 时它是个空函数。 */
    pk_apt_detail_smoke_init();

    const char *file_mount = record_sinks_install_defaults();
    if (file_mount != NULL) {
        ESP_LOGI(TAG, "ADS-B sinks ready (UART + file at %s)", file_mount);
    } else {
        ESP_LOGW(TAG, "ADS-B file sink unavailable — UART sink only");
    }
    /* 落盘全链路的注入式自检——默认关（PK_REC_SELFTEST=0 时是空函数）。
     * 打开后需要 record_sinks_install_defaults() 已经把 record_sink_rec_store
     * 注册好，所以排在它后面。见 pk_rec_selftest.h。 */
    pk_rec_selftest_init();

    /* sdr_task / dsp_task 的创建**故意排到 app_main 末尾**（PFD 起来之后），
     * 不在这里。原因见那边的注释：RTL-SDR 一旦枚举成功就会抢内部 DMA 堆，
     * 早启动会把屏、BLE、IMU、气压计全饿死。 */

    /* ESP-Hosted 握手必须排在 MIPI-DSI 之前——顺序反了整机会 26 秒一重启。
     *
     * 症状：SDIO 物理层一切正常（CMD5、CIS、Function 1 就绪位、4-bit 协商
     * 全部成功），但主机在 "Waiting for esp_hosted slave to be ready" 上死等，
     * 13 秒超时后复位从机重试，重试撞上 "failed to read registers"，最后
     * hosted 自己 "Host is resetting itself" 把整机重启。
     *
     * 定位过程：跳过 pk_display_init() 后握手在 72 ms 内完成；只跳过 PFD
     * 渲染任务（LVGL / PPA / GT911 / 温度传感器全不跑）则照旧失败——所以问题
     * 不在渲染负载，就在点屏本身。同一套 hosted 配置在 2.4″ 板上一直是好的，
     * 而那块板走 SPI 屏、根本不碰 DSI。
     *
     * 机理：DSI PHY 要独占 LDO channel 3 并把它拉到 2.5 V
     * （display.h 的 PK_LCD_DSI_PHY_LDO_CHANNEL / _MV）。这一下扰动足以让
     * 刚被 GPIO54 复位、正在启动的 C6 起不来——它的 SDIO 外设仍能应答卡层
     * 命令，但上层固件跑不到发 INIT event 那一步，于是主机永远等不到。
     *
     * 所以先让 hosted 握完手、BLE 起来，再点屏。开机多花的这一秒正好落在
     * splash 显示窗口里，用户看不出差别。
     */
    /* BLE init. Requires the on-board ESP32-C6 to have been
     * pre-flashed with the matching esp_hosted slave firmware (one-time
     * board setup, see docs/BUILD.md §3). The hosted vhci_drv.c uses
     * ESP_ERROR_CHECK() internally so if C6 doesn't respond, the whole
     * P4 firmware aborts — there's no graceful path. Default is on
     * (CONFIG_PK_BLE_ENABLED=y); turn off via menuconfig if you haven't
     * flashed C6 yet or are running CI without it. */
#if CONFIG_PK_BLE_ENABLED
    /* 用户开关（设置页，NVS）。编译期 CONFIG_PK_BLE_ENABLED 是"这台设备有没有
     * BLE"，运行期这个是"用户要不要用"，两者是与的关系。 */
    pk_config_ble_load();
    /* 自定义广播名（P2-5）。**必须排在 ble_gatt_init() 之前**：广播名在
     * NimBLE 的 on_sync() 回调里一次拼好，那个回调紧跟着 init 就会触发，
     * 晚一步读到的就是空串，第一次广播出去的还是出厂名。 */
    pk_config_devname_load();
    esp_err_t ble_err = pk_ble_enabled_get() ? ble_gatt_init() : ESP_OK;
    if (!pk_ble_enabled_get())
        ESP_LOGI(TAG, "BLE disabled by user setting — skipping ble_gatt_init()");
    if (ble_err != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed (%s) — UART + file sinks only",
                 esp_err_to_name(ble_err));
    } else {
        ESP_LOGI(TAG, "BLE GATT service up — advertised name landed in"
                      " on_sync (see 'ble_gatt: advertising as ...')");
    }
#else
    ESP_LOGI(TAG, "BLE disabled at build time (CONFIG_PK_BLE_ENABLED=n) — "
                  "UART + file sinks only. Flash C6 esp_hosted slave + "
                  "re-enable in menuconfig once you're ready.");
#endif

    /* Bring up the LCD and paint the boot splash (logo +
     * "Booting ..." text). The splash stays on screen until the PFD
     * render task starts — we time-stamp here and enforce a minimum
     * hold (PK_BOOT_SPLASH_MIN_MS) just before pk_pfd_start() below
     * so the user can read the logo + version even if init finishes
     * quickly. Init work (IMU/UI/buttons/BLE/SDR) happens during the
     * visible splash window and counts against the hold, so we only
     * sleep if init was faster than the target. */
    /* 演示模式开关与语言都必须在 splash 之前读出来：splash 上要画那条红色的
     * 「演示模式：数据均为模拟」横幅，而横幅文案走 i18n。
     *
     * i18n_init 原本排在下面（PFD 启动前），于是 splash 恒用默认语言英文——中文
     * 用户开机第一屏是英文，这条安全提示的效果先打了个折。两者都只读 NVS，
     * 此刻 NVS 早已由 config_storage / config_ble 初始化过，提前无副作用。 */
    esp_err_t i18n_err = pk_i18n_init();
    if (i18n_err != ESP_OK) {
        ESP_LOGW(TAG, "i18n init failed (%s) — default language remains English",
                 esp_err_to_name(i18n_err));
    }
#if PK_I18N_ID_SELFTEST
    /* 词条 ID ↔ 文案的开机自检（默认关，把上面的宏改成 1 才编进来）。
     *
     * 由来：2026-08 徽章显示成「(数据为模」——ID 平移 + 陈旧 .o，编译零警告、
     * 烧录校验通过、串口日志正常，完全静默。ID 现在由 scripts/i18n_ids.json
     * 钉死（见那里的说明），但下次再怀疑「屏上这句话不对」时，这段能在串口上
     * 直接给出「本固件里 ID N 到底是哪句」，不用去猜。 */
    ESP_LOGW(TAG, "i18n selftest: DEMO_BADGE id=%d text=\"%s\"",
             (int)PK_TR_DEMO_BADGE, pk_i18n_text(PK_TR_DEMO_BADGE));
    ESP_LOGW(TAG, "i18n selftest: SETTINGS_DEMO_HINT id=%d text=\"%s\"",
             (int)PK_TR_SETTINGS_DEMO_HINT, pk_i18n_text(PK_TR_SETTINGS_DEMO_HINT));
    ESP_LOGW(TAG, "i18n selftest: SETTINGS_TITLE id=%d text=\"%s\"",
             (int)PK_TR_SETTINGS_TITLE, pk_i18n_text(PK_TR_SETTINGS_TITLE));
    ESP_LOGW(TAG, "i18n selftest: ABOUT_TITLE id=%d text=\"%s\"",
             (int)PK_TR_ABOUT_TITLE, pk_i18n_text(PK_TR_ABOUT_TITLE));
    ESP_LOGW(TAG, "i18n selftest: last id=%d PK_TR_COUNT=%d text=\"%s\"",
             (int)PK_TR_COUNT - 1, (int)PK_TR_COUNT,
             pk_i18n_text((pk_tr_id_t)(PK_TR_COUNT - 1)));
#endif

    pk_config_demo_load();

    esp_err_t lcd_err = pk_display_init();
    int64_t splash_shown_us = 0;
    if (lcd_err != ESP_OK) {
        ESP_LOGW(TAG, "display init failed (%s) — running headless",
                 esp_err_to_name(lcd_err));
    } else {
        /* splash 第一帧就带进度条（0/3）。用户从这一刻起看到的不再是一张
         * 静止的 logo，而是"机器在干什么、还剩几步"——出 logo 到能操作之间
         * 还有 3 s，那 3 s 以前是纯白等。 */
        pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_START), 0, 3);
        /* 走档位而不是裸占空比：设置页的高亮读的是同一个 s_bl_step，
         * 开机点亮就必须落在某一档上，否则第一次进设置页三段全不高亮。 */
        pk_backlight_step_set(PK_BL_STEP_MID);
        splash_shown_us = esp_timer_get_time();
    }

    /* BNO085 IMU. Failure is non-fatal — the rest of the
     * firmware (RTL-SDR, BLE, storage) keeps working without attitude. */
    pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_SENSORS), 1, 3);
    esp_err_t imu_err = pk_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed (%s) — PFD will run without attitude",
                 esp_err_to_name(imu_err));
    } else {
        ESP_LOGI(TAG, "BNO085 IMU online");
    }
    pk_qnh_load();     /* 从 NVS 加载 QNH,供 baro_task 立即使用 */
    pk_config_traffic_load();  /* 从 NVS 加载地图朝向 + 雷达量程 */
    pk_baro_start();   /* BMP388 on shared I²C0 */

    /*
     * 地图扫描放在 splash 期间 —— 这是**产品决定压过时序最优**的一处，改之前
     * 先读完这段，别照着"哪个数字小选哪个"又挪回去。
     *
     * pk_map_store_scan() 同步扫 /sdcard/maps 下 4 个 pmtiles 约 3.9 s。位置
     * 实测过四个点：
     *
     *   ① app_main 中段（最初）—— 把 hosted 握手和点屏一起往后顶，出 logo
     *      要 10.3 s，其间背光已亮（BL_EN 被 100 kΩ 上拉，上电即通），就是
     *      "亮着一块空屏"。
     *   ② app_main 最末（SDR 之后）—— 扫描的 SD I/O 撞上 RTL-SDR 的 4 MB/s
     *      IQ 流，每扫一个包丢一次、单次丢到 282 KB。
     *   ③ PFD 之后、SDR 之前 —— 时序最漂亮：PFD 8.40 s 可交互、丢包 0。
     *   ④ 这里（splash 期间）—— PFD 11.37 s、丢包 8 次，都比 ③ 差。
     *
     * 仍然选 ④，因为 ③ 有个数字之外的代价：PFD 一起来 splash 就没了，扫描
     * 退到后台，**进度条照不到它**。而这 3.9 s 恰恰是开机最长的一段等待，
     * 用户要的正是"别让我白等"。摆在进度条里的 4 s，比藏在后台的 4 s 好过。
     *
     * ④ 的两笔代价都可接受：PFD 晚 3 s，但这 3 s 屏幕上有明确进度、不是空等；
     * 丢包 8 次发生在开机瞬间，此时 ADS-B 还没有任何目标可丢。
     *
     * 依赖：只要求晚于 pk_sdcard_init()（早就调过了）。
     */
    pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_MAP), 2, 3);
    pk_tile_loader_init();
    pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_READY), 3, 3);

    /* UI state lives in its own module so the UI can flip
     * the mode without touching the render task directly. Default mode
     * is PFD; survives an IMU-init failure (you can still scroll the
     * ADS-B list with no attitude). */
    esp_err_t ui_err = pk_ui_init();
    if (ui_err != ESP_OK) {
        ESP_LOGW(TAG, "ui_state init failed (%s)", esp_err_to_name(ui_err));
    }
    /* pk_i18n_init() 已提前到 splash 之前，见那里的注释。 */

    /* PFD render task. Starts after the display + IMU init
     * so it can read both straight away. Survives either failing.
     *
     * Before kicking the PFD render task, make sure the boot splash
     * has been visible for at least PK_BOOT_SPLASH_MIN_MS. Init work
     * above has already used some of that budget; we only sleep for
     * the remainder. */
    if (lcd_err == ESP_OK) {
        const int64_t splash_min_ms = PK_BOOT_SPLASH_MIN_MS;
        int64_t elapsed_ms = (esp_timer_get_time() - splash_shown_us) / 1000;
        int64_t remaining_ms = splash_min_ms - elapsed_ms;
        if (remaining_ms > 0) {
            ESP_LOGI(TAG, "splash hold: init took %lld ms, sleeping %lld ms "
                          "more (target %lld ms)",
                     (long long)elapsed_ms,
                     (long long)remaining_ms,
                     (long long)splash_min_ms);
            vTaskDelay(pdMS_TO_TICKS(remaining_ms));
        } else {
            ESP_LOGI(TAG, "splash hold: init took %lld ms (≥ %lld ms target), "
                          "no extra wait",
                     (long long)elapsed_ms, (long long)splash_min_ms);
        }
        esp_err_t pfd_err = pk_pfd_start();
        if (pfd_err != ESP_OK) {
            ESP_LOGW(TAG, "PFD start failed (%s)", esp_err_to_name(pfd_err));
        } else {
            ESP_LOGI(TAG, "PFD render task running");
        }
    }

    /*
     * RTL-SDR 放到最后启动 —— 这个次序是有代价换来的，别再往前挪。
     *
     * 2026-08-03 载板 USB 接好、dongle 第一次真正枚举成功之后，整机反而垮了：
     *
     *     I (9073) rtlsdr_async: alloc'd 15 URBs x 6144 B (free internal heap: 45059 B)
     *     E (10111) display: ST7701 panel create failed: ESP_ERR_NO_MEM
     *     E (10142) vhci_drv: Tx ble_transport_to_ll_cmd_impl: malloc failed
     *     W (10903) pilot_kit: IMU init failed (ESP_ERR_NO_MEM)
     *     E (10925) baro: baro task create failed
     *
     * 屏、BLE、姿态、高度**同时**没了，只剩一个在收 ADS-B 的无头盒子。
     *
     * 机理：USB URB 必须落在 DMA-capable 的**内部** RAM（PSRAM 不行），15×6144
     * ≈ 92 KB，加上 USB host stack 自己的开销，把内部堆从 298 KB 打到 45 KB。
     * 而排在后面的 ST7701 DPI DMA 链表、NimBLE 的 vhci 缓冲、BNO085/BMP388 的
     * 驱动分配全都要内部 RAM——先到先得，SDR 早启动就等于它先把堆吃掉。
     *
     * 之所以此前一直没暴露：dongle 从来没枚举成功过（H1/H2 座子不对外供电，
     * 插上去根本不上电），sdr_task 一直停在等 NEW_DEV，那 92 KB 从未真正分配。
     * 换句话说这个坑是**功能修好之后才浮出来的**，不是新引入的回归。
     *
     * 于是把次序反过来：需求固定且不可降级的（屏 / BLE / IMU / 气压计 / PFD）
     * 先各自拿到内存，SDR 用剩下的。ADS-B 晚几秒开始收没有任何影响——它本来
     * 就要等 dongle 枚举 + 调谐 + PLL 锁定。
     *
     * 前置条件仍然满足：USB host stack 早在 app_main 开头就装好了（上面那句
     * ulTaskNotifyTake 等的就是它），g_iq_ringbuf 也已就绪，record_sink 已注册。
     */
    ok = xTaskCreatePinnedToCore(sdr_task, "sdr", 8192, NULL, 6, NULL, 1);
    assert(ok == pdTRUE);

    ok = xTaskCreatePinnedToCore(dsp_task, "dsp", 4096, NULL, 4, NULL, 1);
    assert(ok == pdTRUE);

    ESP_LOGI(TAG, "SDR + DSP tasks spawned last (free internal heap: %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
