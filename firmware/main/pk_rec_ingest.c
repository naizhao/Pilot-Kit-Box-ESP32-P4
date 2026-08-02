/* pk_rec_ingest.c — 实现见 pk_rec_ingest.h。 */
#include "pk_rec_ingest.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pk_rec_format.h"
#include "pk_rec_store.h"
#include "pk_clock.h"

static const char *TAG = "rec_ingest";

/* 队列深度：ingest 只在"CPR 新解出位置"/"呼号变化"时才入队，比原始
 * 报文帧率低一个数量级，256 足够吃下繁忙空域的突发（照
 * record_sink_rec_store.c 的 ADS-B 行队列深度取值，留够余量）。 */
#define REC_INGEST_QUEUE_DEPTH 256
#define REC_INGEST_TASK_STACK  3072

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;
static volatile uint32_t s_written;

static void icao_to_bytes(uint32_t icao24, uint8_t out[3])
{
    out[0] = (uint8_t)((icao24 >> 16) & 0xFFu);
    out[1] = (uint8_t)((icao24 >> 8) & 0xFFu);
    out[2] = (uint8_t)(icao24 & 0xFFu);
}

/* 非阻塞入队；队满丢弃并计数（照 record_sink_file.c:344-353 的做法，每
 * 256 次丢弃打一条日志，避免刷屏）。dsp_task 热路径上只调这个，不摸
 * s_lock/fwrite。 */
static void enqueue_or_drop(const uint8_t buf[PK_TRK_RECORD_LEN])
{
    if (s_queue == NULL) return;   /* 未 init（不应发生，防御性丢弃） */
    if (xQueueSend(s_queue, buf, 0) == pdTRUE) return;

    uint32_t d = ++s_dropped;
    if ((d & 0xFF) == 0) {
        ESP_LOGW(TAG, "traffic.trk 队列满——累计丢弃 %lu 条", (unsigned long)d);
    }
}

/* 写任务：唯一真正碰 pk_rec_store_append_traffic_record()（内部
 * xSemaphoreTake(portMAX_DELAY) + fwrite）的地方，不拖累 dsp_task。 */
static void rec_ingest_writer_task(void *arg)
{
    (void)arg;
    uint8_t buf[PK_TRK_RECORD_LEN];
    for (;;) {
        if (xQueueReceive(s_queue, buf, portMAX_DELAY) != pdTRUE) continue;
        if (pk_rec_store_append_traffic_record(buf)) {
            s_written++;
        }
    }
}

void pk_rec_ingest_init(void)
{
    if (s_queue != NULL) return;   /* 幂等 */

    s_queue = xQueueCreate(REC_INGEST_QUEUE_DEPTH, PK_TRK_RECORD_LEN);
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(rec_ingest_writer_task, "rec_ingest_wr",
                                            REC_INGEST_TASK_STACK, NULL, 3, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(rec_ingest_wr) failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}

bool pk_rec_ingest_stats(uint32_t *out_written, uint32_t *out_dropped)
{
    if (s_queue == NULL) return false;
    if (out_written) *out_written = s_written;
    if (out_dropped) *out_dropped = s_dropped;
    return true;
}

void pk_rec_ingest_position(uint32_t icao24, int64_t ts_ms, double lat, double lon,
                             bool have_alt, int alt_ft,
                             bool have_gs, int gs_kt,
                             bool have_track, int track_deg,
                             bool have_vs, int vs_fpm,
                             bool on_ground, bool from_surface_cpr)
{
    pk_trk_pos_t rec = {0};
    rec.ts_ms = (uint64_t)ts_ms;
    icao_to_bytes(icao24, rec.icao24);
    rec.lat_e7 = (int32_t)lround(lat * 1e7);
    rec.lon_e7 = (int32_t)lround(lon * 1e7);
    rec.alt_d25     = have_alt   ? (int16_t)(alt_ft / 25)      : PK_REC_ALT_INVALID;
    rec.gs_kt       = have_gs    ? (uint16_t)gs_kt              : PK_REC_GS_INVALID;
    rec.track_deg10 = have_track ? (uint16_t)(track_deg * 10)   : PK_REC_TRACK_INVALID;
    rec.vs_fpm_d64  = have_vs    ? (int16_t)(vs_fpm / 64)       : PK_REC_VS_INVALID;

    rec.flags = 0;
    if (pk_clock_is_synced()) rec.flags |= PK_TRK_FLAG_TIME_SYNCED;
    if (on_ground)            rec.flags |= PK_TRK_FLAG_ON_GROUND;
    if (from_surface_cpr)     rec.flags |= PK_TRK_FLAG_SURFACE_CPR;
    /* ALT_GEOMETRIC：本阶段只有气压高度，不置。 */

    uint8_t buf[PK_TRK_RECORD_LEN];
    pk_trk_pos_encode(&rec, buf);
    enqueue_or_drop(buf);
}

void pk_rec_ingest_identity(uint32_t icao24, int64_t ts_ms, const char *callsign,
                             uint8_t emitter_category)
{
    pk_trk_id_t rec = {0};
    rec.ts_ms = (uint64_t)ts_ms;
    icao_to_bytes(icao24, rec.icao24);
    if (callsign != NULL) {
        strncpy(rec.callsign, callsign, sizeof(rec.callsign) - 1);
    }
    rec.emitter_category = emitter_category;

    uint8_t buf[PK_TRK_RECORD_LEN];
    pk_trk_id_encode(&rec, buf);
    enqueue_or_drop(buf);
}
