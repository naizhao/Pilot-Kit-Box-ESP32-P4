/*
 * ant_adv_page.c — 见 ant_adv_page.h。
 *
 * 布局（800×480）
 * ---------------
 *   y=0    ┌──────────────────────────────────────────────┬────────┐
 *          │ 1090 解调                                    │ 返回   │ 顶栏 48
 *   y=48   ├──────────────────────────────────────────────┴────────┤
 *          │ 解调门限   [-100][-10]  600 ‰  [+10][+100]   [默认]   │
 *          │ 扫频/停止                建议 700‰ …          [采用]   │
 *          ├───────────────────────────────────────────────────────┤
 *          │   ‰     mV      帧    PRE      NOISE       OVR        │ 表头
 *          │  200   724       0      0     175027    289423        │ 9 行
 *          │  …                                                    │
 *          ├───────────────────────────────────────────────────────┤
 *          │ 最优点随天线和环境变，换了就重扫                        │
 *   y=480  └───────────────────────────────────────────────────────┘
 *
 * 步进给 ±10 和 ±100 两档：±10 是协议能表达的最小粒度（cfg val 只有 8 位，
 * 见 adsb_link.h），±100 与扫描步进同粒度。只给 ±10 的话从 200 调到 1000
 * 要点 80 次——座舱里一次多余点击的代价远高于电脑前。
 */
#include "ant_adv_page.h"

#include <stdio.h>
#include <string.h>

#include "adsb_link_task.h"
#include "config_antenna.h"
#include "display.h"
#include "i18n.h"
#include "i18n_catalog.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_layout.h"
#include "ui_state.h"

/* ── 配色：与诊断页同一套，切页时不跳 ── */
#define COL_BG        pk_rgb565( 12,  12,  16)
#define COL_TITLE     pk_rgb565(240, 245, 255)
#define COL_KEY       pk_rgb565(150, 170, 195)
#define COL_VAL       pk_rgb565(255, 255, 255)
#define COL_DIM       pk_rgb565(120, 132, 150)
#define COL_LINE      pk_rgb565( 60,  70,  86)
#define COL_BTN       pk_rgb565( 38,  44,  56)
#define COL_BTN_HOT   pk_rgb565( 24,  86, 140)
#define COL_OK        pk_rgb565(120, 220, 140)
#define COL_WARN      pk_rgb565(235, 190,  90)
#define COL_BEST      pk_rgb565(140, 210, 255)

#define HDR_H         PFD_BAR_BOT
#define ROW_H         24
/* 表格起点：门限行(34) + 用法说明(22) + 扫频行(42) + 建议行(28) 之后。
 * 9 行 × 24 + 表头 20 = 236，到 FOOT_TOP 还有余量。 */
#define TBL_TOP       (HDR_H + 148)
#define FOOT_TOP      (PK_DISPLAY_H - 34)

/* 命中区：render 时登记，touch 时反查。与 settings_draw.c 的 hit_set 同款
 * ——版面只在一处定义（画的地方），命中判定跟着它走，不各写一份坐标。 */
typedef enum {
    HIT_NONE = 0, HIT_BACK, HIT_DEC100, HIT_DEC10, HIT_INC10, HIT_INC100,
    HIT_DEFAULT, HIT_SWEEP, HIT_APPLY, HIT_N
} hit_id_t;

static struct { int16_t x0, y0, x1, y1; } s_hit[HIT_N];

static void hit_set(hit_id_t id, int x0, int y0, int x1, int y1)
{
    s_hit[id].x0 = (int16_t)x0; s_hit[id].y0 = (int16_t)y0;
    s_hit[id].x1 = (int16_t)x1; s_hit[id].y1 = (int16_t)y1;
}

/* 建议值缓存：render 算一次，HIT_APPLY 直接用。重算一遍等于把选点判据抄成
 * 两份，改一处漏一处只是时间问题。0 = 当前没有可采用的建议。 */
static int s_best_permille;

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

/* 画一个按钮并登记命中区；返回按钮右边界，方便横向串排。 */
static int button(uint16_t *fb, hit_id_t id, int x, int y, int w,
                  const char *label, bool hot, bool enabled)
{
    const int h = 34;
    fill_rect(fb, x, y, x + w, y + h, hot ? COL_BTN_HOT : COL_BTN);
    const int tw = pk_aa_text_width(label, PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x + (w - tw) / 2,
               y + (h - pk_aa_cell_h(PK_AA_S)) / 2, label,
               enabled ? COL_VAL : COL_DIM, PK_AA_S);
    /* 禁用态不登记命中区：画成灰的却还能点，比不画还糟。 */
    hit_set(id, enabled ? x : 0, enabled ? y : 0,
            enabled ? x + w : 0, enabled ? y + h : 0);
    return x + w;
}

/*
 * 从结果表挑建议点。判据与 RP2040 侧 threshold_sweep.h 完全同源：有帧按帧选、
 * 帧全 0 退回 pre、两者都全 0 报"没有"。
 *
 * 为什么 P4 这边要再算一次而不是让 RP2040 把结论发过来：RP2040 的结论是在
 * 它自己那一轮结束时算的，而这一页要在**扫描进行中**就显示当前最好的点
 * （一轮 90 秒，不能让人盯着空白等）。判据同源但时机不同。
 */
static int pick_best(const pk_sweep_row_t *r, int n)
{
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!r[i].frames) continue;
        if (best < 0 || r[i].frames > r[best].frames ||
            (r[i].frames == r[best].frames && r[i].pre > r[best].pre))
            best = i;
    }
    if (best >= 0) return best;
    for (int i = 0; i < n; i++) {
        if (!r[i].pre) continue;
        if (best < 0 || r[i].pre > r[best].pre) best = i;
    }
    return best;
}

void pk_ant_adv_page_render(uint16_t *fb)
{
    memset(s_hit, 0, sizeof s_hit);
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    /* ── 顶栏 ── */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 20, PK_UI_TITLE_Y,
               pk_i18n_text(PK_TR_ANT_ADV_TITLE), COL_TITLE, PK_AA_L);
    button(fb, HIT_BACK, PK_DISPLAY_W - 110, 7, 90,
           pk_i18n_text(PK_TR_ANT_ADV_BACK), false, true);
    fill_rect(fb, 0, HDR_H, PK_DISPLAY_W, HDR_H + 1, COL_LINE);

    char buf[96];
    const int th = pk_thresh_1090_get();          /* 0 = 用固件默认 */
    const bool running = pk_sweep_running();

    /* ── 门限行 ── */
    int y = HDR_H + 14;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 20, y + 8,
               pk_i18n_text(PK_TR_ANT_ADV_THRESHOLD), COL_KEY, PK_AA_M);

    int x = 190;
    x = button(fb, HIT_DEC100, x, y, 56, "-100", false, true) + 6;
    x = button(fb, HIT_DEC10,  x, y, 48, "-10",  false, true) + 10;

    if (th > 0) snprintf(buf, sizeof buf, "%d‰", th);
    else        snprintf(buf, sizeof buf, "%s", pk_i18n_text(PK_TR_ANT_ADV_AUTO));
    const int vw = pk_aa_text_width(buf, PK_AA_L);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x + (96 - vw) / 2, y + 2, buf,
               COL_VAL, PK_AA_L);
    x += 96 + 10;

    x = button(fb, HIT_INC10,  x, y, 48, "+10",  false, true) + 6;
    x = button(fb, HIT_INC100, x, y, 56, "+100", false, true) + 16;
    button(fb, HIT_DEFAULT, x, y, 82, pk_i18n_text(PK_TR_ANT_ADV_AUTO),
           th == 0, true);

    /* ── 用法说明 ──
     * 第一版没有这一句，用户原话是"完全没搞懂怎么用"：页面上只有一排按钮和
     * 一张空表，没有任何地方说这是在干什么、要等多久。 */
    y += 40;
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 20, y,
               pk_i18n_text(PK_TR_ANT_ADV_USAGE), COL_DIM, PK_AA_XS);

    /* ── 扫频行 ── */
    y += 22;
    pk_sweep_row_t rows[PK_SWEEP_MAX_ROWS];
    int total = 0;
    const int n = pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, &total);

    if (running) {
        button(fb, HIT_SWEEP, 20, y, 150, pk_i18n_text(PK_TR_ANT_ADV_STOP),
               true, true);
        /* 进度条 + n/total。整轮 90 秒，没有可见的进度就跟死机没区别；
         * total 要等第一行回来才知道，在那之前按 9 档画满格底槽即可。 */
        const int px0 = 186, pw = 300, ph = 10;
        const int py = y + (34 - ph) / 2;
        const int tot = total > 0 ? total : 9;
        fill_rect(fb, px0, py, px0 + pw, py + ph, COL_BTN);
        if (n > 0) {
            int w = pw * n / tot;
            if (w > pw) w = pw;
            fill_rect(fb, px0, py, px0 + w, py + ph, COL_WARN);
        }
        snprintf(buf, sizeof buf, "%s %d/%d",
                 pk_i18n_text(PK_TR_ANT_ADV_SWEEP_RUNNING), n, tot);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, px0 + pw + 12, y + 9, buf,
                   COL_WARN, PK_AA_S);
    } else {
        button(fb, HIT_SWEEP, 20, y, 150, pk_i18n_text(PK_TR_ANT_ADV_SWEEP),
               false, true);
    }

    /* ── 建议值行 ──
     * 独占一行而不是挤在扫频键右边：扫描中那里被进度条占着，两者叠在一起
     * 谁都读不清。扫描中也算建议值，让人提前看出趋势、不必干等 90 秒。 */
    y += 42;
    const int b = pick_best(rows, n);
    s_best_permille = 0;
    if (n > 0 && b < 0) {
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 20, y + 6,
                   pk_i18n_text(PK_TR_ANT_ADV_NO_SIGNAL), COL_WARN, PK_AA_S);
    } else if (b >= 0) {
        s_best_permille = rows[b].permille;
        snprintf(buf, sizeof buf, "%s %d‰  (frm=%lu pre=%lu)",
                 pk_i18n_text(PK_TR_ANT_ADV_BEST), rows[b].permille,
                 (unsigned long)rows[b].frames, (unsigned long)rows[b].pre);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 20, y + 6, buf,
                   COL_BEST, PK_AA_S);
        /* 帧全 0 时建议值是按 pre 挑的，够用来定位但不足以拍板 —— 采用键
         * 照给，但上面那行已经用 frm=0 把出处摆明了。 */
        button(fb, HIT_APPLY, PK_DISPLAY_W - 110, y - 4, 90,
               pk_i18n_text(PK_TR_ANT_ADV_APPLY), false, true);
    }

    /* ── 结果表 ── */
    y = TBL_TOP;
    fill_rect(fb, 20, y - 6, PK_DISPLAY_W - 20, y - 5, COL_LINE);
    static const int COLX[] = { 40, 120, 220, 320, 470, 640 };
    /* 单位是**千分比**（0..1000），不是百分比——用 % 会差 10 倍。字面量
     * 直接写 UTF-8 的 ‰：pk_aa_puts 按码点查表，而该码点已随 catalog
     * 的 ANT_ADV_USAGE 收进字库。原来这里写的是 "%%"，那是 printf 的
     * 转义写法，直接绘制时屏上老老实实画了两个百分号。 */
    static const char *HEAD[] = { "‰", "mV", "FRM", "PRE", "NOISE", "OVR" };
    for (int i = 0; i < 6; i++)
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COLX[i], y, HEAD[i],
                   COL_KEY, PK_AA_XS);
    y += 20;

    for (int i = 0; i < n && y + ROW_H < FOOT_TOP; i++, y += ROW_H) {
        const uint16_t c = (i == b) ? COL_BEST
                         : rows[i].frames ? COL_OK : COL_DIM;
        snprintf(buf, sizeof buf, "%d", rows[i].permille);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COLX[0], y, buf, c, PK_AA_S);
        snprintf(buf, sizeof buf, "%u", rows[i].level_mv);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COLX[1], y, buf, c, PK_AA_S);
        snprintf(buf, sizeof buf, "%lu", (unsigned long)rows[i].frames);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COLX[2], y, buf, c, PK_AA_S);
        snprintf(buf, sizeof buf, "%lu", (unsigned long)rows[i].pre);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COLX[3], y, buf, c, PK_AA_S);
        snprintf(buf, sizeof buf, "%lu", (unsigned long)rows[i].noise);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COLX[4], y, buf, c, PK_AA_S);
        snprintf(buf, sizeof buf, "%lu", (unsigned long)rows[i].ovr);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, COLX[5], y, buf, c, PK_AA_S);
    }

    /* ── 底部提示 ── */
    fill_rect(fb, 0, FOOT_TOP, PK_DISPLAY_W, FOOT_TOP + 1, COL_LINE);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 20, FOOT_TOP + 9,
               pk_i18n_text(PK_TR_ANT_ADV_HINT), COL_DIM, PK_AA_S);
}

/* 门限步进。夹在 10..1000：0 另有含义（用固件默认），只能由「默认」键进入，
 * 不能靠一路减到底撞上去——那样用户会以为自己设了个极低门限，实际拿到的是
 * 固件默认值，两者行为天差地别。 */
static void bump(int delta)
{
    int th = pk_thresh_1090_get();
    if (th <= 0) th = 600;                  /* 从"默认"起步时给个居中的锚点 */
    th += delta;
    if (th < 10)   th = 10;
    if (th > 1000) th = 1000;
    pk_thresh_1090_set(th);
}

bool pk_ant_adv_page_touch(int x, int y)
{
    for (int id = 1; id < HIT_N; id++) {
        if (s_hit[id].x1 <= s_hit[id].x0) continue;      /* 未登记/禁用 */
        if (x < s_hit[id].x0 || x >= s_hit[id].x1 ||
            y < s_hit[id].y0 || y >= s_hit[id].y1) continue;

        switch ((hit_id_t)id) {
        case HIT_BACK:    pk_ui_set_mode(PK_UI_MODE_SETTINGS); break;
        case HIT_DEC100:  bump(-100); break;
        case HIT_DEC10:   bump(-10);  break;
        case HIT_INC10:   bump(+10);  break;
        case HIT_INC100:  bump(+100); break;
        case HIT_DEFAULT: pk_thresh_1090_set(0); break;
        case HIT_SWEEP:
            /* 同一个键兼作启停，按当前状态决定语义——省一个键，且"扫描中"
             * 时它显示的就是「停止」，不会点错。 */
            if (pk_sweep_running()) pk_sweep_stop();
            else                    pk_sweep_start();
            break;
        case HIT_APPLY:
            if (s_best_permille > 0) pk_thresh_1090_set(s_best_permille);
            break;
        default: break;
        }
        return true;
    }
    /* 右侧 FAB 那条竖带必须放行，否则这页就切不走了（列表页正是栽在这里）。 */
    return false;
}
