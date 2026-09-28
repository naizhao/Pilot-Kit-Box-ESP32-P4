/* pk_syslog.c — 见 pk_syslog.h。 */
#include "pk_syslog.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "config_diaglog.h"
#include "pk_sdcard.h"

static const char *TAG = "syslog";

#define SYSLOG_PATH      "/sdcard/p4.log"
#define SYSLOG_OLD_PATH  "/sdcard/p4.old.log"
/* 单文件上限 512 KB，加上轮转保留的一份，最多占用 1 MB。够放十几个小时的
 * 正常日志，而且是个即使用户开着不管也不会把卡写满的数。 */
#define SYSLOG_MAX_BYTES (512 * 1024)
#define SYSLOG_RING      (8 * 1024)
#define SYSLOG_LINE_MAX  256
#define SYSLOG_FLUSH_MS  2000

static StreamBufferHandle_t s_ring;
static vprintf_like_t       s_orig;
static atomic_uint          s_dropped;
/* s_writing 从取得 SD lease 前一直覆盖到 fclose/lease release 后；调用方只有
 * 同时看见 ring 为空且 s_writing=false，才能断言刚才入队的字节已越过
 * FATFS。s_last_flush_ok 防止“队列已经被失败写入吃掉”被误报成成功。 */
static atomic_bool          s_writing;
static atomic_bool          s_last_flush_ok;

unsigned pk_syslog_dropped(void)
{
    return atomic_load_explicit(&s_dropped, memory_order_relaxed);
}

bool pk_syslog_flush(uint32_t timeout_ms)
{
    /* 用户没有打开诊断记录时没有“必须落盘”的承诺，也不能因此阻止关机。 */
    if (!pk_diaglog_enabled()) return true;
    if (!s_ring || !pk_sdcard_is_mounted()) return false;

    uint32_t waited_ms = 0;
    for (;;) {
        if (xStreamBufferBytesAvailable(s_ring) == 0 &&
            !atomic_load_explicit(&s_writing, memory_order_acquire)) {
            return atomic_load_explicit(&s_last_flush_ok,
                                        memory_order_acquire);
        }
        if (waited_ms >= timeout_ms) return false;
        const uint32_t step_ms = (timeout_ms - waited_ms < 10)
                               ? timeout_ms - waited_ms : 10;
        if (step_ms == 0) return false;
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited_ms += step_ms;
    }
}

/*
 * 日志钩子。两件事：原样转给串口（行为完全不变），再复制一份进环形缓冲。
 *
 * 这里**绝不碰文件**——ESP_LOG 可能在任何任务里被调用，在钩子里 fwrite 会
 * 把调用方阻塞在 FATFS 上，一条日志就能拖慢 1090 收帧。
 */
static int syslog_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    const int n = s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);

    if (s_ring && pk_diaglog_enabled()) {
        char line[SYSLOG_LINE_MAX];
        int m = vsnprintf(line, sizeof line, fmt, ap2);
        if (m > 0) {
            const size_t len = (m < (int)sizeof line) ? (size_t)m
                                                      : sizeof line - 1;
            /* 超时 0：绝不让日志调用方为了写盘而等待。满了就丢并计数——
             * 丢**新**的而不是旧的：缓冲写满通常意味着正在刷屏，而刷屏的
             * 头几条才是有信息量的那几条。 */
            if (xStreamBufferSend(s_ring, line, len, 0) != len)
                atomic_fetch_add_explicit(&s_dropped, 1, memory_order_relaxed);
        }
    }
    va_end(ap2);
    return n;
}

/* 超限轮转：当前文件改名成 .old（覆盖上一份），重新开一个。
 * 比"截断前半部分"简单得多——FATFS 上原地挪动文件内容既慢又容易在掉电时
 * 留下半截文件，而轮转最坏只丢最老的那一份。 */
static void rotate_if_needed(void)
{
    struct stat st;
    if (stat(SYSLOG_PATH, &st) != 0 || st.st_size < SYSLOG_MAX_BYTES) return;
    unlink(SYSLOG_OLD_PATH);
    if (rename(SYSLOG_PATH, SYSLOG_OLD_PATH) != 0)
        unlink(SYSLOG_PATH);          /* 改名失败就直接丢，绝不让它无限长 */
}

static void syslog_task(void *arg)
{
    (void)arg;
    static char buf[1024];
    bool marked = false;
    uint32_t last_gen = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SYSLOG_FLUSH_MS));

        /* 关着的时候把缓冲倒干净，否则重新打开时会先吐出一段旧内容——
         * 那段时间用户以为没在记。 */
        if (!pk_diaglog_enabled() || !pk_sdcard_is_mounted()) {
            while (xStreamBufferReceive(s_ring, buf, sizeof buf, 0) > 0) { }
            marked = false;
            continue;
        }

        if (!xStreamBufferBytesAvailable(s_ring)) continue;
        atomic_store_explicit(&s_writing, true, memory_order_release);
        bool flush_ok = false;
        if (!pk_sdcard_io_acquire()) {
            atomic_store_explicit(&s_last_flush_ok, false, memory_order_release);
            atomic_store_explicit(&s_writing, false, memory_order_release);
            continue;
        }
        const uint32_t gen = pk_sdcard_mount_generation();
        if (gen != last_gen) { last_gen = gen; marked = false; }

        rotate_if_needed();
        FILE *fp = fopen(SYSLOG_PATH, "a");
        if (!fp) {
            pk_sdcard_io_release();
            atomic_store_explicit(&s_last_flush_ok, false, memory_order_release);
            atomic_store_explicit(&s_writing, false, memory_order_release);
            continue;
        }

        bool write_ok = true;
        if (!marked) {
            if (fprintf(fp, "\n=== boot reset=%d ===\n",
                        (int)esp_reset_reason()) < 0) {
                write_ok = false;
            } else {
                marked = true;
            }
        }
        size_t got;
        while ((got = xStreamBufferReceive(s_ring, buf, sizeof buf, 0)) > 0) {
            if (fwrite(buf, 1, got, fp) != got) write_ok = false;
        }

        /* FATFS 上只 fflush 不 fsync，目录项里的文件大小不更新，掉电会丢
         * 整段而且平时完全看不出来。2 s 一次付得起。 */
        const bool fflush_ok = fflush(fp) == 0;
        const bool fsync_ok = fflush_ok && fsync(fileno(fp)) == 0;
        const bool close_ok = fclose(fp) == 0;
        flush_ok = write_ok && fflush_ok && fsync_ok && close_ok;
        pk_sdcard_io_release();
        atomic_store_explicit(&s_last_flush_ok, flush_ok, memory_order_release);
        atomic_store_explicit(&s_writing, false, memory_order_release);
    }
}

void pk_syslog_init(void)
{
    static bool started;
    if (started) return;
    started = true;

    s_ring = xStreamBufferCreate(SYSLOG_RING, 1);
    if (!s_ring) {
        ESP_LOGW(TAG, "缓冲分配失败——本次开机不记录系统日志");
        return;
    }
    if (xTaskCreate(syslog_task, "syslog", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "任务创建失败——本次开机不记录系统日志");
        return;
    }
    /* 钩子最后挂：任务和缓冲都就绪之后再接管日志输出，避免中间态。 */
    s_orig = esp_log_set_vprintf(syslog_vprintf);
}
