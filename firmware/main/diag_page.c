/*
 * diag_page.c — "Diagnostics" screen renderer.
 *
 * Layout (320 × 240 landscape):
 *
 *   y =   0   ┌────────────────────────────────────────────────────┐
 *             │ DIAGNOSTICS             (cyan)                      │  header
 *   y =  24   ├────────────────────────────────────────────────────┤
 *             │ IMU  : BNO085 cal N/3  r±RRR p±PPP y±YYY           │
 *             │ SDR  : 2 MS/s  msgs N  drop N                       │
 *             │ GPS  : fix / sats N / lat,lon / alt N ft            │
 *             │ BARO : P hPa / alt ft / vs fpm / temp C             │
 *             │ BLE  : connected / advertising / idle               │
 *             │ BATT : N/A (no sense HW)                            │
 *             │ uSD  : --                                           │
 *             │ TIME : --                                           │
 *   y = 240   └────────────────────────────────────────────────────┘
 *
 * Each row is read fresh every frame via read-only getters.
 * Pure pixel pushing — no I/O, no blocking.
 */

#include "diag_page.h"

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_timer.h"

#include "display.h"
#include "text.h"
#include "pilot_kit.h"       /* PK_RTLSDR_SAMPLERATE_HZ */
#include "imu_task.h"        /* pk_imu_sample_get, pk_imu_sample_t */
#include "dsp_task.h"        /* pk_dsp_get_stats, pk_dsp_stats_t */
#include "gps.h"             /* pk_gps_get, pk_gps_state_t */
#include "baro.h"            /* pk_baro_get, pk_baro_state_t */
#include "config_qnh.h"      /* pk_qnh_get — baro 高度的 QNH 基准 */
#include "ble_gatt.h"        /* ble_gatt_is_connected, ble_gatt_is_advertising */
#include "ui_state.h"        /* pk_ui_diag_scroll_y */
#include "pk_clock.h"        /* pk_clock_is_synced / pk_clock_source */
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* Layout */
#define DIAG_LEFT_PAD        6
#define DIAG_HEADER_UI_Y     6
#define DIAG_BODY_Y         30
#define DIAG_LINE_H         26   /* 8 rows × 26px = 208px, fits within 240 */
#define DIAG_KEY_X          DIAG_LEFT_PAD
#define DIAG_VAL_X          58   /* value column starts here */

/* Palette — matches about_page.c for visual consistency */
#define COL_BG               pk_rgb565( 12,  12,  16)
#define COL_HEADER           pk_rgb565(180, 235, 255)
#define COL_KEY              pk_rgb565(180, 235, 255)
#define COL_ONLINE           pk_rgb565( 80, 220,  80)   /* green  — subsystem live */
#define COL_OFFLINE          pk_rgb565(140, 145, 155)   /* grey   — no data / offline */
#define COL_PLACEHOLDER      pk_rgb565( 70,  72,  80)   /* dark   — N/A placeholder */
#define COL_DIVIDER          pk_rgb565( 90, 100, 120)
#define COL_VAL              pk_rgb565(220, 225, 235)   /* bright — detail value text */
#define COL_ALERT            pk_rgb565(255,  90,  40)   /* red    — antenna OPEN/SHORT */

/* GPS stale threshold: 5 s in microseconds */
#define GPS_FRESH_US         5000000LL

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

/*
 * draw_diag_row — render one key-value status row.
 *
 *   fb    : framebuffer
 *   y     : top pixel of this row
 *   label : short ASCII label (e.g. "IMU", "GPS")
 *   value : value string (ASCII)
 *   color : colour for the value (COL_ONLINE / COL_OFFLINE / COL_PLACEHOLDER)
 */
static void draw_diag_row(uint16_t *fb, int y,
                           const char *label, const char *value,
                           uint16_t color)
{
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           DIAG_KEY_X, y, label, COL_KEY);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           DIAG_VAL_X, y, value, color);
}

/* draw_snr_bars — 按星座分两组画 SNR 竖条：GPS 组 + 北斗组，组下方标 G/B。
 *   柱高=C/N0，组内颜色按强弱(绿≥35 / 黄25-34 / 红<25 dB)。基线在 y_base。
 *   con[] 与 snr[] 平行：0=GPS 1=北斗 2=其它(不画)。 */
static void draw_snr_bars(uint16_t *fb, int x0, int y_base,
                          const uint8_t *snr, const uint8_t *con, int n)
{
    const int bar_w = 5, gap = 2, max_h = 30, snr_full = 50, grp_gap = 12;
    if (n <= 0) {
        pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                               x0, y_base - 9, "(no sats)", COL_OFFLINE);
        return;
    }
    /* 字母表与 pk_gnss_t 同序：GPS/北斗/GLONASS/Galileo/QZSS/其它。 */
    static const char letter[PK_GNSS_COUNT] = { 'G', 'B', 'R', 'E', 'Q', '?' };
    int x = x0;
    for (int grp = 0; grp < PK_GNSS_COUNT; ++grp) {   /* 动态分组：收到哪个星座画哪组 */
        int grp_x0 = x, cnt = 0;
        for (int i = 0; i < n; ++i) {
            if (con[i] != grp) continue;
            int s = snr[i];
            if (s > snr_full) s = snr_full;
            int h = s * max_h / snr_full;
            if (h < 1 && s > 0) h = 1;
            uint16_t col = (s >= 35) ? COL_ONLINE
                         : (s >= 25) ? pk_rgb565(255, 200, 60)
                         :             COL_ALERT;
            fill_rect(fb, x, y_base - h, x + bar_w, y_base, col);
            x += bar_w + gap;
            ++cnt;
            if (x + bar_w > PK_DISPLAY_W - 10) break;
        }
        if (cnt > 0) {                          /* 组标签居中画在组下方 */
            char t[2] = { letter[grp], 0 };
            int mid = grp_x0 + cnt * (bar_w + gap) / 2 - 3;
            pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                   mid, y_base + 2, t, COL_KEY);
            x += grp_gap;
        }
        if (x + bar_w > PK_DISPLAY_W - 10) break;
    }
}

/* fmt_clock — 系统墙钟(被 GPS/BLE 校准) → "HH:MM:SSZ HH:MM L (src)"。
 * 未校时显示 "--"。 */
static void fmt_clock(char *buf, size_t bufsz)
{
    if (!pk_clock_is_synced()) { snprintf(buf, bufsz, "--"); return; }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t utc = tv.tv_sec, loc = tv.tv_sec + 8 * 3600;   /* 本地 = UTC+8 */
    struct tm u, l;
    gmtime_r(&utc, &u);
    gmtime_r(&loc, &l);
    const char *src = pk_clock_source();
    const char *tag = !strcmp(src, "gps")        ? "GPS"
                    : !strcmp(src, "gps-coarse") ? "GPS~"
                    : !strcmp(src, "ble-write")  ? "BLE"
                    : !strcmp(src, "ios-cts")    ? "iOS" : src;
    snprintf(buf, bufsz, "%02d:%02d:%02dZ  %02d:%02d L (%s)",
             u.tm_hour, u.tm_min, u.tm_sec, l.tm_hour, l.tm_min, tag);
}

/* --------------------------------------------------------------------------
 * Main render entry point
 * -------------------------------------------------------------------------- */

void pk_diag_page_render(uint16_t *fb)
{
    /* Clear background — may be coming from any other view. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* Scrollable body — UP/DOWN pans it; header is re-drawn on top at the
     * end so scrolled rows slide under it (mirrors about_page). */
    int y = DIAG_BODY_Y - pk_ui_diag_scroll_y();
    char buf[64];

    /* ------------------------------------------------------------------
     * IMU — BNO085
     * Online: sample.valid == true
     * Shows: cal N/3 and r/p/y angles
     * ------------------------------------------------------------------ */
    {
        pk_imu_sample_t s;
        bool ok = pk_imu_sample_get(&s);
        if (ok && s.valid) {
            uint8_t acc = s.accuracy;   /* 同一快照,免去额外 mutex */
            /* Format: "cal 2/3 r+12 p-3 y270" — compact, fits 320px */
            snprintf(buf, sizeof(buf), "cal %u/3 r%+.0f p%+.0f y%.0f",
                     acc,
                     (double)s.roll_deg,
                     (double)s.pitch_deg,
                     (double)s.yaw_deg);
            draw_diag_row(fb, y, "IMU", buf, COL_ONLINE);
        } else {
            draw_diag_row(fb, y, "IMU", "BNO085 offline", COL_OFFLINE);
        }
    }
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * SDR — RTL-SDR (always present, always "online" once streaming)
     * Shows: sample rate + cumulative msg/drop counters
     * ------------------------------------------------------------------ */
    {
        pk_dsp_stats_t d;
        pk_dsp_get_stats(&d);
        /* SDR 常驻在线;暂无 streaming-active getter,msgs=0 时也显示在线(dongle 未插无法区分)。 */
        /* e.g. "2MS/s msgs 1234 drop 0" */
        snprintf(buf, sizeof(buf), "%luMS/s msgs %lu drop %lu",
                 (unsigned long)(PK_RTLSDR_SAMPLERATE_HZ / 1000000UL),
                 (unsigned long)d.msgs_total,
                 (unsigned long)d.iq_drop_total);
        draw_diag_row(fb, y, "SDR", buf, COL_ONLINE);
    }
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * GPS — 排查多行：状态/HDOP、可见星+SNR+天线、位置，外加 SNR 柱状图
     * ------------------------------------------------------------------ */
    {
        pk_gps_state_t g = {0};
        pk_gps_get(&g);
        int64_t now = esp_timer_get_time();
        bool fresh = g.have_fix && (now - g.updated_us) < GPS_FRESH_US;

        /* 行1：fix 状态 + HDOP */
        if (fresh) snprintf(buf, sizeof(buf), "fix  sats %d     HDOP %.1f",
                            g.sats, (double)g.hdop);
        else       snprintf(buf, sizeof(buf), "no fix         HDOP %.1f",
                            (double)g.hdop);
        draw_diag_row(fb, y, "GPS", buf, fresh ? COL_ONLINE : COL_OFFLINE);
        y += DIAG_LINE_H;

        /* 行2：可见星(分 GPS/北斗) + 最强 SNR + 天线自检（天线单独着色） */
        snprintf(buf, sizeof(buf), "GPS %d  BD %d  max %d",
                 g.sats_in_view_gps, g.sats_in_view_bds, g.snr_max);
        draw_diag_row(fb, y, "", buf, COL_VAL);
        const char *ant = (g.ant_status == PK_GPS_ANT_OK)    ? "ANT OK"
                        : (g.ant_status == PK_GPS_ANT_OPEN)  ? "ANT OPEN"
                        : (g.ant_status == PK_GPS_ANT_SHORT) ? "ANT SHORT" : "ANT ?";
        uint16_t ant_col = (g.ant_status == PK_GPS_ANT_OK)      ? COL_ONLINE
                         : (g.ant_status == PK_GPS_ANT_UNKNOWN) ? COL_OFFLINE
                         :                                        COL_ALERT;
        pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H, 206, y, ant, ant_col);
        y += DIAG_LINE_H;

        /* 行3：位置（定位后才有意义） */
        if (fresh) snprintf(buf, sizeof(buf), "%+.5f,%+.5f  %dft",
                            g.lat, g.lon, g.have_altitude ? g.altitude_ft : 0);
        else       snprintf(buf, sizeof(buf), "--");
        draw_diag_row(fb, y, "", buf, fresh ? COL_VAL : COL_OFFLINE);
        y += DIAG_LINE_H;

        /* 行4：SNR 柱状图，按星座分两组(G/B)，柱下标注 */
        pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                               DIAG_KEY_X, y, "SNR", COL_KEY);
        draw_snr_bars(fb, DIAG_VAL_X, y + 34, g.snr, g.snr_con, g.snr_count);
        y += 52;
    }

    /* ------------------------------------------------------------------
     * BARO — BMP388
     * Online: valid == true
     * Shows: P hPa / alt ft / vs fpm / temp C
     * ------------------------------------------------------------------ */
    {
        pk_baro_state_t b;
        pk_baro_get(&b);
        if (b.valid) {
            /* Pa → hPa = Pa / 100 */
            snprintf(buf, sizeof(buf), "%.1fhPa %dft %dfpm %.1fC",
                     (double)b.pressure_pa / 100.0,
                     b.alt_ft,
                     b.vs_fpm,
                     (double)b.temp_c);
            draw_diag_row(fb, y, "BARO", buf, COL_ONLINE);
        } else {
            draw_diag_row(fb, y, "BARO", "--", COL_OFFLINE);
        }
    }
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * QNH — baro 高度的海平面参考压(settings 可调,默认标准大气 1013.25)。
     * 紧贴 BARO 下方亮出来:上面那个 alt ft 就是按这个基准算的。
     * ------------------------------------------------------------------ */
    {
        snprintf(buf, sizeof(buf), "%.2f hPa", (double)pk_qnh_get());
        draw_diag_row(fb, y, "QNH", buf, COL_VAL);
    }
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * BLE
     * No hard "online" concept — shows connection state
     * ------------------------------------------------------------------ */
    {
        /* 一次性快照,避免两次调用之间状态变化导致 TOCTOU 判断矛盾 */
        bool ble_conn = ble_gatt_is_connected();
        bool ble_adv  = ble_gatt_is_advertising();
        const char *ble_val;
        uint16_t    ble_col;
        if (ble_conn) {
            ble_val = "connected";
            ble_col = COL_ONLINE;
        } else if (ble_adv) {
            ble_val = "advertising";
            ble_col = COL_OFFLINE;
        } else {
            ble_val = "idle";
            ble_col = COL_OFFLINE;
        }
        draw_diag_row(fb, y, "BLE", ble_val, ble_col);
    }
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * BATT — no sense hardware, placeholder
     * ------------------------------------------------------------------ */
    draw_diag_row(fb, y, "BATT", "N/A (no sense HW)", COL_PLACEHOLDER);
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * uSD — placeholder
     * ------------------------------------------------------------------ */
    draw_diag_row(fb, y, "uSD", "--", COL_PLACEHOLDER);
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * TIME — 系统墙钟(被 GPS/BLE 校准)：UTC(Zulu) + 本地(UTC+8) + 来源
     * ------------------------------------------------------------------ */
    {
        char tbuf[48];
        fmt_clock(tbuf, sizeof(tbuf));
        draw_diag_row(fb, y, "TIME", tbuf,
                      pk_clock_is_synced() ? COL_ONLINE : COL_PLACEHOLDER);
    }
    y += DIAG_LINE_H;

    /* Header overlay — fixed, drawn last so the scrolled body slides under it. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, 22, COL_BG);
    pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                    DIAG_LEFT_PAD, DIAG_HEADER_UI_Y, "DIAGNOSTICS", COL_HEADER);
    fill_rect(fb, 0, 22, PK_DISPLAY_W, 24, COL_DIVIDER);
}
