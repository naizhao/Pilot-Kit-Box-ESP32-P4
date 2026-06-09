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
#include "ble_gatt.h"        /* ble_gatt_is_connected, ble_gatt_is_advertising */

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

/* --------------------------------------------------------------------------
 * Main render entry point
 * -------------------------------------------------------------------------- */

void pk_diag_page_render(uint16_t *fb)
{
    /* Clear background — may be coming from any other view. */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* Header title */
    pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                    DIAG_LEFT_PAD, DIAG_HEADER_UI_Y,
                    "DIAGNOSTICS", COL_HEADER);

    /* Divider line under header */
    fill_rect(fb, 0, 22, PK_DISPLAY_W, 24, COL_DIVIDER);

    int y = DIAG_BODY_Y;
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
     * GPS
     * Online: have_fix AND updated_us within GPS_FRESH_US of now
     * Shows: "fix sats N lat,lon alt N ft" or "no fix"
     * ------------------------------------------------------------------ */
    {
        pk_gps_state_t g;
        pk_gps_get(&g);
        int64_t now = esp_timer_get_time();
        bool gps_fresh = g.have_fix && (now - g.updated_us) < GPS_FRESH_US;
        if (gps_fresh) {
            /* Compact: "fix s4 +37.6,-122.4 alt 52ft" */
            snprintf(buf, sizeof(buf), "fix s%d %+.1f,%+.1f %dft",
                     g.sats,
                     g.lat,
                     g.lon,
                     g.have_altitude ? g.altitude_ft : 0);
            draw_diag_row(fb, y, "GPS", buf, COL_ONLINE);
        } else if (g.have_fix) {
            /* 曾有 fix 但 >5s 未更新 —— 与"从未定位"区分 */
            snprintf(buf, sizeof(buf), "stale  sats %d", g.sats);
            draw_diag_row(fb, y, "GPS", buf, COL_OFFLINE);
        } else if (g.updated_us != 0) {
            /* 收到过 NMEA 但无 fix */
            snprintf(buf, sizeof(buf), "no fix  sats %d", g.sats);
            draw_diag_row(fb, y, "GPS", buf, COL_OFFLINE);
        } else {
            /* 从未收到数据 */
            draw_diag_row(fb, y, "GPS", "no fix", COL_OFFLINE);
        }
    }
    y += DIAG_LINE_H;

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
     * BLE
     * No hard "online" concept — shows connection state
     * ------------------------------------------------------------------ */
    {
        const char *ble_val;
        uint16_t    ble_col;
        if (ble_gatt_is_connected()) {
            ble_val = "connected";
            ble_col = COL_ONLINE;
        } else if (ble_gatt_is_advertising()) {
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
     * TIME — placeholder
     * ------------------------------------------------------------------ */
    draw_diag_row(fb, y, "TIME", "--", COL_PLACEHOLDER);
}
