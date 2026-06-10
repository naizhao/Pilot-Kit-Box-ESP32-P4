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

static void file_writer_task(void *arg)
{
    (void)arg;

    int seq = 0, existing = 0;
    scan_existing(&seq, &existing);
    seq++;  /* fresh file for this boot session */

    char    path[64];
    build_path(path, sizeof(path), seq);
    FILE   *fp = fopen(path, "w");
    size_t  bytes = 0;
    if (fp == NULL) {
        ESP_LOGE(TAG, "fopen(%s) failed: %s — file sink disabled",
                 path, strerror(errno));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "logging ADS-B to %s (rotate every %u KiB, keep %d files)",
             path, (unsigned)(s_rotate_bytes / 1024), s_keep_count);

    char line[80];

    while (1) {
        file_record_t rec;
        if (xQueueReceive(s_queue, &rec, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* Idle tick — flush so a power-cut soon after silence doesn't
             * lose the last few minutes' worth of data. */
            if (fp != NULL && bytes > 0) {
                fflush(fp);
            }
            continue;
        }

        int n = snprintf(line, sizeof(line), "%" PRId64 " *%s;\n",
                         rec.ts_ms, rec.hex);
        if (n < 0 || (size_t)n >= sizeof(line)) continue;

        if (fwrite(line, 1, (size_t)n, fp) != (size_t)n) {
            ESP_LOGW(TAG, "fwrite failed: %s", strerror(errno));
            continue;
        }
        bytes += (size_t)n;
        s_written++;

        if (bytes >= s_rotate_bytes) {
            fclose(fp);
            seq++;
            build_path(path, sizeof(path), seq);
            fp = fopen(path, "w");
            bytes = 0;
            if (fp == NULL) {
                ESP_LOGE(TAG, "rotation fopen(%s) failed — file sink ending",
                         path, strerror(errno));
                vTaskDelete(NULL);
            }
            ESP_LOGI(TAG, "rotated to %s", path);
            prune_oldest(s_keep_count);
        }
    }
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
        if (pk_log_store_get() == PK_LOG_STORE_SD) {
            ESP_LOGW(TAG, "log store set to microSD but no card — "
                          "falling back to flash LittleFS");
        }
        if (!file_mount()) {
            return NULL;
        }
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
