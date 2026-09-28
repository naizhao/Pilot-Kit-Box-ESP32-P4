/*
 * power_log.c — 见 power_log.h（为什么需要、载板供电陷阱、写入策略）。
 */
#include "power_log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <time.h>

#include "esp_log.h"
#include "esp_system.h"   /* esp_reset_reason —— boot 标记行 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pk_clock.h"
#include "config_diaglog.h"
#include "pk_sdcard.h"
#include "power_service.h"
#include "power_sy6970.h"

static const char *TAG = "pwrlog";

#define PWRLOG_PATH      "/sdcard/power.csv"
#define PWRLOG_PERIOD_MS 60000      /* 60 s/行，见头文件写入策略 */

/* 表头与列含义。改列必须同时改这里和写行处——两处走偏的症状是"CSV 打开
 * 后每列都错位"，而那时数据已经采了几小时，重采成本很高。 */
#define PWRLOG_HEADER \
    "uptime_s,wall,batt_mv,sys_mv,pct,pct_valid,ichg_ma,chg,term,vbus," \
    "ntc_fault,ntc_pct_x1000,therm,reg0c\n"

/*
 * 打开（或新建）日志文件。文件不存在时补写表头——用 "a" 打开拿不到
 * "是不是新文件"这个信息，所以先用 "r" 探一次。
 * 返回 NULL 表示这一拍写不成（无卡/打开失败），调用方下一拍再试。
 */
static FILE *open_log(void)
{
    bool fresh = true;
    FILE *probe = fopen(PWRLOG_PATH, "r");
    if (probe != NULL) { fresh = false; fclose(probe); }

    FILE *fp = fopen(PWRLOG_PATH, "a");
    if (fp == NULL) return NULL;
    if (fresh) fputs(PWRLOG_HEADER, fp);
    return fp;
}

/*
 * 记一行。
 *
 * 数据来自两处：公共快照（power_service）给电量/充电/VBUS 这些跨 backend
 * 的口径，SY6970 诊断快照给 SYS/NTC/ICHG/REG0C 这些只有这颗芯片才有的
 * 原始值。两者都取不到时写 0，不编造。
 *
 * 墙钟列：校时后才有意义（GPS 或 BLE，见 pk_clock.h）。没校时就写 0，
 * 这时只能靠 uptime 对齐——而 uptime 每次开机归零，所以下面每次启动都会
 * 先写一行 `# boot` 注释，用来在 CSV 里切分多次开机的数据段。
 */
static void write_row(FILE *fp)
{
    const power_snapshot_t s = power_service_snapshot();
    sy6970_diag_t d;
    const bool has_sy = sy6970_diag_get(&d);

    const int64_t up_s = esp_timer_get_time() / 1000000;
    time_t wall = 0;
    if (pk_clock_is_synced()) wall = time(NULL);

    fprintf(fp, "%lld,%lld,%u,%u,%u,%d,%u,%d,%d,%d,%u,%lu,%d,0x%02X\n",
            (long long)up_s, (long long)wall,
            (unsigned)s.batt_mv,
            has_sy ? (unsigned)d.st.sys_mv : 0u,
            (unsigned)s.pct_est, (int)s.pct_valid,
            has_sy ? (unsigned)d.st.ichg_ma : 0u,
            (int)s.charging,
            has_sy ? (int)d.st.term_done : 0,
            (int)s.vbus_present,
            has_sy ? (unsigned)d.st.ntc_fault : 0u,
            has_sy ? (unsigned long)d.st.ntc_pct_x1000 : 0ul,
            has_sy ? (int)d.st.therm_reg : 0,
            has_sy ? d.regs[1] : 0);
}

/*
 * 开机时把已有的 CSV 打到串口一次。
 *
 * 存在的理由：这块板做放电标定必须拔掉 H1（见头文件的载板供电陷阱），
 * 拔了就没串口，数据只能落卡。等设备放空关机、插电回来之后，总得有个
 * 办法把卡上的东西拿出来——拆卡读是一条路，但那要读卡器、要拆壳，而
 * 排障现场往往只有一根 USB 线。
 *
 * 只在开机 dump 一次，之后本次运行不再重复。文件过大时只报大小不打内容
 * ——115200 下每 KB 约 0.09 s，一个通宵的记录（1440 行 ≈ 90 KB）就要刷
 * 8 秒串口，把开机日志淹掉得不偿失；真到那个量级该拆卡读。
 */
#define PWRLOG_DUMP_MAX_BYTES 65536      /* 约 1000 行，≈6 s 串口时间 */

static void dump_existing(void)
{
    FILE *fp = fopen(PWRLOG_PATH, "r");
    if (fp == NULL) return;                 /* 首次运行，没有历史，正常 */

    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return; }
    if (sz > PWRLOG_DUMP_MAX_BYTES) {
        ESP_LOGW(TAG, "%s 已有 %ld 字节，超过 dump 上限，不打印内容"
                      "（请拆卡读取）", PWRLOG_PATH, sz);
        fclose(fp);
        return;
    }
    rewind(fp);

    ESP_LOGI(TAG, "===== %s 开始（%ld 字节）=====", PWRLOG_PATH, sz);
    char line[192];
    int n = 0;
    while (fgets(line, sizeof line, fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        ESP_LOGI(TAG, "CSV %s", line);
        n++;
    }
    fclose(fp);
    ESP_LOGI(TAG, "===== %s 结束（%d 行）=====", PWRLOG_PATH, n);
}

static void power_log_task(void *arg)
{
    (void)arg;
    bool     boot_marked = false;
    uint32_t last_gen    = 0;
    bool     warned      = false;

    for (;;) {
        /* 与 rf1090_log / pk_syslog 共用同一个总开关（默认关）：用户只该有
         * 一个地方能关掉全部诊断记录，而不是每个模块各有各的开关。 */
        if (!pk_diaglog_enabled()) {
            boot_marked = false;
            vTaskDelay(pdMS_TO_TICKS(PWRLOG_PERIOD_MS));
            continue;
        }

        if (!pk_sdcard_is_mounted()) {
            /* 无卡是常态（排障时常先开机后插卡），只在状态翻转时说一次，
             * 不以 1 min 的节奏刷日志。 */
            if (!warned) {
                ESP_LOGI(TAG, "SD 未挂载，电源记录待命（插卡后自动开始）");
                warned = true;
            }
            boot_marked = false;     /* 换卡/重挂后要重新打 boot 标记 */
            vTaskDelay(pdMS_TO_TICKS(PWRLOG_PERIOD_MS));
            continue;
        }
        warned = false;
        if (!pk_sdcard_io_acquire()) continue;

        /* 卡拔了又插回来算新的一段：挂载代数变了就重打 boot 标记。
         * 用代数而不是 is_mounted 的电平——卡不在的窗口可能整个落在两次
         * 轮询之间，电平边沿检测抓不到（pk_sdcard.h:46 的合同）。 */
        const uint32_t gen = pk_sdcard_mount_generation();
        if (gen != last_gen) { last_gen = gen; boot_marked = false; }

        FILE *fp = open_log();
        if (fp == NULL) {
            pk_sdcard_io_release();
            ESP_LOGW(TAG, "打开 %s 失败，下一拍重试", PWRLOG_PATH);
            vTaskDelay(pdMS_TO_TICKS(PWRLOG_PERIOD_MS));
            continue;
        }

        if (!boot_marked) {
            dump_existing();      /* 先把历史吐出来，再往后追加 */
            /* 每次开机/换卡插一行注释：uptime 会归零，没有这条就分不清
             * "设备重启了"和"时间倒流了"。CSV 读取方按 '#' 跳过即可。 */
            fprintf(fp, "# boot reset=%d\n", (int)esp_reset_reason());
            boot_marked = true;
            ESP_LOGI(TAG, "电源记录开始写入 %s（%d s/行）",
                     PWRLOG_PATH, PWRLOG_PERIOD_MS / 1000);
        }
        write_row(fp);

        /* fflush 之后**必须** fsync：FATFS 上目录项里的文件大小要 f_sync
         * 才更新，少了它掉电丢整段、而且平时完全看不出来（教训与实证见
         * pk_rec_store_fs.c:568）。60 s 一次属于低频点，付得起。 */
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
        pk_sdcard_io_release();

        vTaskDelay(pdMS_TO_TICKS(PWRLOG_PERIOD_MS));
    }
}

void power_log_init(void)
{
    static bool started;
    if (started) return;
    started = true;

    /* 栈 3072：fprintf 到 FATFS 这条路径比看上去吃栈（VFS + FatFs + 扇区
     * 缓冲）。优先级 1（低于所有实时任务）——这是诊断记录，晚一秒无所谓，
     * 绝不该跟 PFD 渲染或 1090 收帧抢 CPU。 */
    if (xTaskCreate(power_log_task, "pwrlog", 3072, NULL, 1, NULL) != pdPASS)
        ESP_LOGW(TAG, "任务创建失败——本次开机不记录电源日志");
}
