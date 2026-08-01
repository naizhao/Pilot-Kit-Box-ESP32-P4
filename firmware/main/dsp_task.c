/*
 * dsp_task.c — ADS-B edge decoder.
 *
 * Drains g_iq_ringbuf in fixed-size chunks, runs the dump1090-derived
 * magnitude / preamble / Manchester decode chain inherited from
 * naizhao/esp32-rtl-sdr's `mode-s.c`, and pretty-prints every CRC-valid
 * message to the console.
 *
 *   DF11 (all-call reply)        -> "<ICAO> df=11"
 *   DF17 metype 1..4  (ident)    -> "<ICAO> callsign=<flight>"
 *   DF17 metype 9..18 (airborne) -> "<ICAO> alt=<ft|m> pos=<lat,lon>"
 *   DF17 metype 19    (velocity) -> "<ICAO> hdg=<deg> speed=<kt> vrate=<fpm>"
 *   DF20/21                       -> "<ICAO> df=<dn> alt=<ft|m>"
 *
 * Positions only emerge after the per-aircraft CPR pairing layer
 * (cpr_decode.c) has both an even and an odd frame, both fresh within
 * 10 s, and both falling in the same NL longitude zone — matching
 * RTCA DO-260B. Until that happens, position lines say "pos=pending".
 *
 * In parallel the task emits a throughput dashboard line once per
 * second:
 *
 *   I (xxx) dsp: stream 2.00 MB/s | msgs/s 23 (df17_pos 8 df17_id 2) | aircraft 14
 *
 * On real-hardware verification the dashboard going non-zero proves
 * the data pipeline is intact; the per-message lines prove the
 * decoder is converging on actual aircraft.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_attr.h"               /* EXT_RAM_BSS_ATTR */
#include "esp_log.h"
/* Stall recovery is now graceful: pk_sdr_request_reinit() (sdr_task)
 * tears down + re-opens the dongle without touching IMU / PFD / BLE.
 * esp_restart() only happens inside sdr_task after the attempt cap. */
#include "esp_timer.h"

#include "mode-s.h"
#include "pilot_kit.h"
#include "cpr_decode.h"
#include "aircraft_state.h"
#include "config_demo.h"
#include "record_sink.h"
#include "dsp_task.h"

static const char *TAG      = "dsp";
static const char *TAG_ADSB = "adsb";

/* --- Working buffers ---------------------------------------------------
 *
 * 8192 B of IQ at 2 MSPS == 4 ms of audio per dump1090 invocation. The
 * 480-byte overlap (= 240 magnitude samples = MODE_S_FULL_LEN samples)
 * is just enough to ensure preambles that land near the buffer's tail
 * still have their full message body available on the next pass; the
 * decoder's own ICAO cache transparently dedupes the re-detection that
 * happens at the overlap region.
 */
#define DSP_IQ_BUF_BYTES   8192
#define DSP_MAG_BUF_LEN    (DSP_IQ_BUF_BYTES / 2)
#define DSP_OVERLAP_BYTES  480

static uint8_t   s_iq_buf[DSP_IQ_BUF_BYTES];
static uint16_t  s_mag_buf[DSP_MAG_BUF_LEN];
static mode_s_t  s_decoder;

/* --- 1 Hz dashboard counters ------------------------------------------ */

static uint64_t s_window_bytes   = 0;
static uint32_t s_msgs_total     = 0;
static uint32_t s_msgs_df11      = 0;
static uint32_t s_msgs_df17_id   = 0;
static uint32_t s_msgs_df17_pos  = 0;
static uint32_t s_msgs_df17_vel  = 0;
static uint32_t s_msgs_df20_21   = 0;
static uint32_t s_msgs_other     = 0;
static uint32_t s_pos_decoded    = 0;

/* --- Cumulative diagnostic counters (boot-lifetime, never reset) ------- *
 * Written only from dsp_task; read by diag page via pk_dsp_get_stats().
 * 32-bit aligned r/w is atomic on ESP32-P4 (RV32), so no lock needed;
 * volatile prevents the compiler from caching stale values across tasks.
 */
static volatile uint32_t s_msgs_total_cum  = 0;   /* cumulative CRC-ok Mode-S frames */
static volatile uint32_t s_pos_decoded_cum = 0;   /* cumulative CPR position decodes  */
static volatile uint32_t s_iq_drop_total   = 0;   /* cumulative IQ bytes dropped       */

/*
 * Tiny ICAO seen-set, kept solely so the dashboard can report unique
 * aircraft observed *since boot* without poking inside cpr_decode.c.
 * 1024 slots × 4 bytes = 4 KiB; collisions just under-count, which is
 * fine for a dashboard.
 */
#define ICAO_SEEN_CAPACITY  1024
static uint32_t          s_icao_seen[ICAO_SEEN_CAPACITY];
static volatile uint32_t s_icao_unique = 0;

static void icao_seen_insert(uint32_t icao24)
{
    uint32_t idx = icao24 % ICAO_SEEN_CAPACITY;
    for (uint32_t step = 0; step < ICAO_SEEN_CAPACITY; ++step) {
        uint32_t *slot = &s_icao_seen[(idx + step) % ICAO_SEEN_CAPACITY];
        if (*slot == icao24) return;          /* already seen */
        if (*slot == 0) {
            *slot = icao24;
            s_icao_unique++;
            return;
        }
    }
    /* Table is full; we just stop counting new aircraft. */
}

/* --- Per-message handler ----------------------------------------------
 *
 * Invoked synchronously by mode_s_detect() for every preamble candidate
 * the decoder is willing to call a frame. We filter by CRC and dispatch
 * a human-readable log line per recognised message family. Runs on the
 * dsp_task; no synchronisation needed for the static counters.
 */
static void on_mode_s_msg(mode_s_t *self, struct mode_s_msg *mm)
{
    (void)self;

    /* Drop frames whose CRC is bad (or only became "ok" after a
     * single-bit forced correction — too noisy for live traffic). */
    if (!mm->crcok || mm->errorbit >= 0) return;

    uint32_t icao24 = ((uint32_t)mm->aa1 << 16)
                    | ((uint32_t)mm->aa2 << 8)
                    | (uint32_t)mm->aa3;

    s_msgs_total++;
    icao_seen_insert(icao24);

    /* Update the per-aircraft fusion table (callsign / altitude /
     * velocity). The CPR position is fed in separately further down,
     * once the global decoder has both even+odd frames. */
    const int64_t now_us = esp_timer_get_time();
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
        ESP_LOGI(TAG_ADSB, "[%06" PRIX32 "] DF11 all-call (ca=%d)", icao24, mm->ca);
        break;

    case 17:
        if (mm->metype >= 1 && mm->metype <= 4) {
            s_msgs_df17_id++;
            ESP_LOGI(TAG_ADSB, "[%06" PRIX32 "] DF17 ident   callsign=\"%s\"",
                     icao24, mm->flight);
        } else if (mm->metype >= 9 && mm->metype <= 18) {
            s_msgs_df17_pos++;
            cpr_position_t pos = { .valid = false };
            bool fresh = cpr_decode_position(icao24, mm->fflag,
                                             mm->raw_latitude,
                                             mm->raw_longitude,
                                             now_us, &pos);
            if (pos.valid) {
                if (fresh) s_pos_decoded++;
                /* Mirror the freshly decoded position into aircraft_state
                 * so the GDL90 Traffic Report can carry real lat/lon. */
                aircraft_state_update_position(icao24, pos.lat, pos.lon, now_us);
                ESP_LOGI(TAG_ADSB,
                         "[%06" PRIX32 "] DF17 air-pos alt=%d%s  pos=%.5f,%.5f%s",
                         icao24, mm->altitude, unit_str, pos.lat, pos.lon,
                         fresh ? "  (fresh)" : "");
            } else {
                ESP_LOGI(TAG_ADSB,
                         "[%06" PRIX32 "] DF17 air-pos alt=%d%s  pos=pending "
                         "(%s frame, awaiting %s)",
                         icao24, mm->altitude, unit_str,
                         mm->fflag ? "odd" : "even",
                         mm->fflag ? "even" : "odd");
            }
        } else if (mm->metype == 19) {
            s_msgs_df17_vel++;
            ESP_LOGI(TAG_ADSB,
                     "[%06" PRIX32 "] DF17 velocity hdg=%d speed=%d "
                     "vrate=%d (mesub=%d)",
                     icao24, mm->heading, mm->velocity, mm->vert_rate,
                     mm->mesub);
        } else {
            s_msgs_other++;
            ESP_LOGD(TAG_ADSB, "[%06" PRIX32 "] DF17 metype=%d (uncategorised)",
                     icao24, mm->metype);
        }
        break;

    case 20:
        s_msgs_df20_21++;
        ESP_LOGI(TAG_ADSB, "[%06" PRIX32 "] DF20 Mode-S long  alt=%d%s",
                 icao24, mm->altitude, unit_str);
        break;

    case 21:
        /* DF21 is Comm-B Identity Reply — it carries Squawk identity in
         * the bits where DF20 has altitude. The vendored decoder doesn't
         * populate mm->altitude for DF21 (so it's stack residue from the
         * previous decode); only mm->identity is meaningful here. */
        s_msgs_df20_21++;
        ESP_LOGI(TAG_ADSB, "[%06" PRIX32 "] DF21 Mode-S long  squawk=%04d",
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
 * Snapshot buffer is ~64 * 72 ≈ 4.5 KiB; lives in PSRAM .bss because
 * dsp_task's stack is only 4 KiB. Single-caller — no lock needed. */
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

    if (a->have_velocity) {
        pos += snprintf(buf + pos, bufsz - pos,
                        " hdg=%3d° spd=%dkt vrt=%+dfpm",
                        a->heading_deg,
                        a->ground_speed_kt,
                        a->vert_rate_fpm);
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
    size_t n = aircraft_state_snapshot(s_summary_snap,
                                       AIRCRAFT_TABLE_CAPACITY,
                                       now_us,
                                       SUMMARY_TIER_OLDER_US);

    /* Block-bracket the whole report with a visible divider so it
     * stands out against the per-second "dsp:" / "pfd:" / "adsb:"
     * heartbeat lines. */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "==================== AIRCRAFT SUMMARY ====================");
    /* 演示模式下这张表来自 demo_data.c 而不是空中收到的报文。不标出来的话，
     * 一份串口日志里既有"SDR 没插"又有 17 架飞机，看的人只会认为解码坏了。 */
    if (pk_demo_enabled())
        ESP_LOGW(TAG, "  *** DEMO MODE — the contacts below are SIMULATED ***");

    if (n == 0) {
        ESP_LOGI(TAG, "  (no contacts in the last 30 min)");
        ESP_LOGI(TAG, "==========================================================");
        ESP_LOGI(TAG, "");
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
    ESP_LOGI(TAG,
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
     * 115200-baud UART for ~11 ms) that dsp_task stalled long enough
     * for the IQ ringbuf to overflow. Capping each tier keeps the
     * summary at most ~24 lines + headers (~260 ms blocking) which
     * the 512 KiB ringbuf comfortably absorbs. */
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
                ESP_LOGI(TAG, "  --- %s ---", tiers[t].label);
                header_printed = true;
            }
            char line[160];
            format_aircraft_line(line, sizeof(line), &s_summary_snap[i], now_us);
            ESP_LOGI(TAG, "%s", line);
            printed[i] = true;
            tier_emitted++;
        }
        if (tier_total > SUMMARY_TIER_PRINT_CAP) {
            ESP_LOGI(TAG, "    ... and %u more in this tier",
                     (unsigned)(tier_total - SUMMARY_TIER_PRINT_CAP));
        }
    }

    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "");
}

static void dashboard_emit_and_reset(int64_t now_us, int64_t window_start_us)
{
    int64_t elapsed_us = now_us - window_start_us;
    double  secs = (double)elapsed_us / 1e6;
    double  mbps = (double)s_window_bytes / 1e6 / secs;
    uint32_t drops = pk_iq_dropped_bytes_swap();
    s_iq_drop_total += drops;   /* accumulate into boot-lifetime counter */

    /* Stay quiet when there's no IQ to talk about. This happens whenever
     * the dongle is unplugged (sdr_task is parked in its NEW_DEV wait)
     * or has never been attached; printing "stream 0.00 MB/s" every
     * second in that state is just noise that drowns out the rest of
     * the system. The 1 Hz dashboard resumes the instant IQ flows. */
    if (s_window_bytes == 0 && drops == 0) {
        goto reset;
    }

    if (drops == 0) {
        ESP_LOGI(TAG,
                 "stream %.2f MB/s | msgs/s %lu (df11 %lu  df17 id %lu pos %lu "
                 "(decoded %lu) vel %lu  df20/21 %lu  other %lu) | aircraft %lu",
                 mbps,
                 (unsigned long)s_msgs_total,
                 (unsigned long)s_msgs_df11,
                 (unsigned long)s_msgs_df17_id,
                 (unsigned long)s_msgs_df17_pos,
                 (unsigned long)s_pos_decoded,
                 (unsigned long)s_msgs_df17_vel,
                 (unsigned long)s_msgs_df20_21,
                 (unsigned long)s_msgs_other,
                 (unsigned long)s_icao_unique);
    } else {
        ESP_LOGW(TAG,
                 "stream %.2f MB/s (DROPPED %lu B) | msgs/s %lu | aircraft %lu",
                 mbps, (unsigned long)drops,
                 (unsigned long)s_msgs_total,
                 (unsigned long)s_icao_unique);
    }

reset:;

    /* Flush 1-Hz window totals into boot-lifetime cumulative counters
     * before zeroing the window. s_icao_unique is already cumulative
     * (never reset) — exposed as-is by pk_dsp_get_stats(). */
    s_msgs_total_cum  += s_msgs_total;
    s_pos_decoded_cum += s_pos_decoded;

    s_window_bytes  = 0;
    s_msgs_total    = 0;
    s_msgs_df11     = 0;
    s_msgs_df17_id  = 0;
    s_msgs_df17_pos = 0;
    s_msgs_df17_vel = 0;
    s_msgs_df20_21  = 0;
    s_msgs_other    = 0;
    s_pos_decoded   = 0;
}

void dsp_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "dsp_task running (dump1090-derived edge decode)");

    cpr_init();
    mode_s_init(&s_decoder);
    /* The decoder's own crc check + our crcok filter in the callback
     * are both belt-and-braces. fix_errors and aggressive defeat the
     * point of "real aircraft only" reporting, so leave them off. */
    s_decoder.check_crc  = 1;
    s_decoder.fix_errors = 0;
    s_decoder.aggressive = 0;

    size_t   filled               = 0;
    int64_t  window_start_us      = esp_timer_get_time();
    int64_t  summary_last_emit_us = window_start_us;

    /* IQ stall watchdog: real dongles occasionally just stop pushing
     * samples after hours of uptime (rtl2832u quirk, USB stack
     * starvation, who knows). When we notice the stream has been
     * silent for IQ_STALL_LIMIT_MS, ask sdr_task to tear down its
     * librtlsdr session and re-open the dongle (no USB unplug needed).
     * Re-init happens in-place — IMU, PFD and BLE stay running. After
     * a successful re-init, sdr_task waits for IQ to start flowing
     * again before clearing its own attempt counter.
     *
     * IQ_REINIT_BACKOFF_MS prevents this watchdog from spamming
     * pk_sdr_request_reinit() faster than sdr_task can act on it —
     * the tear-down + re-open path itself takes ~1-2 s, during which
     * last_iq_us obviously won't advance. */
    #define IQ_STALL_LIMIT_MS    10000   /* 10 s with no IQ → reinit request */
    #define IQ_REINIT_BACKOFF_MS  3000   /* don't re-request within 3 s of last */
    int64_t  last_iq_us           = window_start_us;
    int64_t  last_reinit_req_us   = 0;
    bool     iq_ever_received     = false;

    while (1) {
        /* Greedy fill: pull whatever's currently in the ring buffer until
         * either our working buffer is full or 100 ms have passed. */
        while (filled < DSP_IQ_BUF_BYTES) {
            size_t got = 0;
            void *p = xRingbufferReceiveUpTo(
                g_iq_ringbuf, &got, pdMS_TO_TICKS(100),
                DSP_IQ_BUF_BYTES - filled);
            if (p == NULL) break;
            memcpy(s_iq_buf + filled, p, got);
            vRingbufferReturnItem(g_iq_ringbuf, p);
            filled += got;
            last_iq_us = esp_timer_get_time();
            iq_ever_received = true;
        }

        if (filled >= DSP_OVERLAP_BYTES + 2) {
            s_window_bytes += (filled - DSP_OVERLAP_BYTES);  /* fresh bytes */
            uint32_t mag_len = filled / 2;
            mode_s_compute_magnitude_vector(s_iq_buf, s_mag_buf, filled);
            mode_s_detect(&s_decoder, s_mag_buf, mag_len, on_mode_s_msg);

            /* Carry the trailing overlap forward so preambles straddling
             * the boundary aren't dropped. The decoder's internal ICAO
             * cache deduplicates any frame we end up re-detecting. */
            memmove(s_iq_buf, s_iq_buf + filled - DSP_OVERLAP_BYTES,
                    DSP_OVERLAP_BYTES);
            filled = DSP_OVERLAP_BYTES;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - window_start_us >= 1000000) {
            dashboard_emit_and_reset(now_us, window_start_us);
            window_start_us = now_us;
        }
        /* Summary every 30 s, not 5 s: with many aircraft tracked the
         * 30-odd ESP_LOGI lines block the UART (115200 baud) for
         * several hundred ms, stalling dsp_task long enough for the
         * IQ ringbuf to overflow ("DROPPED ~1.8 MB" warnings every 5 s
         * once aircraft count climbed past ~40). 30 s keeps the
         * blackout below 1% of dsp_task wall time. */
        if (now_us - summary_last_emit_us >= 30000000) {
            aircraft_summary_emit(now_us);
            summary_last_emit_us = now_us;
        }

        /* Stall watchdog. Only arms after we've actually received IQ
         * at least once — otherwise we'd request a re-init during normal
         * boot before the dongle has finished enumerating. */
        if (iq_ever_received) {
            int64_t stall_ms = (now_us - last_iq_us) / 1000;
            int64_t since_last_req_ms = (now_us - last_reinit_req_us) / 1000;
            if (stall_ms > IQ_STALL_LIMIT_MS &&
                since_last_req_ms > IQ_REINIT_BACKOFF_MS) {
                char reason[64];
                snprintf(reason, sizeof(reason),
                         "IQ stream stalled %lld ms (limit %d ms)",
                         (long long)stall_ms, IQ_STALL_LIMIT_MS);
                ESP_LOGW(TAG, "%s — requesting sdr_task re-init", reason);
                pk_sdr_request_reinit(reason);
                last_reinit_req_us = now_us;
                /* Don't clear last_iq_us: if the re-init succeeds, on_iq
                 * will refresh it; if it doesn't, we'll re-request after
                 * IQ_REINIT_BACKOFF_MS, and sdr_task's own attempt
                 * counter will escalate to esp_restart() after
                 * SDR_REINIT_MAX_ATTEMPTS failures. */
            }
        }
    }
}

/* --- Diagnostic snapshot getter -------------------------------------- *
 *
 * Returns boot-lifetime cumulative counters as a single flat struct.
 * Called from pfd_task (diag page) at display refresh cadence (~1 Hz).
 * 32-bit aligned reads are atomic on ESP32-P4 (RV32); no locking needed.
 */
void pk_dsp_get_stats(pk_dsp_stats_t *out)
{
    if (out == NULL) return;
    out->msgs_total   = s_msgs_total_cum;
    out->pos_decoded  = s_pos_decoded_cum;
    out->icao_unique  = s_icao_unique;
    out->iq_drop_total = s_iq_drop_total;
}
