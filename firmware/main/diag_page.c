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
#include <stdlib.h>
#include <stdbool.h>
#include "esp_system.h"
#include "esp_timer.h"

#include "display.h"
#include "pilot_kit.h"       /* PK_RTLSDR_SAMPLERATE_HZ */
#include "imu_task.h"        /* pk_imu_sample_get, pk_imu_sample_t */
#include "dsp_task.h"        /* pk_dsp_get_stats, pk_dsp_stats_t */
#include "gps.h"             /* pk_gps_get, pk_gps_state_t */
#include "baro.h"            /* pk_baro_get, pk_baro_state_t */
#include "config_qnh.h"      /* pk_qnh_get — baro 高度的 QNH 基准 */
#include "config_storage.h"  /* pk_log_store_get — 用户希望写哪个后端 */
#include "i18n.h"            /* pk_i18n_text — 页面文案随语言切换 */
#include "ble_gatt.h"        /* ble_gatt_is_connected, ble_gatt_is_advertising */
#include "pk_clock.h"        /* pk_clock_is_synced / pk_clock_source */
#include "pk_sdcard.h"       /* pk_sdcard_state / pk_sdcard_info */
#include "pk_aero_db.h"      /* pk_aero_db_status_get — SD 航空库状态 */
#include "record_sink.h"     /* record_sink_file_stats / _uses_sd */
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* Palette — matches about_page.c for visual consistency */
#define COL_BG               pk_rgb565( 12,  12,  16)
#define COL_HEADER           pk_rgb565(180, 235, 255)
#define COL_KEY              pk_rgb565(180, 235, 255)
#define COL_ONLINE           pk_rgb565( 80, 220,  80)   /* green  — subsystem live */
#define COL_OFFLINE          pk_rgb565(140, 145, 155)   /* grey   — no data / offline */
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
 * --------------------------------------------------------------------------
 *
 * 2026-07-30：删掉了 320×240 时代的逐行详情视图 diag_render_legacy() 与它
 * 专属的 draw_diag_row()。它自总览+详情两层（spec §5.5）上线起就挂着
 * __attribute__((unused))「留着供详情层复用」，但详情层最终是自己写的
 * （见文件末尾 detail 段），只复用了 draw_snr_row()/fmt_clock() 这两个取数
 * 与绘图工具——那两个仍然在用，没有动。硬件已换成 4.3″ 800×480 触摸屏，
 * 不会再退回 2.4″ 逐行版面，这段死代码只是在占 app 分区。
 * -------------------------------------------------------------------------- */

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
#include "pk_ui_nav.h"
#include "pfd_draw.h"
#include "battery.h"
#include "soc_temp.h"

#define CARD_COLS   2
#define CARD_PAD    PK_UI_PAD_L   /* 卡片外边距 = 整页左边距（pfd_layout.h） */
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
/*
 * 四态。绿 / 琥珀 / 红 / 灰，语义严格分开：
 *
 *   ST_OK    绿   正常工作
 *   ST_WARN  琥珀 连着但不正常，需要注意（丢过数据、温度偏高、掉链）
 *   ST_BAD   红   不可用：模块缺失、故障、配置不成立
 *   ST_IDLE  灰   **正常但未激活**——功能好着，只是现在没在用
 *
 * 之前一度只有前三态，于是"BLE 正在广播"（正常等待连接）被涂成琥珀、
 * "日志写 flash 但还没写过一条"被涂成红——两个都是正常状态却在喊救命，
 * 而真正的告警反而被稀释了。罩哥指出后加回 ST_IDLE。
 *
 * 与更早那次"删掉灰态"不矛盾：那次删的是拿灰表示**模块缺失**（GPS 没插
 * 显示灰、别的模块离线显示红，同样是用不了却给了两种权重）。缺失一律红，
 * 灰只留给"好着但闲着"。
 */
typedef enum { ST_OK = 0, ST_WARN, ST_BAD, ST_IDLE } card_state_t;

static uint16_t state_color(card_state_t st)
{
    switch (st) {
    case ST_OK:   return COL_ONLINE;
    case ST_WARN: return COL_WARN;
    case ST_IDLE: return COL_OFFLINE;
    case ST_BAD:
    default:      return COL_ALERT;
    }
}

/* 卡片总数与滚动。
 *
 * spec §5.5 写的是「2 × 4」八格，但那是版面示意不是容量上限——诊断数据本来
 * 就比八条多（SD 卡、QNH、运行时长…），塞不下的不该被删掉，该能滚。所以
 * 行数由卡片数推出来，屏幕放不下就滚动。 */
static int s_card_rows;                        /* 本帧实际画了几行 */
static int s_scroll_y;                         /* 滚动偏移(px)，0 = 顶 */

/*
 * 当前展开的子系统详情，-1 = 总览（spec §5.5 的两层结构）。
 *
 * 记的是**卡片序号**而不是指针：卡片是每帧重画的，序号是稳定的。
 */
static int s_detail = -1;

/* 卡片序号 → 标题。顺序必须与 render 里的 draw_card 调用一致——两处各写
 * 一份序号迟早会错位，所以这张表是唯一的真值来源，render 也从它取标题。
 *
 * 存的是词条 id 而不是字符串：语言可以在运行时切，取到的必须是**当前**语言
 * 的写法。存字符串就意味着切语言后这张表还停在旧语言上。 */
static const pk_tr_id_t CARD_TITLE[] = {
    PK_TR_DIAG_CARD_IMU, PK_TR_DIAG_CARD_BARO, PK_TR_DIAG_CARD_GPS,
    PK_TR_DIAG_CARD_SDR, PK_TR_DIAG_CARD_BLE,  PK_TR_DIAG_CARD_LOG,
    PK_TR_DIAG_CARD_CLK, PK_TR_DIAG_CARD_SYS,  PK_TR_DIAG_CARD_SD,
    PK_TR_DIAG_CARD_QNH, PK_TR_DIAG_CARD_BATT, PK_TR_DIAG_CARD_UPTIME,
};
#define CARD_N  ((int)(sizeof(CARD_TITLE) / sizeof(CARD_TITLE[0])))

/* 卡片标题的当前语言写法。总览与详情页共用，两处不各查一次表。 */
static const char *card_title(int idx)
{
    return pk_i18n_text((idx >= 0 && idx < CARD_N) ? CARD_TITLE[idx]
                                                   : PK_TR_DIAG_K_DETAIL);
}

/* 上次复位原因 → 词条。索引即 esp_reset_reason_t，表长必须覆盖枚举全域，
 * 少一项就是一次越界读（legacy 那张名表就因为照抄时少写了几项出过事）。 */
static const pk_tr_id_t RESET_TR[] = {
    PK_TR_DIAG_RST_UNKNOWN,  PK_TR_DIAG_RST_POWERON, PK_TR_DIAG_RST_EXT,
    PK_TR_DIAG_RST_SW,       PK_TR_DIAG_RST_PANIC,   PK_TR_DIAG_RST_INT_WDT,
    PK_TR_DIAG_RST_TASK_WDT, PK_TR_DIAG_RST_WDT,     PK_TR_DIAG_RST_SLEEP,
    PK_TR_DIAG_RST_BROWNOUT, PK_TR_DIAG_RST_SDIO,    PK_TR_DIAG_RST_USB,
    PK_TR_DIAG_RST_JTAG,
};

static const char *reset_reason_text(esp_reset_reason_t rr)
{
    const int n = (int)(sizeof(RESET_TR) / sizeof(RESET_TR[0]));
    return pk_i18n_text(((int)rr >= 0 && (int)rr < n) ? RESET_TR[rr]
                                                      : PK_TR_DIAG_RST_UNKNOWN);
}

/* 日志后端名。诊断页要同时说「现在实际在写哪个」和「用户设的是哪个」，
 * 两处都从这里取名字，免得一处写 flash 另一处写 FLASH。 */
static const char *log_store_text(bool on_sd)
{
    return pk_i18n_text(on_sd ? PK_TR_DIAG_V_LOG_SD : PK_TR_DIAG_V_LOG_FLASH);
}

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

    /* 卡片标题与 about / settings 的条目名同一档（PK_UI_ITEM_SIZE）：三处都是
     * 「这一项叫什么」，原来这里用 XS，比那两页小两档，翻页时字忽大忽小。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x + 18, y + 10,
               title, COL_KEY, PK_UI_ITEM_SIZE);

    /* 值太长就降到 S 档，避免把接口提示截掉。
     *
     * 宽度必须用 pk_aa_text_width：strlen 数的是字节，一个汉字 3 字节却只画
     * 一个字形。按 strlen 算，「无接收机 - 用 H2 USB-C」会被当成 24 个字符
     * （实际 16 个字形）而误降一档，中文界面上整页的值都会莫名其妙变小。 */
    const int avail = CARD_W - 26;
    const int need  = pk_aa_text_width(value, PK_AA_M);
    const pk_aa_size_t sz = (need <= avail) ? PK_AA_M : PK_AA_S;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x + 18,
               y + CARD_H - 12 - pk_aa_cell_h(sz), value, state_color(st), sz);
}

/* 前向声明：详情那层定义在文件末尾（它复用 draw_snr_row 等 legacy 工具）。 */
static void draw_detail(uint16_t *fb, int which);
static void draw_detail_header(uint16_t *fb, int which);

void pk_diag_page_render(uint16_t *fb)
{
    char buf[64];

    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* 详情层：spec §5.5 的第二层。总览负责"哪个子系统不对"，详情负责
     * "到底怎么不对"，两层的信息密度差着一个量级，不该挤在一起。 */
#ifdef PK_SIM_BUILD
    /* 截图用：PK_SIM_DIAG_DETAIL=<卡片序号> 直接进该详情页。 */
    { const char *e = getenv("PK_SIM_DIAG_DETAIL");
      if (e) s_detail = atoi(e); }
    /* PK_SIM_DIAG_SCROLL=<px> 把总览滚到指定位置。12 张卡片一屏只放得下 6 张，
     * 不给这个旋钮，下面半屏（microSD/QNH/电池/运行时长）就永远截不到——而
     * 「无卡」恰恰是空态最该核对的一格。用法与设置页的 PK_SIM_SET_SCROLL 一致。 */
    { const char *e = getenv("PK_SIM_DIAG_SCROLL");
      if (e) s_scroll_y = atoi(e); }
#endif
    if (s_detail >= 0) {
        draw_detail(fb, s_detail);
        draw_detail_header(fb, s_detail);
        return;
    }

    /* ── IMU ── */
    {
        pk_imu_sample_t s;
        if (pk_imu_sample_get(&s) && s.valid) {
            snprintf(buf, sizeof(buf), "%s %u/3   %s %.0f",
                     pk_i18n_text(PK_TR_DIAG_U_CAL), s.accuracy,
                     pk_i18n_text(PK_TR_DIAG_U_YAW), (double)s.yaw_deg);
            /* 校准等级本身就是可信度：cal 0 时航向可以差几十度，数据却照样
             * "有效"——这正是最该在总览里就看见的。 */
            draw_card(fb, 0, 0, card_title(0), buf,
                      s.accuracy >= 2 ? ST_OK : ST_WARN);
        } else {
            draw_card(fb, 0, 0, card_title(0),
                      pk_i18n_text(PK_TR_DIAG_V_IMU_OFFLINE), ST_BAD);
        }
    }

    /* ── BARO ── */
    {
        pk_baro_state_t b;
        pk_baro_get(&b);
        if (b.valid) {
            snprintf(buf, sizeof(buf), "%d ft   %.1f hPa",
                     b.alt_ft, (double)b.pressure_pa / 100.0);
            draw_card(fb, 1, 0, card_title(1), buf, ST_OK);
        } else {
            draw_card(fb, 1, 0, card_title(1),
                      pk_i18n_text(PK_TR_DIAG_V_BARO_OFFLINE), ST_BAD);
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
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_NO_MODULE), ST_BAD);
        } else if (now - g.last_nmea_us > 5000000LL) {
            /* 曾经在讲话，现在哑了 = 掉线/供电/接触不良，跟没装是两回事。 */
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_MODULE_SILENT), ST_BAD);
        } else if (g.ant_status == PK_GPS_ANT_OPEN) {
            /* 模块自检报的天线开路，比"没星"精确得多——直接说结论。 */
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_ANT_OPEN), ST_BAD);
        } else if (g.ant_status == PK_GPS_ANT_SHORT) {
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_ANT_SHORT), ST_BAD);
        } else if (fresh) {
            /* 数字与量词分开取：整句进 catalog 就等于把 snprintf 的格式串
             * 交给翻译者改，参数个数一对不上就是越界读栈。中英的词序在这里
             * 恰好一致（"fix 7 sats" / "已定位 7 星"），拼起来都读得通。 */
            snprintf(buf, sizeof(buf), "%s   %d %s   HDOP %.1f",
                     pk_i18n_text(PK_TR_DIAG_V_FIX), g.sats,
                     pk_i18n_text(PK_TR_DIAG_U_SATS), (double)g.hdop);
            draw_card(fb, 0, 1, card_title(2), buf, ST_OK);
        } else if (g.snr_count > 0) {
            /* 看得见星却定不了位 = 信号弱/遮挡，动作是"再等等或换个位置"。 */
            snprintf(buf, sizeof(buf), "%s   %d %s",
                     pk_i18n_text(PK_TR_DIAG_V_NO_FIX), g.snr_count,
                     pk_i18n_text(PK_TR_DIAG_U_VISIBLE));
            draw_card(fb, 0, 1, card_title(2), buf, ST_WARN);
        } else {
            /* 模块在讲话、天线自检没报错、但一颗星都没看见：冷启动搜星中，
             * 或者被完全遮挡。这才是"再等等"，不该报成天线故障。 */
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_SEARCHING), ST_WARN);
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
            draw_card(fb, 1, 1, card_title(3),
                      pk_i18n_text(PK_TR_DIAG_V_SDR_NONE), ST_BAD);
            break;
        case PK_SDR_ATTACHED:
            draw_card(fb, 1, 1, card_title(3),
                      pk_i18n_text(PK_TR_DIAG_V_SDR_ATTACH), ST_WARN);
            break;
        case PK_SDR_STALLED:
            draw_card(fb, 1, 1, card_title(3),
                      pk_i18n_text(PK_TR_DIAG_V_SDR_STALL), ST_WARN);
            break;
        default:
            snprintf(buf, sizeof(buf), "%luMS/s  %s %lu",
                     (unsigned long)(PK_RTLSDR_SAMPLERATE_HZ / 1000000UL),
                     pk_i18n_text(PK_TR_DIAG_U_MSGS),
                     (unsigned long)d.msgs_total);
            draw_card(fb, 1, 1, card_title(3), buf, ST_OK);
            break;
        }
    }

    /* ── BLE ── */
    {
        const bool conn = ble_gatt_is_connected();
        const bool adv  = ble_gatt_is_advertising();
        draw_card(fb, 0, 2, card_title(4),
                  pk_i18n_text(conn ? PK_TR_DIAG_V_BLE_CONN
                                    : adv ? PK_TR_DIAG_V_BLE_ADV
                                          : PK_TR_DIAG_V_BLE_IDLE),
                  /* 广播中 = 功能正常、只是还没人连，属于"好着但闲着"。
                   * idle 才是红：那说明 BLE 压根没起来（被设置关掉或初始化
                   * 失败），GDL90 这条输出通路是断的。 */
                  conn ? ST_OK : adv ? ST_IDLE : ST_BAD);
    }

    /* ── LOG ──
     *
     * 这一格说的是**现在实际在写哪个后端**（record_sink_file_uses_sd），不是
     * 用户在设置页选了哪个（pk_log_store_get）。两者语义本来就不同，而且
     * 改设置只在下次创建 file sink 时生效，即重启后。
     *
     * 罩哥验收时点出："切到 SD 卡存储后诊断页仍显示 flash"——那不是 bug，
     * 是这一格只说了实际后端、没说还有个待生效的设置。诊断页的职责是说实话，
     * 所以两个都说：主体仍是实际后端（说假话会让排查者去 SD 上找不存在的
     * 文件），后面补一句「设为 X」把差异挑明。 */
    {
        uint32_t written = 0, dropped = 0;
        if (record_sink_file_stats(&written, &dropped)) {
            const bool on_sd = record_sink_file_uses_sd();
            const bool want_sd = (pk_log_store_get() == PK_LOG_STORE_SD);
            int p = snprintf(buf, sizeof(buf), "%s  %s %lu",
                             log_store_text(on_sd),
                             pk_i18n_text(PK_TR_DIAG_U_W),
                             (unsigned long)written);
            if (want_sd != on_sd && p > 0 && p < (int)sizeof(buf))
                snprintf(buf + p, sizeof(buf) - p, "  (%s %s)",
                         pk_i18n_text(PK_TR_DIAG_V_SET_TO),
                         log_store_text(want_sd));
            /* 丢过条目就是琥珀：还在写，但已经不完整了，跟"没在写"是两回事。 */
            draw_card(fb, 1, 2, card_title(5), buf,
                      /* written == 0 只是"还没收到可写的数据"，不是故障——
                       * 刚开机、或者 SDR 没插时本来就一条都没有。sink 建起来
                       * 了就算正常；丢过条目才是琥珀（还在写但已不完整）。
                       * 设置与实际不一致也是琥珀：不是故障，但用户以为已经
                       * 换过去了，得让这一格自己喊一声。 */
                      dropped > 0 ? ST_WARN
                      : want_sd != on_sd ? ST_WARN
                      : written > 0 ? ST_OK : ST_IDLE);
        } else {
            draw_card(fb, 1, 2, card_title(5),
                      pk_i18n_text(PK_TR_DIAG_V_SINK_DOWN), ST_BAD);
        }
    }

    /* ── CLK ── */
    {
        char tbuf[48];
        fmt_clock(tbuf, sizeof(tbuf));
        draw_card(fb, 0, 3, card_title(6), tbuf,
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
        /* 复位原因与详情页共用 RESET_TR：总览曾经另存一份更短的缩写表
         * （POR / SW / int-WDT…），于是同一个复位原因在两层里长得不一样，
         * 下钻时还得先认一遍是不是同一件事。卡片放得下完整词，就不缩。 */
        const esp_reset_reason_t rr = esp_reset_reason();
        snprintf(buf, sizeof(buf), "SoC %d C   %s %s", temp_c,
                 pk_i18n_text(PK_TR_DIAG_U_RST), reset_reason_text(rr));
        draw_card(fb, 1, 3, card_title(7), buf,
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
                snprintf(buf, sizeof(buf), "%.1f/%.1f GB %s",
                         (double)(total - free_b) / (1024.0 * 1024.0 * 1024.0),
                         (double)total / (1024.0 * 1024.0 * 1024.0),
                         pk_i18n_text(PK_TR_DIAG_U_USED));
                /* 剩余不足 10% 转琥珀：卡还在、还能写，但快写不下了。 */
                draw_card(fb, 0, 4, card_title(8), buf,
                          (total && free_b * 10 < total) ? ST_WARN : ST_OK);
            } else {
                draw_card(fb, 0, 4, card_title(8),
                          pk_i18n_text(PK_TR_DIAG_V_SD_MOUNTED), ST_OK);
            }
            break;
        case PK_SD_FORMATTING:
            draw_card(fb, 0, 4, card_title(8),
                      pk_i18n_text(PK_TR_DIAG_V_SD_FORMATTING), ST_WARN);
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
            draw_card(fb, 0, 4, card_title(8),
                      pk_i18n_text(PK_TR_DIAG_V_SD_NO_CARD), ST_BAD);
            break;
        }
    }

    /* ── QNH ──
     * baro 高度的基准。它不是"状态"而是"设定值"，但设错了高度就整体偏——
     * 标准 1013.25 与当地实际能差几百英尺，值得占一格。 */
    {
        const float q = pk_qnh_get();
        snprintf(buf, sizeof(buf), "%.2f hPa", (double)q);
        draw_card(fb, 1, 4, card_title(9), buf, ST_OK);
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
            draw_card(fb, 0, 5, card_title(10), buf,
                      b.charging ? ST_OK : b.pct >= 20 ? ST_OK : ST_WARN);
        } else {
            /* 没接电池时引脚浮空，读数乱跳——不显示百分比，只说没接。 */
            snprintf(buf, sizeof(buf), "%s (raw %dmV)",
                     pk_i18n_text(PK_TR_DIAG_V_NO_BATTERY), b.raw_mv);
            draw_card(fb, 0, 5, card_title(10), buf, ST_BAD);
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
        draw_card(fb, 1, 5, card_title(11), buf, ST_OK);
    }

    /* ── AERO DB ──
     * SD 航空数据库（/sdcard/aero/pk_aero.bin）的加载状态：周期 + 记录数 +
     * 状态/错误。属于 microSD 那一类"卡上有什么"的信息，跟在存储区块后面。
     *
     * 追加在网格末尾而不是插到 SD 卡旁边：卡片的表序号 = 触摸分派与
     * CARD_TITLE 的下标，往中间插一张会把后面所有卡的详情映射错位。
     * 标题/文案用 ASCII 字面量、不走 catalog：新增词条要重跑字库生成
     * 流程（gen_i18n_assets.py，见 project_i18n_onscreen_text_workflow），
     * 本卡全部内容（周期、计数、状态词）本来就是 ASCII，先不动字库。
     * 序号 12 ≥ CARD_N，触摸层自然不进详情页——本卡暂无详情。 */
    {
        pk_aero_db_status_t a;
        pk_aero_db_status_get(&a);
        card_state_t st;
        switch (a.state) {
        case PK_AERO_DB_READY:
            snprintf(buf, sizeof(buf), "%s READY  %lu apt",
                     a.cycle, (unsigned long)a.n_airports);
            st = ST_OK;
            break;
        case PK_AERO_DB_LOADING:
            snprintf(buf, sizeof(buf), "loading %u%%", (unsigned)a.load_pct);
            st = ST_WARN;   /* 短暂过渡态（约 2 s），看见它就是"稍等" */
            break;
        case PK_AERO_DB_ERROR:
            snprintf(buf, sizeof(buf), "ERR %s", a.err ? a.err : "?");
            st = ST_BAD;
            break;
        default:
            /* 无卡/无文件是正常用法（数据库是可选内容包），灰而不红 */
            snprintf(buf, sizeof(buf), "no data");
            st = ST_IDLE;
            break;
        }
        draw_card(fb, 0, 6, "AERO DB", buf, st);
    }

    s_card_rows = 7;

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
    /* 标题走全局层级（pfd_layout.h）。这一页原来用 COL_HEADER 的淡蓝，是五页
     * 里唯一的蓝标题——罩哥在真机上第一眼就点出来了。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, CARD_PAD,
               PK_UI_TITLE_Y, pk_i18n_text(PK_TR_DIAG_TITLE),
               PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
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
        /* 顶栏不拦：那块是 nav 的 backbar（LVGL 控件），让它自己收点击。
         * 三条退路里"顶栏按钮"与"FAB"都由导航层提供，本页只要不挡路。 */
        if (y < PFD_BAR_BOT) { s_press_valid = false; return false; }

        /* 其余区域一律吃掉：底下是总览的卡片，穿透过去会直接换一页。 */
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
    if (row >= 0 && idx >= 0 && idx < CARD_N) {
        s_detail = idx;
        /* 走导航层的二级页机制，而不是自己画一个返回箭头就算完（上一版就是
         * 那么干的）。spec §4.2：进子页后 FAB 图标变 ←、dock 收起且不可展开，
         * **三条退路（顶栏按钮 / FAB / 右滑）同时可用**——无物理按键的设备
         * 上，任何一条失效都不能让用户困在里面。自画一个箭头只提供了一条。 */
        /* backbar 走 LVGL 的 tiny_ttf（全字库，不是 catalog 子集），中文标题
         * 不会缺字；set_subpage 内部 lv_snprintf 进自己的 buf，不留指针。 */
        pk_ui_nav_set_subpage(true, pk_i18n_text(PK_TR_DIAG_TITLE));
    }
}
void pk_diag_page_touch_cancel(void) { s_press_valid = false; s_moved = false; }

/* ═══════════════════════════════════════════════════════════════════════
 * 子系统详情（spec §5.5 第二层）
 *
 * spec 要求"详情必须保留 diag_page.c 现有全部深度"。所以这里不重新发明，
 * 直接复用 legacy 那套取数与 draw_snr_row()——尤其 GPS 每星座独立一行的
 * SNR 柱状图，那是排查 no-fix 的命门（不能只盯 fix=0，要看 SNR 和天线）。
 * ═════════════════════════════════════════════════════════════════════ */

/*
 * 详情页从上到下三行：
 *
 *   1) backbar「← DIAGNOSTICS」 —— LVGL 层，几何见 pk_ui_nav.h
 *   2) 子系统名（IMU / GPS …）  —— DET_TITLE_TOP，本文件画进 framebuffer
 *   3) 键值内容                 —— DET_TOP 起，每行 DET_LINE_H
 *
 * 两个纵坐标都从导出的 PK_UI_BACKBAR_BOT 推出来，不在这里另抄数字：抄的那份
 * 不会跟着 backbar 变，上一版把 44 抄成 36，内容正好贴着 backbar 没有间隙。
 *
 * 顺序不能反。子系统名一度画在顶栏（0..PFD_BAR_BOT），于是从上往下读成
 * 「IMU / ← DIAGNOSTICS / 内容」——退路排在第二行，得先认出自己在哪儿才找得
 * 到怎么出去。现在 backbar 占第一行，名字跟在它下面。 */
/* 详情页的键列 = 整页左边距。原来是自留的 24，为的是对齐 backbar 的文字
 * （那时胶囊内边距 14 把字推到了 22）；现在 backbar 反过来按 PK_UI_PAD_L 排
 * 版（见 pk_ui_nav.c），两边都不用再互相迁就。 */
#define DET_KEY_X     PK_UI_PAD_L
/* 子系统名的左缘与内容键列同一条线：三行左对齐，视线不用来回找起点。 */
#define DET_TITLE_TOP (PK_UI_BACKBAR_BOT + 8)
#define DET_TOP       (DET_TITLE_TOP + PK_AA_M_H + 10)
#define DET_LINE_H  38
#define DET_VAL_X   240

static void det_kv(uint16_t *fb, int line, const char *k, const char *v,
                   uint16_t vcol)
{
    const int y = DET_TOP + line * DET_LINE_H;
    if (y + DET_LINE_H > PK_DISPLAY_H) return;
    /* 键名与值同档：键列宽 224 px（DET_VAL_X - DET_KEY_X），最长的
     * "CALIBRATION" / "SAMPLE RATE" 各 11 字符 × 15 px = 165 px，装得下。
     * 同档也就不再需要原来那半个字高的垂直补偿。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_KEY_X, y, k,
               COL_KEY, PK_UI_ITEM_SIZE);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_VAL_X, y, v, vcol, PK_AA_M);
}

/* 键名从 catalog 取，值有的取 catalog、有的现算——两种写法混在一行里读着乱，
 * 所以给键包一层：det_kv_tr(fb, line, 词条, 值, 颜色)。 */
static void det_kv_tr(uint16_t *fb, int line, pk_tr_id_t key, const char *v,
                      uint16_t vcol)
{
    det_kv(fb, line, pk_i18n_text(key), v, vcol);
}

/* 键与值都来自 catalog 的那些行（状态词），再省一层。 */
static void det_kv_tr2(uint16_t *fb, int line, pk_tr_id_t key, pk_tr_id_t val,
                       uint16_t vcol)
{
    det_kv(fb, line, pk_i18n_text(key), pk_i18n_text(val), vcol);
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

        det_kv_tr2(fb, line++, PK_TR_DIAG_K_STATUS,
                   g.last_nmea_us == 0 ? PK_TR_DIAG_V_NO_MODULE
                   : fresh             ? PK_TR_DIAG_V_FIX_3D
                                       : PK_TR_DIAG_V_NO_FIX,
                   fresh ? COL_ONLINE : COL_ALERT);

        snprintf(buf, sizeof(buf), "%d %s / %d %s",
                 g.sats, pk_i18n_text(PK_TR_DIAG_U_USED),
                 g.sats_in_view, pk_i18n_text(PK_TR_DIAG_U_IN_VIEW));
        det_kv_tr(fb, line++, PK_TR_DIAG_K_SATELLITES, buf, COL_VAL);

        snprintf(buf, sizeof(buf), "%.1f", (double)g.hdop);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_HDOP, buf, COL_VAL);

        /* 天线自检单独一行：它比"没星"精确得多，直接给结论。 */
        static const pk_tr_id_t kAnt[] = {
            PK_TR_DIAG_V_ANT_UNKNOWN, PK_TR_DIAG_V_ANT_OK,
            PK_TR_DIAG_V_ANT_OPEN_S,  PK_TR_DIAG_V_ANT_SHORT_S };
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_ANTENNA,
                   kAnt[g.ant_status <= PK_GPS_ANT_SHORT ? g.ant_status : 0],
                   g.ant_status == PK_GPS_ANT_OK ? COL_ONLINE
                   : g.ant_status == PK_GPS_ANT_UNKNOWN ? COL_OFFLINE : COL_ALERT);

        if (fresh) snprintf(buf, sizeof(buf), "%.5f  %.5f", g.lat, g.lon);
        else       snprintf(buf, sizeof(buf), "---");
        det_kv_tr(fb, line++, PK_TR_DIAG_K_POSITION, buf,
                  fresh ? COL_VAL : COL_OFFLINE);

        /* 每星座一行 SNR 柱状图。分星座画而不是混在一起：某个星座整体偏弱
         * 指向天线频段或遮挡方向，混画就看不出这个模式了。 */
        /* 与 pk_gnss_t 同序、同长度——legacy 那份是
         * { "GPS","BDS","GLO","GAL","QZS","?" } 六项，我照抄时只写了四项，
         * gi 循环到 PK_GNSS_COUNT-1 会越界读。名表必须跟着枚举走，
         * 少一项就是一次越界。 */
        static const char *const kCon[PK_GNSS_COUNT] =
            { "GPS", "BDS", "GLO", "GAL", "QZS", "?" };
        for (int gi = 0; gi < PK_GNSS_COUNT; ++gi) {
            int cnt = 0;
            for (int i = 0; i < g.snr_count; ++i) if (g.snr_con[i] == gi) cnt++;
            if (cnt == 0) continue;
            const int y = DET_TOP + line * DET_LINE_H;
            if (y + DET_LINE_H > PK_DISPLAY_H) break;
            /* 星座名是 det_kv 的键列的一员，字号必须跟 det_kv 走。 */
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_KEY_X, y,
                       kCon[gi], COL_KEY, PK_UI_ITEM_SIZE);
            draw_snr_row(fb, DET_VAL_X, y + PK_AA_M_H + 4,
                         g.snr, g.snr_con, g.snr_count, gi);
            line++;
        }
        if (g.snr_count == 0)
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_SNR, PK_TR_DIAG_V_NO_SATS,
                       COL_OFFLINE);
        break;
    }

    case 0: {   /* IMU */
        pk_imu_sample_t st;
        const bool ok = pk_imu_sample_get(&st) && st.valid;
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_SENSOR,
                   ok ? PK_TR_DIAG_V_IMU_ONLINE : PK_TR_DIAG_V_OFFLINE,
                   ok ? COL_ONLINE : COL_ALERT);
        if (ok) {
            snprintf(buf, sizeof(buf), "%u / 3", st.accuracy);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_CALIBRATION, buf,
                      st.accuracy >= 2 ? COL_ONLINE : COL_WARN);
            snprintf(buf, sizeof(buf), "%+.1f", (double)st.roll_deg);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_ROLL, buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%+.1f", (double)st.pitch_deg);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_PITCH, buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%.1f", (double)st.yaw_deg);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_YAW, buf, COL_VAL);
        } else {
            /* 离线时补一条接线提示，照 SDR 那页的做法。只写「离线」的话整页
             * 就一行字加四百像素黑，看起来像详情页没画出来，而且没告诉用户
             * 下一步能做什么。 */
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_HINT, PK_TR_DIAG_V_IMU_HINT,
                       COL_WARN);
        }
        break;
    }

    case 1: {   /* BARO */
        pk_baro_state_t b;
        pk_baro_get(&b);
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_SENSOR,
                   b.valid ? PK_TR_DIAG_V_BARO_ONLINE : PK_TR_DIAG_V_OFFLINE,
                   b.valid ? COL_ONLINE : COL_ALERT);
        if (b.valid) {
            snprintf(buf, sizeof(buf), "%.2f hPa", (double)b.pressure_pa / 100.0);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_PRESSURE, buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%d ft", b.alt_ft);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_ALTITUDE, buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%.2f hPa", (double)pk_qnh_get());
            det_kv_tr(fb, line++, PK_TR_DIAG_K_QNH_REF, buf, COL_VAL);
        } else {
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_HINT, PK_TR_DIAG_V_BARO_HINT,
                       COL_WARN);
        }
        break;
    }

    case 3: {   /* SDR */
        uint32_t drop_kb = 0;
        const pk_sdr_state_t st = pk_sdr_state_get(&drop_kb);
        pk_dsp_stats_t d;
        pk_dsp_get_stats(&d);
        static const pk_tr_id_t kSdr[] = {
            PK_TR_DIAG_V_SDR_NONE_S,  PK_TR_DIAG_V_SDR_ATTACH_S,
            PK_TR_DIAG_V_SDR_STALL_S, PK_TR_DIAG_V_SDR_STREAM };
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_STATE, kSdr[st],
                   st == PK_SDR_STREAMING ? COL_ONLINE : COL_ALERT);
        if (st == PK_SDR_NO_DEVICE)
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_HINT, PK_TR_DIAG_V_SDR_HINT,
                       COL_WARN);
        snprintf(buf, sizeof(buf), "%lu MS/s",
                 (unsigned long)(PK_RTLSDR_SAMPLERATE_HZ / 1000000UL));
        det_kv_tr(fb, line++, PK_TR_DIAG_K_SAMPLE_RATE, buf, COL_VAL);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.msgs_total);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_ADSB_MSGS, buf, COL_VAL);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.iq_drop_total);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_IQ_DROPPED, buf,
                  d.iq_drop_total ? COL_WARN : COL_VAL);
        break;
    }

    case 4:     /* BLE */
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_LINK,
                   ble_gatt_is_connected()   ? PK_TR_DIAG_V_BLE_CONN
                   : ble_gatt_is_advertising() ? PK_TR_DIAG_V_BLE_ADV
                                               : PK_TR_DIAG_V_BLE_IDLE,
                   ble_gatt_is_connected() ? COL_ONLINE : COL_OFFLINE);
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_PROTOCOL, PK_TR_DIAG_V_BLE_PROTO,
                   COL_VAL);
        break;

    case 5: {   /* LOG */
        uint32_t written = 0, dropped = 0;
        if (record_sink_file_stats(&written, &dropped)) {
            /* 两行分开说，谁都不冒充谁：
             *   当前后端 = record_sink_file_uses_sd()，**现在真在写的那个**；
             *   设置为   = pk_log_store_get()，用户在设置页选的那个。
             * 只显示前者会让人以为设置没生效（罩哥验收时的原话），只显示
             * 后者则是诊断页在说谎——日志文件明明写在别处。 */
            const bool on_sd   = record_sink_file_uses_sd();
            const bool want_sd = (pk_log_store_get() == PK_LOG_STORE_SD);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_BACKEND,
                      log_store_text(on_sd), COL_VAL);
            if (want_sd == on_sd) {
                det_kv_tr(fb, line++, PK_TR_DIAG_K_SETTING,
                          log_store_text(want_sd), COL_VAL);
            } else {
                /* 差异态才标琥珀并写明何时生效：file sink 只在创建时读一次
                 * 这个设置，所以要等重启（见 config_storage.h 的说明）。 */
                snprintf(buf, sizeof(buf), "%s  (%s)", log_store_text(want_sd),
                         pk_i18n_text(PK_TR_DIAG_V_AFTER_RESTART));
                det_kv_tr(fb, line++, PK_TR_DIAG_K_SETTING, buf, COL_WARN);
            }
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)written);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_WRITTEN, buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)dropped);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_DROPPED, buf,
                      dropped ? COL_WARN : COL_VAL);
        } else {
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_SINK, PK_TR_DIAG_V_DOWN,
                       COL_ALERT);
        }
        break;
    }

    case 6:     /* CLK */
        fmt_clock(buf, sizeof(buf));
        det_kv_tr(fb, line++, PK_TR_DIAG_K_TIME, buf, COL_VAL);
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_SYNCED,
                   pk_clock_is_synced() ? PK_TR_DIAG_V_YES : PK_TR_DIAG_V_NO,
                   pk_clock_is_synced() ? COL_ONLINE : COL_WARN);
        /* 未校时就说明在等谁校（pk_clock 的两条来源：手机 BLE / GPS）。
         * 只写一个「否」，用户不知道这是坏了还是还没轮到。 */
        if (!pk_clock_is_synced())
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_HINT, PK_TR_DIAG_V_CLK_HINT,
                       COL_WARN);
        break;

    case 7: {   /* SYS */
        int temp_c = 0;
        pk_soc_temp_get(&temp_c);
        snprintf(buf, sizeof(buf), "%d C", temp_c);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_SOC_TEMP, buf,
                  temp_c >= 75 ? COL_WARN : COL_VAL);
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(buf, sizeof(buf), "%luh %lum %lus",
                 (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60),
                 (unsigned long)(sec % 60));
        det_kv_tr(fb, line++, PK_TR_DIAG_CARD_UPTIME, buf, COL_VAL);
        const esp_reset_reason_t rr = esp_reset_reason();
        det_kv_tr(fb, line++, PK_TR_DIAG_K_LAST_RESET, reset_reason_text(rr),
                  (rr == ESP_RST_POWERON) ? COL_VAL : COL_WARN);
        break;
    }

    case 8: {   /* microSD */
        uint64_t total = 0, free_b = 0;
        const bool mounted = (pk_sdcard_state() == PK_SD_MOUNTED);
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_STATE,
                   mounted ? PK_TR_DIAG_V_SD_MOUNTED : PK_TR_DIAG_V_SD_NO_CARD,
                   mounted ? COL_ONLINE : COL_ALERT);
        if (mounted && pk_sdcard_info(&total, &free_b)) {
            snprintf(buf, sizeof(buf), "%.1f GB",
                     (double)total / (1024.0 * 1024.0 * 1024.0));
            det_kv_tr(fb, line++, PK_TR_DIAG_K_CAPACITY, buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%.1f GB",
                     (double)free_b / (1024.0 * 1024.0 * 1024.0));
            det_kv_tr(fb, line++, PK_TR_DIAG_K_FREE, buf, COL_VAL);
        } else if (!mounted) {
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_HINT, PK_TR_DIAG_V_SD_HINT,
                       COL_WARN);
        }
        break;
    }

    case 9:     /* QNH */
        snprintf(buf, sizeof(buf), "%.2f hPa", (double)pk_qnh_get());
        det_kv_tr(fb, line++, PK_TR_DIAG_CARD_QNH, buf, COL_VAL);
        /* QNH 是**用户设定值**（设置页步进器 / NVS 持久化），不是 BMP388
         * 测出来的。写清楚来源是有意义的：读数不对时该去改设置，而不是
         * 怀疑气压计坏了。1013.25 是标准大气压，也是未设定时的默认。 */
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_SOURCE, PK_TR_DIAG_V_QNH_SOURCE,
                   COL_VAL);
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_USED_BY, PK_TR_DIAG_V_QNH_USED_BY,
                   COL_OFFLINE);
        break;

    case 10: {  /* BATT */
        pk_batt_t b;
        pk_batt_get(&b);
        if (b.valid) {
            snprintf(buf, sizeof(buf), "%d %%", b.pct);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_CHARGE, buf,
                      b.pct >= 20 ? COL_ONLINE : COL_WARN);
            snprintf(buf, sizeof(buf), "%.3f V", b.batt_mv / 1000.0);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_VOLTAGE, buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%d mV", b.raw_mv);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_ADC_RAW, buf, COL_OFFLINE);
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_CHARGING,
                       b.charging ? PK_TR_DIAG_V_YES : PK_TR_DIAG_V_NO,
                       b.charging ? COL_ONLINE : COL_VAL);
            /* 这块板的电池只接充电通路、没有 power path：拔掉 USB 是彻底
             * 断电再上电（实测复位原因为 power-on，不是 brownout）。这一行
             * 是给排查者的，不是给飞行员的——但它能省掉一轮"为什么会重启"。 */
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_ON_UNPLUG,
                       PK_TR_DIAG_V_ON_UNPLUG, COL_WARN);
        } else {
            det_kv_tr2(fb, line++, PK_TR_DIAG_CARD_BATT,
                       PK_TR_DIAG_V_NOT_DETECTED, COL_ALERT);
        }
        break;
    }

    case 11: {  /* UPTIME */
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        snprintf(buf, sizeof(buf), "%luh %lum %lus",
                 (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60),
                 (unsigned long)(sec % 60));
        det_kv_tr(fb, line++, PK_TR_DIAG_K_SINCE_BOOT, buf, COL_VAL);
        snprintf(buf, sizeof(buf), "%lu s", (unsigned long)sec);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_SECONDS, buf, COL_OFFLINE);
        break;
    }

    default:
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_DETAIL, PK_TR_DIAG_V_NO_FURTHER,
                   COL_OFFLINE);
        break;
    }
}

/*
 * 详情页的标题条。
 *
 * **顶栏不归这里管**——pk_ui_nav_set_subpage() 会显示一条 LVGL 的
 * 「← DIAGNOSTICS」backbar 并 move_foreground，自己再画一条只会被它盖住，
 * 或者两条叠在一起。上一版就是那么写的。
 *
 * 这里只在内容区顶部标一行当前子系统名：backbar 说的是"返回去哪"，这行说的
 * 是"现在看的是谁"，两句话不重复。
 */
static void draw_detail_header(uint16_t *fb, int which)
{
    /*
     * 子系统名是**第二行**，接在 backbar 下面（DET_TITLE_TOP）。
     *
     * 中间试过把它塞进顶栏（0..PFD_BAR_BOT）——那是为了填掉 backbar 上方空
     * 出来的一条黑带，可代价是"返回"被挤到第二行。真正的病根是 backbar 没在
     * 最顶上，改 PK_UI_BACKBAR_TOP 才治本，把标题往空处搬只是把洞挪个地方。
     *
     * 左缘用 DET_KEY_X：这一行归详情页的内容列，跟下面的键列对齐。两者现在
     * 都等于 PK_UI_PAD_L，与总览页卡片边距也在同一条线上——早先 DET_KEY_X=24
     * 而 CARD_PAD=16，下钻前后左边界会跳 8 px。
     */
    /* 与总览标题同字号同色：详情是总览的第二层，标题层级不该在下钻后变。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, DET_KEY_X, DET_TITLE_TOP,
               card_title(which), PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
}

/*
 * 退出子系统详情，回到诊断总览。
 *
 * 三条退路共用它：顶栏返回区、FAB（导航层的 ← ）、右滑手势。退出时必须把
 * 导航层的子页状态一起清掉，否则 FAB 会一直停在 ← 的样子——状态分两处各记
 * 一份就会这样，所以只留这一个出口。
 */
void pk_diag_page_leave_detail(void)
{
    s_detail = -1;
    pk_ui_nav_set_subpage(false, NULL);
}

/* 当前是否在子系统详情里。pk_ui_nav_on_back() 据此决定是"退出详情"还是
 * "切回诊断页"——两者在诊断页上下文里是不同的动作。 */
bool pk_diag_page_in_detail(void) { return s_detail >= 0; }
