/*
 * rf1090_log.c — 见 rf1090_log.h。结构刻意与 power_log.c 保持一致：
 * 同样的"无卡待命 / 挂载代数判换卡 / boot 标记 / fflush+fsync / 低优先级任务"，
 * 两个记录器的行为一致，读日志的人不必记两套规矩。
 */
#include "rf1090_log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adsb_link.h"        /* ADSB_LINK_HEALTH_FIELDS 与字段下标 */
#include "adsb_link_task.h"
#include "config_antenna.h"
#include "config_diaglog.h"
#include "gps.h"
#include "pk_clock.h"
#include "pk_sdcard.h"

static const char *TAG = "rf1090log";

#define RFLOG_PATH      "/sdcard/rf1090.csv"
/* 10 s/行：射频调试要比电源细得多——一轮门限扫描每点才停 10 s，60 s 一行
 * 会把整轮扫描压成两三行，什么都看不出来。 */
#define RFLOG_PERIOD_MS 10000
/* 容量上限，见 config_diaglog.h 的隐私与容量一节。 */
#define RFLOG_MAX_BYTES (512 * 1024)

#define RFLOG_HEADER \
    "wall_s,uptime_s,ant,tl_permille,d_pre,d_f56,d_f112,d_noise,d_ovr," \
    "degraded,lat,lon,gps_fix,gps_sats,gps_view\n"

/* 上一拍的累计值，用来出增量。first=true 时本拍只记锚点不写增量行——
 * 开机第一拍的"增量"其实是从 RP2040 上电到现在的总量，写出去会是一个
 * 突兀的大数，看图的人会以为那一刻爆发了一堆帧。 */
static uint32_t s_prev[ADSB_LINK_HEALTH_FIELDS];
static bool     s_have_prev;

static FILE *open_log(void)
{
    /* 新文件才写表头：追加模式下每次开机都写一遍的话，CSV 中间会散落
     * 多行表头，pandas/Excel 读进来全成了字符串列。 */
    FILE *probe = fopen(RFLOG_PATH, "r");
    const bool fresh = (probe == NULL);
    if (probe) fclose(probe);

    FILE *fp = fopen(RFLOG_PATH, "a");
    if (fp && fresh) fputs(RFLOG_HEADER, fp);
    return fp;
}

/* uint32 累计值的差分。计数在 RP2040 上单调递增、模 2^32 回绕，
 * 无符号减法天然正确；但 RP2040 **单独重启**时它们会归零，此时差值会是一个
 * 接近 2^32 的巨大数——那不是"收了 40 亿帧"，是对面重启了。夹成 0 并不掩盖
 * 问题：同一行的 uptime 和 degraded 仍在，真要追查有据可依。 */
static uint32_t delta(uint32_t now, uint32_t prev)
{
    const uint32_t d = now - prev;
    return (d > 0x80000000u) ? 0 : d;
}

static void write_row(FILE *fp, const uint32_t h[ADSB_LINK_HEALTH_FIELDS],
                      bool have_rf)
{
    pk_gps_state_t g;
    const bool have_gps = pk_gps_get(&g);

    /* 天线/门限优先取 RP 实报的**实际生效值**：扫描期间 RP 逐点改门限、
     * CDC 'a'/'A' 直接改天线，都不经过 P4，P4 本地存的是用户设定值——而
     * 这行计数正是在 RP 当前配置下产生的。对面是旧 RP2040（只发 11 个字段）
     * 时才回退到 P4 本地值。理由见 rf1090_log.h「计数与配置同一行」。 */
    const int tl = have_rf
        ? (int)h[ADSB_LINK_HEALTH_IDX_THRESHOLD]
        : pk_thresh_1090_get();
    const char *ant = (have_rf
        ? (h[ADSB_LINK_HEALTH_IDX_ANT_1090] != 0)
        : (pk_ant_1090_get() == PK_ANT_1090_EXTERNAL))
        ? "EXT-J6" : "ONBOARD-IFA";
    /* 墙钟只在已校时后才有意义，否则写 0 —— 写一个未校时的 1970 时间戳，
     * 事后会被当成真实时刻去和别的记录对齐。uptime 始终可信，两个都留。 */
    time_t wall = 0;
    if (pk_clock_is_synced()) wall = time(NULL);

    fprintf(fp, "%lld,%lld,%s,%d,%lu,%lu,%lu,%lu,%lu,%lu,",
            (long long)wall,
            (long long)(esp_timer_get_time() / 1000000),
            ant,
            tl,
            (unsigned long)delta(h[0], s_prev[0]),
            (unsigned long)delta(h[1], s_prev[1]),
            (unsigned long)delta(h[2], s_prev[2]),
            (unsigned long)delta(h[4], s_prev[4]),
            (unsigned long)delta(h[5], s_prev[5]),
            (unsigned long)h[3]);
    if (have_gps && g.have_fix)
        fprintf(fp, "%.6f,%.6f,1,%d,%d\n",
                g.lat, g.lon, g.sats, g.sats_in_view);
    else
        fprintf(fp, ",,0,%d,%d\n",
                have_gps ? g.sats : 0, have_gps ? g.sats_in_view : 0);
}

void rf1090_log_dump(void)
{
    FILE *fp = fopen(RFLOG_PATH, "r");
    if (!fp) {
        ESP_LOGW(TAG, "%s 打不开（没插卡，或还没记过）", RFLOG_PATH);
        return;
    }
    ESP_LOGI(TAG, "===== %s 开始 =====", RFLOG_PATH);
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        ESP_LOGI(TAG, "%s", line);
        n++;
        /* 让出 CPU：几千行连续 ESP_LOGI 会把串口和看门狗一起压垮。 */
        if ((n % 20) == 0) vTaskDelay(pdMS_TO_TICKS(10));
    }
    fclose(fp);
    ESP_LOGI(TAG, "===== %s 结束（%d 行）=====", RFLOG_PATH, n);
}

static void rf1090_log_task(void *arg)
{
    (void)arg;
    bool     boot_marked = false;
    uint32_t last_gen    = 0;
    bool     warned      = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RFLOG_PERIOD_MS));

        uint32_t h[ADSB_LINK_HEALTH_FIELDS];
        const int nf = pk_adsb_rp_health(h);
        if (nf <= 0) {
            /* RP2040 还没上报过——链路没起来或对面没在跑。不写行，也不把
             * s_have_prev 清掉：链路恢复后接着原来的锚点算增量即可。 */
            continue;
        }

        /* 总开关（默认关）。关着的时候连锚点都不推进——重新打开后第一行
         * 的增量应当从"打开那一刻"算起，而不是把关闭期间的量一次性倒出来。 */
        if (!pk_diaglog_enabled()) {
            s_have_prev = false;
            boot_marked = false;
            continue;
        }

        if (!pk_sdcard_is_mounted()) {
            if (!warned) {
                ESP_LOGI(TAG, "SD 未挂载，1090 记录待命（插卡后自动开始）");
                warned = true;
            }
            boot_marked = false;
            /* 无卡期间照常推进锚点：否则插卡后的第一行会把无卡那段的
             * 总量全算进一个 10 s 周期里。 */
            memcpy(s_prev, h, sizeof s_prev);
            s_have_prev = true;
            continue;
        }
        warned = false;

        /* 拔了又插回来算新的一段。用挂载代数而不是 is_mounted 电平——
         * 卡不在的窗口可能整个落在两次轮询之间，边沿检测抓不到。 */
        const uint32_t gen = pk_sdcard_mount_generation();
        if (gen != last_gen) { last_gen = gen; boot_marked = false; }

        /* 容量上限：10 s 一行、约 100 B，512 KB 够记 14 小时以上。超了就
         * 整个重来而不是保留后半——CSV 从中间截断会留下半行，读的人拿到的
         * 是一个格式坏掉的文件。 */
        {
            struct stat st;
            if (stat(RFLOG_PATH, &st) == 0 && st.st_size >= RFLOG_MAX_BYTES) {
                unlink(RFLOG_PATH);
                boot_marked = false;
                ESP_LOGI(TAG, "%s 超过 %d KB，已重新开始",
                         RFLOG_PATH, RFLOG_MAX_BYTES / 1024);
            }
        }

        FILE *fp = open_log();
        if (fp == NULL) {
            ESP_LOGW(TAG, "打开 %s 失败，下一拍重试", RFLOG_PATH);
            continue;
        }

        if (!boot_marked) {
            fprintf(fp, "# boot reset=%d\n", (int)esp_reset_reason());
            boot_marked = true;
            ESP_LOGI(TAG, "1090 记录开始写入 %s（%d s/行）",
                     RFLOG_PATH, RFLOG_PERIOD_MS / 1000);
        }

        if (s_have_prev)
            write_row(fp, h, nf >= ADSB_LINK_HEALTH_FIELDS);
        memcpy(s_prev, h, sizeof s_prev);
        s_have_prev = true;

        /* fflush 之后**必须** fsync：FATFS 上目录项里的文件大小要 f_sync 才
         * 更新，少了它掉电丢整段、而且平时完全看不出来。10 s 一次付得起。 */
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
    }
}

void rf1090_log_init(void)
{
    static bool started;
    if (started) return;
    started = true;

    /* 栈 3072、优先级 1：与 power_log 同规格。fprintf 到 FATFS 比看上去吃栈
     * （VFS + FatFs + 扇区缓冲）；这是诊断记录，绝不该跟 PFD 渲染或 1090
     * 收帧抢 CPU。 */
    if (xTaskCreate(rf1090_log_task, "rf1090log", 3072, NULL, 1, NULL) != pdPASS)
        ESP_LOGW(TAG, "任务创建失败——本次开机不记录 1090 日志");
}
