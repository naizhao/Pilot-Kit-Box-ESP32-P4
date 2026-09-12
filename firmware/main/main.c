/*
 * main.c — Pilot Kit Box (ESP32-P4) application boot strap.
 *
 *   1. Bring up storage sinks, GPS, record pipelines, and BLE.
 *   2. Bring up LCD, IMU, UI state, and PFD.
 *   3. Spawn the ADS-B link task last (RP2040 UART, adsb_link_task.c).
 *
 * app_main returns; the spawned tasks own the rest of the runtime.
 */

#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "pilot_kit.h"
#include "adsb_link_task.h"
#include "aircraft_db.h"
#include "aircraft_state.h"
#include "gps.h"
#include "ble_gatt.h"
#include "boot_splash.h"
#include "config_qnh.h"
#include "config_storage.h"
#include "config_traffic.h"
#include "config_ac_category.h"
#include "config_own_icao.h"
#include "pk_aero_db.h"
#include "pk_win.h"
#include "pk_aero_layer.h"
#include "apt_detail_page.h"
#include "nav_grid_page.h"
#include "search_page.h"
#include "pk_sdcard.h"
#include "pk_i2c0_bus.h"   /* 板级 I²C0 总线：先于一切 I²C 器件 init 创建 */
#include "demo_track_sd.h"
#include "pk_rec_store.h"
#include "pk_rec_ingest.h"
#include "pk_own_sampler.h"
#include "pk_rec_selftest.h"
#include "pk_tile_loader.h"
#include "display.h"
#include "imu_task.h"
#include "baro.h"
#include "qmc5883p.h"
#include "power_sy6970.h"
#include "power_service.h"
#include "config_ble.h"
#include "config_demo.h"
#include "config_antenna.h"
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

    /* NVS 的唯一初始化点（2026-09-10）：分区异常时**只在这里擦一次**。
     * 此前 11 个模块各自 nvs_flash_init()+nvs_flash_erase()，任一模块命中
     * NO_FREE_PAGES / NEW_VERSION_FOUND 就会把整个分区擦掉——包括 IMU 的
     * pk_imu/tare_quat，表现为"长按调平重启后不生效"。这里统一处理；各模块
     * 的 ensure_nvs() 只保留幂等 init（见 config_*.c）。 */
    {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS needs erase (%s) — erasing once",
                     esp_err_to_name(nvs_err));
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_err = nvs_flash_init();
        }
        if (nvs_err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_flash_init: %s — persistence degraded",
                     esp_err_to_name(nvs_err));
        }
    }

    /* ADS-B 链路任务的 UART 在 adsb_link_task 内部初始化，
     * 这里不再有 USB host 栈的启动次序约束。 */

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
    /* I²C0 总线在这里（电源链之前）创建——从原先点屏之后的位置前移
     * （WP-D Task 4）：电源链里的 SY6970 探测需要总线已在位，而
     * pk_i2c0_bus.h 的既定时机本来就是「app_main 最前面、先于一切
     * I²C 器件 init」。失败不中止启动的深审裁定（2026-09-05）不变：
     * 探测/器件 init 各自对 NULL 总线有优雅失败路径，降级为无 IMU/baro/
     * touch/SY6970 的 1090 盒子。 */
    esp_err_t bus_err = pk_i2c0_bus_init();
    if (bus_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 bus init failed (%s) — continuing without "
                      "IMU/baro/touch (1090 unaffected)", esp_err_to_name(bus_err));
    }

    /* 电源链（用户 2026-09-10 口径）：电池挂在扩展板，链路是
     * 「电池→扩展板 USB-C→微雪 USB-C」。SY6970 是扩展板充电芯片，
     * **唯一权威电池源**；微雪主板 BATT（ETA6098）不再读（用不上），
     * 故**不注册**——此前"两块板电池值互跳"按构造消失。
     * SY6970 探测：ACK（powered 扩展板在位）→ 注册；NACK（v3 载板 /
     * 未上电 v4 的预期路径）→ 不注册，仅 INFO。powered/unpowered 只由
     * ACK 表达，与 Kconfig 板型正交（pk_board.h:30-32）。两个调用都幂等。
     * 注：SY6970 无"电池在位"寄存器，空座检测需实板标定
     * （见 IMPLEMENTATION_PLAN.md B6）。 */
    power_sy6970_init();
    power_service_init();
    pk_sdcard_init();
    /* ADS-B / 本机数据落盘的 session 目录管理，须晚于 pk_sdcard_init()。
     * 阶段 3a：只建目录/开文件，不接数据源（ADS-B 解码链 / own_ship / 相位
     * 状态机是 3b 的事）。pre-unmount 静默复用 record_sink_file.c 的
     * sd_close_log_cb 转调，不额外占 pk_sdcard 的回调槽位。 */
    pk_rec_store_init();
    /* traffic.trk 生产端的非阻塞入队 + 独立写任务，须晚于 pk_rec_store_init()
     * （写任务要调 pk_rec_store_append_traffic_record()）、早于 ADS-B 链路
     * 任务起跑（下面链路任务创建之前）——链路任务的 Mode-S 解码热路径调
     * pk_rec_ingest_position/identity()，队列必须已经建好，否则
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
    /* pk_tile_loader_init() **故意不在这里**——它内部的 pk_map_store_scan()
     * 是同步的，实测扫 4 个 pmtiles 要 4 秒，卡在这里会把后面的 hosted 握手
     * 和点屏一起往后推，开机要 10 秒才出 logo。已挪到 app_main 末尾，见那边。 */
    /* 演示模式的自定义轨迹：SD 卡 /sdcard/demo/ 里有 .gpx 就换过去，没有就
     * 继续播编进固件的内置轨迹。只创建一次性后台任务、零 IO，须晚于
     * pk_sdcard_init()（任务靠 pk_sdcard_is_mounted() 决定何时开读）。 */
    pk_demo_track_sd_init();
    /* SD 航空数据库懒加载：只创建后台任务、零 IO（开机不加载是定案）。
     * 须晚于 pk_sdcard_init()——任务靠 pk_sdcard_is_mounted() 决定何时
     * 开始分块加载 /sdcard/aero/pk_aero.bin。 */
    pk_aero_db_init();
    /* SD 机型库（ICAO24 → 型号/注册号）懒加载，同样只建任务、零 IO。
     * 须晚于 pk_sdcard_init()；任务自带 12 s 静默期，避开 pk_aero 的加载
     * 窗口，不与它抢 SD 带宽。未就绪时查询返回 NULL，UI 显示 ICAO24。 */
    pk_aircraft_db_init();
    /* 以本机为中心的滚动窗口（W1 骨架，设计见
     * 窗口化数据架构设计（内部文档））。
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

    /* ADS-B 链路任务的创建**故意排到 app_main 末尾**（PFD 起来之后），
     * 不在这里。原因见末尾启动处的注释：record sinks 与 pk_rec_ingest_init()
     * 必须先就绪，Mode-S 热路径的首批报文才不会丢进空队列。 */

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
    /* 天线选择：NVS 是真源，RP2040 上电只回到安全默认，等链路握手后推下去
     * （见 config_antenna.h）。必须在 pk_adsb_link_start() 之前读出来。 */
    pk_config_antenna_load();

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

    /* I²C0 总线的创建已前移到电源链之前（SY6970 探测需要总线，
     * 见上方电源链处的说明）。
     *
     * BNO085 IMU. Failure is non-fatal — the rest of the
     * firmware (RTL-SDR, BLE, storage) keeps working without attitude. */
    pk_boot_splash_progress(pk_i18n_text(PK_TR_BOOT_STAGE_SENSORS), 1, 3);
    esp_err_t imu_err = pk_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed (%s) — PFD will run without attitude",
                 esp_err_to_name(imu_err));
    } else {
        /* 别写成 "IMU online"：这里只证明任务建起来了，BNO085 回没回话要看
         * imu 任务自己那条 bring-up 日志（失败会退避重试，见 imu_task.c）。 */
        ESP_LOGI(TAG, "BNO085 IMU task started (bring-up runs in-task)");
    }
    pk_qnh_load();     /* 从 NVS 加载 QNH,供 baro_task 立即使用 */
    pk_config_traffic_load();  /* 从 NVS 加载地图朝向 + 雷达量程 */
    pk_baro_start();   /* BMP388 on shared I²C0 */

    /* QMC5883P 磁力计诊断（WP-B Task 4）：optional 器件，缺失/探测失败
     * 只在驱动里 WARN + 计数，不影响其余功能；R1——只出原始三轴诊断，
     * 不出航向。 */
    if (qmc5883p_init(pk_i2c0_bus_get()) != ESP_OK) {
        ESP_LOGW(TAG, "QMC5883P init failed — mag diagnostics disabled");
    }

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
    /* 从 NVS 恢复本机 ICAO 绑定（用户上次在 ADS-B 列表里选的"这架是我"）。
     * 必须排在 pk_ui_init() 之后：恢复走 pk_ui_set_own_icao()，它内部
     * xSemaphoreTake(s_lock)——s_lock 在 pk_ui_init() 里才建，之前调会因
     * s_lock==NULL 静默退化（pk_ui_set_own_icao 头部的 NULL 守卫），绑定
     * 根本没恢复。sink 轮询（record_sinks_install_defaults，上面 :252）靠
     * have_last_own 标志位在首次轮询必触发，不依赖 load 的先后。 */
    pk_config_own_icao_load();

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
     * ADS-B 链路任务保持最后启动：record sinks 与 pk_rec_ingest_init()
     * 必须先就绪，Mode-S 热路径的首批报文才不会丢进空队列（原 dsp_task
     * 时代同样约束，见 pk_rec_ingest.h）。UART 不抢内部 DMA 堆，旧
     * RTL-SDR 时代的 92 KB URB 内存饥饿问题（2026-08-03 事故）不复存在；
     * 次序保持只为最小改动。
     */
    pk_adsb_link_start();

    ESP_LOGI(TAG, "ADS-B link task spawned last (free internal heap: %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
