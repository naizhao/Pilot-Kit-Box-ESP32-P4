/*
 * pk_own_sampler.c — 本机 1 Hz 航迹采样：own.trk 的生产者。设计说明见
 * pk_own_sampler.h 与设计文档「own.trk」「相位标记」「写入管线」三节。
 *
 * 两个任务，职责分开：
 *   - own_sample_task：每 1000 ms 取 GPS/baro/IMU + 跑一遍相位状态机，
 *     编码一条 48 B 记录，塞进 s_queue（**带 3 s 超时**，不能
 *     portMAX_DELAY——见「写入管线」节：拔卡瞬间 SD 命令可能卡在固定
 *     ~2.01 s 的超时上，无限阻塞会把 1 Hz 采样任务连同它内嵌的相位状态机
 *     一起拖停）。3 s 超时仍排不进去就丢这一点、计数——这是本机航迹自己
 *     的**独立**队列，不能跟 ADS-B 报文共用那条"满则丢"的队列：本机航迹
 *     是飞行本身，繁忙空域（ADS-B 队列最容易满的时候）恰恰不该是丢本机
 *     数据的时候。
 *   - own_writer_task：阻塞式拿队列，调用 pk_rec_store_append_own_record()
 *     做真正的 SD I/O——这一步可能内部阻塞（mutex + fwrite），但那是这个
 *     专用写任务的事，不拖累采样节奏。
 *
 * 校时回调（pk_clock_register_sync_cb）：低频事件，直接同步落一条 own.trk
 * 时间修正记录，不经队列——调用方是触发校时的那个任务（GPS/BLE），不是
 * 本模块的任务，硬塞进 s_queue 反而要多一层跨任务同步，收益不大。
 *
 * 机型分类（ac_category）与"机场范围内"（near_airport）两个输入本阶段
 * 都是占位：设置页机型分类是阶段 5 的范围（本次任务书明确排除），近机场
 * 判定需要航空数据库距离查询，同样不在本阶段范围内。用安全默认值
 * （PISTON_LIGHT / near_airport=false）不会导致相位判定错误地放宽，只是
 * 少享受 UC7"不封段"的优待——按 pk_flight_phase.h 的说明，这两个都是
 * "缺省 false/默认档位是安全值"的设计。
 */
#include "pk_own_sampler.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_attr.h"    /* EXT_RAM_BSS_ATTR */
#include "esp_log.h"
#include "esp_timer.h"

#include "gps.h"
#include "baro.h"
#include "imu_task.h"
#include "pk_flight_phase.h"
#include "pk_rec_format.h"
#include "pk_rec_store.h"
#include "pk_clock.h"
#include "ui_state.h"       /* pk_ui_get_own_icao() */
#include "aircraft_state.h" /* aircraft_state_get_own() —— 绑定机 on_ground 位 */

static const char *TAG = "own_sampler";

#define OWN_SAMPLE_PERIOD_MS           1000
#define OWN_SAMPLE_QUEUE_DEPTH         8
#define OWN_SAMPLE_ENQUEUE_TIMEOUT_MS  3000
#define OWN_SAMPLER_TASK_STACK         4096
#define OWN_WRITER_TASK_STACK          3072
/* 绑定机 ADS-B on_ground 位的新鲜度窗口——超过这个岁数就当作"没有新鲜
 * 数据"，相位状态机的 bound_valid 置 false，退回自主传感器判定
 * （UC6：绑错飞机/数据陈旧时不信 ADS-B）。 */
#define OWN_BOUND_MAX_AGE_US            (5LL * 1000000)

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;
static volatile uint32_t s_written;

/* 60 s @ 1Hz 位移窗口环形缓冲（PK_FLIGHT_PHASE_RING_CAP=64 槽 × 16 B ≈ 1 KB）
 * ——按内存红线一律 EXT_RAM_BSS_ATTR，别让它挤内部 .bss（见
 * check_early_heap.py 的 66000 B 阈值）。 */
static EXT_RAM_BSS_ATTR pk_flight_phase_state_t s_phase_state;

/* --- pk_clock 校时回调：直接同步写一条时间修正记录 -------------------- */

static void on_clock_sync(int64_t epoch_ms, int64_t prev_ms, const char *source)
{
    pk_own_time_sync_t rec = {0};
    rec.ts_ms      = (uint64_t)epoch_ms;
    rec.prev_ts_ms = (uint64_t)prev_ms;
    /* source 取值集合固定为 "gps"/"gps-coarse"/"ble-write"/"ios-cts"
     * （pk_clock.h 的说明），含 "gps" 子串的两种都算 GPS 来源。 */
    rec.sync_source = (strstr(source, "gps") != NULL) ? PK_OWN_SYNC_SOURCE_GPS
                                                        : PK_OWN_SYNC_SOURCE_BLE_CTS;
    rec.sync_reason = 0;   /* spec 未细定 reason 语义，占位 */

    uint8_t buf[PK_OWN_RECORD_LEN];
    pk_own_time_sync_encode(&rec, buf);
    pk_rec_store_append_own_record(buf);
}

/* --- 采样任务 ----------------------------------------------------------- */

static void own_sample_task(void *arg)
{
    (void)arg;
    pk_flight_phase_reset(&s_phase_state, PK_AC_CAT_PISTON_LIGHT);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(OWN_SAMPLE_PERIOD_MS));

        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t ts_ms  = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
        int64_t now_us = esp_timer_get_time();

        pk_gps_state_t gps = {0};
        bool gps_fix = pk_gps_get(&gps);

        pk_baro_state_t baro = {0};
        bool baro_ok = pk_baro_get(&baro) && baro.valid;

        pk_imu_sample_t imu = {0};
        bool imu_ok = pk_imu_sample_get(&imu) && imu.valid;

        /* 绑定机 ADS-B on_ground 位：来自融合表，只在新鲜时采信（相位
         * 状态机内部还会再跟 GPS/速度做一次矛盾检测，见 UC6）。 */
        uint32_t own_icao = pk_ui_get_own_icao();
        aircraft_t own_ac = {0};
        bool bound_valid = own_icao != 0 &&
                            aircraft_state_get_own(own_icao, now_us,
                                                   OWN_BOUND_MAX_AGE_US, &own_ac);

        pk_flight_phase_input_t in = {0};
        in.ts_ms          = (uint64_t)ts_ms;
        in.gps_valid       = gps_fix;
        in.lat_e7           = gps_fix ? (int32_t)lround(gps.lat * 1e7) : 0;
        in.lon_e7           = gps_fix ? (int32_t)lround(gps.lon * 1e7) : 0;
        in.gs_kt            = gps_fix ? (uint16_t)gps.ground_speed_kt : 0;
        in.baro_valid       = baro_ok;
        in.vs_fpm           = baro_ok ? (int16_t)baro.vs_fpm : 0;
        in.vib_level        = imu_ok ? imu.vib_level : 0;
        in.bound_valid      = bound_valid;
        in.bound_on_ground  = bound_valid && own_ac.on_ground;
        in.near_airport     = false;                /* 见文件头说明 */
        in.ac_category      = PK_AC_CAT_PISTON_LIGHT;  /* 见文件头说明 */

        pk_flight_phase_debug_t dbg = {0};
        pk_flight_phase_t phase = pk_flight_phase_update(&s_phase_state, &in, &dbg);

        pk_own_sample_t rec = {0};
        rec.ts_ms  = (uint64_t)ts_ms;
        rec.phase  = (uint8_t)phase;
        rec.flags  = 0;
        if (pk_clock_is_synced()) rec.flags |= PK_OWN_FLAG_TIME_SYNCED;
        if (gps_fix)              rec.flags |= PK_OWN_FLAG_GPS_FIX;
        if (imu_ok)               rec.flags |= PK_OWN_FLAG_IMU_VALID;
        if (baro_ok)              rec.flags |= PK_OWN_FLAG_BARO_VALID;
        rec.sats   = gps_fix ? (uint8_t)gps.sats : 0;
        rec.lat_e7 = gps_fix ? (int32_t)lround(gps.lat * 1e7) : 0;
        rec.lon_e7 = gps_fix ? (int32_t)lround(gps.lon * 1e7) : 0;
        rec.alt_baro_ft     = baro_ok ? (int32_t)baro.alt_ft : 0;
        rec.alt_gnss_msl_ft = (gps_fix && gps.have_altitude) ? (int32_t)gps.altitude_ft : 0;
        rec.gs_kt       = gps_fix ? (uint16_t)gps.ground_speed_kt : PK_REC_GS_INVALID;
        rec.track_deg10 = gps_fix ? (uint16_t)(gps.track_deg * 10) : PK_REC_TRACK_INVALID;
        rec.vs_fpm      = baro_ok ? (int16_t)baro.vs_fpm : PK_REC_VS_INVALID;
        rec.roll_d10  = imu_ok ? (int16_t)lroundf(imu.roll_deg  * 10.0f) : 0;
        rec.pitch_d10 = imu_ok ? (int16_t)lroundf(imu.pitch_deg * 10.0f) : 0;
        rec.yaw_d10   = imu_ok ? (int16_t)lroundf(imu.yaw_deg   * 10.0f) : 0;

        float hdop_x10f = gps_fix ? gps.hdop * 10.0f : 0.0f;
        if (hdop_x10f > 255.0f) hdop_x10f = 255.0f;
        if (hdop_x10f < 0.0f)   hdop_x10f = 0.0f;
        rec.hdop_x10 = (uint8_t)hdop_x10f;

        rec.vib_level = imu.vib_level;   /* pk_imu_sample_t 自带 0=不可用哨兵 */

        double disp = dbg.disp_m_60s;
        if (disp < 0.0) disp = 0.0;
        rec.disp_m_60s = (uint16_t)(disp > 65535.0 ? 65535.0 : disp);
        rec.ac_category = (uint8_t)PK_AC_CAT_PISTON_LIGHT;

        uint8_t buf[PK_OWN_RECORD_LEN];
        pk_own_sample_encode(&rec, buf);

        if (xQueueSend(s_queue, buf, pdMS_TO_TICKS(OWN_SAMPLE_ENQUEUE_TIMEOUT_MS)) != pdTRUE) {
            s_dropped++;
            ESP_LOGW(TAG, "own.trk 采样丢弃（写任务 %d ms 内没排上）——累计 %u",
                     OWN_SAMPLE_ENQUEUE_TIMEOUT_MS, (unsigned)s_dropped);
        }
    }
}

static void own_writer_task(void *arg)
{
    (void)arg;
    uint8_t buf[PK_OWN_RECORD_LEN];
    for (;;) {
        if (xQueueReceive(s_queue, buf, portMAX_DELAY) != pdTRUE) continue;
        if (pk_rec_store_append_own_record(buf)) {
            s_written++;
        }
    }
}

void pk_own_sampler_start(void)
{
    if (s_queue != NULL) return;   /* 幂等 */

    s_queue = xQueueCreate(OWN_SAMPLE_QUEUE_DEPTH, PK_OWN_RECORD_LEN);
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return;
    }

    pk_clock_register_sync_cb(on_clock_sync);

    BaseType_t ok = xTaskCreatePinnedToCore(own_writer_task, "own_wr",
                                            OWN_WRITER_TASK_STACK, NULL, 3, NULL, 0);
    if (ok != pdTRUE) ESP_LOGE(TAG, "own_writer task create failed");

    ok = xTaskCreatePinnedToCore(own_sample_task, "own_sample",
                                 OWN_SAMPLER_TASK_STACK, NULL, 2, NULL, 0);
    if (ok != pdTRUE) ESP_LOGE(TAG, "own_sample task create failed");
}

bool pk_own_sampler_stats(uint32_t *out_written, uint32_t *out_dropped)
{
    if (s_queue == NULL) return false;
    if (out_written) *out_written = s_written;
    if (out_dropped) *out_dropped = s_dropped;
    return true;
}
