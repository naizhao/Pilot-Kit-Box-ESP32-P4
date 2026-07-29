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
 *             │ microSD : mounted X.X/XX.X GB used / no card        │
 *             │ LOG  : flash|microSD  w N  drop N                   │
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
#include "esp_system.h"
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
#include "pk_sdcard.h"       /* pk_sdcard_state / pk_sdcard_info */
#include "record_sink.h"     /* record_sink_file_stats / _uses_sd */
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
/* 琥珀 —— 「连着但不正常」，介于绿与红之间。SDR 已枚举却不出数就属于这一档：
 * 说它离线是错的（设备在），说它在线也是错的（没数据）。 */
#define COL_WARN             pk_rgb565(255, 176,   0)

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

/* draw_snr_row — 画单个星座(want)的 SNR 竖条一行：柱高=C/N0，颜色按强弱
 *   (绿≥35 / 黄25-34 / 红<25 dB)。基线在 y_base，向上生长。
 *   每星座独占一行，避免卫星多时一行挤不下;调用方在行首标完整星座名。 */
static void draw_snr_row(uint16_t *fb, int x0, int y_base,
                         const uint8_t *snr, const uint8_t *con, int n, int want)
{
    const int bar_w = 5, gap = 2, max_h = 22, snr_full = 50;
    int x = x0;
    for (int i = 0; i < n; ++i) {
        if (con[i] != want) continue;
        int s = snr[i];
        if (s > snr_full) s = snr_full;
        int h = s * max_h / snr_full;
        if (h < 1 && s > 0) h = 1;
        uint16_t col = (s >= 35) ? COL_ONLINE
                     : (s >= 25) ? pk_rgb565(255, 200, 60)
                     :             COL_ALERT;
        fill_rect(fb, x, y_base - h, x + bar_w, y_base, col);
        x += bar_w + gap;
        if (x + bar_w > PK_DISPLAY_W - 4) break;   /* 满屏宽截断 */
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

/*
 * 旧的逐行详情视图。
 *
 * spec §5.5 把诊断改成两层：**总览 2×4 卡片**（每格只给标题 + 一行核心值），
 * 点卡片才进子系统详情页；而详情页"必须保留 diag_page.c 现有全部深度"——
 * 尤其 GPS 那段每星座独立的 SNR 柱状图，那是排查 no-fix 的命门。
 *
 * 所以这段一行都不删，改名留着，等详情那一层接上来直接复用。当前未被调用。
 */
__attribute__((unused))
static void diag_render_legacy(uint16_t *fb)
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
     * SDR — RTL-SDR dongle。
     *
     * 这一行以前恒显示"在线"（旧注释原话："dongle 未插无法区分"）。2026-07-29
     * 旧板曾有另一套 USB 接口说明；Rev1.2 上 RTL-SDR 应接 H2（丝印 USB）
     * 原生 USB 2.0 HS Type-C。H1 是 CH343P 调试串口，P1 是 C6 下载排针。
     *
     * 现在四态分开显示，并且**没枚举时直接把该插哪儿写在屏上**：这是接线
     * 问题，写"OFFLINE"帮不上忙，直接写 H2 才能解决问题。
     * ------------------------------------------------------------------ */
    {
        pk_dsp_stats_t d;
        pk_dsp_get_stats(&d);

        uint32_t drop_kb = 0;
        const pk_sdr_state_t st = pk_sdr_state_get(&drop_kb);
        uint16_t col = COL_ONLINE;

        switch (st) {
        case PK_SDR_NO_DEVICE:
            /* 把排查方向直接写出来——这一行的读者正拿着 dongle 在找哪个口。 */
            snprintf(buf, sizeof(buf), "NO DONGLE - use H2 USB-C");
            col = COL_ALERT;
            break;
        case PK_SDR_ATTACHED:
            snprintf(buf, sizeof(buf), "attached, opening...");
            col = COL_WARN;
            break;
        case PK_SDR_STALLED:
            /* 已经打开却不出数：供电不足 / 过热 / USB 掉链，都不是"离线"。 */
            snprintf(buf, sizeof(buf), "STALLED - no IQ for >1s");
            col = COL_WARN;
            break;
        case PK_SDR_STREAMING:
        default:
            snprintf(buf, sizeof(buf), "%luMS/s msgs %lu drop %lu",
                     (unsigned long)(PK_RTLSDR_SAMPLERATE_HZ / 1000000UL),
                     (unsigned long)d.msgs_total,
                     (unsigned long)d.iq_drop_total);
            break;
        }
        draw_diag_row(fb, y, "SDR", buf, col);
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

        /* SNR 区：每个出现的星座单独一行 —— 行首完整星座名 + 该星座 SNR 柱状图。
         * 卫星多时一行放不下,故分行;诊断页可滚动,行数不限。 */
        if (g.snr_count <= 0) {
            draw_diag_row(fb, y, "SNR", "(no sats)", COL_OFFLINE);
            y += DIAG_LINE_H;
        } else {
            /* 名表与 pk_gnss_t 同序：GPS/北斗/GLONASS/Galileo/QZSS/其它。 */
            static const char *const con_name[PK_GNSS_COUNT] =
                { "GPS", "BDS", "GLO", "GAL", "QZS", "?" };
            for (int gi = 0; gi < PK_GNSS_COUNT; ++gi) {
                int cnt = 0;
                for (int i = 0; i < g.snr_count; ++i)
                    if (g.snr_con[i] == gi) cnt++;
                if (cnt == 0) continue;
                /* 标签垂直居中于本行(行高 28、字高 7 → 顶部偏移约 10)，
                 * 与右侧柱状图视觉对齐。 */
                pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                       DIAG_KEY_X, y + 10, con_name[gi], COL_KEY);
                draw_snr_row(fb, DIAG_VAL_X, y + 24, g.snr, g.snr_con,
                             g.snr_count, gi);
                y += 28;
            }
            y += 8;   /* SNR 区(柱状图基线在 +24)与下一行 BARO 之间留间隙 */
        }
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
     * microSD — 挂载状态 + 已用/总容量（pk_sdcard 探测任务缓存，零 I/O）
     * ------------------------------------------------------------------ */
    {
        char        sd_buf[40];
        const char *sd_val;
        uint16_t    sd_col;
        uint64_t    total = 0, free_b = 0;

        switch (pk_sdcard_state()) {
        case PK_SD_MOUNTED:
            if (pk_sdcard_info(&total, &free_b)) {
                snprintf(sd_buf, sizeof(sd_buf), "mounted %.1f/%.1f GB used",
                         (double)(total - free_b) / (1024.0 * 1024.0 * 1024.0),
                         (double)total / (1024.0 * 1024.0 * 1024.0));
                sd_val = sd_buf;
            } else {
                sd_val = "mounted";
            }
            sd_col = COL_ONLINE;
            break;
        case PK_SD_FORMATTING:
            sd_val = "formatting...";
            sd_col = COL_PLACEHOLDER;
            break;
        default:
            sd_val = "no card";
            sd_col = COL_OFFLINE;
            break;
        }
        draw_diag_row(fb, y, "microSD", sd_val, sd_col);
    }
    y += DIAG_LINE_H;

    /* ------------------------------------------------------------------
     * LOG — file sink 后端 + 本次开机已写/丢弃条数（计数器只读，零 I/O）
     * ------------------------------------------------------------------ */
    {
        uint32_t written = 0, dropped = 0;
        if (record_sink_file_stats(&written, &dropped)) {
            snprintf(buf, sizeof(buf), "%s  w %lu  drop %lu",
                     record_sink_file_uses_sd() ? "microSD" : "flash",
                     (unsigned long)written, (unsigned long)dropped);
            draw_diag_row(fb, y, "LOG", buf,
                          written > 0 ? COL_ONLINE : COL_OFFLINE);
        } else {
            draw_diag_row(fb, y, "LOG", "sink down", COL_OFFLINE);
        }
    }
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

/* ═══════════════════════════════════════════════════════════════════════
 * 总览：2 × 4 卡片（spec §5.5）
 *
 * 每格只给「标题 + 一行核心值」，深度留给详情页。这个取舍是 spec 定的，
 * 理由也站得住：诊断页最常见的用法是**扫一眼看哪个子系统不对**，而不是读
 * 具体数值——旧的逐行版把十几行数据平铺出来，反而要逐行找哪一行是红的。
 *
 * 八格固定，不随状态增减：格子位置固定，肌肉记忆才建立得起来；某个子系统
 * 没数据就在它自己那格里说，而不是从版面上消失。
 * ═════════════════════════════════════════════════════════════════════ */

#include "pfd_aa_text.h"
#include "pfd_layout.h"
#include "pfd_draw.h"
#include "battery.h"
#include "soc_temp.h"

#define CARD_COLS   2
#define CARD_PAD    16
#define CARD_GAP    12
#define CARD_W      ((PK_DISPLAY_W - CARD_PAD * 2 - CARD_GAP) / CARD_COLS)
#define CARD_TOP    (PFD_BAR_BOT + 8)
#define CARD_H      ((PK_DISPLAY_H - CARD_TOP - CARD_PAD + CARD_GAP) \
                     / 4 - CARD_GAP)

/* 状态灯：一格一个圆点，颜色即结论。绿=正常、琥珀=连着但不正常、
 * 红=不可用、灰=不适用/未启用。四种颜色与 PFD、交通页、列表页一致。 */
/*
 * 三态，没有"灰色"。
 *
 * 原先有个 ST_BAD 用灰色表示"不适用/未装"，于是 GPS 没插模块显示灰色
 * "no module"，而别的模块离线是红色——同样是"这个功能现在用不了"，却给了
 * 两种视觉权重，扫一眼分不清哪些是真该管的。罩哥要求统一：
 *
 *   「统一文字颜色为红色，no module 就 no module，no data 那就 no data」
 *
 * 即**颜色只表达严重度，文字负责说清是哪种缺失**。用不了就是红的，至于是
 * 没插、没数据还是没硬件，看那一行字。 */
typedef enum { ST_OK = 0, ST_WARN, ST_BAD } card_state_t;

static uint16_t state_color(card_state_t st)
{
    switch (st) {
    case ST_OK:   return COL_ONLINE;
    case ST_WARN: return COL_WARN;
    case ST_BAD:
    default:      return COL_ALERT;
    }
}

/* 卡片总数与滚动。
 *
 * spec §5.5 写的是「2 × 4」八格，但那是版面示意不是容量上限——诊断数据本来
 * 就比八条多（SD 卡、QNH、运行时长…），塞不下的不该被删掉，该能滚。所以
 * 行数由卡片数推出来，屏幕放不下就滚动。 */
#define CARD_ROWS_VIS  4                       /* 一屏能完整看到的行数 */
static int s_card_rows;                        /* 本帧实际画了几行 */
static int s_scroll_y;                         /* 滚动偏移(px)，0 = 顶 */

/*
 * 当前展开的子系统详情，-1 = 总览（spec §5.5 的两层结构）。
 *
 * 记的是**卡片序号**而不是指针：卡片是每帧重画的，序号是稳定的。
 */
static int s_detail = -1;

/* 卡片序号 → 标题。顺序必须与 render 里的 draw_card 调用一致——两处各写
 * 一份序号迟早会错位，所以这张表是唯一的真值来源，render 也从它取标题。 */
static const char *const CARD_TITLE[] = {
    "IMU", "BARO", "GPS", "SDR", "BLE", "LOG", "CLK", "SYS",
    "microSD", "QNH", "BATT", "UPTIME",
};
#define CARD_N  ((int)(sizeof(CARD_TITLE) / sizeof(CARD_TITLE[0])))

static void draw_card(uint16_t *fb, int col, int row, const char *title,
                      const char *value, card_state_t st)
{
    const int x = CARD_PAD + col * (CARD_W + CARD_GAP);
    const int y = CARD_TOP + row * (CARD_H + CARD_GAP) - s_scroll_y;

    /* 滚出屏幕的卡整张跳过：省掉绘制，也避免半张卡压到顶栏上。 */
    if (y + CARD_H <= PFD_BAR_BOT || y >= PK_DISPLAY_H) return;

    pk_pfd_fill_rect(fb, x, y, x + CARD_W, y + CARD_H, pk_rgb565(16, 22, 32));
    /* 左侧一条状态色带，比只在角上点一个圆点更容易在余光里扫到——诊断页
     * 的第一诉求就是「哪一格不对」。 */
    pk_pfd_fill_rect(fb, x, y, x + 5, y + CARD_H, state_color(st));

    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x + 18, y + 10,
               title, COL_KEY, PK_AA_XS);

    /* 值太长就降到 S 档，避免把接口提示截掉。 */
    const int avail = CARD_W - 26;
    const int need  = (int)strlen(value) * pk_aa_cell_w(PK_AA_M);
    const pk_aa_size_t sz = (need <= avail) ? PK_AA_M : PK_AA_S;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x + 18,
               y + CARD_H - 12 - pk_aa_cell_h(sz), value, state_color(st), sz);
}

/* 详情页返回区宽度。放在这里而不是详情那段里：触摸判定用得比绘制更早。 */
#define DET_BACK_W  96

/* 前向声明：详情那层定义在文件末尾（它复用 draw_snr_row 等 legacy 工具）。 */
static void draw_detail(uint16_t *fb, int which);
static void draw_detail_header(uint16_t *fb, int which);

void pk_diag_page_render(uint16_t *fb)
{
    char buf[64];

    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* 详情层：spec §5.5 的第二层。总览负责"哪个子系统不对"，详情负责
     * "到底怎么不对"，两层的信息密度差着一个量级，不该挤在一起。 */
    if (s_detail >= 0) {
        draw_detail(fb, s_detail);
        draw_detail_header(fb, s_detail);
        return;
    }

    /* ── IMU ── */
    {
        pk_imu_sample_t s;
        if (pk_imu_sample_get(&s) && s.valid) {
            snprintf(buf, sizeof(buf), "cal %u/3   yaw %.0f",
                     s.accuracy, (double)s.yaw_deg);
            /* 校准等级本身就是可信度：cal 0 时航向可以差几十度，数据却照样
             * "有效"——这正是最该在总览里就看见的。 */
            draw_card(fb, 0, 0, "IMU", buf,
                      s.accuracy >= 2 ? ST_OK : ST_WARN);
        } else {
            draw_card(fb, 0, 0, "IMU", "BNO085 offline", ST_BAD);
        }
    }

    /* ── BARO ── */
    {
        pk_baro_state_t b;
        pk_baro_get(&b);
        if (b.valid) {
            snprintf(buf, sizeof(buf), "%d ft   %.1f hPa",
                     b.alt_ft, (double)b.pressure_pa / 100.0);
            draw_card(fb, 1, 0, "BARO", buf, ST_OK);
        } else {
            draw_card(fb, 1, 0, "BARO", "BMP388 offline", ST_BAD);
        }
    }

    /* ── GPS ──
     * 有没有 fix 与看得见几颗星是两件事：sats 有数而 fix=0 说明信号弱/遮挡，
     * sats=0 才是天线或接线问题。总览里两个数都给，省得点进详情才发现方向
     * 找错了（这正是 no-fix 那次排查的教训）。 */
    {
        pk_gps_state_t g = {0};
        pk_gps_get(&g);
        const int64_t now = esp_timer_get_time();
        const bool fresh = g.have_fix && (now - g.updated_us) < GPS_FRESH_US;

        /*
         * 五种情况必须分开说，它们指向完全不同的处理动作。
         *
         * 上一版只分了「有 fix / 看得见星 / 看不见星」，于是**根本没插 GPS
         * 板卡**也报成 "no sats - check antenna"——罩哥在真机上一眼看出与
         * 实际不符。模块在不在，看的是有没有收到 NMEA，跟有没有星无关。
         */
        if (g.last_nmea_us == 0) {
            /* 一行 NMEA 都没收到 = 模块没插 / UART 没通。不是故障，是没装。 */
            draw_card(fb, 0, 1, "GPS", "no module", ST_BAD);
        } else if (now - g.last_nmea_us > 5000000LL) {
            /* 曾经在讲话，现在哑了 = 掉线/供电/接触不良，跟没装是两回事。 */
            draw_card(fb, 0, 1, "GPS", "module silent >5s", ST_BAD);
        } else if (g.ant_status == PK_GPS_ANT_OPEN) {
            /* 模块自检报的天线开路，比"没星"精确得多——直接说结论。 */
            draw_card(fb, 0, 1, "GPS", "antenna OPEN", ST_BAD);
        } else if (g.ant_status == PK_GPS_ANT_SHORT) {
            draw_card(fb, 0, 1, "GPS", "antenna SHORT", ST_BAD);
        } else if (fresh) {
            snprintf(buf, sizeof(buf), "fix   %d sats   HDOP %.1f",
                     g.sats, (double)g.hdop);
            draw_card(fb, 0, 1, "GPS", buf, ST_OK);
        } else if (g.snr_count > 0) {
            /* 看得见星却定不了位 = 信号弱/遮挡，动作是"再等等或换个位置"。 */
            snprintf(buf, sizeof(buf), "no fix   %d visible", g.snr_count);
            draw_card(fb, 0, 1, "GPS", buf, ST_WARN);
        } else {
            /* 模块在讲话、天线自检没报错、但一颗星都没看见：冷启动搜星中，
             * 或者被完全遮挡。这才是"再等等"，不该报成天线故障。 */
            draw_card(fb, 0, 1, "GPS", "searching...", ST_WARN);
        }
    }

    /* ── SDR ── */
    {
        uint32_t drop_kb = 0;
        const pk_sdr_state_t st = pk_sdr_state_get(&drop_kb);
        pk_dsp_stats_t d;
        pk_dsp_get_stats(&d);
        switch (st) {
        case PK_SDR_NO_DEVICE:
            draw_card(fb, 1, 1, "SDR", "NO DONGLE - use H2 USB-C", ST_BAD);
            break;
        case PK_SDR_ATTACHED:
            draw_card(fb, 1, 1, "SDR", "attached, opening...", ST_WARN);
            break;
        case PK_SDR_STALLED:
            draw_card(fb, 1, 1, "SDR", "STALLED - no IQ >1s", ST_WARN);
            break;
        default:
            snprintf(buf, sizeof(buf), "%luMS/s  msgs %lu",
                     (unsigned long)(PK_RTLSDR_SAMPLERATE_HZ / 1000000UL),
                     (unsigned long)d.msgs_total);
            draw_card(fb, 1, 1, "SDR", buf, ST_OK);
            break;
        }
    }

    /* ── BLE ── */
    {
        const bool conn = ble_gatt_is_connected();
        const bool adv  = ble_gatt_is_advertising();
        draw_card(fb, 0, 2, "BLE",
                  conn ? "connected" : adv ? "advertising" : "idle",
                  conn ? ST_OK : adv ? ST_WARN : ST_BAD);
    }

    /* ── LOG ── */
    {
        uint32_t written = 0, dropped = 0;
        if (record_sink_file_stats(&written, &dropped)) {
            snprintf(buf, sizeof(buf), "%s  w %lu",
                     record_sink_file_uses_sd() ? "microSD" : "flash",
                     (unsigned long)written);
            /* 丢过条目就是琥珀：还在写，但已经不完整了，跟"没在写"是两回事。 */
            draw_card(fb, 1, 2, "LOG", buf,
                      dropped > 0 ? ST_WARN : written > 0 ? ST_OK : ST_BAD);
        } else {
            draw_card(fb, 1, 2, "LOG", "sink down", ST_BAD);
        }
    }

    /* ── CLK ── */
    {
        char tbuf[48];
        fmt_clock(tbuf, sizeof(tbuf));
        draw_card(fb, 0, 3, "CLK", tbuf,
                  pk_clock_is_synced() ? ST_OK : ST_WARN);
    }

    /* ── SYS ──
     * spec §5.5 点名新增：产品定位是「Garmin 高温死机时的备份」，那么自身
     * 温度必须可见——一个会因为过热而死机的备份等于没有备份。 */
    {
        int temp_c = 0;
        const bool over = pk_soc_temp_get(&temp_c);

        /*
         * 温度 + **上次复位原因**。
         *
         * 复位原因放在这里而不是单开一格：它和"设备自身健康"是同一件事，
         * 而且平时是 POR（上电），只有出过问题才值得看一眼。
         *
         * 区分能力是它的价值所在——BROWNOUT（欠压）说明供电撑不住瞬时负载，
         * POR 说明真的断过电，PANIC/WDT 说明是固件的锅。三者的排查方向完全
         * 不同，靠"它重启了"这一句话分不出来。
         */
        static const char *const kRst[] = {
            "unknown", "POR", "ext", "SW", "panic", "int-WDT", "task-WDT",
            "WDT", "deepsleep", "brownout", "SDIO", "USB", "JTAG",
        };
        const esp_reset_reason_t rr = esp_reset_reason();
        const char *rs = (rr < (int)(sizeof(kRst) / sizeof(kRst[0])))
                       ? kRst[rr] : "?";
        snprintf(buf, sizeof(buf), "SoC %d C   rst %s", temp_c, rs);
        draw_card(fb, 1, 3, "SYS", buf,
                  over ? ST_BAD
                  : (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC ||
                     rr == ESP_RST_INT_WDT  || rr == ESP_RST_TASK_WDT) ? ST_WARN
                  : temp_c >= 75 ? ST_WARN : ST_OK);
    }

    /* ── microSD ──
     * 与 LOG 分开：LOG 说的是「有没有在写」，SD 说的是「卡在不在、还剩多少」。
     * 卡满了 LOG 仍会显示在写（写进 flash），只看 LOG 会漏掉换卡这件事。 */
    {
        uint64_t total = 0, free_b = 0;
        switch (pk_sdcard_state()) {
        case PK_SD_MOUNTED:
            if (pk_sdcard_info(&total, &free_b)) {
                snprintf(buf, sizeof(buf), "%.1f/%.1f GB used",
                         (double)(total - free_b) / (1024.0 * 1024.0 * 1024.0),
                         (double)total / (1024.0 * 1024.0 * 1024.0));
                /* 剩余不足 10% 转琥珀：卡还在、还能写，但快写不下了。 */
                draw_card(fb, 0, 4, "microSD", buf,
                          (total && free_b * 10 < total) ? ST_WARN : ST_OK);
            } else {
                draw_card(fb, 0, 4, "microSD", "mounted", ST_OK);
            }
            break;
        case PK_SD_FORMATTING:
            draw_card(fb, 0, 4, "microSD", "formatting...", ST_WARN);
            break;
        default:
            /* 只说"没插卡"。
             *
             * retry 计数是排查 slot 没注销那个 bug 时加的临时诊断，问题定位
             * 之后它就成了噪音——不插卡使用是完全正常的用法，屏上却挂着一个
             * 一直在涨的数字，看起来像有什么东西反复失败。
             *
             * 重试逻辑本身保留（热插拔靠它），只是不再摆到台面上。真要再排查
             * 同类问题，看串口日志里的 "mount attempt #N failed" 就够了。 */
            draw_card(fb, 0, 4, "microSD", "no card", ST_BAD);
            break;
        }
    }

    /* ── QNH ──
     * baro 高度的基准。它不是"状态"而是"设定值"，但设错了高度就整体偏——
     * 标准 1013.25 与当地实际能差几百英尺，值得占一格。 */
    {
        const float q = pk_qnh_get();
        snprintf(buf, sizeof(buf), "%.2f hPa", (double)q);
        draw_card(fb, 1, 4, "QNH", buf, ST_OK);
    }

    /* ── BATT ──
     * 板上没有电量检测通路：右排针只有 VSYS（Battery / external 5 V input），
     * 没有分压到 ADC、没有充电 IC 状态脚（docs/hardware/board_pinout.md:93）。
     * 所以接上电池也读不到电量或充电状态。
     *
     * 如实写"无检测硬件"而不是显示 0% 或藏起来：藏起来会让人以为固件漏了，
     * 显示 0% 则是编造数据——而这一格的读者正想知道还能飞多久。 */
    {
        pk_batt_t b;
        pk_batt_get(&b);
        if (b.valid) {
            /* 同时给百分比、电压和 raw：raw 是标定分压比的唯一依据，
             * 拿万用表量到的电池电压除以它就是比值（见 CONFIG_PK_BATT_
             * DIVIDER_X100）。标定完这一项就没用了，但留着不碍事，
             * 换板子时还得再标一次。 */
            snprintf(buf, sizeof(buf), "%d%% %.2fV%s raw %dmV",
                     b.pct, b.batt_mv / 1000.0, b.charging ? " CHG" : "",
                     b.raw_mv);
            draw_card(fb, 0, 5, "BATT", buf,
                      b.charging ? ST_OK : b.pct >= 20 ? ST_OK : ST_WARN);
        } else {
            /* 没接电池时引脚浮空，读数乱跳——不显示百分比，只说没接。 */
            snprintf(buf, sizeof(buf), "no battery (raw %dmV)", b.raw_mv);
            draw_card(fb, 0, 5, "BATT", buf, ST_BAD);
        }
    }

    /* ── UPTIME ──
     * 排查偶发重启的第一手证据：屏上这个数突然归零，说明刚重启过，而不是
     * "某个子系统自己恢复了"。26 秒重启循环那次就是靠它才看出问题的性质。 */
    {
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        if (sec < 3600) snprintf(buf, sizeof(buf), "%lum %lus",
                                 (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        else            snprintf(buf, sizeof(buf), "%luh %lum",
                                 (unsigned long)(sec / 3600),
                                 (unsigned long)((sec % 3600) / 60));
        draw_card(fb, 1, 5, "UPTIME", buf, ST_OK);
    }

    s_card_rows = 6;

    /* 滚动条：贴右缘，只在有内容超出一屏时出现。 */
    {
        const int total_h = s_card_rows * (CARD_H + CARD_GAP);
        const int view_h  = PK_DISPLAY_H - CARD_TOP;
        if (total_h > view_h) {
            const int tx = PK_DISPLAY_W - 6;
            const int bar_h = view_h * view_h / total_h;
            const int bar_y = CARD_TOP + s_scroll_y * view_h / total_h;
            pk_pfd_fill_rect(fb, tx, CARD_TOP, tx + 3, PK_DISPLAY_H,
                             pk_rgb565(30, 38, 50));
            pk_pfd_fill_rect(fb, tx, bar_y, tx + 3, bar_y + bar_h,
                             pk_rgb565(120, 135, 155));
        }
    }

    /* 顶栏最后画：卡片从它底下滑过去，而不是压在它上面。 */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PFD_BAR_BOT, COL_BG);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, CARD_PAD,
               (PFD_BAR_BOT - PK_AA_M_H) / 2, "DIAGNOSTICS", COL_HEADER,
               PK_AA_M);
}

/* ── 触摸：拖动滚卡片 ──────────────────────────────────────────────
 *
 * 与列表页同一套：按下只记起点，位移超过阈值才算拖动。这里暂时没有"点击"
 * 动作（点卡进详情是下一步），但阈值逻辑先立住——等详情页接上来时，判定
 * 规则不必再改一遍。
 *
 * 与 touch_gt911.c 的约定同 pk_adsb_list_*：返回 true 表示这一下被本页消费。
 * dock 展开时由 read_cb 统一让路，这里不重复判断。 */
static int  s_press_x, s_press_y, s_press_scroll;
static bool s_press_valid, s_moved;

#define DIAG_DRAG_SLOP  12

/* 卡片区之外（顶栏、右侧 FAB 那条）一律不消费。 */
static bool diag_in_cards(int x, int y)
{
    return y >= PFD_BAR_BOT && y < PK_DISPLAY_H &&
           x >= CARD_PAD - 8 && x < PK_DISPLAY_W - 80;
}

bool pk_diag_page_touch(int x, int y)
{
    /* 详情页：顶栏左侧那块是返回。它是这一页唯一的出路，命中区做到 96 px
     * 宽、整个顶栏高——比箭头本身大得多，宁可多占空间也不能让人点不中。 */
    if (s_detail >= 0) {
        if (y < PFD_BAR_BOT && x < DET_BACK_W) {
            s_detail = -1;
            s_press_valid = false;
            return true;
        }
        /* 详情页其余区域一律吃掉：底下是总览的卡片，穿透过去会直接换一页。 */
        s_press_valid = false;
        return (x < PK_DISPLAY_W - 80);
    }

    s_press_valid = diag_in_cards(x, y);
    if (!s_press_valid) return false;
    s_press_x      = x;
    s_press_y      = y;
    s_press_scroll = s_scroll_y;
    s_moved        = false;
    return true;
}

bool pk_diag_page_drag(int x, int y)
{
    if (!s_press_valid) return false;
    (void)x;
    const int dy = y - s_press_y;
    if (!s_moved && (dy > DIAG_DRAG_SLOP || dy < -DIAG_DRAG_SLOP)) s_moved = true;
    if (!s_moved) return true;

    /* 方向与手指一致：手指往上滑，内容往上走。 */
    int sy = s_press_scroll - dy;
    const int total_h = s_card_rows * (CARD_H + CARD_GAP);
    const int view_h  = PK_DISPLAY_H - CARD_TOP;
    const int max_y   = (total_h > view_h) ? (total_h - view_h) : 0;
    if (sy < 0)     sy = 0;
    if (sy > max_y) sy = max_y;
    s_scroll_y = sy;
    return true;
}

/* 松手：没拖动过才算点击 → 进入该卡片的详情。
 * 用按下时的坐标分派，理由同列表页：12 px 内的位移可能已经跨到相邻卡了。 */
void pk_diag_page_touch_up(void)
{
    const bool click = s_press_valid && !s_moved;
    const int  x = s_press_x, y = s_press_y;
    s_press_valid = false;
    s_moved       = false;
    if (!click || s_detail >= 0) return;

    const int col = (x < CARD_PAD + CARD_W + CARD_GAP / 2) ? 0 : 1;
    const int row = (y + s_scroll_y - CARD_TOP) / (CARD_H + CARD_GAP);
    const int idx = row * CARD_COLS + col;
    if (row >= 0 && idx >= 0 && idx < CARD_N) s_detail = idx;
}
void pk_diag_page_touch_cancel(void) { s_press_valid = false; s_moved = false; }

/* ═══════════════════════════════════════════════════════════════════════
 * 子系统详情（spec §5.5 第二层）
 *
 * spec 要求"详情必须保留 diag_page.c 现有全部深度"。所以这里不重新发明，
 * 直接复用 legacy 那套取数与 draw_snr_row()——尤其 GPS 每星座独立一行的
 * SNR 柱状图，那是排查 no-fix 的命门（不能只盯 fix=0，要看 SNR 和天线）。
 * ═════════════════════════════════════════════════════════════════════ */

#define DET_TOP     (PFD_BAR_BOT + 8)
#define DET_LINE_H  38
#define DET_KEY_X   24
#define DET_VAL_X   240

static void det_kv(uint16_t *fb, int line, const char *k, const char *v,
                   uint16_t vcol)
{
    const int y = DET_TOP + line * DET_LINE_H;
    if (y + DET_LINE_H > PK_DISPLAY_H) return;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_KEY_X,
               y + (PK_AA_M_H - PK_AA_XS_H) / 2, k, COL_KEY, PK_AA_XS);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_VAL_X, y, v, vcol, PK_AA_M);
}

static void draw_detail(uint16_t *fb, int which)
{
    char buf[64];
    int line = 0;

    switch (which) {
    case 2: {   /* GPS —— spec 点名的那一页 */
        pk_gps_state_t g = {0};
        pk_gps_get(&g);
        const bool fresh = g.have_fix &&
                           (esp_timer_get_time() - g.updated_us) < GPS_FRESH_US;

        snprintf(buf, sizeof(buf), "%s", g.last_nmea_us == 0 ? "no module"
                                       : fresh ? "3D fix" : "no fix");
        det_kv(fb, line++, "STATUS", buf, fresh ? COL_ONLINE : COL_ALERT);

        snprintf(buf, sizeof(buf), "%d used / %d in view", g.sats, g.sats_in_view);
        det_kv(fb, line++, "SATELLITES", buf, COL_VAL);

        snprintf(buf, sizeof(buf), "%.1f", (double)g.hdop);
        det_kv(fb, line++, "HDOP", buf, COL_VAL);

        /* 天线自检单独一行：它比"没星"精确得多，直接给结论。 */
        static const char *const kAnt[] = { "unknown", "OK", "OPEN", "SHORT" };
        det_kv(fb, line++, "ANTENNA",
               kAnt[g.ant_status <= PK_GPS_ANT_SHORT ? g.ant_status : 0],
               g.ant_status == PK_GPS_ANT_OK ? COL_ONLINE
               : g.ant_status == PK_GPS_ANT_UNKNOWN ? COL_OFFLINE : COL_ALERT);

        if (fresh) snprintf(buf, sizeof(buf), "%.5f  %.5f", g.lat, g.lon);
        else       snprintf(buf, sizeof(buf), "---");
        det_kv(fb, line++, "POSITION", buf, fresh ? COL_VAL : COL_OFFLINE);

        /* 每星座一行 SNR 柱状图。分星座画而不是混在一起：某个星座整体偏弱
         * 指向天线频段或遮挡方向，混画就看不出这个模式了。 */
        static const char *const kCon[PK_GNSS_COUNT] = { "GPS", "BDS", "GLO", "GAL" };
        for (int gi = 0; gi < PK_GNSS_COUNT; ++gi) {
            int cnt = 0;
            for (int i = 0; i < g.snr_count; ++i) if (g.snr_con[i] == gi) cnt++;
            if (cnt == 0) continue;
            const int y = DET_TOP + line * DET_LINE_H;
            if (y + DET_LINE_H > PK_DISPLAY_H) break;
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_KEY_X,
                       y + (PK_AA_M_H - PK_AA_XS_H) / 2, kCon[gi],
                       COL_KEY, PK_AA_XS);
            draw_snr_row(fb, DET_VAL_X, y + PK_AA_M_H + 4,
                         g.snr, g.snr_con, g.snr_count, gi);
            line++;
        }
        if (g.snr_count == 0)
            det_kv(fb, line++, "SNR", "(no satellites in view)", COL_OFFLINE);
        break;
    }

    case 0: {   /* IMU */
        pk_imu_sample_t st;
        const bool ok = pk_imu_sample_get(&st) && st.valid;
        det_kv(fb, line++, "SENSOR", ok ? "BNO085 online" : "offline",
               ok ? COL_ONLINE : COL_ALERT);
        if (ok) {
            snprintf(buf, sizeof(buf), "%u / 3", st.accuracy);
            det_kv(fb, line++, "CALIBRATION", buf,
                   st.accuracy >= 2 ? COL_ONLINE : COL_WARN);
            snprintf(buf, sizeof(buf), "%+.1f", (double)st.roll_deg);
            det_kv(fb, line++, "ROLL", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%+.1f", (double)st.pitch_deg);
            det_kv(fb, line++, "PITCH", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%.1f", (double)st.yaw_deg);
            det_kv(fb, line++, "YAW", buf, COL_VAL);
        }
        break;
    }

    case 1: {   /* BARO */
        pk_baro_state_t b;
        pk_baro_get(&b);
        det_kv(fb, line++, "SENSOR", b.valid ? "BMP388 online" : "offline",
               b.valid ? COL_ONLINE : COL_ALERT);
        if (b.valid) {
            snprintf(buf, sizeof(buf), "%.2f hPa", (double)b.pressure_pa / 100.0);
            det_kv(fb, line++, "PRESSURE", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%d ft", b.alt_ft);
            det_kv(fb, line++, "ALTITUDE", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%.2f hPa", (double)pk_qnh_get());
            det_kv(fb, line++, "QNH REF", buf, COL_VAL);
        }
        break;
    }

    case 3: {   /* SDR */
        uint32_t drop_kb = 0;
        const pk_sdr_state_t st = pk_sdr_state_get(&drop_kb);
        pk_dsp_stats_t d;
        pk_dsp_get_stats(&d);
        static const char *const kSdr[] = {
            "NO DONGLE", "attached", "STALLED", "streaming" };
        det_kv(fb, line++, "STATE", kSdr[st], st == PK_SDR_STREAMING
                                              ? COL_ONLINE : COL_ALERT);
        if (st == PK_SDR_NO_DEVICE)
            det_kv(fb, line++, "HINT", "connect to H2 (USB OTG)", COL_WARN);
        snprintf(buf, sizeof(buf), "%lu MS/s",
                 (unsigned long)(PK_RTLSDR_SAMPLERATE_HZ / 1000000UL));
        det_kv(fb, line++, "SAMPLE RATE", buf, COL_VAL);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.msgs_total);
        det_kv(fb, line++, "ADS-B MSGS", buf, COL_VAL);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.iq_drop_total);
        det_kv(fb, line++, "IQ DROPPED", buf,
               d.iq_drop_total ? COL_WARN : COL_VAL);
        break;
    }

    case 4:     /* BLE */
        det_kv(fb, line++, "LINK",
               ble_gatt_is_connected() ? "connected"
               : ble_gatt_is_advertising() ? "advertising" : "idle",
               ble_gatt_is_connected() ? COL_ONLINE : COL_OFFLINE);
        det_kv(fb, line++, "PROTOCOL", "GDL90 over BLE", COL_VAL);
        break;

    case 5: {   /* LOG */
        uint32_t written = 0, dropped = 0;
        if (record_sink_file_stats(&written, &dropped)) {
            det_kv(fb, line++, "BACKEND",
                   record_sink_file_uses_sd() ? "microSD" : "flash", COL_VAL);
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)written);
            det_kv(fb, line++, "WRITTEN", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)dropped);
            det_kv(fb, line++, "DROPPED", buf, dropped ? COL_WARN : COL_VAL);
        } else {
            det_kv(fb, line++, "SINK", "down", COL_ALERT);
        }
        break;
    }

    case 6:     /* CLK */
        fmt_clock(buf, sizeof(buf));
        det_kv(fb, line++, "TIME", buf, COL_VAL);
        det_kv(fb, line++, "SYNCED", pk_clock_is_synced() ? "yes" : "no",
               pk_clock_is_synced() ? COL_ONLINE : COL_WARN);
        break;

    case 7: {   /* SYS */
        int temp_c = 0;
        pk_soc_temp_get(&temp_c);
        snprintf(buf, sizeof(buf), "%d C", temp_c);
        det_kv(fb, line++, "SOC TEMP", buf, temp_c >= 75 ? COL_WARN : COL_VAL);
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(buf, sizeof(buf), "%luh %lum %lus",
                 (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60),
                 (unsigned long)(sec % 60));
        det_kv(fb, line++, "UPTIME", buf, COL_VAL);
        static const char *const kRst2[] = {
            "unknown", "power-on", "external", "software", "panic",
            "int WDT", "task WDT", "other WDT", "deep sleep", "brownout",
            "SDIO", "USB", "JTAG" };
        const esp_reset_reason_t rr = esp_reset_reason();
        det_kv(fb, line++, "LAST RESET",
               rr < (int)(sizeof(kRst2) / sizeof(kRst2[0])) ? kRst2[rr] : "?",
               (rr == ESP_RST_POWERON) ? COL_VAL : COL_WARN);
        break;
    }

    case 8: {   /* microSD */
        uint64_t total = 0, free_b = 0;
        const bool mounted = (pk_sdcard_state() == PK_SD_MOUNTED);
        det_kv(fb, line++, "STATE", mounted ? "mounted" : "no card",
               mounted ? COL_ONLINE : COL_ALERT);
        if (mounted && pk_sdcard_info(&total, &free_b)) {
            snprintf(buf, sizeof(buf), "%.1f GB",
                     (double)total / (1024.0 * 1024.0 * 1024.0));
            det_kv(fb, line++, "CAPACITY", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%.1f GB",
                     (double)free_b / (1024.0 * 1024.0 * 1024.0));
            det_kv(fb, line++, "FREE", buf, COL_VAL);
        }
        break;
    }

    case 9:     /* QNH */
        snprintf(buf, sizeof(buf), "%.2f hPa", (double)pk_qnh_get());
        det_kv(fb, line++, "QNH", buf, COL_VAL);
        det_kv(fb, line++, "NOTE", "baro altitude reference", COL_OFFLINE);
        break;

    case 10: {  /* BATT */
        pk_batt_t b;
        pk_batt_get(&b);
        if (b.valid) {
            snprintf(buf, sizeof(buf), "%d %%", b.pct);
            det_kv(fb, line++, "CHARGE", buf, b.pct >= 20 ? COL_ONLINE : COL_WARN);
            snprintf(buf, sizeof(buf), "%.3f V", b.batt_mv / 1000.0);
            det_kv(fb, line++, "VOLTAGE", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%d mV", b.raw_mv);
            det_kv(fb, line++, "ADC RAW", buf, COL_OFFLINE);
            det_kv(fb, line++, "CHARGING", b.charging ? "yes" : "no",
                   b.charging ? COL_ONLINE : COL_VAL);
            /* 这块板的电池只接充电通路、没有 power path：拔掉 USB 是彻底
             * 断电再上电（实测复位原因为 power-on，不是 brownout）。这一行
             * 是给排查者的，不是给飞行员的——但它能省掉一轮"为什么会重启"。 */
            det_kv(fb, line++, "ON UNPLUG", "device reboots (no power path)",
                   COL_WARN);
        } else {
            det_kv(fb, line++, "BATTERY", "not detected", COL_ALERT);
        }
        break;
    }

    case 11: {  /* UPTIME */
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(buf, sizeof(buf), "%luh %lum %lus",
                 (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60),
                 (unsigned long)(sec % 60));
        det_kv(fb, line++, "SINCE BOOT", buf, COL_VAL);
        snprintf(buf, sizeof(buf), "%lu s", (unsigned long)sec);
        det_kv(fb, line++, "SECONDS", buf, COL_OFFLINE);
        break;
    }

    default:
        det_kv(fb, line++, "DETAIL", "no further data", COL_OFFLINE);
        break;
    }
}

/* 详情页顶栏：返回箭头 + 子系统名。返回区（DET_BACK_W）做得比箭头本身大
 * 得多——它是这一页唯一的出路，宁可多吃空间也不能让人点不中。 */
static void draw_detail_header(uint16_t *fb, int which)
{
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PFD_BAR_BOT, COL_BG);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 20,
               (PFD_BAR_BOT - PK_AA_M_H) / 2, "\u2190", COL_HEADER, PK_AA_M);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_BACK_W,
               (PFD_BAR_BOT - PK_AA_M_H) / 2,
               (which >= 0 && which < CARD_N) ? CARD_TITLE[which] : "DETAIL",
               COL_HEADER, PK_AA_M);
    pk_pfd_fill_rect(fb, 0, PFD_BAR_BOT - 1, PK_DISPLAY_W, PFD_BAR_BOT,
                     pk_rgb565(38, 48, 62));
}
