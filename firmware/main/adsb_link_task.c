/*
 * adsb_link_task.c — RP2040 UART 链路 + 原 dsp_task 业务链。
 * 迁移自 dsp_task.c（2026-09-05，WP-C1）；IQ/USB 路径不再回来。
 */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "adsb_link.h"
#include "modes_ingest.h"
#include "uat_ingest.h"
#include "pilot_kit.h"
#include "cpr_decode.h"
#include "aircraft_state.h"
#include "config_demo.h"
#include "record_sink.h"
#include "pk_rec_ingest.h"
#include "gps.h"
#include "adsb_link_task.h"
#include "config_antenna.h"   /* 天线选择：NVS 真源，经 CONFIG_REQ 下发 */
#include "freertos/semphr.h"

static const char *TAG      = "dsp";     /* 沿用旧 TAG，日志检索连续 */
static const char *TAG_ADSB = "adsb";

/* --- 链路硬件参数（PLAN.md §3.2 / 协议 §0）---------------------------- */
#define ADSB_UART        UART_NUM_2
#define ADSB_UART_BAUD   921600
#define ADSB_TX_GPIO     32      /* P4 TX → RP2040 RXD（J3-31）*/
#define ADSB_UART_RX_BUF 4096
#define ADSB_UART_TX_BUF 1024
#define LINK_STALE_US    (5 * 1000000LL)

static adsb_link_dec_t      s_dec;
static atomic_llong         s_last_frame_us;   /* 跨任务读（诊断页），64 位
                                                * 必须 _Atomic 防撕裂 */
/* 跨任务只读的链路统计（audit round 4）：每字段独立 atomic_uint，读侧
 * 做逐字段 load、**不做跨字段一致性承诺**（与 pk_dsp_stats 注释同一
 * 口径——各计数单调，1 Hz 诊断页可容忍字段间撕裂）。 */
static struct {
    atomic_uint rx_frames;     /* codec 成功递交帧数（含未知类型） */
    atomic_uint rx_crc_errors;
    atomic_uint rx_seq_gaps;
    atomic_uint rx_resyncs;
    atomic_uint modes_fed;     /* 实际送入 modes_ingest 的 MODES_RAW 帧数 */
    atomic_uint uat_len_drop;  /* UAT_UPLINK 长度不符丢弃（CRC 已过，
                                * codec 统计不可见——单独计数） */
} s_stats;
/* 单写者（本任务写 true，永不复位）跨任务读的布尔旗标。选 atomic_bool
 * 而非 volatile bool：同样的"编译器不得跨调用缓存"保证，但语义由 C11
 * 定义（volatile 无跨线程语义，只防优化）；relaxed 序足够——最坏多滞后
 * 一个 1 Hz 诊断刷新。 */
static atomic_bool          s_ever_linked;
static atomic_bool          s_proto_mismatch_seen;
static uint8_t              s_rxchunk[256];

/* ── 以下整块【迁移】自 dsp_task.c，除注明外逐字搬运 ────────────────
 *   - 1 Hz 窗口计数 s_msgs_* / s_pos_decoded 与 atomic 累计
 *     s_msgs_total_cum / s_pos_decoded_cum
 *   - ICAO_SEEN_CAPACITY / s_icao_seen / s_icao_unique / icao_seen_insert()
 *   - PK_REC_LOOKUP_MAX_AGE_US
 *   - s_summary_snap[AIRCRAFT_TABLE_CAPACITY]（EXT_RAM_BSS_ATTR 保留）
 *   - format_aircraft_line() / aircraft_summary_emit()
 *   - on_mode_s_msg() → on_ingest_msg()（签名换 modes_ingest sink）
 *   - dashboard_emit_and_reset()：搬运但**改造**——去掉 stream MB/s，
 *     换链路计数（rx/s、crcerr/s），窗口基线 s_win_rx / s_win_crc。
 * 【删除不迁】s_iq_buf / s_mag_buf / DSP_*_BYTES / s_window_bytes /
 *   IQ stall watchdog / mode_s_detect 调用 / EXT_RAM 的 s_decoder
 *   （解码器实例移入 modes_ingest.c）。
 */

/* --- 1 Hz dashboard counters ------------------------------------------ */

static uint32_t s_msgs_total     = 0;
static uint32_t s_msgs_df11      = 0;
static uint32_t s_msgs_df17_id   = 0;
static uint32_t s_msgs_df17_pos  = 0;
static uint32_t s_msgs_df17_vel  = 0;
static uint32_t s_msgs_df20_21   = 0;
static uint32_t s_msgs_other     = 0;
static uint32_t s_pos_decoded    = 0;

/* 链路计数 1 Hz 窗口基线：emit 时与累计值求差得本窗增量，之后追平。 */
static uint32_t s_win_rx  = 0;
static uint32_t s_win_crc = 0;
/* UART sink 丢弃数的窗口基线。record_sink_uart_stats() 报的是**自启动累计**，
 * 不比基线就会在丢过一次之后每秒都打——而那正是控制台已经拥塞时最不该做的
 * 事。见 dashboard_emit_and_reset()。 */
static uint32_t s_win_uart_drop = 0;

/* --- Cumulative diagnostic counters (boot-lifetime, never reset) ------- *
 * Written only from this task; read cross-task by pk_dsp_get_stats() and
 * pk_adsb_link_state_get(). audit round 4: per-field atomic_uint loads —
 * no cross-field consistency claim (each counter is monotonic, so a torn
 * multi-field snapshot is fine for 1 Hz diagnostics; same convention as
 * the link-stats struct above and the RP2040-side health stats).
 * atomic_* replaces the old volatile approach: same "don't cache across
 * calls" guarantee with defined C11 semantics instead of volatile's
 * optimization-barrier-only behavior. The 64-bit s_last_frame_us stays
 * _Atomic too (RV32 has no atomic 64-bit loads; the compiler emits a
 * lock). The atomic_bool flags (s_ever_linked / s_proto_mismatch_seen)
 * are single-writer (this task), relaxed ops — worst case is one extra
 * 1 Hz tick of staleness.
 */
static atomic_uint s_msgs_total_cum  = 0;   /* cumulative CRC-ok Mode-S frames */
static atomic_uint s_pos_decoded_cum = 0;   /* cumulative CPR position decodes  */

/*
 * aircraft_state_get_own() 的"旧值/当前值"快照查询用——大到覆盖任何合理
 * 的两帧间隔（呼号变化探测），小到不会真的匹配上一次开机遗留的陈旧数据
 * （s_table 每次 aircraft_state_init() 都会清空，同一次开机内不存在这个
 * 问题）。
 */
#define PK_REC_LOOKUP_MAX_AGE_US (24LL * 3600 * 1000000)

/*
 * Tiny ICAO seen-set, kept solely so the dashboard can report unique
 * aircraft observed *since boot* without poking inside cpr_decode.c.
 * 1024 slots × 4 bytes = 4 KiB; collisions just under-count, which is
 * fine for a dashboard.
 */
#define ICAO_SEEN_CAPACITY  1024
static uint32_t          s_icao_seen[ICAO_SEEN_CAPACITY];
static atomic_uint       s_icao_unique = 0;

static void icao_seen_insert(uint32_t icao24)
{
    uint32_t idx = icao24 % ICAO_SEEN_CAPACITY;
    for (uint32_t step = 0; step < ICAO_SEEN_CAPACITY; ++step) {
        uint32_t *slot = &s_icao_seen[(idx + step) % ICAO_SEEN_CAPACITY];
        if (*slot == icao24) return;          /* already seen */
        if (*slot == 0) {
            *slot = icao24;
            atomic_fetch_add_explicit(&s_icao_unique, 1,
                                      memory_order_relaxed);
            return;
        }
    }
    /* Table is full; we just stop counting new aircraft. */
}

/* --- Per-message handler ----------------------------------------------
 *
 * Registered as the modes_ingest sink: invoked synchronously by
 * modes_ingest_feed() for every frame that passes the decoder's CRC
 * check. We filter by CRC and dispatch a human-readable log line per
 * recognised message family. Runs on adsb_link_task; no synchronisation
 * needed for the static counters.
 *
 * 逐帧日志一律 ESP_LOGD，不是 ESP_LOGI（P1-D）。这里是整条链路最热的一段：
 * 一条 INFO 带上 "I (12345) adsb: " 前缀后约 90 字节，115200 波特的控制台上
 * ≈ 7.8 ms，而本任务是 921600 波特链路唯一的 RX 消费者，RX 环只有 4096 字节
 * （≈ 44 ms 的数据量）。繁忙空域每秒几百帧时，这一秒里要等掉几百个 7.8 ms，
 * RX 环必然溢出——帧丢在 UART 驱动里，没有任何计数看得见。
 *
 * CONFIG_LOG_MAXIMUM_LEVEL=INFO（本工程默认）下 ESP_LOGD 在编译期就没了，
 * 阻塞预算恒为 0；要看逐帧解码就把等级开到 DEBUG，那是排障场景，丢帧可以
 * 接受。常驻可观测性由 1 Hz 的 dashboard_emit_and_reset() 承担。
 */
#ifndef PK_HOST_TEST
static
#endif
void on_ingest_msg(const struct mode_s_msg *mm,
                   const modes_ingest_meta_t *meta,
                   void *user)
{
    (void)meta; (void)user;

    /* Drop frames whose CRC is bad (or only became "ok" after a
     * single-bit forced correction — too noisy for live traffic). */
    if (!mm->crcok || mm->errorbit >= 0) return;

    uint32_t icao24 = ((uint32_t)mm->aa1 << 16)
                    | ((uint32_t)mm->aa2 << 8)
                    | (uint32_t)mm->aa3;

    s_msgs_total++;
    /* 「本次开机见过多少架飞机」只数真 ICAO 地址：DF18 的 TIS-B / 匿名
     * 报文里那三个字节可能是 Mode-A 码 + 航迹文件号，地面站会复用它，
     * 数进去等于把同一架飞机数成好几架。 */
    if (mm->aa_is_icao) icao_seen_insert(icao24);

    /* 落盘：呼号是否变化要在 ingest 覆盖 aircraft_state 之前判断——ingest
     * 一跑完，表里就只剩新呼号了。只有 DF17/18 metype 1-4（身份帧）才需要
     * 这份"旧值"快照，其它消息类型不必为此多付一次带锁查表的开销。 */
    const int64_t now_us = esp_timer_get_time();
    const bool is_ident_msg = (mm->msgtype == 17 || mm->msgtype == 18) &&
                              mm->metype >= 1 && mm->metype <= 4;
    aircraft_t prev_ac = {0};
    bool had_prev_callsign = false;
    if (is_ident_msg) {
        had_prev_callsign = aircraft_state_get_own(icao24, now_us,
                                                    PK_REC_LOOKUP_MAX_AGE_US, &prev_ac)
                            && prev_ac.have_callsign;
    }

    /* Update the per-aircraft fusion table (callsign / altitude /
     * velocity). The CPR position is fed in separately further down,
     * once the global decoder has both even+odd frames. */
    aircraft_state_ingest(mm, now_us);

    /* Fan-out to record sinks (UART debug + file storage + BLE raw).
     * The hex payload feeds Pilot-Kit/scripts/adsb_to_track.py verbatim. */
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        record_t rec = {
            .ts_ms   = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL,
            .icao24  = icao24,
            .df      = (uint8_t)mm->msgtype,
            .hex_len = (uint8_t)(mm->msgbits / 8 * 2),
        };
        const int msg_bytes = mm->msgbits / 8;
        for (int i = 0; i < msg_bytes; ++i) {
            static const char hex_chars[] = "0123456789ABCDEF";
            rec.hex[i * 2]     = hex_chars[(mm->msg[i] >> 4) & 0xF];
            rec.hex[i * 2 + 1] = hex_chars[mm->msg[i]        & 0xF];
        }
        rec.hex[msg_bytes * 2] = '\0';
        record_dispatch(&rec);
    }

    const char *unit_str = (mm->unit == MODE_S_UNIT_METERS) ? "m" : "ft";

    switch (mm->msgtype) {
    case 11:
        s_msgs_df11++;
        ESP_LOGD(TAG_ADSB, "[%06" PRIX32 "] DF11 all-call (ca=%d)", icao24, mm->ca);
        break;

    case 17:
    case 18:
        /* aircraft_state_ingest() already rejects non-ICAO DF18 addresses.
         * The post-ingest CPR fan-out below must not bypass that gate:
         * CF3/4/7 leave the standard ME fields zeroed, so decoding them as
         * surface CPR would invent a ground target at the reference point. */
        if (!mm->aa_is_icao) break;
        if (mm->metype >= 1 && mm->metype <= 4) {
            s_msgs_df17_id++;
            ESP_LOGD(TAG_ADSB, "[%06" PRIX32 "] DF17 ident   callsign=\"%s\"",
                     icao24, mm->flight);

            /* 落盘：呼号变化时写一条 traffic.trk 身份记录（rec_type=1）。
             * 用 ingest 前后的快照比较——aircraft_state 已经把 dump1090 的
             * 尾部下划线/空格去掉了，cur_ac.callsign 就是要落盘的干净值。 */
            aircraft_t cur_ac;
            if (aircraft_state_get_own(icao24, now_us, PK_REC_LOOKUP_MAX_AGE_US, &cur_ac) &&
                cur_ac.have_callsign) {
                bool changed = !had_prev_callsign ||
                              strncmp(prev_ac.callsign, cur_ac.callsign,
                                      AIRCRAFT_CALLSIGN_LEN) != 0;
                if (changed) {
                    struct timeval tv_id;
                    gettimeofday(&tv_id, NULL);
                    int64_t ts_ms = (int64_t)tv_id.tv_sec * 1000LL + tv_id.tv_usec / 1000LL;
                    pk_rec_ingest_identity(icao24, ts_ms, cur_ac.callsign,
                                           (uint8_t)cur_ac.wake);
                }
            }
        } else if ((mm->metype >= 9 && mm->metype <= 18) ||
                   (mm->metype >= 20 && mm->metype <= 22)) {
            s_msgs_df17_pos++;
            cpr_position_t pos = { .valid = false };
            bool fresh = cpr_decode_position(icao24, mm->fflag, /*is_surface=*/false,
                                             mm->raw_latitude,
                                             mm->raw_longitude,
                                             now_us, &pos);
            if (pos.valid) {
                if (fresh) s_pos_decoded++;
                /* Mirror the freshly decoded position into aircraft_state
                 * so the GDL90 Traffic Report can carry real lat/lon. */
                aircraft_state_update_position(icao24, pos.lat, pos.lon, now_us);

                if (fresh) {
                    /* 落盘：只在 CPR 解出**新**位置时写一条 traffic.trk 位置
                     * 记录——不能用 pos.valid，那个在缓存命中时也是 true，
                     * 会重复写同一个位置（spec「写入时机」节点名的这条）。
                     * gs/track/vs 这三个不在本消息里（来自 metype 19），从
                     * aircraft_state 融合表取当前已知值——空中位置帧里这
                     * 三者各自有独立的有效位（垂速会单独 N/A，地速与航迹
                     * 的两个分量也会各自 N/A），必须分别传——早先一个
                     * have_velocity 传三次，等于把"没报垂速"落盘成
                     * "垂速 0"；altitude 就是本消息自己的（ingest 已经在
                     * 上面把它写进了 aircraft_state，这里复用同一份转换
                     * 结果，不用再重算一次 m→ft）。
                     *
                     * 注意 aircraft_state_get_own() 现在自带字段级过期：
                     * 这里传的 24 小时窗口只决定"这架飞机还在不在表里"，
                     * 拿到的每个字段都已经按 60 s 判过新鲜度。 */
                    aircraft_t cur_ac;
                    bool have_cur = aircraft_state_get_own(icao24, now_us,
                                                           PK_REC_LOOKUP_MAX_AGE_US, &cur_ac);
                    struct timeval tv_pos;
                    gettimeofday(&tv_pos, NULL);
                    int64_t ts_ms = (int64_t)tv_pos.tv_sec * 1000LL + tv_pos.tv_usec / 1000LL;
                    pk_rec_ingest_position(icao24, ts_ms, pos.lat, pos.lon,
                                           have_cur && cur_ac.have_altitude,
                                           have_cur ? cur_ac.altitude_ft : 0,
                                           have_cur && cur_ac.have_ground_speed,
                                           have_cur ? cur_ac.ground_speed_kt : 0,
                                           have_cur && cur_ac.have_heading,
                                           have_cur ? cur_ac.heading_deg : 0,
                                           have_cur && cur_ac.have_vertical_rate,
                                           have_cur ? cur_ac.vert_rate_fpm : 0,
                                           have_cur && cur_ac.have_air_ground &&
                                               cur_ac.on_ground,
                                           /*from_surface_cpr=*/false);
                }

                ESP_LOGD(TAG_ADSB,
                         "[%06" PRIX32 "] DF17 air-pos alt=%d%s  pos=%.5f,%.5f%s",
                         icao24, mm->altitude, unit_str, pos.lat, pos.lon,
                         fresh ? "  (fresh)" : "");
            } else {
                ESP_LOGD(TAG_ADSB,
                         "[%06" PRIX32 "] DF17 air-pos alt=%d%s  pos=pending "
                         "(%s frame, awaiting %s)",
                         icao24, mm->altitude, unit_str,
                         mm->fflag ? "odd" : "even",
                         mm->fflag ? "even" : "odd");
            }
        } else if (mm->metype >= 5 && mm->metype <= 8) {
            /* Surface position (阶段 4b). Local (single-frame) CPR decode
             * per DO-260B A.1.7.3 — see cpr_decode.h's header comment for
             * why global odd+even pairing doesn't work well for ground
             * targets (squitter rate drops to ~5s once stationary,
             * routinely exceeding CPR_PAIR_MAX_AGE_US).
             *
             * Reference position priority: own-ship GPS fix first (most
             * accurate, always near the receiver so always within the
             * ±45 NM local-decode radius of any traffic close enough to
             * be picked up on 1090ES anyway); falling back to this
             * aircraft's own last known position (airborne or surface —
             * whichever is freshest in aircraft_state) when GPS has no
             * fix. No reference at all → reject; a "close enough" guess
             * risks a wrong-but-plausible-looking position, which is
             * worse than pos=pending. */
            s_msgs_df17_pos++;

            double ref_lat = 0.0, ref_lon = 0.0;
            bool have_ref = false;
            const char *ref_src = "none";

            pk_gps_state_t gps;
            if (pk_gps_get(&gps) && gps.have_fix) {
                ref_lat  = gps.lat;
                ref_lon  = gps.lon;
                have_ref = true;
                ref_src  = "gps";
            } else {
                aircraft_t last_ac;
                if (aircraft_state_get_own(icao24, now_us, PK_REC_LOOKUP_MAX_AGE_US,
                                           &last_ac) && last_ac.have_position) {
                    ref_lat  = last_ac.lat;
                    ref_lon  = last_ac.lon;
                    have_ref = true;
                    ref_src  = "last-known";
                }
            }

            cpr_position_t pos = { .valid = false };
            if (have_ref) {
                cpr_decode_surface_local(mm->fflag, mm->raw_latitude, mm->raw_longitude,
                                         ref_lat, ref_lon, &pos);
            }

            if (pos.valid) {
                s_pos_decoded++;
                aircraft_state_update_position(icao24, pos.lat, pos.lon, now_us);

                /* 落盘：局部解码单帧即出结果，没有"缓存命中重复写"这回
                 * 事（不像空中 CPR 的全局配对那样 pos.valid 在缓存命中时
                 * 也为 true），每个成功解码都是新位置，直接写。gs/track
                 * 直接取本消息自身字段——地面目标不发 metype 19，等不到
                 * 也不该等 aircraft_state 融合表（那边可能还是起飞前最
                 * 后一次空中速度的陈旧值）。vs 地面帧没有这个字段，恒
                 * 无效。 */
                struct timeval tv_pos;
                gettimeofday(&tv_pos, NULL);
                int64_t ts_ms = (int64_t)tv_pos.tv_sec * 1000LL + tv_pos.tv_usec / 1000LL;
                bool have_gs = mm->surface_ground_speed >= 0.0;
                pk_rec_ingest_position(icao24, ts_ms, pos.lat, pos.lon,
                                       /*have_alt=*/false, 0,
                                       have_gs, have_gs ? (int)lround(mm->surface_ground_speed) : 0,
                                       mm->surface_track_valid,
                                       mm->surface_track_valid ? (int)lround(mm->surface_track) : 0,
                                       /*have_vs=*/false, 0,
                                       /*on_ground=*/true, /*from_surface_cpr=*/true);

                ESP_LOGD(TAG_ADSB,
                         "[%06" PRIX32 "] DF17 surf-pos pos=%.5f,%.5f (ref=%s)",
                         icao24, pos.lat, pos.lon, ref_src);
            } else {
                ESP_LOGD(TAG_ADSB,
                         "[%06" PRIX32 "] DF17 surf-pos pos=pending (ref=%s)",
                         icao24, ref_src);
            }
        } else if (mm->metype == 19) {
            s_msgs_df17_vel++;
            /* subtype 1/2 是地速矢量，3/4 是空速 + 空中航向；两者的
             * heading/velocity 含义不同，日志里点明 mesub 才看得懂。
             * "n/a" 直接写出来——排障时"没报"与"报了 0"必须一眼可分。 */
            ESP_LOGD(TAG_ADSB,
                     "[%06" PRIX32 "] DF17 %s hdg=%.1f%s speed=%d%s "
                     "vrate=%d%s (mesub=%d)",
                     icao24,
                     (mm->mesub <= 2) ? "velocity" : "airspeed",
                     mm->heading, mm->heading_is_valid ? "" : " n/a",
                     mm->velocity, mm->velocity_valid ? "" : " n/a",
                     mm->vert_rate, mm->vert_rate_valid ? "" : " n/a",
                     mm->mesub);
        } else {
            s_msgs_other++;
            ESP_LOGD(TAG_ADSB, "[%06" PRIX32 "] DF17 metype=%d (uncategorised)",
                     icao24, mm->metype);
        }
        break;

    case 20:
        s_msgs_df20_21++;
        ESP_LOGD(TAG_ADSB, "[%06" PRIX32 "] DF20 Mode-S long  alt=%d%s",
                 icao24, mm->altitude, unit_str);
        break;

    case 21:
        /* DF21 is Comm-B Identity Reply — it carries Squawk identity in
         * the bits where DF20 has altitude. The vendored decoder doesn't
         * populate mm->altitude for DF21 (so it's stack residue from the
         * previous decode); only mm->identity is meaningful here. */
        s_msgs_df20_21++;
        ESP_LOGD(TAG_ADSB, "[%06" PRIX32 "] DF21 Mode-S long  squawk=%04d",
                 icao24, mm->identity);
        break;

    default:
        s_msgs_other++;
        ESP_LOGD(TAG_ADSB, "[%06" PRIX32 "] DF%d (uncategorised)",
                 icao24, mm->msgtype);
        break;
    }
}

/* Dump every aircraft seen in the trailing 30-minute window, bucketed
 * by recency so the user can tell "in view now" apart from "lost a
 * while ago." Tier thresholds:
 *   - fresh:  last seen in the trailing 60s
 *   - recent: 60s .. 15min
 *   - older:  15min .. 30min
 * Each aircraft appears in exactly one tier. Aircraft last seen > 30
 * min ago are dropped (also the LRU table caps at 64 slots so they
 * eventually get evicted on first contact with new traffic).
 *
 * Snapshot buffer is ~64 * 72 ≈ 4.5 KiB; lives in PSRAM .bss to keep
 * it off the task stack. Single-caller — no lock needed. */
#define SUMMARY_TIER_FRESH_US   (60ULL * 1000000ULL)          /* 60 s   */
#define SUMMARY_TIER_RECENT_US  (15ULL * 60ULL * 1000000ULL)  /* 15 min */
#define SUMMARY_TIER_OLDER_US   (30ULL * 60ULL * 1000000ULL)  /* 30 min */

static EXT_RAM_BSS_ATTR aircraft_t s_summary_snap[AIRCRAFT_TABLE_CAPACITY];

static void format_aircraft_line(char *buf, size_t bufsz,
                                 const aircraft_t *a, int64_t now_us)
{
    int pos = 0;
    pos += snprintf(buf + pos, bufsz - pos, "    [%06" PRIX32 "]", a->icao24);

    if (a->have_callsign) {
        pos += snprintf(buf + pos, bufsz - pos, " cs=%-8s", a->callsign);
    } else {
        pos += snprintf(buf + pos, bufsz - pos, " cs=--------");
    }

    if (a->have_altitude) {
        pos += snprintf(buf + pos, bufsz - pos, " alt=%dft", a->altitude_ft);
    } else {
        pos += snprintf(buf + pos, bufsz - pos, " alt=--");
    }

    if (a->have_position) {
        pos += snprintf(buf + pos, bufsz - pos,
                        " pos=%.4f,%.4f", a->lat, a->lon);
    } else {
        pos += snprintf(buf + pos, bufsz - pos, " pos=--,--");
    }

    /* 三个量各自独立有效——诊断行分别打，"没报"与"报了 0"混在一起，
     * 排障时会把一架不广播垂速的飞机看成平飞。 */
    if (a->have_heading) {
        pos += snprintf(buf + pos, bufsz - pos, " hdg=%3d°", a->heading_deg);
    }
    if (a->have_ground_speed) {
        pos += snprintf(buf + pos, bufsz - pos, " spd=%dkt", a->ground_speed_kt);
    }
    if (a->have_vertical_rate) {
        pos += snprintf(buf + pos, bufsz - pos, " vrt=%+dfpm", a->vert_rate_fpm);
    }
    if (a->have_air_ground && a->on_ground) {
        pos += snprintf(buf + pos, bufsz - pos, " GND");
    }

    int64_t age_us = now_us - a->last_seen_us;
    double  age_s  = (double)age_us / 1e6;
    if (age_s < 60.0) {
        pos += snprintf(buf + pos, bufsz - pos, " (age=%.1fs)", age_s);
    } else {
        pos += snprintf(buf + pos, bufsz - pos, " (age=%.1fmin)", age_s / 60.0);
    }
}

static void aircraft_summary_emit(int64_t now_us)
{
    /* 等级门必须排在 aircraft_state_snapshot() **之前**（P1-D）。
     *
     * 光把下面的 ESP_LOGI 降成 ESP_LOGD 是不够的：快照要拷最多 64 个
     * aircraft_t 并且要拿 aircraft_state 的表锁，format_aircraft_line() 每架
     * 还要一次 snprintf——这些都不在日志宏里，降等级它们照跑。默认等级下整段
     * 直接返回，代价只剩一次 esp_log_level_get()。
     *
     * 为什么这段非降不可：最多 ~26 行 × ~10 ms ≈ 260 ms 连续阻塞，而它跑在
     * RX 任务的循环里。那 260 ms 内 921600 波特的链路能灌进约 30 KB，RX 环
     * 是 4096 字节——溢出是必然，不是概率。 */
    if (esp_log_level_get(TAG) < ESP_LOG_DEBUG) return;

    size_t n = aircraft_state_snapshot(s_summary_snap,
                                       AIRCRAFT_TABLE_CAPACITY,
                                       now_us,
                                       SUMMARY_TIER_OLDER_US);

    /* Block-bracket the whole report with a visible divider so it
     * stands out against the per-second "dsp:" / "pfd:" / "adsb:"
     * heartbeat lines. */
    ESP_LOGD(TAG, "");
    ESP_LOGD(TAG, "==================== AIRCRAFT SUMMARY ====================");
    /* 演示模式下这张表来自 demo_data.c 而不是空中收到的报文。不标出来的话，
     * 一份串口日志里既有"SDR 没插"又有 17 架飞机，看的人只会认为解码坏了。 */
    if (pk_demo_enabled())
        ESP_LOGD(TAG, "  *** DEMO MODE — the contacts below are SIMULATED ***");

    if (n == 0) {
        ESP_LOGD(TAG, "  (no contacts in the last 30 min)");
        ESP_LOGD(TAG, "==========================================================");
        ESP_LOGD(TAG, "");
        return;
    }

    /* Bucket count first so the header can be precise. */
    size_t fresh_cnt = 0, recent_cnt = 0, older_cnt = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t age = (uint64_t)(now_us - s_summary_snap[i].last_seen_us);
        if      (age <= SUMMARY_TIER_FRESH_US)  ++fresh_cnt;
        else if (age <= SUMMARY_TIER_RECENT_US) ++recent_cnt;
        else                                    ++older_cnt;
    }
    ESP_LOGD(TAG,
             "  %u tracked  |  fresh<60s: %u   recent<15min: %u   older<30min: %u",
             (unsigned)n,
             (unsigned)fresh_cnt, (unsigned)recent_cnt, (unsigned)older_cnt);

    /* Print one section per tier; each aircraft lands in exactly the
     * freshest tier its age qualifies for. */
    struct { const char *label; uint64_t hi_us; } tiers[] = {
        { "last 60s",       SUMMARY_TIER_FRESH_US  },
        { "60s .. 15min",   SUMMARY_TIER_RECENT_US },
        { "15min .. 30min", SUMMARY_TIER_OLDER_US  },
    };
    /* Per-tier print cap. With > ~40 tracked aircraft, the unbounded
     * loop emitted enough ESP_LOGI lines (each ~130 B blocking the
     * 115200-baud UART for ~11 ms) that the old IQ-era dsp task stalled
     * long enough for the IQ ring buffer to overflow (2026-08 实测，
     * ringbuffer 已随 SDR 路径退役). Capping each tier keeps the
     * summary at most ~24 lines + headers (~260 ms blocking) — the
     * UART stall itself hasn't gone away, so the cap stays. */
    #define SUMMARY_TIER_PRINT_CAP  8
    bool printed[AIRCRAFT_TABLE_CAPACITY] = { 0 };
    for (size_t t = 0; t < sizeof(tiers) / sizeof(tiers[0]); ++t) {
        bool   header_printed = false;
        size_t tier_emitted   = 0;
        size_t tier_total     = 0;
        for (size_t i = 0; i < n; ++i) {
            if (printed[i]) continue;
            uint64_t age = (uint64_t)(now_us - s_summary_snap[i].last_seen_us);
            if (age > tiers[t].hi_us) continue;
            tier_total++;
            if (tier_emitted >= SUMMARY_TIER_PRINT_CAP) {
                printed[i] = true;   /* still mark consumed so the next tier skips it */
                continue;
            }
            if (!header_printed) {
                ESP_LOGD(TAG, "  --- %s ---", tiers[t].label);
                header_printed = true;
            }
            char line[160];
            format_aircraft_line(line, sizeof(line), &s_summary_snap[i], now_us);
            ESP_LOGD(TAG, "%s", line);
            printed[i] = true;
            tier_emitted++;
        }
        if (tier_total > SUMMARY_TIER_PRINT_CAP) {
            ESP_LOGD(TAG, "    ... and %u more in this tier",
                     (unsigned)(tier_total - SUMMARY_TIER_PRINT_CAP));
        }
    }

    ESP_LOGD(TAG, "==========================================================");
    ESP_LOGD(TAG, "");
}

static void dashboard_emit_and_reset(int64_t now_us, int64_t window_start_us)
{
    (void)now_us; (void)window_start_us;  /* 无 MB/s 归一后不再参与计算 */
    const uint32_t rx  = atomic_load_explicit(&s_stats.rx_frames,
                                              memory_order_relaxed) - s_win_rx;
    const uint32_t crc = atomic_load_explicit(&s_stats.rx_crc_errors,
                                              memory_order_relaxed) - s_win_crc;

    /* UART sink 的丢弃行。
     *
     * 看串口的人恰恰是被丢弃直接影响的那个人（丢掉的就是他本该看到的那几
     * 行），得当场告诉他，而不是只留在诊断页里等他自己去翻。
     *
     * 但只在本窗口**有新增**丢弃时出这一行：计数是自启动累计的，写成
     * `if (uart_drop)` 就是无条件打印——丢过一次之后每秒都打，给已经拥塞的
     * 控制台再加负载。预算：这一行约 80 字节 ≈ 7 ms，1 Hz 且仅在丢弃期间，
     * 相对同期 256 行/秒的正常输出是 0.4%。
     *
     * 排在下面的"无链路则静默"之前：那条 goto 会跳过整段输出，基线也就不再
     * 推进，等链路恢复时会把停摆期间的陈旧累计一次性报出来。 */
    {
        uint32_t uart_drop = 0;
        if (record_sink_uart_stats(NULL, &uart_drop, NULL) &&
            uart_drop != s_win_uart_drop) {
            ESP_LOGW(TAG, "uart sink dropped %u this window (%u since boot) — "
                          "console can't keep up",
                     (unsigned)(uart_drop - s_win_uart_drop),
                     (unsigned)uart_drop);
            s_win_uart_drop = uart_drop;
        }
    }

    /* Stay quiet when there's no link to talk about. This happens
     * whenever the RP2040 isn't wired up / isn't sending yet; printing
     * "rx/s 0" every second in that state is just noise that drowns
     * out the rest of the system. The 1 Hz dashboard resumes the
     * instant frames flow. */
    if (rx == 0 && crc == 0) {
        goto reset;
    }

    const uint32_t icao_seen = atomic_load_explicit(&s_icao_unique,
                                                    memory_order_relaxed);
    if (crc == 0) {
        ESP_LOGI(TAG,
                 "link rx/s %u crcerr/s %u | msgs/s %u (df17_pos %u "
                 "df17_id %u) | aircraft %u",
                 (unsigned)rx,
                 (unsigned)crc,
                 (unsigned)s_msgs_total,
                 (unsigned)s_msgs_df17_pos,
                 (unsigned)s_msgs_df17_id,
                 (unsigned)icao_seen);
    } else {
        ESP_LOGW(TAG,
                 "link rx/s %u crcerr/s %u (BAD CRC) | msgs/s %u | aircraft %u",
                 (unsigned)rx,
                 (unsigned)crc,
                 (unsigned)s_msgs_total,
                 (unsigned)icao_seen);
    }

    /* UAT 前门累计计数（uat_ingest 口径同 modes_ingest：成功/RS 不可纠/
     * 全零无帧分桶、单调累计；诊断页接入属后续任务）。只在出现过 UAT
     * 帧后输出，纯 1090 环境不加常驻噪声。 */
    {
        uint32_t uat_ok = 0, uat_bad = 0, uat_nosync = 0;
        uat_ingest_get_stats(&uat_ok, &uat_bad, &uat_nosync);
        if (uat_ok || uat_bad)
            ESP_LOGI(TAG, "uat uplink: ok=%u bad_rs=%u no_sync=%u",
                     (unsigned)uat_ok, (unsigned)uat_bad,
                     (unsigned)uat_nosync);
    }

reset:;

    /* Flush 1-Hz window totals into boot-lifetime cumulative counters
     * before zeroing the window. s_icao_unique is already cumulative
     * (never reset) — exposed as-is by pk_dsp_get_stats(). */
    atomic_fetch_add_explicit(&s_msgs_total_cum, s_msgs_total,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&s_pos_decoded_cum, s_pos_decoded,
                              memory_order_relaxed);

    s_win_rx        = atomic_load_explicit(&s_stats.rx_frames,
                                           memory_order_relaxed);
    s_win_crc       = atomic_load_explicit(&s_stats.rx_crc_errors,
                                           memory_order_relaxed);
    s_msgs_total    = 0;
    s_msgs_df11     = 0;
    s_msgs_df17_id  = 0;
    s_msgs_df17_pos = 0;
    s_msgs_df17_vel = 0;
    s_msgs_df20_21  = 0;
    s_msgs_other    = 0;
    s_pos_decoded   = 0;
}

/* --- UAT 上行 sink（协议 v1.1 §6 UAT_UPLINK；前门 uat_ingest 的业务层）--
 *
 * 融合口径（Task 2 裁决，证据 = UAT 事实卡 §4/§9）：UAT **上行**消息层
 * 没有目标身份字段——帧头的 lat/lon 是地面站站点坐标，信息帧载荷
 * （FIS-B 产品数据 / TIS-B 目标报告）语义要到 T5 才解码。所以本 sink
 * 目前只打日志，不喂 aircraft_state；T5 解出 TIS-B/ADS-R 目标后，目标
 * 必须走与 1090 相同的 aircraft_state 入口（icao24 键、同一张表，见
 * uat_ingest.h 头注释的融合约定），不得另起并行状态库。地面站 1 帧/s，
 * 每帧一条 LOGI 的量级与 Mode-S 逐帧日志一致。 */
static void on_uat_uplink(const uat_uplink_t *up,
                          const uat_ingest_meta_t *meta,
                          uint8_t rs_corrected, void *user)
{
    (void)meta; (void)user;
    ESP_LOGI(TAG_ADSB,
             "UAT uplink: site=%.5f,%.5f slot=%u tisb=%u info=%u rs_corr=%u",
             up->lat_deg_e6 / 1e6, up->lon_deg_e6 / 1e6,
             up->slot_id, up->tisb_site_id, up->num_info_frames, rs_corrected);
}

/* --- 链路消息分发 ------------------------------------------------------ */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * P4→RP 的统一发送口：共用一个 seq 计数器 + 一把互斥锁。
 *
 * 协议 §2 的 seq 是「按发送方递增」。此前 HELLO 回应用的是自己的静态计数
 * 器，再多一个调用点就会出现两条独立序列，RP 侧的 seq_gaps 会持续虚增而
 * 且没人能解释。锁是因为 HELLO 回应跑在链路任务、配置下发跑在 UI 任务：
 * IDF 的 uart_write_bytes 自身有 tx_mux，单次调用写整帧不会字节级交错，
 * 但 seq 的读改写没有它保护。
 *
 * out 用 static 而不是栈：ADSB_LINK_MAX_FRAME 是 586 B，UI 任务的栈不该
 * 为一次配置下发抖这么一下。它在锁内使用，是安全的。
 */
static SemaphoreHandle_t s_tx_mux;
static uint8_t s_tx_seq;

static void link_tx(uint8_t type, const uint8_t *pl, size_t len)
{
    if (!s_tx_mux) return;               /* 链路任务还没起来，等 HELLO 那次 */
    static uint8_t out[ADSB_LINK_MAX_FRAME];
    xSemaphoreTake(s_tx_mux, portMAX_DELAY);
    size_t n = adsb_link_encode(out, sizeof out, type, s_tx_seq++, pl, len);
    if (n) uart_write_bytes(ADSB_UART, out, n);
    xSemaphoreGive(s_tx_mux);
}

void pk_adsb_link_push_config(void)
{
    uint8_t pl[1 + ADSB_LINK_CFG_MAX_ITEMS * 2];
    size_t n = pk_antenna_build_config_payload(pl, sizeof pl);
    if (n) link_tx(ADSB_LINK_MSG_CONFIG_REQ, pl, n);
}

static void on_link_msg(void *user, const adsb_link_msg_t *m)
{
    (void)user;
    atomic_store_explicit(&s_last_frame_us, esp_timer_get_time(),
                          memory_order_relaxed);
    atomic_store_explicit(&s_ever_linked, true, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_stats.rx_frames, 1, memory_order_relaxed);

    switch (m->type) {
    case ADSB_LINK_MSG_MODES_RAW: {
        if (m->payload_len < 6) break;
        int msgbits = (m->payload[0] & ADSB_LINK_MODES_LONG) ? 112 : 56;
        size_t need = 6 + (size_t)(msgbits / 8);
        if (m->payload_len < need) break;          /* 短包：丢弃，等 HEALTH 里看 */
        modes_ingest_meta_t meta = {
            .rssi     = m->payload[1],
            .rp_ts_us = (uint32_t)(m->payload[2] | (m->payload[3] << 8) |
                                   ((uint32_t)m->payload[4] << 16) |
                                   ((uint32_t)m->payload[5] << 24)),
        };
        modes_ingest_feed(m->payload + 6, msgbits, &meta);
        atomic_fetch_add_explicit(&s_stats.modes_fed, 1, memory_order_relaxed);
        break;
    }
    case ADSB_LINK_MSG_UAT_UPLINK: {
        /* v1.1 §6：固定 557 B payload = 5 B 元数据 + 552 B 交织帧原样
         * （帧内容合同见 uat_decode.h / 事实卡 §7）。RP2040 侧生产者
         * （CC1312R 经 SPI 转发）在 T3 实装——本分发先就位。 */
        uint8_t u_rssi; uint32_t u_ts; const uint8_t *u_fr;
        if (!adsb_link_uat_uplink_decode(m->payload, m->payload_len,
                                         &u_rssi, &u_ts, &u_fr)) {
            /* 长度不符：整帧丢弃（协议 §6.1）。CRC 已过所以 codec
             * 统计不可见——这里单独计数（终审 Minor：集成诊断页时
             * 消费）。 */
            atomic_fetch_add_explicit(&s_stats.uat_len_drop, 1,
                                      memory_order_relaxed);
            break;
        }
        uat_ingest_meta_t meta = { .rssi = u_rssi, .rp_ts_us = u_ts };
        uat_ingest_feed(u_fr, &meta);
        break;
    }
    case ADSB_LINK_MSG_HELLO:
    case ADSB_LINK_MSG_CAPABILITIES: {
        /* 协议 §5：P4 对**每个**合法 HELLO 都回一帧（audit round 3，ledger
         * R9）。旧的一次性闩锁在 RP 侧重启后永远等不到回应——RP 侧只有
         * 未 linked 才发 HELLO、限速 1 Hz（p4_link.c），逐帧回应无洪泛
         * 风险。日志只首条 LOGI，之后降 DEBUG。seq 用本侧单调计数（协议
         * §2 seq 是按发送方递增的）：恒 0 会让 RP 侧 seq_gaps 持续虚增。 */
        static bool s_hello_logged;
        uint8_t pl[17] = { 0 };              /* min_minor + build[16] */
        memcpy(pl + 1, "p4-mvp", sizeof "p4-mvp");
        link_tx(ADSB_LINK_MSG_HELLO, pl, sizeof pl);
        /* RP2040 不持久化配置，上电只回到 rf_safety 的安全默认。它每重启
         * 一次就重新发 HELLO，所以把"推配置"挂在这里而不是 linked 边沿：
         * 边沿判定漏一次，用户的天线选择就要等到下次开机才生效。 */
        pk_adsb_link_push_config();
        if (s_hello_logged)
            ESP_LOGD(TAG, "RP2040 %s seq=%u", m->type == ADSB_LINK_MSG_HELLO
                     ? "HELLO" : "CAPABILITIES", m->seq);
        else {
            ESP_LOGI(TAG, "RP2040 %s seq=%u", m->type == ADSB_LINK_MSG_HELLO
                     ? "HELLO" : "CAPABILITIES", m->seq);
            s_hello_logged = true;
        }
        break;
    }
    case ADSB_LINK_MSG_HEALTH_STATS:
        if (m->payload_len < 40) break;
        /* 1 Hz 概要打进日志；诊断页取 P4 本地计数。 */
        ESP_LOGI(TAG, "RP health: pre=%u f56=%u f112=%u degraded=%u noise=%u "
                      "ovr=%u tx=%u txdrop=%u rx=%u gap=%u",
                 le32(m->payload + 0),  le32(m->payload + 4),
                 le32(m->payload + 8),  le32(m->payload + 12),
                 le32(m->payload + 16), le32(m->payload + 20),
                 le32(m->payload + 24), le32(m->payload + 28),
                 le32(m->payload + 32), le32(m->payload + 36));
        break;
    case ADSB_LINK_MSG_ERROR:
        ESP_LOGW(TAG_ADSB, "RP error code=%u", m->payload_len ? m->payload[0] : 0);
        break;
    default:
        break;                                     /* 未知类型：忽略（规范§3.5） */
    }
}

static void adsb_link_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "adsb_link_task running (UART%d rx=%d tx=%d %d baud)",
             ADSB_UART, 46, ADSB_TX_GPIO, ADSB_UART_BAUD);

    cpr_init();
    modes_ingest_init(on_ingest_msg, NULL);
    uat_ingest_init(on_uat_uplink, NULL);
    adsb_link_dec_init(&s_dec, on_link_msg, NULL);
    atomic_store_explicit(&s_last_frame_us, esp_timer_get_time(),
                          memory_order_relaxed);

    const uart_config_t uc = {
        .baud_rate  = ADSB_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(ADSB_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(ADSB_UART, ADSB_TX_GPIO, 46,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(ADSB_UART, ADSB_UART_RX_BUF,
                                        ADSB_UART_TX_BUF, 0, NULL, 0));

    int64_t window_start_us      = esp_timer_get_time();
    int64_t summary_last_emit_us = window_start_us;

    while (1) {
        int n = uart_read_bytes(ADSB_UART, s_rxchunk, sizeof(s_rxchunk),
                                pdMS_TO_TICKS(100));
        if (n > 0) adsb_link_dec_feed(&s_dec, s_rxchunk, (size_t)n);

        atomic_store_explicit(&s_stats.rx_crc_errors, s_dec.crc_errors,
                              memory_order_relaxed);
        atomic_store_explicit(&s_stats.rx_seq_gaps, s_dec.seq_gaps,
                              memory_order_relaxed);
        atomic_store_explicit(&s_stats.rx_resyncs, s_dec.resyncs,
                              memory_order_relaxed);
        if (s_dec.version_mismatch)
            atomic_store_explicit(&s_proto_mismatch_seen, true,
                                  memory_order_relaxed);

        int64_t now_us = esp_timer_get_time();
        if (now_us - window_start_us >= 1000000) {
            dashboard_emit_and_reset(now_us, window_start_us);  /*【迁移+改造】*/
            window_start_us = now_us;
        }
        if (now_us - summary_last_emit_us >= 30000000) {
            aircraft_summary_emit(now_us);                       /*【迁移】*/
            summary_last_emit_us = now_us;
        }
    }
}

pk_adsb_link_state_t pk_adsb_link_state_get(pk_adsb_link_stats_t *stats)
{
    if (stats) {
        /* 逐字段 relaxed load：无跨字段一致性承诺（各计数单调，诊断页
         * 口径），见 s_stats 声明处注释。 */
        stats->rx_frames     = atomic_load_explicit(&s_stats.rx_frames,
                                                    memory_order_relaxed);
        stats->rx_crc_errors = atomic_load_explicit(&s_stats.rx_crc_errors,
                                                    memory_order_relaxed);
        stats->rx_seq_gaps   = atomic_load_explicit(&s_stats.rx_seq_gaps,
                                                    memory_order_relaxed);
        stats->rx_resyncs    = atomic_load_explicit(&s_stats.rx_resyncs,
                                                    memory_order_relaxed);
        stats->modes_fed     = atomic_load_explicit(&s_stats.modes_fed,
                                                    memory_order_relaxed);
    }
    if (atomic_load_explicit(&s_proto_mismatch_seen, memory_order_relaxed))
        return PK_ADSB_LINK_PROTO_MISMATCH;
    if (!atomic_load_explicit(&s_ever_linked, memory_order_relaxed))
        return PK_ADSB_LINK_NO_LINK;
    return (esp_timer_get_time() -
            atomic_load_explicit(&s_last_frame_us, memory_order_relaxed)
            > LINK_STALE_US)
               ? PK_ADSB_LINK_STALLED : PK_ADSB_LINK_LINKED;
}

void pk_dsp_get_stats(pk_dsp_stats_t *out)
{
    if (out == NULL) return;
    uint32_t ok = 0, bad = 0;
    modes_ingest_get_stats(&ok, &bad);
    out->msgs_total     = ok;
    out->frames_bad_crc = bad;
    out->pos_decoded    = atomic_load_explicit(&s_pos_decoded_cum,
                                               memory_order_relaxed);
    out->icao_unique    = atomic_load_explicit(&s_icao_unique,
                                               memory_order_relaxed);
}

void pk_adsb_link_start(void)
{
    /* 先建锁再起任务：任务一跑起来就可能收到 HELLO 并回帧，link_tx 在锁
     * 还是 NULL 时会静默跳过，那一帧就白丢了。 */
    s_tx_mux = xSemaphoreCreateMutex();
    assert(s_tx_mux != NULL);
    BaseType_t ok = xTaskCreatePinnedToCore(adsb_link_task, "adsb_lnk",
                                            8192, NULL, 5, NULL, 1);
    assert(ok == pdTRUE);
}
