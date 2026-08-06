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
 * 机型分类（ac_category）阶段 5a 起已接上设置页：每 tick 读一次
 * pk_ac_category_get()（config_ac_category.c，volatile + portMUX，热路径
 * 可放心调）。"机场范围内"（near_airport）已接入 pk_aero_db 查询——GPS
 * 有 fix 时查最近机场，≤ 2 NM 置 true（罩哥 2026-08-04 拍板阈值）。航空
 * 库未就绪/无 GPS fix 时自然 false（安全默认，与占位行为一致），只是少
 * 享受 UC7"不封段"的优待，按 pk_flight_phase.h 的说明是安全默认。
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
#include "config_ac_category.h"
#include "pk_aero_db.h"
#include "pk_win.h"          /* pk_win_nearest（W1.5：跟本机、窗口必覆盖）*/
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
/* 「在机场范围内」判定阈值（设计文档 UC7，罩哥 2026-08-04 拍板 2 NM）。 */
#define OWN_NEAR_AIRPORT_NM             2.0

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;
static volatile uint32_t s_written;

/* 渲染层读的"当前相位"快照。单一写者(own_sample_task)/多读者，见
 * pk_own_sampler_get_phase() 头注的线程安全说明。默认 unknown——采样任务
 * 第一拍算出真实相位之前，读者必须看到"不压暗任何一侧"这个安全值。 */
static volatile pk_flight_phase_t s_current_phase = PK_PHASE_UNKNOWN;

/* 60 s @ 1Hz 位移窗口环形缓冲（PK_FLIGHT_PHASE_RING_CAP=64 槽 × 16 B ≈ 1 KB）
 * ——按内存红线一律 EXT_RAM_BSS_ATTR，别让它挤内部 .bss（见
 * check_early_heap.py 的 66000 B 阈值）。 */
static EXT_RAM_BSS_ATTR pk_flight_phase_state_t s_phase_state;

/* ── 本机航迹 ring（地图飞行轨迹线）──────────────────────────────────
 * 按飞行段记录：taxi 起算、ground_stopped 滞回清空（见 own_sample_task 里）。
 * 存 1Hz 全量（与 own.trk 落盘一致），最坏 2h = 7200 点，取 8192（2 的幂）。
 * 8192×16 = 128 KB，放 PSRAM（EXT_RAM_BSS_ATTR）；内部 .bss 余量 1.5KB 绝不进内部
 * （见 project_early_heap_cliff）。轨迹不依赖 ADS-B 绑定，数据源是本机 GPS。 */
/* 2048 槽 × 16B = 32 KB。权衡：PSRAM 紧张（瓦片缓存水位线 3MB，free 仅 ~6MB），
 * 128KB(8192) 会压破水位线致瓦片淘汰风暴（实测 free 2.9MB→evicts 39）；
 * 32KB(2048) ≈ 34 分钟 1Hz 全量，配合渲染降采样（飞行 15s/地面 60s）够画一段
 * 完整轨迹，PSRAM 占用可控。更长轨迹靠 own.trk 落盘回放（设计 defer）。 */
#define PK_OWN_TRAIL_CAP 2048u
static EXT_RAM_BSS_ATTR struct {
    pk_own_trail_point_t pt[PK_OWN_TRAIL_CAP];
    uint32_t head;   /* 下一个写入位置（裸递增，取模靠 & (CAP-1)） */
    uint32_t count;  /* 有效点数（≤ CAP） */
} s_trail;

/* ── trail_push：向 ring 写入一个采样点（文件内 static）────────────── */
static void trail_push(uint32_t ts_ms, int32_t lat_e7, int32_t lon_e7, uint8_t phase)
{
    s_trail.pt[s_trail.head & (PK_OWN_TRAIL_CAP - 1)] = (pk_own_trail_point_t){
        .ts_1k = ts_ms, .lat_e7 = lat_e7, .lon_e7 = lon_e7, .phase = phase,
    };
    s_trail.head++;
    if (s_trail.count < PK_OWN_TRAIL_CAP) s_trail.count++;
}

/* ── 飞行段清空决策的持久状态（own_sample_task 跨 tick 使用）────────── */
static pk_flight_phase_t s_prev_trail_phase = PK_PHASE_UNKNOWN;
static uint32_t s_gs_steady_since_ms = 0;   /* ground_stopped 滞回计时 */

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
    pk_flight_phase_reset(&s_phase_state, pk_ac_category_get());

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
        /* near_airport：喂给相位状态机，影响 UC7「跑道口排队 10 分钟不封段」
         * 的不降级优待（设计文档「用户场景」UC7 + pk_flight_phase.h:26）。
         * 罩哥 2026-08-04 拍板阈值 2 NM。W1.5（2026-08-04）：走窗口 nearest
         *（跟本机、椭圆必覆盖，不需 fallback）；窗口未就绪时回退全量。
         * own_sampler 是 1Hz 独立任务、不在渲染热路径上。航空库未就绪/无 GPS
         * fix 时自然 false（安全默认，与占位行为一致）。 */
        in.near_airport = false;
        if (gps_fix) {
            pk_aero_near_t near[1];
            int na = pk_win_nearest(PK_AERO_SEC_AIRPORTS,
                                    (double)gps.lat, (double)gps.lon, near, 1);
            if (na == 0)
                na = pk_aero_db_nearest_airports((double)gps.lat, (double)gps.lon,
                                                  near, 1);
            in.near_airport = (na >= 1 && near[0].dist_nm <= OWN_NEAR_AIRPORT_NM);
        }
        pk_ac_category_t ac_cat = pk_ac_category_get();
        in.ac_category      = ac_cat;

        pk_flight_phase_debug_t dbg = {0};
        pk_flight_phase_t phase = pk_flight_phase_update(&s_phase_state, &in, &dbg);
        s_current_phase = phase;   /* 渲染层的读快照，见头文件线程安全说明 */

        /* ── 本机航迹 ring 写入 / 飞行段清空 ──────────────────────────
         * taxi 起算（运动开始）、ground_stopped 滞回 60s 清空（落地停稳后清）。
         * takeoff_roll/landing_rollout 虽属 ground_family，但它们是飞行段中间
         * 态（起飞滑跑/着陆滑跑），必须继续画轨迹——只有在 ground_stopped 停稳
         * 才算飞行段结束。taxi 是飞行段的起点（滑出位开始要画）。
         * ground_stopped 滞回 60s：避免触地复飞（UC8）中间瞬态 ground_stopped
         * 误清轨迹——触地复飞里 landing_rollout 会直接弹回 takeoff_roll，不会
         * 停在 ground_stopped 达 60s，但保守起见用滞回更稳。
         * trail_push 位置源见下方：优先绑定机 ADS-B，退回 GPS（与地图/PFD 的
         * pk_own_ship_resolve 同源）——否则 GPS 没定位时轨迹 ring 永远空。 */
        const bool moving = (phase == PK_PHASE_TAXI)
                            || !pk_flight_phase_is_ground_family(phase);
        if (moving && s_prev_trail_phase == PK_PHASE_GROUND_STOPPED) {
            s_trail.head = 0; s_trail.count = 0;   /* 新飞行段，清空 */
        }
        if (phase == PK_PHASE_GROUND_STOPPED) {
            /* 滞回：连续 ground_stopped 达 60s 才清（避免触地复飞 UC8 误清）。
             * 清完保持不清，直到下次进入 moving 再开新段。 */
            if (s_gs_steady_since_ms == 0) s_gs_steady_since_ms = (uint32_t)ts_ms;
            if ((uint32_t)ts_ms - s_gs_steady_since_ms >= 60000) {
                s_trail.head = 0; s_trail.count = 0;
            }
        } else {
            s_gs_steady_since_ms = 0;
        }
        /* 轨迹位置源：优先绑定机 ADS-B（与地图/PFD 的 pk_own_ship_resolve 同源），
         * 绑定机无位置时退回 GPS。GPS 全丢时相位机走 UC9 保持上一态（UNKNOWN/
         * AIRBORNE 都非 ground_family → moving=true），故门控只再把"GPS 有 fix"
         * 换成"位置可得"即可——否则座舱内 GPS 没定位、却已绑定本机时，地图靠
         * ADS-B 跟着飞、轨迹 ring 却永远空。相位机输入 in.lat_e7 仍走 GPS，不动。 */
        const bool trail_from_bound = bound_valid && own_ac.have_position;
        const bool trail_pos_ok     = trail_from_bound || gps_fix;
        const int32_t trail_lat_e7  = trail_from_bound
            ? (int32_t)lround(own_ac.lat * 1e7)
            : (gps_fix ? (int32_t)lround(gps.lat * 1e7) : 0);
        const int32_t trail_lon_e7  = trail_from_bound
            ? (int32_t)lround(own_ac.lon * 1e7)
            : (gps_fix ? (int32_t)lround(gps.lon * 1e7) : 0);
        if (moving && trail_pos_ok) {
            trail_push((uint32_t)ts_ms, trail_lat_e7, trail_lon_e7, (uint8_t)phase);
        }
        s_prev_trail_phase = phase;

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
        rec.ac_category = (uint8_t)ac_cat;   /* 记的是"当时生效的分类"，见
                                                 own.trk 字段表 44 号 ac_category
                                                 的注释：改设置不能改回放依据 */

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

pk_flight_phase_t pk_own_sampler_get_phase(void)
{
    return s_current_phase;
}

const pk_own_trail_point_t *pk_own_sampler_get_trail(uint32_t *out_count)
{
    /* 返回连续数组视图：ring 未满时 pt[0]..pt[count-1] 是写入顺序（最老→最新）。
     * ring 满了（count==CAP）后 head 回绕，pt[0] 不再是最老的点——这是已知简化：
     * CAP=8192 而 2 小时才 7200 点，正常飞行不会写满。即使极端场景写满了，
     * 渲染层逐个投影+降采样，多画/乱序几个共线点无视觉影响（见 .h 注释）。
     * count 读取可能在 push 时变动，多读一个点无影响（追加式 ring）。 */
    if (out_count) *out_count = s_trail.count;
    return s_trail.pt;
}
