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

int pk_diag_card_index_for_cell(int row, int col)
{
    if (row < 0 || col < 0 || col >= PK_DIAG_CARD_COLS) return -1;
    const int idx = row * PK_DIAG_CARD_COLS + col;
    return idx < PK_DIAG_CARD_N ? idx : -1;
}

pk_diag_airband_action_t pk_diag_airband_action_at(int x, int content_y)
{
    const int right_x0 = PK_DIAG_AIRBAND_BTN_X0 + PK_DIAG_AIRBAND_BTN_W +
                         PK_DIAG_AIRBAND_BTN_GAP;
    const bool left = x >= PK_DIAG_AIRBAND_BTN_X0 &&
                      x < PK_DIAG_AIRBAND_BTN_X0 + PK_DIAG_AIRBAND_BTN_W;
    const bool right = x >= right_x0 &&
                       x < right_x0 + PK_DIAG_AIRBAND_BTN_W;
    if ((!left && !right) || content_y < 0 ||
        content_y % PK_DIAG_DETAIL_LINE_H >= 34) {
        return PK_DIAG_AIRBAND_ACT_NONE;
    }
    const int content_line = content_y / PK_DIAG_DETAIL_LINE_H;
    switch (content_line) {
    case 4:
        return left ? PK_DIAG_AIRBAND_ACT_FREQ_GUARD
                    : PK_DIAG_AIRBAND_ACT_FREQ_AIR_AIR;
    case 5:
        return left ? PK_DIAG_AIRBAND_ACT_SCAN_TOGGLE
                    : PK_DIAG_AIRBAND_ACT_SQUELCH_TOGGLE;
    case 6:
        return left ? PK_DIAG_AIRBAND_ACT_SQUELCH_DEC
                    : PK_DIAG_AIRBAND_ACT_SQUELCH_INC;
    case 7:
        return left ? PK_DIAG_AIRBAND_ACT_SPEAKER_TEST
                    : PK_DIAG_AIRBAND_ACT_NONE;
    default:
        return PK_DIAG_AIRBAND_ACT_NONE;
    }
}

bool pk_diag_airband_make_command(pk_diag_airband_action_t action,
                                  const pk_diag_airband_control_state_t *state,
                                  pk_diag_airband_command_t *out)
{
    if (state == NULL || out == NULL) return false;
    *out = (pk_diag_airband_command_t){0};
    switch (action) {
    case PK_DIAG_AIRBAND_ACT_FREQ_GUARD:
        out->kind = PK_DIAG_AIRBAND_CMD_SET_FREQUENCY;
        out->frequency_hz = 121500000u;
        return true;
    case PK_DIAG_AIRBAND_ACT_FREQ_AIR_AIR:
        out->kind = PK_DIAG_AIRBAND_CMD_SET_FREQUENCY;
        out->frequency_hz = 123450000u;
        return true;
    case PK_DIAG_AIRBAND_ACT_SCAN_TOGGLE:
        if (state->scanning) {
            out->kind = PK_DIAG_AIRBAND_CMD_STOP_SCAN;
        } else {
            out->kind = PK_DIAG_AIRBAND_CMD_START_SCAN;
            out->scan_start_hz = 118000000u;
            out->scan_end_hz = 137000000u;
            out->scan_step_hz = 25000u;
            out->scan_dwell_ms = 250u;
        }
        return true;
    case PK_DIAG_AIRBAND_ACT_SQUELCH_TOGGLE:
        out->kind = PK_DIAG_AIRBAND_CMD_SET_SQUELCH;
        out->squelch_enabled = !state->squelch_enabled;
        out->squelch_open_dbm_x2 = state->squelch_open_dbm_x2;
        return true;
    case PK_DIAG_AIRBAND_ACT_SQUELCH_DEC:
    case PK_DIAG_AIRBAND_ACT_SQUELCH_INC: {
        int threshold = state->squelch_open_dbm_x2;
        threshold += action == PK_DIAG_AIRBAND_ACT_SQUELCH_INC ? 4 : -4;
        if (threshold < -240) threshold = -240;
        if (threshold > -80) threshold = -80;
        out->kind = PK_DIAG_AIRBAND_CMD_SET_SQUELCH;
        out->squelch_enabled = state->squelch_enabled;
        out->squelch_open_dbm_x2 = (int16_t)threshold;
        return true;
    }
    case PK_DIAG_AIRBAND_ACT_SPEAKER_TEST:
        out->kind = PK_DIAG_AIRBAND_CMD_SPEAKER_TEST;
        return true;
    case PK_DIAG_AIRBAND_ACT_NONE:
    default:
        return false;
    }
}

#ifndef PK_DIAG_PAGE_HOST_TEST
#ifndef PK_SIM_BUILD
#include "esp_log.h"
#endif
#include "esp_system.h"
#include "esp_timer.h"

#include "display.h"
#include "imu_task.h"        /* pk_imu_sample_get, pk_imu_sample_t */
#include "modes_ingest.h"    /* pk_dsp_get_stats, pk_dsp_stats_t */
#include "adsb_link_task.h"  /* pk_adsb_link_state_get */
#include "adsb_link.h"       /* HEALTH_STATS UAT RF diagnostic fields */
#include "gps.h"             /* pk_gps_get, pk_gps_state_t */
#include "gps_antenna_diag.h"/* raw GPTXT 与不会误报的解释结果 */
#include "config_antenna.h"  /* GNSS 当前请求的天线来源 */
#include "baro.h"            /* pk_baro_get, pk_baro_state_t */
#include "config_qnh.h"      /* pk_qnh_get — baro 高度的 QNH 基准 */
#include "config_storage.h"  /* pk_log_store_get — 用户希望写哪个后端 */
#include "i18n.h"            /* pk_i18n_text — 页面文案随语言切换 */
#include "ui_state.h"        /* pk_ui_cal_jammed — 磁环境是否被判为强磁干扰 */
#include "ble_gatt.h"        /* ble_gatt_is_connected, ble_gatt_is_advertising */
#include "pk_clock.h"        /* pk_clock_is_synced / pk_clock_source */
#include "pk_sdcard.h"       /* pk_sdcard_state / pk_sdcard_info */
#include "pk_aero_db.h"      /* pk_aero_db_status_get — SD 航空库状态 */
#include "pk_win.h"          /* pk_win_status_get — 滚动窗口运行状态 */
#include "airband.h"        /* BK4819 + PCM1808 最近 1 s 闭环快照 */
#include "airband_record.h"
#include "record_sink.h"     /* record_sink_file_stats / _uses_sd */
#include "pk_rec_store.h"    /* pk_rec_store_get_health —— LOG 卡片接入 SD 写失败/降级持续状态 */
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifndef PK_SIM_BUILD
static const char *TAG = "diag";
#endif

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
#include "power_service.h"   /* power_service_snapshot — 电源聚合快照（WP-D） */
#include "power_sy6970.h"    /* sy6970_diag_get — v4 powered 充电芯片取证（WP-D） */
#include "soc_temp.h"

#define CARD_COLS   PK_DIAG_CARD_COLS
#define CARD_PAD    PK_UI_PAD_L   /* 卡片外边距 = 整页左边距（pfd_layout.h） */
#define CARD_GAP    12
#define CARD_W      ((PK_DISPLAY_W - CARD_PAD * 2 - CARD_GAP) / CARD_COLS)
#define CARD_TOP    (PFD_BAR_BOT + 8)
/* 详情页版面（排版意图见下方 draw_detail 之前那段注释）。放在这里而不是跟
 * draw_detail 挨着，是因为触摸侧的 pk_diag_page_drag() 要用 DET_TOP/DET_LINE_H
 * 算详情页的可滚范围，而那个函数在文件更上面。 */
#define DET_KEY_X     PK_UI_PAD_L
#define DET_TITLE_TOP (PK_UI_BACKBAR_BOT + 8)
#define DET_TOP       (DET_TITLE_TOP + PK_AA_M_H + 10)
#define DET_LINE_H    PK_DIAG_DETAIL_LINE_H
#define DET_VAL_X     240
#define CARD_H      ((PK_DISPLAY_H - CARD_TOP - CARD_PAD + CARD_GAP) \
                     / 4 - CARD_GAP)

/* 状态灯：一格一个圆点，颜色即结论。绿=正常、琥珀=连着但不正常、
 * 红=不可用、灰=不适用/未启用。四种颜色与 PFD、交通页、列表页一致。 */
/*
 * 三态，没有"灰色"。
 *
 * 原先有个 ST_BAD 用灰色表示"不适用/未装"，于是 GPS 没插模块显示灰色
 * "no module"，而别的模块离线是红色——同样是"这个功能现在用不了"，却给了
 * 两种视觉权重，扫一眼分不清哪些是真该管的。评审要求统一：
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
 * 而真正的告警反而被稀释了。评审指出后加回 ST_IDLE。
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

/* 详情页的滚动状态。与总览的 s_scroll_y/s_card_rows 一对一对应：内容比一屏多
 * 就能滚，内容高度由上一帧实际画了几行现算（详情行数随状态伸缩，写死会对不
 * 上）。进入/退出详情时归零，否则下一个子系统会莫名其妙停在半空。 */
static int s_detail_scroll_y;
static int s_detail_lines;

/* 卡片序号 → 标题。顺序必须与 render 里的 draw_card 调用一致——两处各写
 * 一份序号迟早会错位，所以这张表是唯一的真值来源，render 也从它取标题。
 *
 * 存的是词条 id 而不是字符串：语言可以在运行时切，取到的必须是**当前**语言
 * 的写法。存字符串就意味着切语言后这张表还停在旧语言上。 */
typedef struct {
    bool localized;
    pk_tr_id_t tr;
    const char *literal;
} diag_card_title_t;

static const diag_card_title_t CARD_TITLE[] = {
    {true, PK_TR_DIAG_CARD_IMU, NULL},
    {true, PK_TR_DIAG_CARD_BARO, NULL},
    {true, PK_TR_DIAG_CARD_GPS, NULL},
    {true, PK_TR_DIAG_CARD_SDR, NULL},
    {true, PK_TR_DIAG_CARD_BLE, NULL},
    {true, PK_TR_DIAG_CARD_LOG, NULL},
    {true, PK_TR_DIAG_CARD_CLK, NULL},
    {true, PK_TR_DIAG_CARD_SYS, NULL},
    {true, PK_TR_DIAG_CARD_SD, NULL},
    {true, PK_TR_DIAG_CARD_QNH, NULL},
    {true, PK_TR_DIAG_CARD_BATT, NULL},
    {true, PK_TR_DIAG_CARD_UPTIME, NULL},
    {false, PK_TR_DIAG_TITLE, "AERO DB"},
    {false, PK_TR_DIAG_TITLE, "WINDOW"},
    {true, PK_TR_APTD_AIRBAND, NULL},
};
#define CARD_N  ((int)(sizeof(CARD_TITLE) / sizeof(CARD_TITLE[0])))
_Static_assert(CARD_N == PK_DIAG_CARD_N,
               "诊断卡表数量与纯命中模型不一致");

/* 卡片标题的当前语言写法。总览与详情页共用，两处不各查一次表。 */
static const char *card_title(int idx)
{
    if (idx < 0 || idx >= CARD_N) return pk_i18n_text(PK_TR_DIAG_K_DETAIL);
    return CARD_TITLE[idx].localized ? pk_i18n_text(CARD_TITLE[idx].tr)
                                     : CARD_TITLE[idx].literal;
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

/*
 * SY6970 在位判据 —— powered variant 电池呈现分支的唯一开关。
 *
 * 判「diag 拿得到数据且 updated_us 距今 <5 s」，刻意**不是**：
 *   - pk_board_profile()：板型只说明"出厂可能带"，合同禁止拿它门控运行期
 *     电源语义（pk_board.h:30-32）——powered 板也可能没上电，SY6970 探测
 *     NACK 是预期路径（power_sy6970.c 的 init 注释）；
 *   - snapshot.source：SY6970 电池放电时同样报 POWER_SRC_BATTERY，分不出
 *     两代载板（power_service.h:45-47 的档位语义）。
 * 阈值复用 POWER_SERVICE_STALE_US：与服务的 stale 判定同口径，diag 新鲜
 * 而快照过期不可能出现（同一拍更新），万一出现由下面的 stale 分支兜底。
 * 从未报数（探测 NACK / bring-up 未成功）sy6970_diag_get() 返回 false，
 * 自然走 ETA6098 分支——v3 与未上电的 v4 都落在这里。
 */
static bool sy6970_live(sy6970_diag_t *out)
{
    if (out == NULL || !sy6970_diag_get(out)) return false;
    return (esp_timer_get_time() - out->updated_us) < POWER_SERVICE_STALE_US;
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
    /*
     * 可用宽度还要**避开 FAB**。
     *
     * FAB 是浮在这一层之上的 LVGL 控件，被它盖住的字等于没画。以前撞不到是
     * 因为右列最后一格恰好是空的；WINDOW 卡片补上那一格之后，中文最糟版面
     * 的末位数字正好被那个蓝圆吃掉一半（sim 截图 win-zh-warn 第一版）。
     * 地图页的缩放钮当年也是没避让才被压住的（map_page.c:82 那段）。
     *
     * 问 pk_ui_nav_fab_rect() 要真实矩形而不是抄一组坐标：FAB 可以被用户拖到
     * 另一侧或另一个高度，抄下来的常量第一次拖动就失效。相交才收窄，不相交
     * 时这一格的可用宽度一点没变（= 原来的 CARD_W - 26）。
     */
    int right = x + CARD_W;
    int fx, fy, fw, fh;
    if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh) &&
        fy < y + CARD_H && fy + fh > y && fx > x && fx < right)
        right = fx;

    const int avail = right - (x + 18) - 8;
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
    /* PK_SIM_DIAG_DETAIL_SCROLL=<px> 把**详情页**滚到指定位置。理由同上：
     * 详情页底部被截掉的那几行，不给这个旋钮就永远截不到图。 */
    { const char *e = getenv("PK_SIM_DIAG_DETAIL_SCROLL");
      if (e) s_detail_scroll_y = atoi(e); }
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
            /*
             * 强磁干扰时**用磁环境顶掉航向那一格**，而不是再挤出第三格。
             *
             * 卡片值行只有 352 px（见 draw_card 里那笔宽度账），中文
             * 「校准 3/3   航向 123」已占 253 px，再追一个词英文侧就会整张卡
             * 降到 S 档——为一个瞬态结论把常态版面改小，代价给错了地方。
             *
             * 顶掉的偏偏是航向也不是凑合：磁场被拽偏的时候，磁航向正是那个
             * 不该照着信的数（下面那条注释说的"cal 0 时航向可以差几十度"，
             * 干扰态是同一件事的另一个成因）。把它换成成因本身，信息量只增
             * 不减，宽度还省了——真要看角度，详情页那一行原样还在。
             * 格数因此恒为二，卡片版面不随状态伸缩。
             */
            const bool jam = pk_ui_cal_jammed();
            if (jam) {
                snprintf(buf, sizeof(buf), "%s %u/3   %s",
                         pk_i18n_text(PK_TR_DIAG_U_CAL), s.accuracy,
                         pk_i18n_text(PK_TR_DIAG_V_MAG_JAM_S));
            } else {
                snprintf(buf, sizeof(buf), "%s %u/3   %s %.0f",
                         pk_i18n_text(PK_TR_DIAG_U_CAL), s.accuracy,
                         pk_i18n_text(PK_TR_DIAG_U_YAW), (double)s.yaw_deg);
            }
            /* 校准等级本身就是可信度：cal 0 时航向可以差几十度，数据却照样
             * "有效"——这正是最该在总览里就看见的。
             * 干扰态一律压成琥珀，不看当帧的 accuracy：干扰的定义就是这个数
             * 在反复跳，截图/扫一眼的那一拍它可能正好停在 3，绿灯配着「磁干扰」
             * 三个字自相矛盾。 */
            draw_card(fb, 0, 0, card_title(0), buf,
                      (!jam && s.accuracy >= 2) ? ST_OK : ST_WARN);
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
     * 有没有 fix 与看得见几颗星是两件事：sats 有数而 fix=0 说明信号弱/遮挡；
     * sats=0 还可能是冷启动、完全遮挡、天线不匹配或 RF 路径异常，不能单凭它
     * 判定物理断线。总览里两个数都给，省得点进详情才发现排查方向错了。 */
    {
        pk_gps_state_t g = {0};
        pk_gps_get(&g);
        pk_ant_gnss_status_t ant_cfg = {0};
        pk_ant_gnss_status_get(&ant_cfg);
        const int64_t now = esp_timer_get_time();
        const bool fresh = g.have_fix && (now - g.updated_us) < GPS_FRESH_US;

        /*
         * 五种情况必须分开说，它们指向完全不同的处理动作。
         *
         * 上一版只分了「有 fix / 看得见星 / 看不见星」，于是**根本没插 GPS
         * 板卡**也报成 "no sats - check antenna"——评审在真机上一眼看出与
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
        } else if (!ant_cfg.actual_known) {
            /* 请求已经落 NVS 不等于 RP2040 已经驱动 U17。ACK 前不展示旧 fix，
             * 否则用户刚点 INT 就会看到上一根 EXT 的定位证据。 */
            draw_card(fb, 0, 1, card_title(2),
                      ant_cfg.request_pending ? "GNSS source: pending ACK"
                                              : "GNSS source: unknown",
                      ST_WARN);
        } else if (fresh) {
            /* 有效定位优先于天线自检：ATGM336H-6N 对无源/某些有源天线会
             * 恒报 ANTENNA OPEN（手册 §2.8），若把 OPEN 画在 fix 之前，
             * 会出现"35 颗星、定位正常"却报红色天线开路（2026-09-10 实板）。
             * 数字与量词分开取：整句进 catalog 就等于把 snprintf 的格式串
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
        } else if (g.ant_status == PK_GPS_ANT_OPEN) {
            /* 原始 OPEN 只说明 VCC_RF 负载低于阈值。6N 手册 §2.8 明确说明
             * 无源天线恒报 OPEN；没有 RF 证据时只能提示，不能判硬件故障。 */
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_ANT_OPEN), ST_WARN);
        } else if (g.ant_status == PK_GPS_ANT_SHORT) {
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_ANT_SHORT), ST_BAD);
        } else {
            /* 模块在讲话、天线自检没报错、但一颗星都没看见：冷启动搜星中，
             * 或者被完全遮挡。这才是"再等等"，不该报成天线故障。 */
            draw_card(fb, 0, 1, card_title(2),
                      pk_i18n_text(PK_TR_DIAG_V_SEARCHING), ST_WARN);
        }
    }

    /* ── ADS-B 链路（RP2040 UART）── */
    {
        const pk_adsb_link_state_t st = pk_adsb_link_state_get(NULL);
        switch (st) {
        case PK_ADSB_LINK_NO_LINK:
            draw_card(fb, 1, 1, card_title(3),
                      pk_i18n_text(PK_TR_DIAG_V_LINK_NONE), ST_BAD);
            break;
        case PK_ADSB_LINK_PROTO_MISMATCH:
            draw_card(fb, 1, 1, card_title(3),
                      pk_i18n_text(PK_TR_DIAG_V_LINK_PROTO), ST_WARN);
            break;
        case PK_ADSB_LINK_STALLED:
            draw_card(fb, 1, 1, card_title(3),
                      pk_i18n_text(PK_TR_DIAG_V_LINK_STALLED), ST_WARN);
            break;
        default:    /* LINKED：报文在流动 */
            draw_card(fb, 1, 1, card_title(3),
                      pk_i18n_text(PK_TR_DIAG_V_LINK_LIVE), ST_OK);
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
     * 评审验收时点出："切到 SD 卡存储后诊断页仍显示 flash"——那不是 bug，
     * 是这一格只说了实际后端、没说还有个待生效的设置。诊断页的职责是说实话，
     * 所以两个都说：主体仍是实际后端（说假话会让排查者去 SD 上找不存在的
     * 文件），后面补一句「设为 X」把差异挑明。 */
    {
        uint32_t written = 0, dropped = 0;
        /* 阶段 5b：这一格再叠一路独立的写入管线——pk_rec_store（rec/ 目录
         * 的 session 持久化，SD 满/写失败降级那条）。它跟 record_sink_file
         * （"别人飞机"那条 GDL90 日志）是两套完全不同的写入器，但评审要求
         * "复用现有实现"，持续状态不新开卡片，接到这一格的值行末尾——
         * 反复弹同一个 toast 的结果是飞行员学会忽略它，诊断页才是"想看就
         * 有、不想看不用管"的常驻信息源（设计文档「告警呈现」节）。 */
        pk_rec_store_health_t rec_health;
        pk_rec_store_get_health(&rec_health);

        if (record_sink_file_stats(&written, &dropped)) {
            const bool on_sd = record_sink_file_uses_sd();
            const bool want_sd = (pk_log_store_get() == PK_LOG_STORE_SD);
            int p = snprintf(buf, sizeof(buf), "%s  %s %lu",
                             log_store_text(on_sd),
                             pk_i18n_text(PK_TR_DIAG_U_W),
                             (unsigned long)written);
            if (want_sd != on_sd && p > 0 && p < (int)sizeof(buf))
                p += snprintf(buf + p, sizeof(buf) - p, "  (%s %s)",
                              pk_i18n_text(PK_TR_DIAG_V_SET_TO),
                              log_store_text(want_sd));
            /* REC 后缀：只挑一个最要紧的说，不叠加——卡片这一行本来就已经
             * 塞了后端名 + 写入数，再加"降级档位 + 失效数"两段很容易在中文
             * 界面上把值行挤到 S 档还溢出相邻卡片（截图 ui-4.3-diag-rec-fail
             * 第一版就撞过这个）。失效 sink 比单纯降级更严重（数据已经在
             * 丢），优先说它；没有失效 sink 时才说降级档位；两者都正常
             * （FULL 且 0 失效）这段完全不出现。 */
            if (p > 0 && p < (int)sizeof(buf) && rec_health.disabled_count > 0) {
                snprintf(buf + p, sizeof(buf) - p, "  %s%u",
                         pk_i18n_text(PK_TR_DIAG_V_REC_FAIL),
                         (unsigned)rec_health.disabled_count);
            } else if (p > 0 && p < (int)sizeof(buf) &&
                       rec_health.tier != PK_REC_DEGRADE_FULL) {
                snprintf(buf + p, sizeof(buf) - p, "  %s",
                         pk_i18n_text(rec_health.tier == PK_REC_DEGRADE_OWN_ONLY
                                      ? PK_TR_DIAG_V_REC_OWNONLY
                                      : PK_TR_DIAG_V_REC_NORAW));
            }
            /* 丢过条目就是琥珀：还在写，但已经不完整了，跟"没在写"是两回事。
             * REC 失效 sink（写失败）比降级更严重——数据已经在丢，升到红；
             * 单纯降级（SD 空间紧张但还在写）维持琥珀。 */
            draw_card(fb, 1, 2, card_title(5), buf,
                      /* written == 0 只是"还没收到可写的数据"，不是故障——
                       * 刚开机、或者 SDR 没插时本来就一条都没有。sink 建起来
                       * 了就算正常；丢过条目才是琥珀（还在写但已不完整）。
                       * 设置与实际不一致也是琥珀：不是故障，但用户以为已经
                       * 换过去了，得让这一格自己喊一声。 */
                      rec_health.disabled_count > 0 ? ST_BAD
                      : dropped > 0 ? ST_WARN
                      : want_sd != on_sd ? ST_WARN
                      : rec_health.tier != PK_REC_DEGRADE_FULL ? ST_WARN
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
            /* No CD pin: after the finite probe budget, a late insertion
             * requires restart. Keep this distinct from a FAT/media error. */
            if (pk_sdcard_probe_exhausted()) {
                snprintf(buf, sizeof(buf), "%s / %s",
                         pk_i18n_text(PK_TR_DIAG_V_SD_NO_CARD),
                         pk_i18n_text(PK_TR_POWER_RESTART));
                draw_card(fb, 0, 4, card_title(8), buf, ST_WARN);
            } else {
                draw_card(fb, 0, 4, card_title(8),
                          pk_i18n_text(PK_TR_DIAG_V_SD_NO_CARD), ST_BAD);
            }
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
     * 两代载板的电源通路不同，这一格按 SY6970 在位（sy6970_live，见上）
     * 分两套呈现：
     *   v4 powered —— SY6970（I²C 0x6A）是权威源，卡片报它的充电状态、
     *     故障、VBUS 与充电电流；
     *   v3 / 未上电的 v4 —— 探测 NACK，回落 ETA6098 的分压采样，
     *     维持原来的 pct/V/CHG/raw 版面。
     * 无论哪套，都如实写"无数据"而不是显示 0% 或藏起来：藏起来会让人
     * 以为固件漏了，编个 0 出来则是说谎——而这一格的读者正想知道
     * 还能飞多久。 */
    {
        sy6970_diag_t d;
        if (sy6970_live(&d)) {
            const power_snapshot_t s = power_service_snapshot();
            /* 同源守卫（审计）：快照赢家不是 SY6970——典型是 SY6970 stale
             * 后服务端已回落 ETA6098——时，快照里的电量/电压就是另一个源
             * 的数据，并进本卡等于张冠李戴；此时只渲染诊断快照（寄存器
             * 译码）自己的字段。stale 闸也只对同源快照有意义：过期的是
             * 别人的数据，不该替 SY6970 的译码顶罪。 */
            const bool same_src = (s.backend == POWER_BACKEND_SY6970);
            if (same_src && s.stale) {
                /* 服务端 5 s 无新数据：宁可说过期，也不把旧数当实时。
                 * 常规路径到不了这里（diag 新鲜则快照同拍新鲜），摆出来
                 * 是把「数据过期就别信数字」这条规则钉死在这格上。 */
                draw_card(fb, 0, 5, card_title(10),
                          pk_i18n_text(PK_TR_DIAG_V_PWR_STALE), ST_WARN);
            } else {
                const sy6970_status_t *st = &d.st;
                /* 电量/电压：仅同源时取公共快照（pct_valid 是服务端与
                 * ETA6098 同口径的量程闸）；不同源时电压取 SY6970 自己的
                 * BATV 译码、不显示电量（本层不出 pct 的合同不变）。
                 * 故障/电流/VBUS 恒取诊断快照（锁内整体拷贝，线程合同
                 * 见 power_sy6970.h）。 */
                int p;
                if (same_src) {
                    p = s.pct_valid
                        ? snprintf(buf, sizeof(buf), "%u%% %.2fV",
                                   (unsigned)s.pct_est, (double)s.batt_mv / 1000.0)
                        : snprintf(buf, sizeof(buf), "%.2fV",
                                   (double)s.batt_mv / 1000.0);
                } else {
                    p = snprintf(buf, sizeof(buf), "%.2fV",
                                 (double)st->batt_mv / 1000.0);
                }
                /* 故障优先级：看门狗故障意味着寄存器被整体打回默认（关狗
                 * 写被清掉，F1 的前提没了），比一次充电故障更要紧；CHRG_
                 * FAULT 的 01/10/11 各指向不同的排查方向（适配器/散热/
                 * 电池），分词说清（[DS] p.22-23）。 */
                const char *fault =
                    st->wd_fault           ? pk_i18n_text(PK_TR_DIAG_V_PWR_WDFAULT)   :
                    st->chrg_fault == 1    ? pk_i18n_text(PK_TR_DIAG_V_PWR_CHG_IN)    :
                    st->chrg_fault == 2    ? pk_i18n_text(PK_TR_DIAG_V_PWR_CHG_THERM) :
                    st->chrg_fault == 3    ? pk_i18n_text(PK_TR_DIAG_V_PWR_CHG_TIMER) :
                    st->bat_ovp_fault      ? pk_i18n_text(PK_TR_DIAG_V_PWR_BATOVP)    :
                    st->boost_fault        ? pk_i18n_text(PK_TR_DIAG_V_PWR_BOOST)     : NULL;
                if (fault && p > 0 && p < (int)sizeof(buf))
                    p = snprintf(buf + p, sizeof(buf) - p, " %s", fault);
                /* 这三个数取代了 ETA6098 分支的 raw-mV 标定槽：SY6970 自己
                 * 的 BUSV/ICHGR ADC 就是供电证据（[DS] p.24），不借 ETA6098
                 * 的分压读数（v4 上那是另一条网络，指不上）。CHG 与电流只
                 * 在真充电时给——充满维持时电流≈0，摆出来只会让人疑惑。 */
                if (!fault && st->charging && p > 0 && p < (int)sizeof(buf))
                    p = snprintf(buf + p, sizeof(buf) - p, " CHG");
                if (st->vbus_present && p > 0 && p < (int)sizeof(buf))
                    p = snprintf(buf + p, sizeof(buf) - p, " %.1fV",
                                 (double)st->vbus_mv / 1000.0);
                if (!fault && st->charging && p > 0 && p < (int)sizeof(buf))
                    p = snprintf(buf + p, sizeof(buf) - p, " %.1fA",
                                 (double)st->ichg_ma / 1000.0);
                /* 状态灯：REG0C 任一硬故障即红；插着电（充电或充满维持）
                 * 绿；纯电池放电沿用 20% 琥珀线——还够飞就不打扰（琥珀线
                 * 依据的是公共快照的电量，仅同源时有意义）。 */
                draw_card(fb, 0, 5, card_title(10), buf,
                          fault ? ST_BAD
                          : (st->charging || st->vbus_present) ? ST_OK
                          : (same_src && s.pct_valid && s.pct_est < 20) ? ST_WARN : ST_OK);
            }
        } else {
            /* SY6970 不是快照赢家 = 这块板上没有可用的电源数据源。
             *
             * 2026-09-12 之前这里画的是 ETA6098 分压采样（v3 载板 / 未上电
             * 的 v4）。v3 支持已取消、ETA6098 整体退役，于是这条分支的含义
             * 收敛成单一情况：**SY6970 探测 NACK 或掉线超过 5 s**。
             * 不再编造任何读数——没有源就如实说没有。 */
            draw_card(fb, 0, 5, card_title(10),
                      pk_i18n_text(PK_TR_DIAG_V_NOT_DETECTED), ST_BAD);
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
     * 序号 12 已纳入统一卡表；当前进入详情后明确显示“无进一步信息”，不会再
     * 因 CARD_N 落后于实际卡数而让后续 AIRBAND 卡永远不可达。 */
    {
        pk_aero_db_status_t a;
        pk_aero_db_status_get(&a);
        card_state_t st;
        switch (a.state) {
        case PK_AERO_DB_READY:
            /* 带上 bin 版本：用户换卡后靠它确认新库生效（v2 上搜索索引
             * 缺席，按名/按码搜索是空的；v3 上空域/航路段缺席，地图叠加层
             * 是空的——都不是坏了，是数据旧）。
             *
             * 空域/航路条数**不上卡**：这一行的宽度只够"周期 + 版本 + 一个
             * 计数"，再挂两个数就会掉到 S 档还压到隔壁卡（现有的导航台/FIX
             * 计数同理也没显示）。而"v4 生效没有"这个问题版本号本身就答完了
             * ——v4 容器必然带这两段。要看具体条数去串口：READY 时
             * pk_aero_db.c 会打一行 "airspace/airway: … airspaces … legs"。 */
            snprintf(buf, sizeof(buf), "%s v%u  %lu apt",
                     a.cycle, (unsigned)a.version,
                     (unsigned long)a.n_airports);
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
        draw_card(fb, 0, 6, card_title(12), buf, st);
    }

    /* ── WINDOW ──
     * pk_win 滚动窗口（以本机为中心的增量加载）的运行状态。跟在 AERO DB 旁边
     * 是有道理的：那一格说的是"全量库加载完没有"，这一格说的是"窗口这条独立
     * 读路现在驻留了多少"，两者是同一份 bin 上的两条路。
     *
     * 追加在网格末尾、不往中间插——理由同 AERO DB 那格：卡片序号就是触摸分派
     * 与 CARD_TITLE 的下标，插中间会错位后面所有卡的详情映射。
     * 序号 13 已纳入统一卡表；当前详情使用通用“无进一步信息”占位。
     *
     * ── 字段取舍 ────────────────────────────────────────────────────
     * 值行只有 352 px（CARD_W - 26），装不下 pk_win_status_t 的十几个指标，
     * 所以只留**看一眼就能下结论**的那两个 + 一个异常后缀：
     *
     *   驻留格数 / 驻留字节  → 主行。它们回答"窗口到底在不在干活、吃了多少
     *                          PSRAM"，是这张卡存在的第一理由；两个数任何一个
     *                          长期是 0，就说明这条读路没跑起来。
     *   str_skipped / forced → 只在非零时以后缀出现，且**只说更严重的那个**。
     *                          skip 意味着那一格的名字确实缺了（回落到
     *                          pk_aero_db 老路径），forced 只是抢了一次卡、
     *                          数据是全的——所以 skip 优先。取舍写法照 LOG 卡。
     *   yields               → **不上卡**。它在稳态下是个一直在涨的大数（每
     *                          100 ms 让一次就 +1），摆在屏上像是有什么东西在
     *                          反复失败，实际上"让路成功"才是设计意图。这类
     *                          需要看趋势、不需要看瞬时值的量归串口日志
     *                          （pk_win.c 的 "status ... yields=N"）。
     *                          microSD 卡当年那个 retry 计数就是这么被撤下去的。
     *   loads/evicts/psram/  → 同理归日志，卡片放不下也不该放。
     *   in/out/last_load_us
     */
    {
        pk_win_status_t w;
        pk_win_status_get(&w);
        card_state_t st;
        if (!w.enabled) {
            /* 编译开关关掉了（PK_WIN_ENABLE=0 的回滚态）。不是故障，是没开。 */
            snprintf(buf, sizeof(buf), "%s",
                     pk_i18n_text(PK_TR_DIAG_V_WIN_OFF));
            st = ST_IDLE;
        } else if (!w.open) {
            /* 卡上没有 aero bin / 没插卡。数据库是可选内容包，灰而不红——
             * 与 AERO DB 那格的空态口径保持一致。 */
            snprintf(buf, sizeof(buf), "%s",
                     pk_i18n_text(PK_TR_DIAG_V_WIN_NODATA));
            st = ST_IDLE;
        } else if (w.n_ready == 0) {
            /* 句柄开着但一格都没就绪：开机首填中，或者还没拿到本机位置
             * （没 GPS fix 时 resolve_shape 直接返回，窗口无中心可用）。 */
            snprintf(buf, sizeof(buf), "%s",
                     pk_i18n_text(PK_TR_DIAG_V_WIN_FILL));
            st = ST_WARN;
        } else {
            /* 字节按量级换单位：驻留量从几百 KB 到十几 MB 都可能，固定用 MB
             * 会在稀疏区显示成 "0.0 MB"（看起来像没加载），固定用 KB 又在密集
             * 区变成七位数。 */
            int p;
            if (w.bytes >= 1024 * 1024)
                p = snprintf(buf, sizeof(buf), "%u %s  %.1f MB",
                             (unsigned)w.n_ready,
                             pk_i18n_text(PK_TR_DIAG_U_CELLS),
                             (double)w.bytes / (1024.0 * 1024.0));
            else
                p = snprintf(buf, sizeof(buf), "%u %s  %lu KB",
                             (unsigned)w.n_ready,
                             pk_i18n_text(PK_TR_DIAG_U_CELLS),
                             (unsigned long)(w.bytes / 1024));

            if (p > 0 && p < (int)sizeof(buf) && w.str_skipped > 0) {
                snprintf(buf + p, sizeof(buf) - p, "  %s %lu",
                         pk_i18n_text(PK_TR_DIAG_V_WIN_SKIP),
                         (unsigned long)w.str_skipped);
                st = ST_WARN;
            } else if (p > 0 && p < (int)sizeof(buf) && w.forced > 0) {
                snprintf(buf + p, sizeof(buf) - p, "  %s %lu",
                         pk_i18n_text(PK_TR_DIAG_V_WIN_FORCED),
                         (unsigned long)w.forced);
                st = ST_WARN;
            } else {
                st = ST_OK;
            }
        }
        draw_card(fb, 1, 6, card_title(13), buf, st);
    }

    /* ── AIRBAND ──
     * 这张卡只判断硬件闭环，不拿“有没有收到航空话音”当健康判据：室内无台时
     * RSSI/噪声底与 PCM1808 帧仍应持续变化。R/N/G 是 BK4819 原始寄存器，
     * A 是本秒收到的 I2S 帧数；完整 dBm、极值、RMS 和读错计数保留在串口。 */
    {
        pk_airband_state_t a;
        card_state_t st = ST_BAD;
        if (!pk_airband_get(&a)) {
            snprintf(buf, sizeof(buf), "no data");
        } else {
            snprintf(buf, sizeof(buf), "R%u N%u G%u A%luk",
                     (unsigned)a.rf.rssi_raw, (unsigned)a.rf.noise_raw,
                     (unsigned)a.rf.glitch_raw,
                     (unsigned long)(a.audio_frames / 1000u));
            st = a.health == PK_AIRBAND_HEALTH_READY ? ST_OK
               : a.health == PK_AIRBAND_HEALTH_DEGRADED ||
                 a.health == PK_AIRBAND_HEALTH_AUDIO_IDLE ? ST_WARN
               : ST_BAD;
        }
        draw_card(fb, 0, 7, card_title(PK_DIAG_CARD_AIRBAND), buf, st);
    }

    s_card_rows = 8;

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
     * 里唯一的蓝标题——评审在真机上第一眼就点出来了。 */
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
static pk_diag_airband_action_t s_airband_press;

#define DIAG_DRAG_SLOP  12

static void airband_apply_action(pk_diag_airband_action_t action)
{
    pk_airband_control_status_t current;
    if (!pk_airband_control_get(&current)) return;
    const pk_diag_airband_control_state_t state = {
        .scanning = current.scanning,
        .squelch_enabled = current.squelch_enabled,
        .squelch_open_dbm_x2 = current.squelch_open_dbm_x2,
    };
    pk_diag_airband_command_t cmd;
    if (!pk_diag_airband_make_command(action, &state, &cmd)) return;

    switch (cmd.kind) {
    case PK_DIAG_AIRBAND_CMD_SET_FREQUENCY:
        (void)pk_airband_set_frequency(cmd.frequency_hz);
        break;
    case PK_DIAG_AIRBAND_CMD_START_SCAN: {
        const pk_airband_scan_plan_t plan = {
            .start_hz = cmd.scan_start_hz,
            .end_hz = cmd.scan_end_hz,
            .step_hz = cmd.scan_step_hz,
            .dwell_ms = cmd.scan_dwell_ms,
        };
        (void)pk_airband_scan_start(&plan);
        break;
    }
    case PK_DIAG_AIRBAND_CMD_STOP_SCAN:
        pk_airband_scan_stop();
        break;
    case PK_DIAG_AIRBAND_CMD_SET_SQUELCH:
        if (pk_airband_set_squelch(cmd.squelch_open_dbm_x2,
                                   current.squelch_hysteresis_db_x2)) {
            pk_airband_set_squelch_enabled(cmd.squelch_enabled);
        }
        break;
    case PK_DIAG_AIRBAND_CMD_SPEAKER_TEST:
#ifndef PK_SIM_BUILD
        ESP_LOGI(TAG, "AIRBAND diagnostic speaker button -> %s",
                 pk_airband_speaker_test() ? "accepted" : "rejected");
#else
        (void)pk_airband_speaker_test();
#endif
        break;
    case PK_DIAG_AIRBAND_CMD_NONE:
    default:
        break;
    }
}

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

        /* AIRBAND 是当前唯一有原生动作的诊断详情。按钮从 x=48 开始，左缘
         * 0..31 仍完整留给右滑返回；按下只锁定动作，真正写寄存器在松手时。 */
        if (s_detail == PK_DIAG_CARD_AIRBAND) {
            const pk_diag_airband_action_t action = pk_diag_airband_action_at(
                x, y + s_detail_scroll_y - DET_TOP);
            if (action != PK_DIAG_AIRBAND_ACT_NONE) {
                s_airband_press = action;
                s_press_valid = true;
                s_press_x = x;
                s_press_y = y;
                s_moved = false;
                return true;
            }
        }

        /* ⚠️ 内容不足一屏时必须**一个字节都不改**地走旧路径。
         *
         * 旧路径是 s_press_valid=false + 返回 true：这一下被吃掉（不穿透到
         * 底下总览的卡片），但续帧 pk_diag_page_drag() 第一行就返回 false，
         * 于是每一帧都漏给 LVGL——右滑返回（pk_ui_nav.c 的 backbar_gesture_cb）
         * 全靠这条活着。2026-09-13 我给详情页加滚动时把 s_press_valid 改成了
         * true，等于把这条退路掐了，实机上"返回坏了"。
         *
         * 所以接管触摸的前提是**真的有东西可滚**，而不是"进了详情页"。绝大多数
         * 详情页都装得下一屏，它们的行为因此与改动前完全一致。 */
        const int total_h = s_detail_lines * DET_LINE_H;
        const int view_h  = PK_DISPLAY_H - DET_TOP;
        /* 左缘那条带子同样走旧路径：右滑返回认的就是"从左缘起手"
         * （pk_ui_nav.c:131）。在这里提前放行，滚动就永远不会跟它抢第一帧，
         * 而不是等到 drag 里再靠 |dx|>|dy| 事后补救——那时第一帧已经被吃了。 */
        if (total_h <= view_h || x < PK_UI_PAD_L * 2) {
            s_press_valid = false;
            return (x < PK_DISPLAY_W - 80);
        }

        /* 确实超屏：记下起点接管滚动。滚动量按"相对按下时的位移"算，归属在
         * 按下这一刻定死、续帧不再重判（arbiter 的规矩，见 pk_touch_arbiter.h）。
         * 即便如此，横向为主的拖动仍会在 drag 里让出去给右滑返回。 */
        s_press_valid  = (x < PK_DISPLAY_W - 80);
        s_press_x      = x;
        s_press_y      = y;
        s_press_scroll = s_detail_scroll_y;
        s_moved        = false;
        return s_press_valid;
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

    if (s_airband_press != PK_DIAG_AIRBAND_ACT_NONE) {
        const int dx = x - s_press_x, dy = y - s_press_y;
        const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        if (adx > DIAG_DRAG_SLOP || ady > DIAG_DRAG_SLOP) {
            s_moved = true;
            s_airband_press = PK_DIAG_AIRBAND_ACT_NONE;
        }
        return true;
    }

    /* ── 详情页：纵向滚内容，横向让路 ─────────────────────────────────
     *
     * 右滑返回是详情页三条退路之一，实现在 pk_ui_nav.c 的 backbar_gesture_cb
     * （LVGL 手势，判据是"横向位移明显大于纵向"）。LVGL 只有在本函数返回
     * false、这一帧没被吃掉时才收得到——所以横向为主的拖动必须原样让出去，
     * 否则滚动一上线就把那条退路废了。两边的判据刻意互补。 */
    if (s_detail >= 0) {
        const int dx  = x - s_press_x;
        const int dy2 = y - s_press_y;
        const int adx = dx  < 0 ? -dx  : dx;
        const int ady = dy2 < 0 ? -dy2 : dy2;
        if (adx > ady) return false;             /* 横向为主 → 交给右滑返回 */

        const int total_h = s_detail_lines * DET_LINE_H;
        const int view_h  = PK_DISPLAY_H - DET_TOP;
        const int max_y   = (total_h > view_h) ? (total_h - view_h) : 0;
        /* 内容不足一屏，没什么可滚的：同样让出去，右滑返回更跟手。 */
        if (max_y == 0) return false;

        if (!s_moved && ady > DIAG_DRAG_SLOP) s_moved = true;
        if (!s_moved) return true;

        int sy = s_press_scroll - dy2;
        if (sy < 0)     sy = 0;
        if (sy > max_y) sy = max_y;
        s_detail_scroll_y = sy;
        return true;
    }

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
    const pk_diag_airband_action_t airband_action = s_airband_press;
    const int  x = s_press_x, y = s_press_y;
    s_press_valid = false;
    s_moved       = false;
    s_airband_press = PK_DIAG_AIRBAND_ACT_NONE;
    if (click && airband_action != PK_DIAG_AIRBAND_ACT_NONE) {
        airband_apply_action(airband_action);
        return;
    }
    if (!click || s_detail >= 0) return;

    const int col = (x < CARD_PAD + CARD_W + CARD_GAP / 2) ? 0 : 1;
    const int row = (y + s_scroll_y - CARD_TOP) / (CARD_H + CARD_GAP);
    const int idx = pk_diag_card_index_for_cell(row, col);
    if (row >= 0 && idx >= 0 && idx < CARD_N) {
        s_detail = idx;
        /* 每次下钻都从顶开始，别继承上一个子系统的位置。行数也一并清零：
         * 它由 draw_detail() 在**渲染时**写，下钻后的第一次按下可能早于第一帧，
         * 那时读到的是上一个子系统的行数——上一个超屏、这个不超，就会错误地
         * 接管触摸并掐掉右滑返回。清零 = 第一帧不滚，宁可慢一帧。 */
        s_detail_scroll_y = 0;
        s_detail_lines    = 0;
        /* 走导航层的二级页机制，而不是自己画一个返回箭头就算完（上一版就是
         * 那么干的）。spec §4.2：进子页后 FAB 图标变 ←、dock 收起且不可展开，
         * **三条退路（顶栏按钮 / FAB / 右滑）同时可用**——无物理按键的设备
         * 上，任何一条失效都不能让用户困在里面。自画一个箭头只提供了一条。 */
        /* backbar 走 LVGL 的 tiny_ttf（全字库，不是 catalog 子集），中文标题
         * 不会缺字；set_subpage 内部 lv_snprintf 进自己的 buf，不留指针。 */
        pk_ui_nav_set_subpage(true, pk_i18n_text(PK_TR_DIAG_TITLE));
    }
}
void pk_diag_page_touch_cancel(void)
{
    s_press_valid = false;
    s_moved = false;
    s_airband_press = PK_DIAG_AIRBAND_ACT_NONE;
}

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
/* ⚠️ DET_KEY_X / DET_TITLE_TOP / DET_TOP / DET_LINE_H / DET_VAL_X 的定义已经
 * 前移到文件上部 CARD_* 宏旁边——详情页加滚动之后，触摸侧的
 * pk_diag_page_drag() 也要用 DET_TOP/DET_LINE_H 算可滚范围，而它在本行之前。
 * 上面那段版面说明留在原处，因为它解释的是这一块的排版意图。 */

/* 详情页也要能滚——数据比一屏多的页面（GPS 五个星座全有星时）底部会被截掉，
 * 而截掉的部分在截图上看不出来，只有算版面才发现。规则与总览页同一套：
 * s_detail_scroll_y 是偏移，内容高度由上一帧实际画了几行推出来。
 *
 * 行的可见性判据是「与内容区有交集」而不是「整行装得下」：滚动过程中首尾两行
 * 必然只露一半，按整行判会让它们整体闪现/消失。滑到 DET_TOP 之上的那半行由
 * draw_detail_header() 的背景填充盖掉（它在 draw_detail 之后画）。 */
static int det_line_y(int line)
{
    return DET_TOP + line * DET_LINE_H - s_detail_scroll_y;
}

static bool det_line_visible(int y)
{
    return (y + DET_LINE_H > DET_TOP) && (y < PK_DISPLAY_H);
}

static void det_kv(uint16_t *fb, int line, const char *k, const char *v,
                   uint16_t vcol)
{
    const int y = det_line_y(line);
    if (!det_line_visible(y)) return;
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

static void det_button(uint16_t *fb, int line, bool right, const char *label,
                       bool hot)
{
    const int x = PK_DIAG_AIRBAND_BTN_X0 +
                  (right ? PK_DIAG_AIRBAND_BTN_W +
                               PK_DIAG_AIRBAND_BTN_GAP : 0);
    const int y = det_line_y(line);
    if (!det_line_visible(y)) return;
    fill_rect(fb, x, y, x + PK_DIAG_AIRBAND_BTN_W, y + 34,
              hot ? pk_rgb565(24, 86, 140) : pk_rgb565(38, 44, 56));
    const int tw = pk_aa_text_width(label, PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x + (PK_DIAG_AIRBAND_BTN_W - tw) / 2,
               y + (34 - pk_aa_cell_h(PK_AA_S)) / 2,
               label, COL_VAL, PK_AA_S);
}

/* FAULTS 行的片段拼接：seg 追加到 buf[pos]，非首段以空格分隔。返回新
 * 长度，缓冲不够返回 -1（调用方放弃后续片段；片段都是短词，给足缓冲
 * 就到不了这步——worst case 全部故障同时置位约 72 B）。 */
static int diag_fault_cat(char *buf, size_t cap, int pos, bool *first,
                          const char *seg)
{
    if (pos < 0) return -1;
    const int n = snprintf(buf + pos, cap - (size_t)pos, "%s%s",
                           *first ? "" : " ", seg);
    if (n < 0 || (size_t)n >= cap - (size_t)pos) return -1;
    *first = false;
    return pos + n;
}

static void draw_detail(uint16_t *fb, int which)
{
    char buf[64];
    int line = 0;

    switch (which) {
    case 2: {   /* GPS —— spec 点名的那一页 */
        pk_gps_state_t g = {0};
        pk_gps_get(&g);
        pk_ant_gnss_status_t ant_cfg = {0};
        pk_ant_gnss_status_get(&ant_cfg);
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

        /* 一行同时展示请求来源、ACK 确认的实际来源、模块 raw 和解释结果。
         * V4.9 的 U7.14 VCC_RF
         * 已经接到 Q4/Q5 源极；但 OPEN 仍可能是无源天线，故写成 INDET。
         * CONFIG_ACK 只能确认 RP 接受了整套配置，仍不是 GPIO 电平回读；台架
         * 最终可再用 G 命令核对。但请求值绝不能伪装成“实际已切换”。 */
        const bool ant_receiving = ant_cfg.actual_known &&
                                   (fresh || g.snr_count > 0);
        const pk_gps_ant_result_t ant_result =
            pk_gps_ant_assess(g.ant_status, ant_receiving);
        const char *result_short =
            ant_result == PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK ? "LOAD-OK"
            : ant_result == PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE ? "INDET"
            : ant_result == PK_GPS_ANT_RESULT_RF_RECEIVING ? "RECEIVING"
            : ant_result == PK_GPS_ANT_RESULT_SHORT ? "SHORT" : "UNKNOWN";
        const char *req = ant_cfg.requested == PK_ANT_GNSS_EXTERNAL
                              ? "EXT" : "INT";
        if (!ant_cfg.actual_known) {
            snprintf(buf, sizeof(buf), "req=%s actual=%s raw=UNKNOWN",
                     req, ant_cfg.request_pending ? "PENDING" : "UNKNOWN");
        } else {
            const char *actual = ant_cfg.actual == PK_ANT_GNSS_EXTERNAL
                                     ? "EXT" : "INT";
            snprintf(buf, sizeof(buf), "req=%s actual=%s raw=%s %s",
                     req, actual, pk_gps_ant_raw_name(g.ant_status), result_short);
        }
        det_kv_tr(fb, line++, PK_TR_DIAG_K_ANTENNA, buf,
                  !ant_cfg.actual_known ? COL_WARN
                  : ant_result == PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK ||
                  ant_result == PK_GPS_ANT_RESULT_RF_RECEIVING ? COL_ONLINE
                  : ant_result == PK_GPS_ANT_RESULT_SHORT ? COL_ALERT
                  : ant_result == PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE ? COL_WARN
                  : COL_OFFLINE);

        if (fresh) snprintf(buf, sizeof(buf), "%.5f  %.5f", g.lat, g.lon);
        else       snprintf(buf, sizeof(buf), "---");
        det_kv_tr(fb, line++, PK_TR_DIAG_K_POSITION, buf,
                  fresh ? COL_VAL : COL_OFFLINE);

        /* 模块自身 RTC 的走时证据（键名刻意用 ASCII：新增中文要改 i18n
         * catalog 并重跑字库生成，为一行诊断值不起那个动静）。
         *
         * 读法：右边的 A/V 是**那一条 RMC 自己**报的定位状态。
         *   V + 时间在逐秒前进  → 模块内部 RTC 独立于定位在走时 ← 要的就是这个
         *   V + 时间不动/为 ---  → 模块没有在走时
         * age 跟着跳说明 RMC 还在持续进来，不是冻住的旧值。
         *
         * ⚠️ 版面：本页 DET_TOP=90、行高 38、屏高 480 → 最多 10 行。此行占掉
         * 第 6 行后，只有「5 个星座同时有星」时最后一个星座的 SNR 行会被下面
         * 循环的 break 挡掉（不会画出屏幕）。取这个舍是因为「模块钟在不在走」
         * 比第 5 个星座的柱高更要紧。 */
        if (g.rmc_epoch_ms == 0) {
            snprintf(buf, sizeof(buf), "---");
        } else {
            time_t t = (time_t)(g.rmc_epoch_ms / 1000);
            struct tm u;
            gmtime_r(&t, &u);
            long age_s = (long)((esp_timer_get_time() - g.rmc_epoch_us) / 1000000);
            snprintf(buf, sizeof(buf), "%02d:%02d:%02dZ %c %lds",
                     u.tm_hour, u.tm_min, u.tm_sec,
                     g.rmc_epoch_from_fix ? 'A' : 'V', age_s);
        }
        det_kv(fb, line++, "RMC UTC", buf,
               g.rmc_epoch_ms == 0 ? COL_OFFLINE
               : g.rmc_epoch_from_fix ? COL_VAL : COL_ONLINE);

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
            const int y = det_line_y(line);
            /* 这里不能像 det_kv 那样直接跳过就算了：line 必须照常自增，
             * 否则滚上去的星座不占行号，下面的行会整体上移、滚动就错位了。 */
            if (!det_line_visible(y)) { line++; continue; }
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
            /*
             * 磁环境紧跟在校准档位下面：上面那行说「标定到第几档」，这行说
             * 「这个档位值不值得当真」。分开摆，是因为两者指向的动作相反——
             * 一个是「拿起来画 8 字」，另一个是「换个地方站，在这儿画也没用」。
             *
             * 这是 pk_cal_advisor 判定 jammed 之后**唯一**看得见的地方。干扰
             * 态下自动弹页和顶栏罗盘图标都被有意关掉了（见 ui_state.h 的
             * pk_ui_cal_jammed），设备表现为「什么都不提示」；没有这一行，
             * 一台在机坪上安静的盒子与一台坏了的盒子在屏上完全一样。
             *
             * 正常态用绿而不是 COL_VAL 的中性白：这一行只有两个取值，配色
             * 就该跟着结论走，与同页 SENSOR / 天线自检那两行同一口径。
             */
            const bool jam = pk_ui_cal_jammed();
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_MAG_ENV,
                       jam ? PK_TR_DIAG_V_MAG_JAM : PK_TR_DIAG_V_MAG_OK,
                       jam ? COL_WARN : COL_ONLINE);
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

    case 3: {   /* ADS-B 链路（RP2040 UART）*/
        pk_adsb_link_stats_t ls;
        const pk_adsb_link_state_t st = pk_adsb_link_state_get(&ls);
        pk_dsp_stats_t d;
        pk_dsp_get_stats(&d);
        static const pk_tr_id_t kLink[] = {
            PK_TR_DIAG_V_LINK_NONE,     /* NO_LINK */
            PK_TR_DIAG_V_LINK_PROTO,    /* PROTO_MISMATCH */
            PK_TR_DIAG_V_LINK_STALLED,  /* STALLED */
            PK_TR_DIAG_V_LINK_LIVE };   /* LINKED */
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_STATE, kLink[st],
                   st == PK_ADSB_LINK_LINKED ? COL_ONLINE : COL_ALERT);
        /* 链路收发两行（audit round 3）：旧 SDR 口径的采样率/IQ 丢弃行
         * 已随 RTL-SDR 退役，换成 UART 链路的实际计数——收帧看链路通量，
         * 坏 CRC 看信号质量，琥珀色只在真的有坏帧时亮。 */
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)ls.rx_frames);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_LINK_RX, buf, COL_VAL);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)ls.rx_crc_errors);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_BAD_CRC, buf,
                  ls.rx_crc_errors ? COL_WARN : COL_VAL);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.msgs_total);
        det_kv_tr(fb, line++, PK_TR_DIAG_K_ADSB_MSGS, buf, COL_VAL);
        snprintf(buf, sizeof(buf), "frames=%lu aircraft=%lu",
                 (unsigned long)ls.uat_dl_frames,
                 (unsigned long)ls.uat_aircraft);
        det_kv(fb, line++, "UAT 978", buf, COL_VAL);
        snprintf(buf, sizeof(buf), "fec=%lu transport=%lu",
                 (unsigned long)ls.uat_bad_rs,
                 (unsigned long)ls.uat_transport_drops);
        det_kv(fb, line++, "UAT ERROR", buf,
               (ls.uat_bad_rs || ls.uat_transport_drops) ? COL_WARN : COL_VAL);
        uint32_t rh[ADSB_LINK_HEALTH_FIELDS] = {0};
        const int rhn = pk_adsb_rp_health(rh);
        if (rhn > ADSB_LINK_HEALTH_IDX_UAT_RF_REINITS) {
            const uint32_t flags = rh[ADSB_LINK_HEALTH_IDX_UAT_RF_FLAGS];
            const bool active = (flags & ADSB_LINK_UAT_RF_ACTIVE) != 0;
            const bool probe_ok = (flags & ADSB_LINK_UAT_RF_PROBE_OK) != 0;
            const bool recovering =
                (flags & ADSB_LINK_UAT_RF_RECOVERING) != 0;
            const bool link_fresh = st == PK_ADSB_LINK_LINKED;
            const bool rf_live = link_fresh && active && probe_ok && !recovering;
            const char *rf_state = !link_fresh ? "LINK STALE"
                : recovering ? "RECOVERING"
                : (active && probe_ok) ? "LIVE (QUIET OK)" : "RF FAULT";
            det_kv(fb, line++, "UAT RF CORE", rf_state,
                   rf_live ? COL_ONLINE : COL_WARN);
            if (flags & ADSB_LINK_UAT_RF_RSSI_VALID) {
                snprintf(buf, sizeof(buf), "%ld dBm (idle/noise)",
                         (long)(int32_t)rh[ADSB_LINK_HEALTH_IDX_UAT_IDLE_RSSI]);
            } else {
                snprintf(buf, sizeof(buf), "unavailable");
            }
            det_kv(fb, line++, "UAT IDLE RSSI", buf,
                   (flags & ADSB_LINK_UAT_RF_RSSI_VALID) ? COL_VAL : COL_WARN);
            snprintf(buf, sizeof(buf), "raw=0x%04lX ok=%lu err=%lu reinit=%lu",
                     (unsigned long)rh[ADSB_LINK_HEALTH_IDX_UAT_LAST_ERROR],
                     (unsigned long)rh[ADSB_LINK_HEALTH_IDX_UAT_PROBE_OK],
                     (unsigned long)rh[ADSB_LINK_HEALTH_IDX_UAT_PROBE_ERRORS],
                     (unsigned long)rh[ADSB_LINK_HEALTH_IDX_UAT_RF_REINITS]);
            det_kv(fb, line++, "UAT RF DIAG", buf,
                   rh[ADSB_LINK_HEALTH_IDX_UAT_PROBE_ERRORS] ? COL_WARN : COL_VAL);
        } else {
            det_kv(fb, line++, "UAT RF CORE", "unavailable (old firmware)",
                   COL_WARN);
        }
        break;
    }

    case 4: {   /* BLE */
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_LINK,
                   ble_gatt_is_connected()   ? PK_TR_DIAG_V_BLE_CONN
                   : ble_gatt_is_advertising() ? PK_TR_DIAG_V_BLE_ADV
                                               : PK_TR_DIAG_V_BLE_IDLE,
                   ble_gatt_is_connected() ? COL_ONLINE : COL_OFFLINE);
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_PROTOCOL, PK_TR_DIAG_V_BLE_PROTO,
                   COL_VAL);
        pk_ble_airband_status_t a = {0};
#ifdef ESP_PLATFORM
        (void)ble_gatt_airband_status(&a);
#else
        /* Simulator has no NimBLE host; keep the card structurally visible
         * without inventing a connected phone/audio stream. */
        a.mtu = 23;
        a.route = PK_AIRBAND_BLE_ROUTE_DEVICE;
        a.mtu_paused = true;
#endif
        static const char *routes[] = { "DEVICE", "PHONE", "MUTE" };
        snprintf(buf, sizeof(buf), "%s sub=%u MTU=%u%s(%lu)",
                 routes[a.route <= PK_AIRBAND_BLE_ROUTE_MUTE ? a.route : 0],
                 a.subscribed, a.mtu, a.mtu_paused ? " PAUSED" : "",
                 (unsigned long)a.mtu_paused_count);
        det_kv(fb, line++, "AIR AUDIO", buf,
               a.mtu_paused ? COL_WARN : COL_VAL);
        snprintf(buf, sizeof(buf), "%u/%u clk=%lu",
                 a.stream_id, a.frame_seq, (unsigned long)a.sample_clock);
        det_kv(fb, line++, "STREAM/SEQ", buf, COL_VAL);
        snprintf(buf, sizeof(buf), "enc=%lu sent=%lu drop=%lu q=%lu",
                 (unsigned long)a.encoded, (unsigned long)a.sent,
                 (unsigned long)a.dropped, (unsigned long)a.queue_drops);
        det_kv(fb, line++, "AUDIO TX", buf,
               (a.dropped || a.queue_drops) ? COL_WARN : COL_VAL);
        snprintf(buf, sizeof(buf), "now=%lums max=%lums cong=%lu err=%u",
                 (unsigned long)a.current_age_ms, (unsigned long)a.max_age_ms,
                 (unsigned long)a.congested, a.last_error);
        det_kv(fb, line++, "AGE/ERR", buf,
               (a.congested || a.last_error) ? COL_WARN : COL_VAL);
        break;
    }

    case 5: {   /* LOG */
        uint32_t written = 0, dropped = 0;
        if (record_sink_file_stats(&written, &dropped)) {
            /* 两行分开说，谁都不冒充谁：
             *   当前后端 = record_sink_file_uses_sd()，**现在真在写的那个**；
             *   设置为   = pk_log_store_get()，用户在设置页选的那个。
             * 只显示前者会让人以为设置没生效（验收时的原话），只显示
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

            /* 丢弃这一行说两路 sink，不是只说 file（P1-D）。
             *
             * UART sink 改成非阻塞入队之后，控制台吃不下的部分记进它自己的
             * dropped。没有消费者的计数器等于没有计数器：改造前丢帧是"丢在
             * UART 驱动里、无计数"，如果只是把丢弃搬进一个没人读的计数器，
             * 运维侧看到的东西一点没变。
             *
             * 复用同一行而不是新开一格：两者都是"这一路丢了多少"，语义同类；
             * 而且值文本全是 ASCII，不引入新的 i18n 词条（新增中文要改
             * i18n_catalog.py 并重跑字库生成，见该脚本的说明）。sink 没起来
             * 时不显示 uart 段——那时报 0 是假话，这一路压根不存在。 */
            uint32_t uart_dropped = 0;
            const bool uart_up = record_sink_uart_stats(NULL, &uart_dropped, NULL);
            if (uart_up) {
                snprintf(buf, sizeof(buf), "file %lu  uart %lu",
                         (unsigned long)dropped, (unsigned long)uart_dropped);
            } else {
                snprintf(buf, sizeof(buf), "%lu", (unsigned long)dropped);
            }
            det_kv_tr(fb, line++, PK_TR_DIAG_K_DROPPED, buf,
                      (dropped || uart_dropped) ? COL_WARN : COL_VAL);
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
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_HINT,
                       pk_sdcard_probe_exhausted() ? PK_TR_POWER_RESTART
                                                  : PK_TR_DIAG_V_SD_HINT,
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
        sy6970_diag_t d;
        if (sy6970_live(&d)) {
            /* v4 powered：SY6970 的寄存器级取证。这一页的读者在排查供电，
             * 给原始证据（REG00 回读、REG0C 故障字节）比只给结论更能定位
             * ——F1 的关狗写还在不在、REG0C 锁了哪个 bit，看这两行就知道。
             * 电量/电压在**同源**时取公共快照，与状态栏、总览卡同一口径
             * （同源守卫见下：快照赢家不是 SY6970 时不用它的字段）。 */
            const power_snapshot_t s = power_service_snapshot();
            const bool same_src = (s.backend == POWER_BACKEND_SY6970);
            if (same_src && s.stale) {
                /* 服务端 5 s 无新数据：说过期，不报旧数（同总览卡的取舍）。 */
                det_kv_tr2(fb, line++, PK_TR_DIAG_K_STATUS,
                           PK_TR_DIAG_V_PWR_STALE, COL_WARN);
                break;
            }
            const sy6970_status_t *st = &d.st;
            /* CHRG_STAT 四态分开报：01/10 都是充电中（预充/快充），11 是
             * 充满维持；不充时按 VBUS 在位与否分"外部供电/电池放电"——
             * 插着电没充和靠电池飞是两个完全不同的排查起点。 */
            det_kv_tr2(fb, line++, PK_TR_DIAG_K_STATUS,
                       st->charging       ? PK_TR_DIAG_V_PWR_CHARGING
                       : st->term_done    ? PK_TR_DIAG_V_PWR_TERM
                       : st->vbus_present ? PK_TR_DIAG_V_PWR_EXT
                                          : PK_TR_DIAG_V_PWR_DISCHG,
                       st->charging ? COL_ONLINE : COL_VAL);
            if (same_src && s.pct_valid) {
                snprintf(buf, sizeof(buf), "%u %%", (unsigned)s.pct_est);
                det_kv_tr(fb, line++, PK_TR_DIAG_K_CHARGE, buf,
                          s.pct_est >= 20 ? COL_ONLINE : COL_WARN);
            }
            /* 电压：同源取公共快照；不同源取 SY6970 自己的 BATV 译码
             * （回落源的电压不是这颗芯片的读数，不并进来）。 */
            snprintf(buf, sizeof(buf), "%.3f V",
                     (double)(same_src ? s.batt_mv : st->batt_mv) / 1000.0);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_VOLTAGE, buf, COL_VAL);
            /* BUSV/ICHGR 是 SY6970 自己的 ADC（REG11/REG12，[DS] p.24-25）。
             * ICHGR 在 VBAT<VSHORT 时芯片读回 0mA——那是芯片口径，不是读
             * 数坏了；VBUS 没插电时寄存器仍有残值，照实显示、灰掉。 */
            snprintf(buf, sizeof(buf), "%.1f V", (double)st->vbus_mv / 1000.0);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_VBUS, buf,
                      st->vbus_present ? COL_ONLINE : COL_OFFLINE);
            /* ICHGR 在**不充电时不刷新**，保持上一次的有效值（2026-09-12
             * 实测：拔掉输入后仍恒报 250 mA）。原样画出来会让人以为还在充，
             * 而这一行恰恰是排查"到底有没有在充"时最先看的。不充电时灰掉
             * 并标 stale——不拿陈值冒充读数，也不改写成 0（那同样是我们
             * 没有的信息）。 */
            snprintf(buf, sizeof(buf), "%u mA%s", (unsigned)st->ichg_ma,
                     st->charging ? "" : " (stale)");
            det_kv_tr(fb, line++, PK_TR_DIAG_K_ICHG, buf,
                      st->charging ? COL_VAL : COL_OFFLINE);
            /* FAULTS：REG0C 各位译码，多处同时故障全部列出（读者正对着手
             * 册查 bit）；NTC 只拼寄存器原码——Warm/Cool/Cold/Hot 对应
             * 010/011/101/110，报原码才好对照（[DS] p.22-23）。全零时复
             * 用天线自检那条「OK/正常」。 */
            {
                char fbuf[96];
                /* "NTC 2 (43.2%)"：原码后面带上 NTCPCT（REG10）实测
                 * 百分比。只报原码时没法区分"NTC 真的热了"和"分压网络
                 * /贴装不对"——前者百分比会随温度走，后者钉死不动。
                 * 板上 R39=5.62k/R40=31.6k/RT1=10k 下 25°C≈57.5%，
                 * Warm 门限 V_T3=44.75%、Hot 门限 V_T4=34.35%（DS p.10-11）。 */
                char ntc[24];
                bool first = true, any = false;
                int q = 0;
                fbuf[0] = '\0';
                if (st->wd_fault) {
                    any = true;
                    q = diag_fault_cat(fbuf, sizeof(fbuf), q, &first,
                                       pk_i18n_text(PK_TR_DIAG_V_PWR_WDFAULT));
                }
                if (st->chrg_fault == 1) {
                    any = true;
                    q = diag_fault_cat(fbuf, sizeof(fbuf), q, &first,
                                       pk_i18n_text(PK_TR_DIAG_V_PWR_CHG_IN));
                }
                if (st->chrg_fault == 2) {
                    any = true;
                    q = diag_fault_cat(fbuf, sizeof(fbuf), q, &first,
                                       pk_i18n_text(PK_TR_DIAG_V_PWR_CHG_THERM));
                }
                if (st->chrg_fault == 3) {
                    any = true;
                    q = diag_fault_cat(fbuf, sizeof(fbuf), q, &first,
                                       pk_i18n_text(PK_TR_DIAG_V_PWR_CHG_TIMER));
                }
                if (st->bat_ovp_fault) {
                    any = true;
                    q = diag_fault_cat(fbuf, sizeof(fbuf), q, &first,
                                       pk_i18n_text(PK_TR_DIAG_V_PWR_BATOVP));
                }
                if (st->boost_fault) {
                    any = true;
                    q = diag_fault_cat(fbuf, sizeof(fbuf), q, &first,
                                       pk_i18n_text(PK_TR_DIAG_V_PWR_BOOST));
                }
                if (st->ntc_fault != 0) {
                    any = true;
                    /* ntc_pct_x1000 是 0.001% 整数单位（21000~80055），
                     * 无浮点拆成 xx.x%——整数部分 /1000，一位小数取
                     * 余数的百位（power_sy6970.h 的字段口径）。 */
                    snprintf(ntc, sizeof(ntc), "NTC %u (%lu.%lu%%)",
                             (unsigned)st->ntc_fault,
                             (unsigned long)(st->ntc_pct_x1000 / 1000u),
                             (unsigned long)(st->ntc_pct_x1000 % 1000u / 100u));
                    q = diag_fault_cat(fbuf, sizeof(fbuf), q, &first, ntc);
                }
                /* 颜色按**后果**分级，不是"非零就红"。
                 *
                 * NTC 的四档里只有 Cold(101)/Hot(110) 会**停充**；
                 * Warm(010) 自 JEITA_VSET=1 之后连降压都不做了，对充电
                 * 毫无影响，Cool(011) 只降流到 20%（[DS] p.32）。
                 * 2026-09-12 用户实测就撞上这点：电池在 42°C 正常充电、
                 * 一切正常，屏上却挂着红色"NTC 2"——广东室温本来就 40°C+，
                 * 这个红字会天天出现而没有任何可操作的含义，久了就没人看
                 * 故障行了。
                 * 所以：真正阻断充电的才红，降档/提示性的走黄。文字照旧
                 * 全列，信息一条不少。 */
                const bool blocking =
                    st->wd_fault || st->boost_fault || st->bat_ovp_fault ||
                    st->chrg_fault != 0 ||
                    st->ntc_fault == 5 || st->ntc_fault == 6;
                if (any)
                    det_kv_tr(fb, line++, PK_TR_DIAG_K_PWR_FAULT, fbuf,
                              blocking ? COL_ALERT : COL_WARN);
                else
                    det_kv_tr2(fb, line++, PK_TR_DIAG_K_PWR_FAULT,
                               PK_TR_DIAG_V_ANT_OK, COL_ONLINE);
            }
            /* F1 的落定证据：REG00 回读 = 0x40 | IINLIM（EN_HIZ=0、
             * EN_ILIM=1，[DS] p.15）。看门狗把寄存器打回默认的话，这里
             * 会看到 POR 以外的异动，与 WD fault 那行互证。 */
            snprintf(buf, sizeof(buf), "0x%02X", d.reg00);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_REG00, buf, COL_VAL);
            /* REG0C 原始字节：上一行 FAULTS 是它的译码，并排供对照。
             * 窗口布局 regs[0]=REG0B、regs[1]=REG0C（power_sy6970.h）。 */
            snprintf(buf, sizeof(buf), "0x%02X", d.regs[1]);
            det_kv_tr(fb, line++, PK_TR_DIAG_K_REG0C, buf, COL_OFFLINE);
        } else {
            /* 同上：ETA6098 退役后这条分支只剩"没有可用电源源"一种含义。
             * SY6970 探测 NACK / 掉线 >5 s 都落到这里。 */
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

    case PK_DIAG_CARD_AIRBAND: {
        pk_airband_state_t a = {0};
        const bool have_diag = pk_airband_get(&a);
        pk_airband_control_status_t ctl;
        const bool have_ctl = pk_airband_control_get(&ctl);

        det_kv(fb, line++, "STATUS",
               have_diag && a.online ? "RX ONLINE" : "RX OFFLINE",
               have_diag && a.online ? COL_ONLINE : COL_ALERT);

        const bool rf_online = have_diag && a.online;
        const uint32_t hz = rf_online ? a.frequency_hz
                                      : have_ctl ? ctl.requested_hz : 0;
        if (hz != 0) {
            snprintf(buf, sizeof(buf), "%lu.%03lu MHz%s",
                     (unsigned long)(hz / 1000000u),
                     (unsigned long)((hz % 1000000u) / 1000u),
                     have_ctl && ctl.scanning ? " SCAN" : "");
        } else {
            snprintf(buf, sizeof(buf), "---");
        }
        det_kv(fb, line++, rf_online ? "ACTUAL FREQ" : "TARGET FREQ", buf,
               !rf_online ? COL_WARN
                          : have_ctl && ctl.scanning ? COL_WARN : COL_VAL);

        if (rf_online) {
            snprintf(buf, sizeof(buf), "RSSI %d.%d dBm  N%u G%u",
                     a.rf.rssi_dbm_x2 / 2,
                     (a.rf.rssi_dbm_x2 & 1) ? 5 : 0,
                     (unsigned)a.rf.noise_raw, (unsigned)a.rf.glitch_raw);
            det_kv(fb, line++, "RF", buf, COL_VAL);
        } else {
            det_kv(fb, line++, "RF", "unavailable", COL_ALERT);
        }
        if (have_diag) {
            snprintf(buf, sizeof(buf), "L%lu R%lu RMS  %lu frames",
                     (unsigned long)a.audio_rms,
                     (unsigned long)a.audio_right_rms,
                     (unsigned long)a.audio_frames);
            det_kv(fb, line++, "AUDIO", buf,
                   a.read_errors ? COL_WARN : COL_VAL);
        } else {
            det_kv(fb, line++, "AUDIO", "no data", COL_OFFLINE);
        }

        det_button(fb, line, false, "121.500 GUARD",
                   !have_ctl || (!ctl.scanning && ctl.requested_hz == 121500000u));
        det_button(fb, line++, true, "123.450 AIR-AIR",
                   have_ctl && !ctl.scanning && ctl.requested_hz == 123450000u);
        det_button(fb, line, false,
                   have_ctl && ctl.scanning ? "SCAN: ON" : "SCAN: OFF",
                   have_ctl && ctl.scanning);
        det_button(fb, line++, true,
                   have_ctl && ctl.squelch_enabled ? "SQUELCH: ON"
                                                   : "SQUELCH: OFF",
                   have_ctl && ctl.squelch_enabled);
        snprintf(buf, sizeof(buf), "SQ -2 dB");
        det_button(fb, line, false, buf, false);
        snprintf(buf, sizeof(buf), "SQ +2 dB  (%d.%d)",
                 have_ctl ? ctl.squelch_open_dbm_x2 / 2 : -90,
                 have_ctl && (ctl.squelch_open_dbm_x2 & 1) ? 5 : 0);
        det_button(fb, line++, true, buf, false);
        det_button(fb, line++, false, "SPEAKER TEST 1s",
                   have_diag && a.speaker_tone_active);
        if (have_diag) {
            const char *result = a.speaker_test_result ==
                                     PK_AIRBAND_SPEAKER_TEST_PASS ? "PASS"
                               : a.speaker_test_result ==
                                     PK_AIRBAND_SPEAKER_TEST_FAIL ? "FAIL"
                               : a.speaker_test_result ==
                                     PK_AIRBAND_SPEAKER_TEST_RUNNING ? "RUNNING"
                               : a.speaker_test_result ==
                                     PK_AIRBAND_SPEAKER_TEST_STOPPED ? "STOPPED"
                                                                    : "IDLE";
            snprintf(buf, sizeof(buf), "%s %lu/%lu frames  PA=%d",
                     result,
                     (unsigned long)a.speaker_test_written_frames,
                     (unsigned long)a.speaker_test_expected_frames,
                     a.speaker_pa_enabled);
            det_kv(fb, line++, "SPK TEST", buf,
                   a.speaker_test_result == PK_AIRBAND_SPEAKER_TEST_PASS
                       ? COL_ONLINE
                       : a.speaker_test_result == PK_AIRBAND_SPEAKER_TEST_FAIL
                             ? COL_ALERT : COL_VAL);
            snprintf(buf, sizeof(buf), "obs %.1f / ref %.1f dBm",
                     a.rf.rssi_dbm_x2 / 2.0, a.reference_rssi_dbm_x2 / 2.0);
            det_kv(fb, line++, "AM RSSI", buf, a.online ? COL_VAL : COL_ALERT);
            snprintf(buf, sizeof(buf), "%u / %ddB  %s", a.gain_index, a.gain_db,
                     pk_airband_am_gain_reason_name(a.gain_reason));
            det_kv(fb, line++, "AM GAIN", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "7E=%04X 13=%04X W%lu R%lu",
                     a.gain_regs.reg7e, a.gain_regs.reg13,
                     (unsigned long)a.gain_regs.write_errors,
                     (unsigned long)a.gain_regs.read_errors);
            det_kv(fb, line++, "AM REGS", buf,
                   a.gain_regs.write_errors || a.gain_regs.read_errors ? COL_WARN : COL_VAL);
            snprintf(buf, sizeof(buf), "%luus late %luus over %lu",
                     (unsigned long)a.gain_control_max_us,
                     (unsigned long)a.gain_control_max_late_us,
                     (unsigned long)a.gain_control_overruns);
            det_kv(fb, line++, "AM CONTROL", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "raw %lu pre %lu post %lu",
                     (unsigned long)a.pcm16.raw_peak, (unsigned long)a.pcm16.pre_peak,
                     (unsigned long)a.pcm16.post_peak);
            det_kv(fb, line++, "PCM PEAK", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "%lu / %lu / %lu",
                     (unsigned long)pk_airband_audio_rms(a.pcm16.raw_sum_sq, a.pcm16.input_frames),
                     (unsigned long)pk_airband_audio_rms(a.pcm16.pre_sum_sq, a.pcm16.output_frames),
                     (unsigned long)pk_airband_audio_rms(a.pcm16.post_sum_sq, a.pcm16.output_frames));
            det_kv(fb, line++, "PCM RMS", buf, COL_VAL);
            snprintf(buf, sizeof(buf), "raw/pre/post %lu/%lu/%lu",
                     (unsigned long)a.pcm16.raw_clips, (unsigned long)a.pcm16.pre_clips,
                     (unsigned long)a.pcm16.post_clips);
            det_kv(fb, line++, "PCM CLIP", buf, a.pcm16.post_clips ? COL_WARN : COL_VAL);
        }
        pk_airband_record_status_t rec;
        pk_airband_record_get(&rec);
        snprintf(buf,sizeof(buf),"%s enabled=%u active=%u q=%lu/%lu",
            pk_airband_record_label(&rec),rec.enabled,rec.active,
            (unsigned long)rec.queued_blocks,(unsigned long)rec.queue_high_water);
        det_kv(fb,line++,"SD REC",buf,rec.error ? COL_ALERT : COL_VAL);
        det_kv(fb,line++,"REC FILE",rec.file[0] ? rec.file : "--",COL_VAL);
        snprintf(buf,sizeof(buf),"%llu B / %lu segments / gen %lu",
            (unsigned long long)rec.bytes,(unsigned long)rec.segments,(unsigned long)rec.mount_generation);
        det_kv(fb,line++,"REC DATA",buf,COL_VAL);
        snprintf(buf,sizeof(buf),"drop %lu/%lu samples write %lu sync %lu close %lu",
            (unsigned long)rec.dropped_blocks,(unsigned long)rec.dropped_samples,
            (unsigned long)rec.write_errors,(unsigned long)rec.sync_errors,(unsigned long)rec.close_errors);
        det_kv(fb,line++,"REC ERR",buf,rec.error || rec.dropped_blocks ? COL_WARN : COL_VAL);
        snprintf(buf,sizeof(buf),"source %lu  write/sync max %lu/%lu us",
            (unsigned long)rec.source_gaps,(unsigned long)rec.max_write_us,
            (unsigned long)rec.max_sync_us);
        det_kv(fb,line++,"REC LATENCY",buf,
               rec.dropped_blocks ? COL_WARN : COL_VAL);
        snprintf(buf,sizeof(buf),"kept %lu / errors %lu",
            (unsigned long)rec.recovery_skipped,(unsigned long)rec.recovery_errors);
        det_kv(fb,line++,"REC REPAIR",buf,rec.recovery_errors ? COL_ALERT : COL_VAL);
        break;
    }

    default:
        det_kv_tr2(fb, line++, PK_TR_DIAG_K_DETAIL, PK_TR_DIAG_V_NO_FURTHER,
                   COL_OFFLINE);
        break;
    }

    /* 行数是本帧现算的：详情内容随状态伸缩（星座有几个有星、电池有没有故障
     * 位），写死一个常数迟早对不上，而对不上的表现是"滚不到底"或"能滚出一片
     * 空白"。拖动侧因此只读这个值，不自己推算。 */
    s_detail_lines = line;

    /* 滚动条：与总览页同一套画法，只在内容超出一屏时出现。 */
    {
        const int total_h = line * DET_LINE_H;
        const int view_h  = PK_DISPLAY_H - DET_TOP;
        if (total_h > view_h) {
            const int tx = PK_DISPLAY_W - 6;
            const int bar_h = view_h * view_h / total_h;
            const int bar_y = DET_TOP + s_detail_scroll_y * view_h / total_h;
            pk_pfd_fill_rect(fb, tx, DET_TOP, tx + 3, PK_DISPLAY_H,
                             pk_rgb565(30, 38, 50));
            pk_pfd_fill_rect(fb, tx, bar_y, tx + 3, bar_y + bar_h,
                             pk_rgb565(120, 135, 155));
        }
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
    /* 先把标题行那条擦干净，再画标题。
     *
     * 这一笔是详情页滚动的配套：滚上去的那半行会越过 DET_TOP 画进标题区，
     * 靠这块背景盖掉（本函数在 draw_detail 之后调用，次序不能颠倒）。总览页
     * 用的是同一招——那边是「顶栏最后画，卡片从它底下滑过去」。
     *
     * 上边界取 PK_UI_BACKBAR_BOT 而不是 0：backbar 是 LVGL 控件层、
     * move_foreground 到最前，canvas 这一笔本来就盖不住它，但把范围划在它
     * 下面，意图才是明确的——这块归详情页，那块归导航层，谁也别碰谁。 */
    fill_rect(fb, 0, PK_UI_BACKBAR_BOT, PK_DISPLAY_W, DET_TOP, COL_BG);

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
    s_detail_scroll_y = 0;
    s_detail_lines    = 0;   /* 理由同下钻处：别把行数留给下一个子系统 */
    pk_ui_nav_set_subpage(false, NULL);
}

/* 当前是否在子系统详情里。pk_ui_nav_on_back() 据此决定是"退出详情"还是
 * "切回诊断页"——两者在诊断页上下文里是不同的动作。 */
bool pk_diag_page_in_detail(void) { return s_detail >= 0; }

#endif /* !PK_DIAG_PAGE_HOST_TEST */
