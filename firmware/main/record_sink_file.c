/*
 * record_sink_file.c — append-only file sink, LittleFS backend.
 *
 * Lives entirely off the dsp_task hot path: write() formats the ts-line
 * into a small struct, puts it on a queue, returns. A dedicated
 * file_writer_task drains the queue, appending lines to the currently
 * open ts file under /storage/. When the active file exceeds
 * FILE_ROTATE_BYTES we close it, open a new pilot_kit_ts_<N+1>.txt, and
 * prune the oldest siblings to keep at most FILE_KEEP_COUNT around.
 *
 * Why this design:
 *   - Pilot-Kit/scripts/adsb_to_track.py globs pilot_kit_ts_*.txt and
 *     pyModeS-decodes each line, so the firmware's output is the exact
 *     wire format the Python pipeline already expects.
 *   - LittleFS has built-in wear levelling and power-cut tolerance. We
 *     do not fsync per line — power-loss cost is at most the current
 *     ~1 MiB file's tail, which is fine for archival recordings.
 *   - The user mentioned wanting a future config toggle to SD card.
 *     Storage selection sits behind one function — file_mount() — so
 *     adding an SDMMC backend is purely additive (no API churn for
 *     callers of record_sink_file_create()).
 *   - SD 后端的热插拔（2026-08-01 回归排查补课）：写句柄是**常开**的
 *     （空闲只 fflush，轮转才 fclose），而 IDF 的 esp_vfs_fat_sdcard_unmount
 *     会无条件 free 掉含 FIL 数组的 fat_ctx——拔卡时句柄若还开着就是
 *     use-after-free。所以句柄状态提升为 static + s_file_lock 保护，
 *     pk_sdcard 的 pre-unmount 回调在卸载前收走它；重插后 writer 收到
 *     记录时自动重扫序号、开新文件续录。flash LittleFS 后端不受影响。
 */

#include "record_sink.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_littlefs.h"

#include "config_storage.h"
#include "pk_sdcard.h"

#define FILE_MOUNT_POINT    "/storage"
#define FILE_PARTITION      "storage"
#define FILE_NAME_PREFIX    "pilot_kit_ts_"
#define FILE_NAME_SUFFIX    ".txt"
#define FILE_QUEUE_DEPTH    256
#define FILE_ROTATE_BYTES   (1 * 1024 * 1024)   /* flash: 1 MiB per file */
#define FILE_KEEP_COUNT     12                  /* count ceiling; 10 MiB partition is the practical limit */
#define SD_ROTATE_BYTES     (16 * 1024 * 1024)  /* SD: 16 MiB per file */
#define SD_KEEP_COUNT       64                  /* SD: 64 × 16 MiB ≈ 1 GiB */
#define FILE_WRITER_STACK   4096

static const char *TAG = "rec_file";

/* 后端在 record_sink_file_create() 时一次性选定（flash LittleFS 或
 * microSD FATFS），之后不再变。SD 选中但缺卡时回退 flash。 */
static const char *s_base_path    = FILE_MOUNT_POINT;
static size_t      s_rotate_bytes = FILE_ROTATE_BYTES;
static int         s_keep_count   = FILE_KEEP_COUNT;
static bool        s_on_sdcard    = false;

typedef struct {
    int64_t ts_ms;
    uint8_t hex_len;
    char    hex[RECORD_HEX_MAX_LEN + 1];
} file_record_t;

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;
static volatile uint32_t s_written;

/* 当前写句柄状态。原本是 writer 任务的局部变量，热插拔修复后提升为
 * static：pk_sdcard 的 pre-unmount 回调要在 sd_detect 任务上收走这个
 * 句柄（拔卡时它若还开着，esp_vfs_fat_sdcard_unmount 的无条件
 * free(fat_ctx) 就是 use-after-free）。s_file_lock 罩住 writer 循环里
 * 所有 fp 操作与轮转的目录扫描/清理——回调 take 到锁即等到在途 SD I/O
 * 退出。xQueueReceive 在锁外，排队的生产者不受影响。 */
static SemaphoreHandle_t s_file_lock;
static FILE  *s_fp;
static size_t s_fp_bytes;
static int    s_fp_seq;
static char   s_fp_path[64];

/* --- Filename helpers ------------------------------------------------- */

static int parse_seq(const char *name)
{
    /* Expects FILE_NAME_PREFIX<digits>FILE_NAME_SUFFIX. Returns -1 if not. */
    size_t prefix_len = strlen(FILE_NAME_PREFIX);
    size_t suffix_len = strlen(FILE_NAME_SUFFIX);
    if (strncmp(name, FILE_NAME_PREFIX, prefix_len) != 0) return -1;
    size_t total = strlen(name);
    if (total < prefix_len + suffix_len + 1) return -1;
    if (strcmp(name + total - suffix_len, FILE_NAME_SUFFIX) != 0) return -1;

    int seq = 0;
    for (size_t i = prefix_len; i < total - suffix_len; ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        seq = seq * 10 + (name[i] - '0');
    }
    return seq;
}

static void build_path(char *out, size_t cap, int seq)
{
    snprintf(out, cap, "%s/" FILE_NAME_PREFIX "%d" FILE_NAME_SUFFIX,
             s_base_path, seq);
}

/* Scan /storage for existing pilot_kit_ts_*.txt files. *max_seq is the
 * highest sequence number found (or 0 if none); *count is the number of
 * matching files. */
static void scan_existing(int *max_seq, int *count)
{
    *max_seq = 0;
    *count = 0;

    DIR *d = opendir(s_base_path);
    if (d == NULL) {
        ESP_LOGW(TAG, "opendir(%s) failed: %s", s_base_path, strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int seq = parse_seq(ent->d_name);
        if (seq >= 0) {
            (*count)++;
            if (seq > *max_seq) *max_seq = seq;
        }
    }
    closedir(d);
}

/* Delete oldest pilot_kit_ts_*.txt files until at most `keep` remain. */
static void prune_oldest(int keep)
{
    while (1) {
        int max_seq = 0, count = 0;
        scan_existing(&max_seq, &count);
        if (count <= keep) return;

        /* Find the smallest seq present and unlink it. */
        DIR *d = opendir(s_base_path);
        if (d == NULL) return;
        int min_seq = max_seq;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            int seq = parse_seq(ent->d_name);
            if (seq > 0 && seq < min_seq) min_seq = seq;
        }
        closedir(d);

        char path[64];
        build_path(path, sizeof(path), min_seq);
        if (unlink(path) != 0) {
            ESP_LOGW(TAG, "unlink(%s) failed: %s", path, strerror(errno));
            return;  /* avoid infinite loop on persistent error */
        }
        ESP_LOGI(TAG, "pruned %s", path);
    }
}

/* --- Mount ------------------------------------------------------------ */

static bool file_mount(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path              = FILE_MOUNT_POINT,
        .partition_label        = FILE_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(FILE_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u KiB used",
                 FILE_MOUNT_POINT,
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    } else {
        ESP_LOGI(TAG, "LittleFS mounted at %s", FILE_MOUNT_POINT);
    }
    return true;
}

/* --- Writer task ------------------------------------------------------ */

/* 重扫序号并开一个全新文件（调用方须持 s_file_lock）。开机首开与拔卡后
 * 续录共用：重插的可能是另一张卡，序号必须以卡上实际文件重新起算，
 * 不能沿用内存里的旧值。 */
static bool file_open_fresh_locked(void)
{
    int max_seq = 0, existing = 0;
    scan_existing(&max_seq, &existing);
    s_fp_seq = max_seq + 1;
    build_path(s_fp_path, sizeof(s_fp_path), s_fp_seq);
    s_fp       = fopen(s_fp_path, "w");
    s_fp_bytes = 0;
    return s_fp != NULL;
}

static void file_writer_task(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_file_lock, portMAX_DELAY);
    bool opened = file_open_fresh_locked();
    xSemaphoreGive(s_file_lock);
    if (!opened) {
        if (!s_on_sdcard) {
            /* flash LittleFS 开不了文件没有"稍后会好"的可能，维持原行为。 */
            ESP_LOGE(TAG, "fopen(%s) failed: %s — file sink disabled",
                     s_fp_path, strerror(errno));
            vTaskDelete(NULL);
        }
        /* SD 后端多半是插拔时序的瞬态问题：任务留着，下面循环的续录
         * 路径会在卡回来后自动重开。 */
        ESP_LOGW(TAG, "fopen(%s) failed: %s — will retry when card returns",
                 s_fp_path, strerror(errno));
    } else {
        ESP_LOGI(TAG, "logging ADS-B to %s (rotate every %u KiB, keep %d files)",
                 s_fp_path, (unsigned)(s_rotate_bytes / 1024), s_keep_count);
    }

    char line[80];

    while (1) {
        file_record_t rec;
        if (xQueueReceive(s_queue, &rec, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* Idle tick — flush so a power-cut soon after silence doesn't
             * lose the last few minutes' worth of data. */
            xSemaphoreTake(s_file_lock, portMAX_DELAY);
            if (s_fp != NULL && s_fp_bytes > 0) {
                fflush(s_fp);
            }
            xSemaphoreGive(s_file_lock);
            continue;
        }

        int n = snprintf(line, sizeof(line), "%" PRId64 " *%s;\n",
                         rec.ts_ms, rec.hex);
        if (n < 0 || (size_t)n >= sizeof(line)) continue;

        xSemaphoreTake(s_file_lock, portMAX_DELAY);

        if (s_fp == NULL) {
            /* 句柄被 pre-unmount 回调收走（拔卡）或 SD 首开失败。卡回来了
             * 就重扫序号开新文件续录（重挂后日志自动续上）；还没回来就丢
             * 弃这条，计入 s_dropped 沿用队列满的节流口径。flash 后端到不
             * 了这里——它的句柄缺失只发生在轮转失败，而那条路直接结束任务。 */
            bool resumed = s_on_sdcard && pk_sdcard_is_mounted() &&
                           file_open_fresh_locked();
            if (!resumed) {
                xSemaphoreGive(s_file_lock);
                uint32_t d = ++s_dropped;
                if ((d & 0xFF) == 0) {
                    ESP_LOGW(TAG, "no log file — %lu records dropped total",
                             (unsigned long)d);
                }
                continue;
            }
            ESP_LOGI(TAG, "card back — resumed logging to %s", s_fp_path);
        }

        if (fwrite(line, 1, (size_t)n, s_fp) != (size_t)n) {
            ESP_LOGW(TAG, "fwrite failed: %s", strerror(errno));
            xSemaphoreGive(s_file_lock);
            continue;
        }
        s_fp_bytes += (size_t)n;
        s_written++;

        if (s_fp_bytes >= s_rotate_bytes) {
            fclose(s_fp);
            s_fp = NULL;
            s_fp_seq++;
            build_path(s_fp_path, sizeof(s_fp_path), s_fp_seq);
            s_fp       = fopen(s_fp_path, "w");
            s_fp_bytes = 0;
            if (s_fp == NULL) {
                xSemaphoreGive(s_file_lock);
                if (!s_on_sdcard) {
                    ESP_LOGE(TAG, "rotation fopen(%s) failed: %s — file sink ending",
                             s_fp_path, strerror(errno));
                    vTaskDelete(NULL);
                }
                /* SD：留 NULL，让上面的续录路径接手（多半正赶上拔卡）。 */
                ESP_LOGW(TAG, "rotation fopen(%s) failed: %s — will retry",
                         s_fp_path, strerror(errno));
                continue;
            }
            ESP_LOGI(TAG, "rotated to %s", s_fp_path);
            /* prune 的 opendir/unlink 也是 SD I/O，必须留在锁内——否则
             * 拔卡回调的栅栏等不到它，正在 readdir 的目录句柄同样会踩到
             * unmount 后被 free 的 fat_ctx。 */
            prune_oldest(s_keep_count);
        }
        xSemaphoreGive(s_file_lock);
    }
}

/* pk_sdcard 卸载前回调：收走常开写句柄。契约见 pk_sdcard.h——只拿本模块
 * 的 s_file_lock；take 到手即说明 writer 的 fwrite/轮转/prune 已退出临界
 * 区（最坏等一次已拔卡上的 sdmmc 命令超时，有限）。fclose 即使因卡已不
 * 在而 f_close 报错，IDF 的 vfs_fat_close 也会无条件释放 FIL 槽与 fd，
 * 所以回调返回时本模块保证无打开的 SD fd。base path 判定是防御：本回调
 * 只在 SD 后端注册，但句柄归属再核对一次不吃亏。
 *
 * 顺风车。两个模块用各自的锁保护各自的句柄，互不干扰，只是共享同一次
 * "卸载前静默" 的调用时机。 */
static void sd_close_log_cb(void)
{
    xSemaphoreTake(s_file_lock, portMAX_DELAY);
    if (s_fp != NULL && strcmp(s_base_path, pk_sdcard_mount_point()) == 0) {
        fclose(s_fp);
        s_fp = NULL;
        ESP_LOGW(TAG, "card removing — log file closed (resume on re-insert)");
    }
    xSemaphoreGive(s_file_lock);
}

/* --- Sink interface --------------------------------------------------- */

static bool file_write(record_sink_t *self, const record_t *rec)
{
    (void)self;
    if (s_queue == NULL) return false;

    file_record_t copy = {
        .ts_ms   = rec->ts_ms,
        .hex_len = rec->hex_len,
    };
    memcpy(copy.hex, rec->hex, rec->hex_len + 1);

    if (xQueueSend(s_queue, &copy, 0) != pdTRUE) {
        uint32_t d = ++s_dropped;
        /* Log every 256th drop so we notice back-pressure without flooding. */
        if ((d & 0xFF) == 0) {
            ESP_LOGW(TAG, "queue full — %lu records dropped total",
                     (unsigned long)d);
        }
        return false;
    }
    return true;
}

static record_sink_t s_file_sink = {
    .name  = "file_littlefs",
    .write = file_write,
    .priv  = NULL,
};

bool record_sink_file_uses_sd(void)
{
    return s_on_sdcard;
}

bool record_sink_file_stats(uint32_t *out_written, uint32_t *out_dropped)
{
    if (s_queue == NULL) return false;   /* sink 没起来(挂载失败) */
    if (out_written) *out_written = s_written;
    if (out_dropped) *out_dropped = s_dropped;
    return true;
}

record_sink_t *record_sink_file_create(void)
{
    /* 后端选择：设置选 microSD 且卡已挂载（pk_sdcard_init 已先行）→
     * 写 /sdcard，轮转上限放大；否则回退 flash LittleFS。 */
    if (pk_log_store_get() == PK_LOG_STORE_SD && pk_sdcard_is_mounted()) {
        s_base_path        = pk_sdcard_mount_point();
        s_rotate_bytes     = SD_ROTATE_BYTES;
        s_keep_count       = SD_KEEP_COUNT;
        s_on_sdcard        = true;
        s_file_sink.name   = "file_sdcard";
        ESP_LOGI(TAG, "log store: microSD (%s)", s_base_path);
    } else {
        /* flash 后端不注册 pre-unmount 回调：句柄不在 SD 上，拔卡与它无关。 */
        if (pk_log_store_get() == PK_LOG_STORE_SD) {
            ESP_LOGW(TAG, "log store set to microSD but no card — "
                          "falling back to flash LittleFS");
        }
        if (!file_mount()) {
            return NULL;
        }
    }

    /* 锁要先于 writer 任务与回调注册就位——两者都无条件 take 它。 */
    s_file_lock = xSemaphoreCreateMutex();
    if (s_file_lock == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return NULL;
    }
    /* 仅 SD 后端注册：main.c 里本函数在 pk_sdcard_init() 之后，时序安全。 */
    if (s_on_sdcard) {
        pk_sdcard_register_pre_unmount_cb(sd_close_log_cb);
    }

    s_queue = xQueueCreate(FILE_QUEUE_DEPTH, sizeof(file_record_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return NULL;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        file_writer_task, "rec_file", FILE_WRITER_STACK, NULL, 3, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(rec_file) failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return NULL;
    }

    s_file_sink.priv = (void *)s_base_path;
    return &s_file_sink;
}
