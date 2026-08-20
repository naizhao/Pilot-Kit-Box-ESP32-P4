/*
 * record_sink_rec_store.c — ADS-B 原始报文落盘 sink，接到 pk_rec_store。
 *
 * 设计依据 ADS-B 数据持久化设计（内部文档）
 * 「写入管线」节：ADS-B 报文走**非阻塞入队（深度 256），满则丢并计数**，
 * 绝不阻塞 DSP 热路径——本文件的 write() 只做入队，真正的 SD I/O 全部
 * 移到 rec_store_sink_task 这个专用写任务上，跟 record_sink_file.c 的
 * file_writer_task 是同一个模式（照抄）。
 *
 * 不碰 record_sink_file.c 的既有写入逻辑——它继续写它的旧文件
 * （pilot_kit_ts_<N>.txt），这里是完全独立的第四路 sink，占
 * RECORD_SINK_MAX（=4）的最后一个槽位（uart / file / ble 已用 3 个，
 * 见 record_sink.c 的注释）。
 *
 * 绑定机专属文件（own_adsb.tsl）：写任务每次出队都读一次
 * pk_ui_get_own_icao()，变化时调 pk_rec_store_set_own_icao() 同步绑定
 * 关系；current icao24 与绑定一致时额外写一份到 own_adsb.tsl。
 * pk_rec_store_set_own_icao() 内部本来就是 memcmp 后才真正改动（重开
 * fd），这里在写任务侧再判一次只是省一次跨模块调用，不是必需的正确性
 * 前提。
 */
#include "record_sink.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pk_rec_store.h"
#include "ui_state.h"   /* pk_ui_get_own_icao() */

static const char *TAG = "rec_store_sink";

#define REC_STORE_SINK_QUEUE_DEPTH 256
#define REC_STORE_SINK_TASK_STACK  3072
/* "<ts_ms> *<HEX>;\n"：ts_ms 最多 20 位数字余量（实际远用不到那么长）+
 * " *" + 最长 28 字节 hex + ";\n" + NUL。 */
#define REC_STORE_SINK_LINE_CAP    (20 + 2 + RECORD_HEX_MAX_LEN + 2 + 1)

typedef struct {
    int64_t  ts_ms;
    uint32_t icao24;
    uint8_t  hex_len;
    char     hex[RECORD_HEX_MAX_LEN + 1];
} rec_store_sink_item_t;

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;
static volatile uint32_t s_written;
static record_sink_t s_sink;

static bool sink_write(record_sink_t *self, const record_t *rec)
{
    (void)self;
    if (s_queue == NULL) return false;

    rec_store_sink_item_t item;
    item.ts_ms   = rec->ts_ms;
    item.icao24  = rec->icao24;
    item.hex_len = rec->hex_len;
    memcpy(item.hex, rec->hex, (size_t)rec->hex_len + 1);

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        s_dropped++;
        return false;
    }
    return true;
}

/* "<ts_ms> *<HEX>;\n" —— 与客户端既有 ts-line 解码器逐字节兼容，不能加
 * 任何多余字符（见设计文档「与客户端的既有契约」节）。 */
static size_t format_line(const rec_store_sink_item_t *item, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%lld *%s;\n", (long long)item->ts_ms, item->hex);
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

static void rec_store_sink_task(void *arg)
{
    (void)arg;
    uint32_t last_own_icao  = 0;
    bool     have_last_own  = false;
    rec_store_sink_item_t item;

    for (;;) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        char line[REC_STORE_SINK_LINE_CAP];
        size_t len = format_line(&item, line, sizeof(line));

        if (pk_rec_store_append_adsb_line(line, len)) {
            s_written++;
        }

        uint32_t own_icao = pk_ui_get_own_icao();
        if (own_icao != last_own_icao || !have_last_own) {
            if (own_icao != 0) {
                uint8_t icao_bytes[3] = {
                    (uint8_t)((own_icao >> 16) & 0xFFu),
                    (uint8_t)((own_icao >> 8) & 0xFFu),
                    (uint8_t)(own_icao & 0xFFu),
                };
                pk_rec_store_set_own_icao(icao_bytes);
            } else {
                pk_rec_store_set_own_icao(NULL);
            }
            last_own_icao = own_icao;
            have_last_own = true;
        }

        if (own_icao != 0 && item.icao24 == own_icao) {
            pk_rec_store_append_own_adsb_line(line, len);
        }
    }
}

record_sink_t *record_sink_rec_store_create(void)
{
    s_queue = xQueueCreate(REC_STORE_SINK_QUEUE_DEPTH, sizeof(rec_store_sink_item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return NULL;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(rec_store_sink_task, "rec_sink_wr",
                                            REC_STORE_SINK_TASK_STACK, NULL, 3, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(rec_sink_wr) failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return NULL;
    }

    s_sink.name  = "rec_store";
    s_sink.write = sink_write;
    s_sink.priv  = NULL;
    return &s_sink;
}

bool record_sink_rec_store_stats(uint32_t *out_written, uint32_t *out_dropped)
{
    if (s_queue == NULL) return false;
    if (out_written) *out_written = s_written;
    if (out_dropped) *out_dropped = s_dropped;
    return true;
}
