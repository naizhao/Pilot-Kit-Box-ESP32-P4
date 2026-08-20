/*
 * apt_detail_page.c — 见 apt_detail_page.h。
 *
 * 版面（800×480）
 * ---------------
 *   y=0   ┌────────┬──────────────────────────────┬──────────────┐
 *         │ BACK   │ ZGGG  CAN                    │ SHOW ON MAP  │ 48
 *   y=48  ├────────┴──────────────────────────────┴──────────────┤
 *         │ Guangzhou Baiyun International Airport                │
 *         │ Guangzhou  CN                                         │ 96
 *         │ [ELEV 50 ft] [CONTROLLED] [12.3NM 045]                │
 *         ├───────────────────────────────────────────────────────┤
 *         │ RUNWAYS                                               │ 28
 *         │ 02L   12467 x 197 ft                                  │ 56
 *         │       ASPH   MAG 022   THR                            │
 *         │ …                                                     │
 *         │ FREQUENCIES                                           │ 28
 *         │ [TWR] GUANGZHOU TOWER              118.250            │ 44
 *   y=480 └───────────────────────────────────────────────────────┘
 *
 * 为什么行高不按 9 mm 触摸下限来：这一页的列表行**不是触摸目标**。全页只有
 * 三个可点的东西（BACK / SHOW ON MAP / 滚动），它们各自够大。把跑道行也撑到
 * 76 px 只会让 ZGGG 那 10 条方向占掉 760 px、白白多滚一屏。
 *
 * 为什么跑道行是两行：一条方向要同时说清楚"多长多宽""什么道面""磁航向多少"
 * 四件事，挤一行的话每项只剩 190 px，道面那个词就得缩写成两个字母。分两行
 * 之后第一行是扫视用的（跑道号 + 尺寸），第二行是核对用的。
 *
 * 触摸与 search_page.c 同一套约定：按下只记起点，位移超阈值才算拖动，松手时
 * 没拖过才当点击。三页共用同一套判定，手感才一致。本页没有行级命中表——
 * 列表行不可点，也就不需要每帧把几何留下来。
 */
#include "apt_detail_page.h"

#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * 纯函数区（无 OS / 无全局状态）——host 单测直接把本文件拉进翻译单元。
 * ═══════════════════════════════════════════════════════════════════ */

/* SERVICE_ENUM / SURFACE_ENUM 的取值照抄 Pilot-Kit 仓
 * scripts/aero_data_pipeline/export_box_bin.py（与 pk_aero_layer.c 顶部那张
 * AIRPORT_TYPE_ENUM / CTRL_ENUM 表同源：**生成器改了这里就得跟着改，
 * 没有第三方能替我们发现**）。 */
#define SVC_TWR       1
#define SVC_GND       2
#define SVC_APP       3
#define SVC_DEP       4
#define SVC_ATIS      5
#define SVC_CTAF      6
#define SVC_UNICOM    7
#define SVC_CD        8
#define SVC_AWOS      9
#define SVC_RADIO    10
#define SVC_AFIS     11
#define SVC_INFO     12
#define SVC_FSS      13
#define SVC_RADAR    14
#define SVC_VOLMET   15
#define SVC_MULTICOM 16
#define SVC_EMERG    17
#define SVC_FIS      18
#define SVC_ARR      19
#define SVC_STAR     20

pk_ui_modal_t pk_ui_modal_top(bool navgrid_active, bool keyboard_active,
                              bool detail_active, bool search_active)
{
    if (navgrid_active)  return PK_UI_MODAL_NAVGRID;
    if (keyboard_active) return PK_UI_MODAL_KEYBOARD;
    if (detail_active)   return PK_UI_MODAL_DETAIL;
    if (search_active)   return PK_UI_MODAL_SEARCH;
    return PK_UI_MODAL_NONE;
}

bool pk_ui_fab_hidden_for(pk_ui_modal_t top)
{
    return top != PK_UI_MODAL_NONE;
}

pk_sheet_state_t pk_sheet_next(pk_sheet_state_t st, pk_sheet_ev_t ev)
{
    switch (ev) {
    case PK_SHEET_EV_OPEN:
        /* 三个来态都落到 OPEN。COLLAPSED 那一条是"恢复旧内容"而不是"重开"，
         * 差别不在这个返回值里，在调用方要不要重置查询串——见 search_page.c
         * 的 pk_search_page_open()。 */
        return PK_SHEET_OPEN;
    case PK_SHEET_EV_COLLAPSE:
        /* 没开过的收不起来：不然从地图直接进详情、再「在地图上显示」，
         * 会给一枚返回钮把从未打开过的搜索页"恢复"出来。 */
        return (st == PK_SHEET_OPEN) ? PK_SHEET_COLLAPSED : st;
    case PK_SHEET_EV_RESTORE:
        return (st == PK_SHEET_COLLAPSED) ? PK_SHEET_OPEN : st;
    case PK_SHEET_EV_CLOSE:
    default:
        return PK_SHEET_CLOSED;
    }
}

bool pk_sheet_may_requery(pk_sheet_state_t st)
{
    return st == PK_SHEET_OPEN;
}

uint8_t pk_apt_freq_rank(uint8_t service)
{
    switch (service) {
    case SVC_TWR:  return 0;
    case SVC_GND:  return 1;
    case SVC_ATIS: return 2;
    case SVC_APP:  return 3;
    case SVC_DEP:  return 4;
    case SVC_AFIS: return 5;
    default:       return 99;
    }
}

void pk_apt_freq_sort(pk_apt_freq_item_t *arr, int n)
{
    if (arr == NULL || n < 2) return;
    for (int i = 1; i < n; ++i) {
        const pk_apt_freq_item_t key = arr[i];
        const uint8_t kr = pk_apt_freq_rank(key.service);
        int j = i - 1;
        /* 严格大于才往后挪 = 稳定排序：权重相等时 key 停在原有元素之后，
         * 存储序原样保住（主用/备用频率的先后含义不能被打乱）。 */
        while (j >= 0 && pk_apt_freq_rank(arr[j].service) > kr) {
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
}

const char *pk_apt_service_tag(uint8_t service)
{
    switch (service) {
    case SVC_TWR:      return "TWR";
    case SVC_GND:      return "GND";
    case SVC_APP:      return "APP";
    case SVC_DEP:      return "DEP";
    case SVC_ATIS:     return "ATIS";
    case SVC_CTAF:     return "CTAF";
    case SVC_UNICOM:   return "UNICOM";
    case SVC_CD:       return "CD";
    case SVC_AWOS:     return "AWOS";
    case SVC_RADIO:    return "RADIO";
    case SVC_AFIS:     return "AFIS";
    case SVC_INFO:     return "INFO";
    case SVC_FSS:      return "FSS";
    case SVC_RADAR:    return "RADAR";
    case SVC_VOLMET:   return "VOLMET";
    case SVC_MULTICOM: return "MULTI";   /* MULTICOM 6 字符顶破 60 px 徽章 */
    case SVC_EMERG:    return "EMERG";
    case SVC_FIS:      return "FIS";
    case SVC_ARR:      return "ARR";
    case SVC_STAR:     return "STAR";
    default:           return "---";
    }
}

const char *pk_apt_surface_tag(uint8_t surface)
{
    switch (surface) {
    case 1: return "ASPH";
    case 2: return "CONC";
    case 3: return "GRASS";
    case 4: return "GRAVEL";
    case 5: return "SOIL";
    case 6: return "SAND";
    case 7: return "WATER";
    case 8: return "SNOW";
    case 9: return "ICE";
    default: return NULL;    /* 未知：整项不画，见头文件 */
    }
}

void pk_apt_format_freq(char *out, size_t cap, uint32_t freq_khz)
{
    if (out == NULL || cap == 0) return;
    /* 整数分解而不是 %.3f：118275 kHz 转成 double 再格式化会落在
     * 118.27499999… 上，靠 printf 的舍入侥幸补回来。频率是要照着念进
     * 无线电的数，不留这种侥幸。 */
    snprintf(out, cap, "%lu.%03lu",
             (unsigned long)(freq_khz / 1000u), (unsigned long)(freq_khz % 1000u));
}

bool pk_apt_rwy_bearing_deg(const pk_apt_rwy_item_t *r, int *out_deg)
{
    if (r == NULL || !r->has_bearing) return false;
    /* 0.1° → 整度，四舍五入后归一到 0..359（360.0 要回到 0，不是画成 "360"）。*/
    int d = ((int)r->mag_bearing_dd + 5) / 10;
    d %= 360;
    if (out_deg) *out_deg = d;
    return true;
}

#ifndef PK_APT_DETAIL_HOST_TEST

/* ═══════════════════════════════════════════════════════════════════
 * 平台区：打开时取数 + 渲染 + 触摸
 * ═══════════════════════════════════════════════════════════════════ */

#include "display.h"
#include "geo.h"
#include "i18n.h"
#include "keyboard_page.h"
#include "map_page.h"
#include "nav_grid_page.h"
#include "own_ship.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "pk_aero_db.h"
#include "pk_ui_nav.h"
#include "search_page.h"
#include "ui_state.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/*
 * FAB 显隐的唯一入口（见 apt_detail_page.h 里这个函数的声明）。
 *
 * 落在本文件是因为模态次序的真源 pk_ui_modal_top() 就在这里，两者是同一条
 * 规则的两个面：谁在最上面决定「点得中谁」，有没有人在上面决定「FAB 露不露」。
 */
void pk_ui_fab_sync(void)
{
    pk_ui_nav_set_fab_hidden(pk_ui_fab_hidden_for(
        pk_ui_modal_top(pk_nav_grid_page_active(),
                        pk_keyboard_page_active(),
                        pk_apt_detail_page_active(),
                        pk_search_page_active())));
}

/* 收起态的现场版，落在本文件的理由同上：模态栈的真源在这里。 */
bool pk_ui_sheet_has_collapsed(void)
{
    return pk_search_page_collapsed() || pk_apt_detail_page_collapsed();
}

void pk_ui_sheet_restore(void)
{
    /* 先搜索后详情只是书写顺序：两者互不影响，最终由 pk_ui_modal_top
     * 决定屏上是谁（详情在搜索之上）。 */
    pk_search_page_restore();
    pk_apt_detail_page_restore();
    pk_ui_fab_sync();
}

#ifndef PK_SIM_BUILD
#include "esp_log.h"
static const char *TAG = "aptdetail";
#endif

/* ── 版面常量 ───────────────────────────────────────────────────── */

#define APTD_HDR_H      PFD_BAR_BOT                 /* 48，与各页顶栏同高 */
#define APTD_LIST_TOP   APTD_HDR_H
#define APTD_VIEW_H     (PK_DISPLAY_H - APTD_LIST_TOP)   /* 432 */

#define APTD_PAD        PK_UI_PAD_L                 /* 16 */
#define APTD_R          (PK_DISPLAY_W - PK_UI_PAD_L) /* 784 */

/* 页首两枚按钮。视觉 40 px 高、命中放宽到整条 48 px 的带——同 search_page
 * 「视觉按 spec、命中放宽到整行」那条规矩。 */
#define APTD_BTN_H      40
#define APTD_BTN_Y0     ((APTD_HDR_H - APTD_BTN_H) / 2)
#define APTD_BACK_W     110
#define APTD_BACK_X0    APTD_PAD
#define APTD_BACK_HIT_X1 (APTD_BACK_X0 + APTD_BACK_W + 30)  /* 手指比按钮胖 */
#define APTD_MAP_W      190
#define APTD_MAP_X0     (APTD_R - APTD_MAP_W)
#define APTD_MAP_HIT_X0 (APTD_MAP_X0 - 30)

/* 头部信息块 */
#define APTD_HEAD_H     96
#define APTD_CHIP_H     28
#define APTD_CHIP_GAP   10

/* 分组标题、两种列表行 */
#define APTD_SEC_H      28
#define APTD_RWY_H      56
#define APTD_FREQ_H     44

/* 跑道行的列位：跑道号一列，其余三项在第二行等距排开。x 值按最坏内容量出来
 * ——"GRAVEL" 6 字符 60 px、"MAG 359" 7 字符 70 px，两列之间留 40 px 以上
 * 才不会读成一个词。 */
#define APTD_RWY_COL_X   130
#define APTD_RWY_SURF_X  APTD_RWY_COL_X
#define APTD_RWY_MAG_X   (APTD_RWY_COL_X + 130)
#define APTD_RWY_THR_X   (APTD_RWY_MAG_X + 130)

/* 频率行：服务徽章沿用搜索页那枚类型徽章的形状与高度，宽度按**最长的服务
 * 缩写**放大：UNICOM 6 个 PK_AA_XS 字符 = 60 px，68 px 的胶囊左右各只剩
 * 4 px，屏上是"字贴着圆角边"。76 给到左右各 8 px，与其它胶囊的内边距一致。 */
#define APTD_BADGE_W    76
#define APTD_BADGE_H    26
#define APTD_TEXT_X     (APTD_PAD + APTD_BADGE_W + 16)

/* 跳地图时用哪一档 zoom。与 search_page 的 SRCH_GOTO_ZOOM 同值——同一个
 * 动作（把视口挪到一个点并落 PIN）在两个入口上必须落到同一档，否则"从搜索
 * 跳过去"和"从详情跳过去"看到的是两张不一样的图。 */
#define APTD_GOTO_ZOOM  11

#define APTD_DRAG_SLOP  12

/* ── 屏上文案（全部走 i18n catalog）────────────────────────────────
 * 词条与翻译理由写在 firmware/scripts/i18n_catalog.py 的「机场详情页」那一段
 * （含宽度账与"哪些缩写不译"的清单）。这里只留取词的短别名。 */
#define TXT_BACK        pk_i18n_text(PK_TR_APTD_BACK)
#define TXT_SHOW_MAP    pk_i18n_text(PK_TR_APTD_SHOW_ON_MAP)
#define TXT_SEC_RWY     pk_i18n_text(PK_TR_APTD_SEC_RUNWAYS)
#define TXT_SEC_FREQ    pk_i18n_text(PK_TR_APTD_SEC_FREQ)
#define TXT_ELEV        pk_i18n_text(PK_TR_APTD_ELEV)
#define TXT_CTRL        pk_i18n_text(PK_TR_APTD_CTRL)
#define TXT_UNCTRL      pk_i18n_text(PK_TR_APTD_UNCTRL)
#define TXT_CTRL_UNK    pk_i18n_text(PK_TR_APTD_CTRL_UNKNOWN)
#define TXT_NO_RWY      pk_i18n_text(PK_TR_APTD_NO_RUNWAY)
#define TXT_NO_FREQ     pk_i18n_text(PK_TR_APTD_NO_FREQ)
#define TXT_UNAVAIL     pk_i18n_text(PK_TR_APTD_UNAVAILABLE)

/* ── 页面数据（打开那一刻拷进来，之后每帧只读）────────────────────
 *
 * 全部进 PSRAM：内部 .bss 是本项目的硬约束（见 map_page.c 里那段链接顺序的
 * 记录，以及 firmware/scripts/check_early_heap.py）。本块约 3.3 KB，正是
 * "只有 UI 线程碰、且只在页面打开时碰"的冷数据，该挪出去的那一类。 */
typedef struct {
    bool     valid;                 /* false = 库没就绪 / 下标越界 */
    char     icao[8], iata[8], country[4];
    char     name[PK_APT_DETAIL_NAME_MAX];
    char     city[PK_APT_DETAIL_NAME_MAX];
    int16_t  elev_ft;
    uint8_t  ctrl;
    double   lat, lon;
    bool     have_dist;             /* 无本机位置时整枚胶囊**不显示** */
    double   dist_nm, brg_deg;
    int      n_rwy, n_freq;
    /* 库里真实有多少条（可能 > 上限）。屏上不显示这两个数，日志与自检用——
     * 有一天真出现 65 条频率的机场，日志里会看得见被截断了。 */
    uint32_t total_rwy, total_freq;
    pk_apt_rwy_item_t  rwy[PK_APT_DETAIL_RWY_MAX];
    pk_apt_freq_item_t freq[PK_APT_DETAIL_FREQ_MAX];
} detail_t;

EXT_RAM_BSS_ATTR static detail_t s_d;

/* 三态可见性，取代原来的 `static bool s_active`（见 apt_detail_page.h 的
 * pk_sheet_state_t）。s_d 里那份取好的跑道/频率快照在 COLLAPSED 期间原样
 * 留着——它本来就是打开那一刻拷贝下来的值，不含任何指向 PSRAM 池的指针，
 * 拔卡也不会悬空。 */
static uint8_t s_vis = PK_SHEET_CLOSED;
static uint8_t s_from = PK_APT_DETAIL_FROM_MAP;
static int  s_scroll;
static int  s_content_h;

static int  s_press_x, s_press_y, s_press_scroll;
static bool s_press_valid, s_moved;

/* ── 配色（与搜索页/叠加层同一套语言）───────────────────────────── */

static uint16_t col_bg(void)     { return pk_rgb565(  7,  10,  16); }
static uint16_t col_panel(void)  { return pk_rgb565( 28,  36,  48); }
static uint16_t col_txt(void)    { return pk_rgb565(235, 240, 248); }
static uint16_t col_dim(void)    { return pk_rgb565(120, 130, 145); }
static uint16_t col_sub(void)    { return pk_rgb565(170, 182, 200); }
static uint16_t col_line(void)   { return pk_rgb565( 26,  33,  44); }
static uint16_t col_amber(void)  { return pk_rgb565(255, 176,   0); }
/* 机场蓝：与地图叠加层的管制机场符号、搜索页的 APT 徽章同一个蓝
 * （60,130,235）。三处一致，用户才会把"蓝 = 机场"当成一条规则记住。 */
static uint16_t col_apt(void)    { return pk_rgb565( 60, 130, 235); }

/* 频率徽章的底色：**只分两档**。塔台/地面/放行这类"要照着念进无线电的"
 * 用机场蓝，其余（ATIS/AWOS/情报）用灰蓝。不给每种服务各配一个颜色——
 * 20 种服务上 20 种颜色，RGB565 上根本分不开，反而把"哪几条要紧"这个
 * 唯一有用的区分冲掉了。 */
static uint16_t col_freq_badge(uint8_t service)
{
    switch (service) {
    case SVC_TWR: case SVC_GND: case SVC_CD: case SVC_APP: case SVC_DEP:
    case SVC_CTAF: case SVC_UNICOM:
        return col_apt();
    default:
        return pk_rgb565(70, 84, 104);
    }
}

/* ═══════════════ 取数（打开那一刻跑一次）═══════════════════════ */

/* 名称拷贝 + 超长截断，写法同 search_page.c 的 copy_name（那边有一段更长的
 * 论证：硬切会在屏上留下半截词，读的人分不清是名字长还是渲染坏了）。 */
static void copy_name(char *dst, size_t cap, const char *src)
{
    if (src == NULL) src = "";
    const size_t n = strlen(src);
    if (n < cap) { memcpy(dst, src, n + 1); return; }
    memcpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    if (cap >= 4) memcpy(dst + cap - 4, "...", 4);
}

static void load(uint32_t apt_idx)
{
    memset(&s_d, 0, sizeof(s_d));

    pk_aero_airport_t a;
    if (pk_aero_db_state() != PK_AERO_DB_READY ||
        !pk_aero_db_airport_get(apt_idx, &a))
        return;                                  /* valid 保持 false */

    /* 字符串一律当场拷走：a.name / a.city 指向 PSRAM 池，拔卡即悬空
     * （pk_aero_db.h:23-25）。这一条是本页全部 snprintf 的理由。 */
    snprintf(s_d.icao, sizeof(s_d.icao), "%s", a.icao);
    snprintf(s_d.iata, sizeof(s_d.iata), "%s", a.iata);
    snprintf(s_d.country, sizeof(s_d.country), "%s", a.country);
    copy_name(s_d.name, sizeof(s_d.name), a.name);
    copy_name(s_d.city, sizeof(s_d.city), a.city);
    s_d.elev_ft = a.elev_ft;
    s_d.ctrl    = a.ctrl;
    s_d.lat     = a.lat;
    s_d.lon     = a.lon;

    /* 距离/方位：没有本机位置时整枚胶囊不画，不是画 0.0NM——0 NM 会被读成
     * "就在脚下"（同 search_page 那一列的处置）。 */
    aircraft_t own;
    pk_own_src_t src;
    if (pk_own_ship_resolve(esp_timer_get_time(),
                            (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL,
                            &own, &src)) {
        s_d.have_dist = true;
        geo_dist_brg(own.lat, own.lon, a.lat, a.lon, &s_d.dist_nm, &s_d.brg_deg);
    }

    uint32_t first = 0, count = 0;
    if (pk_aero_db_airport_runways(apt_idx, &first, &count)) {
        s_d.total_rwy = count;
        for (uint32_t i = 0; i < count && s_d.n_rwy < PK_APT_DETAIL_RWY_MAX; ++i) {
            pk_aero_rwy_dir_t r;
            if (!pk_aero_db_rwy_dir_get(first + i, &r)) continue;
            pk_apt_rwy_item_t *e = &s_d.rwy[s_d.n_rwy++];
            snprintf(e->desig, sizeof(e->desig), "%s", r.designator);
            e->length_ft      = r.length_ft;
            e->width_ft       = r.width_ft;
            e->surface        = r.surface;
            e->has_bearing    = r.has_bearing;
            e->mag_bearing_dd = r.mag_bearing_dd;
            e->has_coord      = r.has_coord;
        }
    }

    if (pk_aero_db_airport_freqs(apt_idx, &first, &count)) {
        s_d.total_freq = count;
        for (uint32_t i = 0; i < count && s_d.n_freq < PK_APT_DETAIL_FREQ_MAX; ++i) {
            pk_aero_freq_t f;
            if (!pk_aero_db_freq_get(first + i, &f)) continue;
            pk_apt_freq_item_t *e = &s_d.freq[s_d.n_freq++];
            e->freq_khz = f.freq_khz;
            e->service  = f.service;
            snprintf(e->callsign, sizeof(e->callsign), "%s",
                     f.callsign ? f.callsign : "");
        }
        /* 业务序**在这里排一次**，不是每帧排：排序结果不随时间变，而这一页
         * 每帧都要重画 63 行。 */
        pk_apt_freq_sort(s_d.freq, s_d.n_freq);
    }

    s_d.valid = true;
}

/* ═══════════════ 绘制 ═══════════════════════════════════════════ */

/* 一枚按钮：圆角底 + 居中文字。与 search_page.c 的 draw_button 同款
 * （那份是 static，照抄写法而不是导出——同一套视觉语言，各页各留一份）。 */
static void draw_button(uint16_t *fb, int x0, int y0, int w, int h,
                        const char *label, uint16_t bg, uint16_t fg,
                        pk_aa_size_t size)
{
    pk_pfd_fill_round_rect(fb, x0, y0, x0 + w, y0 + h, 10, bg);
    const int tw = pk_aa_text_width(label, size);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + (w - tw) / 2, y0 + (h - pk_aa_cell_h(size)) / 2,
               label, fg, size);
}

/* 一枚信息胶囊，返回它的宽度好让调用点把 x 推下去。 */
static int draw_chip(uint16_t *fb, int x, int y, const char *txt, uint16_t fg)
{
    const int tw = pk_aa_text_width(txt, PK_AA_XS);
    const int w  = tw + 24;
    pk_pfd_fill_round_rect(fb, x, y, x + w, y + APTD_CHIP_H,
                           APTD_CHIP_H / 2, col_panel());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x + 12,
               y + (APTD_CHIP_H - PK_AA_XS_H) / 2, txt, fg, PK_AA_XS);
    return w;
}

/* 分组标题。几何与 search_page.c 的 draw_section 一致。 */
static int draw_section(uint16_t *fb, int y, const char *label)
{
    if (y > APTD_LIST_TOP - APTD_SEC_H && y < PK_DISPLAY_H) {
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_PAD,
                   y + (APTD_SEC_H - PK_AA_XS_H) / 2, label, col_dim(),
                   PK_AA_XS);
        pk_pfd_fill_rect(fb, APTD_PAD, y + APTD_SEC_H - 1, APTD_R,
                         y + APTD_SEC_H, col_line());
    }
    return APTD_SEC_H;
}

static int draw_empty(uint16_t *fb, int y, const char *title, uint16_t col)
{
    const int h = 64;
    if (y > APTD_LIST_TOP - h && y < PK_DISPLAY_H)
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_PAD, y + 20, title,
                   col, PK_AA_S);
    return h;
}

static int draw_head(uint16_t *fb, int y)
{
    if (y <= APTD_LIST_TOP - APTD_HEAD_H || y >= PK_DISPLAY_H)
        return APTD_HEAD_H;

    if (s_d.name[0])
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_PAD, y + 4, s_d.name,
                   col_txt(), PK_AA_S);

    /* 城市与国家拼成一行。两者都可能缺（真库里 city 的缺失率不低），
     * 缺哪个就少哪一段，不留占位符——"—, CN" 读起来像数据坏了。 */
    {
        char sub[PK_APT_DETAIL_NAME_MAX + 8];
        if (s_d.city[0] && s_d.country[0])
            snprintf(sub, sizeof(sub), "%s  %s", s_d.city, s_d.country);
        else
            snprintf(sub, sizeof(sub), "%s",
                     s_d.city[0] ? s_d.city : s_d.country);
        if (sub[0])
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_PAD, y + 30, sub,
                       col_sub(), PK_AA_XS);
    }

    int x = APTD_PAD;
    const int cy = y + 58;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %d ft", TXT_ELEV, (int)s_d.elev_ft);
    x += draw_chip(fb, x, cy, buf, col_txt()) + APTD_CHIP_GAP;

    /* 管制三态。CTRL_ENUM：1=uncontrolled 2=controlled，其余（含 0）= 未知。
     * 未知不并进"无管制"——那是替飞行员做一个他没授权的判断。 */
    {
        const char *w = (s_d.ctrl == 2) ? TXT_CTRL
                      : (s_d.ctrl == 1) ? TXT_UNCTRL : TXT_CTRL_UNK;
        x += draw_chip(fb, x, cy, w,
                       (s_d.ctrl == 2) ? col_apt() : col_sub()) + APTD_CHIP_GAP;
    }

    if (s_d.have_dist) {
        snprintf(buf, sizeof(buf), "%.1fNM %03d", s_d.dist_nm,
                 ((int)(s_d.brg_deg + 0.5) % 360 + 360) % 360);
        x += draw_chip(fb, x, cy, buf, col_txt());
    }
    (void)x;

    return APTD_HEAD_H;
}

static void draw_rwy_row(uint16_t *fb, int y, const pk_apt_rwy_item_t *r)
{
    if (y <= APTD_LIST_TOP - APTD_RWY_H || y >= PK_DISPLAY_H) return;

    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_PAD, y + 14,
               r->desig[0] ? r->desig : "--", col_txt(), PK_AA_M);

    /* 尺寸。**长宽各自可缺**（库里 0 = 无数据），所以三种组合各写一句，
     * 不做 "0 x 0 ft" 那种一定会出现在某个小场上的假数据。 */
    {
        char dim[40];
        if (r->length_ft && r->width_ft)
            snprintf(dim, sizeof(dim), "%u x %u ft",
                     (unsigned)r->length_ft, (unsigned)r->width_ft);
        else if (r->length_ft)
            snprintf(dim, sizeof(dim), "%u ft", (unsigned)r->length_ft);
        else
            dim[0] = '\0';
        if (dim[0])
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_RWY_COL_X, y + 8,
                       dim, col_txt(), PK_AA_S);
    }

    const char *surf = pk_apt_surface_tag(r->surface);
    if (surf != NULL)
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_RWY_SURF_X, y + 34,
                   surf, col_sub(), PK_AA_XS);

    int mag = 0;
    if (pk_apt_rwy_bearing_deg(r, &mag)) {
        char b[16];
        snprintf(b, sizeof(b), "MAG %03d", mag);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_RWY_MAG_X, y + 34, b,
                   col_sub(), PK_AA_XS);
    }

    /* 入口坐标：**约 81% 的跑道方向没有**，所以这里是"有才标"而不是
     * "没有就写无"——64 条里 52 条挂着"无坐标"只是噪音。有坐标意味着将来
     * 能对着入口做进近引导，值得一个记号。 */
    if (r->has_coord)
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_RWY_THR_X, y + 34,
                   "THR", col_amber(), PK_AA_XS);

    pk_pfd_fill_rect(fb, APTD_PAD, y + APTD_RWY_H - 1, APTD_R,
                     y + APTD_RWY_H, col_line());
}

static void draw_freq_row(uint16_t *fb, int y, const pk_apt_freq_item_t *f)
{
    if (y <= APTD_LIST_TOP - APTD_FREQ_H || y >= PK_DISPLAY_H) return;

    const char *tag = pk_apt_service_tag(f->service);
    const int by = y + (APTD_FREQ_H - APTD_BADGE_H) / 2;
    pk_pfd_fill_round_rect(fb, APTD_PAD, by, APTD_PAD + APTD_BADGE_W,
                           by + APTD_BADGE_H, APTD_BADGE_H / 2,
                           col_freq_badge(f->service));
    {
        const int tw = pk_aa_text_width(tag, PK_AA_XS);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   APTD_PAD + (APTD_BADGE_W - tw) / 2,
                   by + (APTD_BADGE_H - PK_AA_XS_H) / 2, tag,
                   pk_rgb565(10, 14, 20), PK_AA_XS);
    }

    char mhz[16];
    pk_apt_format_freq(mhz, sizeof(mhz), f->freq_khz);
    const int fw = pk_aa_text_width(mhz, PK_AA_M);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_R - fw,
               y + (APTD_FREQ_H - PK_AA_M_H) / 2, mhz, col_txt(), PK_AA_M);

    /* 呼号夹在徽章与频率之间，**按剩余宽度截**：一条 "GUANGZHOU APPROACH
     * SECTOR 3" 顶上去会盖住频率数字，而频率是这一行唯一不能读错的东西。 */
    if (f->callsign[0]) {
        const int avail = (APTD_R - fw - 16) - APTD_TEXT_X;
        char cs[PK_APT_DETAIL_CALL_MAX];
        snprintf(cs, sizeof(cs), "%s", f->callsign);
        while (cs[0] && pk_aa_text_width(cs, PK_AA_S) > avail)
            cs[strlen(cs) - 1] = '\0';
        if (cs[0])
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, APTD_TEXT_X,
                       y + (APTD_FREQ_H - PK_AA_S_H) / 2, cs, col_sub(),
                       PK_AA_S);
    }

    pk_pfd_fill_rect(fb, APTD_PAD, y + APTD_FREQ_H - 1, APTD_R,
                     y + APTD_FREQ_H, col_line());
}

void pk_apt_detail_page_render(uint16_t *fb)
{
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg());

    int y = APTD_LIST_TOP - s_scroll;

    if (!s_d.valid) {
        y += draw_empty(fb, y, TXT_UNAVAIL, col_amber());
    } else {
        y += draw_head(fb, y);

        y += draw_section(fb, y, TXT_SEC_RWY);
        if (s_d.n_rwy == 0) {
            y += draw_empty(fb, y, TXT_NO_RWY, col_dim());
        } else {
            for (int i = 0; i < s_d.n_rwy; ++i) {
                draw_rwy_row(fb, y, &s_d.rwy[i]);
                y += APTD_RWY_H;
            }
        }

        y += draw_section(fb, y, TXT_SEC_FREQ);
        if (s_d.n_freq == 0) {
            y += draw_empty(fb, y, TXT_NO_FREQ, col_dim());
        } else {
            for (int i = 0; i < s_d.n_freq; ++i) {
                draw_freq_row(fb, y, &s_d.freq[i]);
                y += APTD_FREQ_H;
            }
        }
    }

    s_content_h = y + s_scroll - APTD_LIST_TOP;

    /* 滚动条：贴右缘，只在超出一屏时出现（几何照 diag_page.c:656）。
     * 这一页**必须有**：ZGGG 是 96+28+10×56+28+63×44 = 3484 px 的内容，
     * 八屏多。 */
    if (s_content_h > APTD_VIEW_H) {
        const int tx = PK_DISPLAY_W - 6;
        const int bar_h = APTD_VIEW_H * APTD_VIEW_H / s_content_h;
        const int bar_y = APTD_LIST_TOP + s_scroll * APTD_VIEW_H / s_content_h;
        pk_pfd_fill_rect(fb, tx, APTD_LIST_TOP, tx + 3, PK_DISPLAY_H,
                         pk_rgb565(30, 38, 50));
        pk_pfd_fill_rect(fb, tx, bar_y, tx + 3, bar_y + bar_h,
                         pk_rgb565(120, 135, 155));
    }

    /* 顶栏最后画：内容从它底下滑过去，而不是压在上面。 */
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, APTD_LIST_TOP, col_bg());
    draw_button(fb, APTD_BACK_X0, APTD_BTN_Y0, APTD_BACK_W, APTD_BTN_H,
                TXT_BACK, col_panel(), col_txt(), PK_AA_S);
    /* 取不到数据时**整枚按钮不画**：坐标也是数据的一部分，按下去无处可去。
     * 画一枚点了没反应的按钮，比少一枚按钮更让人怀疑机器坏了。命中那侧同样
     * 用 s_d.valid 挡着，两处判据是同一个。 */
    if (s_d.valid)
        draw_button(fb, APTD_MAP_X0, APTD_BTN_Y0, APTD_MAP_W, APTD_BTN_H,
                    TXT_SHOW_MAP, pk_rgb565(24, 52, 92), col_txt(), PK_AA_S);

    /* 标题 = 代码本身。ICAO 与 IATA 并排（IATA 是旅客那一侧的码，飞行员
     * 偶尔要对上），两个都没有就摆 "----"——留空看着像渲染坏了，同
     * search_page 结果行的处置。 */
    {
        char title[24];
        if (s_d.icao[0] && s_d.iata[0])
            snprintf(title, sizeof(title), "%s  %s", s_d.icao, s_d.iata);
        else if (s_d.icao[0] || s_d.iata[0])
            snprintf(title, sizeof(title), "%s",
                     s_d.icao[0] ? s_d.icao : s_d.iata);
        else
            snprintf(title, sizeof(title), "----");
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   APTD_BACK_X0 + APTD_BACK_W + 24, PK_UI_TITLE_Y, title,
                   PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
    }
}

/* ═══════════════ 打开 / 关闭 / 触摸 ═════════════════════════════ */

#ifdef PK_SIM_BUILD
static void sim_setup_once(void);
#endif

void pk_apt_detail_page_open(uint32_t apt_idx, pk_apt_detail_from_t from)
{
    s_scroll      = 0;
    s_press_valid = false;
    s_moved       = false;
    s_from        = (uint8_t)from;
    load(apt_idx);
    s_vis = (uint8_t)pk_sheet_next((pk_sheet_state_t)s_vis, PK_SHEET_EV_OPEN);
    /* 藏掉 FAB，理由与 search_page / keyboard_page 完全相同：本页铺满全屏、
     * 命中判定排在 LVGL 之前，FAB 留着就是"它自己点不动、又盖住底下的行"。
     * 出口写在屏上：页首的 BACK。s_vis 已经是 OPEN，sync 自己会算出"藏"。 */
    pk_ui_fab_sync();
#ifndef PK_SIM_BUILD
    ESP_LOGI(TAG, "open apt #%lu \"%s\": %d/%lu runways, %d/%lu freqs%s",
             (unsigned long)apt_idx, s_d.icao, s_d.n_rwy,
             (unsigned long)s_d.total_rwy, s_d.n_freq,
             (unsigned long)s_d.total_freq, s_d.valid ? "" : " (no data)");
#else
    sim_setup_once();
#endif
}

bool pk_apt_detail_page_active(void) { return s_vis == PK_SHEET_OPEN; }

bool pk_apt_detail_page_collapsed(void) { return s_vis == PK_SHEET_COLLAPSED; }

/* 收起：状态全留着，只是不再算活跃层。地图上那枚返回钮会把它拉回来。 */
void pk_apt_detail_page_collapse(void)
{
    s_vis = (uint8_t)pk_sheet_next((pk_sheet_state_t)s_vis, PK_SHEET_EV_COLLAPSE);
    s_press_valid = false;
    s_moved       = false;
    /* 不在这里 sync：collapse 通常与 search 的 collapse、set_mode 连着做，
     * 由那条动作收尾统一 sync 一次（goto_map / goto_item）。 */
}

void pk_apt_detail_page_restore(void)
{
    s_vis = (uint8_t)pk_sheet_next((pk_sheet_state_t)s_vis, PK_SHEET_EV_RESTORE);
    s_press_valid = false;
    s_moved       = false;
    /* 滚动位置**故意不重置**：用户翻到第 40 条频率、去地图看了一眼再回来，
     * 该停在他离开的地方。这正是 sheet 与"重新打开一页"的区别。 */
}

void pk_apt_detail_page_close(void)
{
    s_vis         = (uint8_t)pk_sheet_next((pk_sheet_state_t)s_vis,
                                           PK_SHEET_EV_CLOSE);
    s_press_valid = false;
    /*
     * 返回目标就在这一行里。
     *
     * 从搜索进来的话，搜索页从头到尾就没关过（模态层是"盖住"不是"切走"），
     * 分派次序一放开它就重新露出来——FAB 必须**继续藏着**，否则搜索页上会
     * 冒出一枚点不动的悬浮球。从地图进来的则要把 FAB 放回去。
     *
     * 2026-08-04：这一行原来手算成 `s_from == PK_APT_DETAIL_FROM_SEARCH`，
     * 改走 pk_ui_fab_sync()。opener 与"此刻谁还活着"两者在这里恰好等价，但
     * 另外三层各自手算的版本里有两处算错了（见 apt_detail_page.h 里
     * pk_ui_fab_hidden_for 的注释），把四处收成同一条判据比留一个"这里恰好
     * 对"的特例划算。
     */
    pk_ui_fab_sync();
}

/* 「在地图上显示」：把视口挪过去 + 落 PIN + 切到地图页 + **一路收起模态层**。
 *
 * 2026-08-04 起是「收起」而不是「关闭」（sheet 语义，见 apt_detail_page.h 的
 * pk_sheet_state_t）。用户去地图只是"看一眼"，看完多半要回来挑下一个。
 *
 * 收起的是**整叠**：详情与它底下的搜索一起收（搜索没开着时那一下是空操作）。
 * 地图上点返回钮时也整叠一起恢复，于是回到的是详情，再按 BACK 才回搜索——
 * **逐层返回**。为什么选逐层而不是一步回搜索：
 *   - 产品要求是「原来在哪里进来的，那就回哪里去」，从地图回来的上一层
 *     就是详情；
 *   - 详情页装的是跑道长度/道面/塔台频率，"去地图看了眼位置再回来核对跑道"
 *     是真实动作，一步跳回搜索会把它抹掉，而且用户没有别的路回到详情
 *     （回到搜索后要重新点那一条，反而更远）；
 *   - 代价是"挑下一个"多一次点击，但那一次点击落在详情页页首的 BACK 上，
 *     位置固定、不用找。
 * 直接从地图点机场符号进来的详情（搜索是 CLOSED）同样能被收起并恢复——
 * pk_sheet_next 保证没开过的搜索不会被"恢复"出来。
 *
 * 为什么必须显式 pk_ui_set_mode(PK_UI_MODE_MAP)（2026-08-04 修）
 * ------------------------------------------------------------
 * 这里原来写着"不必切 mode：这条链路的两个入口都只可能在 PK_UI_MODE_MAP 下
 * 发生"。那句话在写下的时候是真的——当时搜索页只有一个入口，就是地图页右侧
 * 那枚放大镜。dock 换成全屏导航网格之后（commit f560c8a），网格里多了一格
 * 「搜索」，而网格从**任何一页**都能用 FAB 叫出来。于是"底下那一页一定是
 * 地图"这个前提整体失效：在 PFD 上叫出网格 → 搜索 → 点一个机场 → 详情 →
 * 「在地图上显示」，视口和 PIN 都摆好了，模态层也关干净了，露出来的却是 PFD。
 * 用户看到的现象就是"点了在地图上显示，什么都没发生"。
 *
 * 修法只能是显式切页，不能是"让入口去保证 mode"：入口是会增加的（这次就是
 * 增加了一个），而这个动作的语义本来就是"我要去地图看这个点"——终态由动作
 * 自己负责，才不会每加一个入口就漏一次。
 */
static void goto_map(void)
{
    pk_map_page_set_pin(s_d.lat, s_d.lon,
                        s_d.icao[0] ? s_d.icao : s_d.iata);
    pk_map_page_goto(s_d.lat, s_d.lon, APTD_GOTO_ZOOM);
    pk_apt_detail_page_collapse();
    pk_search_page_collapse();       /* 没开着时是空操作，见 pk_sheet_next */
    pk_ui_set_mode(PK_UI_MODE_MAP);
    /* 收起态不算活跃层 → 这一下把 FAB 放出来，地图也重新点得动。 */
    pk_ui_fab_sync();
}

#ifdef PK_SIM_BUILD
/*
 * 截图钩子（同 search_page.c 的 sim_setup_once、nav_grid_page.c 的 sim_setup）：
 *
 *   PK_SIM_APT_GOTO_MAP=1   打开本页后立刻按一下页首的「在地图上显示」
 *
 * 摆的不是内部状态，是**一次真实的触摸**：坐标取页首右侧那枚按钮的命中区，
 * 走 touch()+touch_up() 与真机逐字相同的那条路，所以截出来的就是用户点完那
 * 一下之后的终态。配 PK_SIM_PAGE=map + PK_SIM_MAP_TAP 用：sim/main.c 在那条
 * 分支里按 pk_apt_detail_page_active() 决定重画详情还是重画地图，于是这一个
 * 钩子就把「地图点机场符号 → 详情 → 在地图上显示 → 回到地图并落 PIN」整条
 * 链路截成了一张图。
 *
 * 唯一截不到的是 pk_ui_set_mode(PK_UI_MODE_MAP) 那一步——模拟器里
 * pk_ui_set_mode 是空壳（sim/compat/page_stub.c），截哪一页由 PK_SIM_PAGE 决定，
 * 不由 mode 决定。那一步只能在真机上验。
 */
#include <stdlib.h>

static void sim_setup_once(void)
{
    if (getenv("PK_SIM_APT_GOTO_MAP") == NULL) return;
    /* 「在地图上显示」按钮的命中区：顶栏内、x ≥ APTD_MAP_HIT_X0。 */
    const int x = (APTD_MAP_HIT_X0 + PK_DISPLAY_W) / 2;
    const int y = APTD_HDR_H / 2;
    pk_apt_detail_page_touch(x, y);
    pk_apt_detail_page_touch_up();
}
#endif /* PK_SIM_BUILD */

bool pk_apt_detail_page_touch(int x, int y)
{
    if (!pk_apt_detail_page_active()) return false;
    s_press_x      = x;
    s_press_y      = y;
    s_press_scroll = s_scroll;
    s_press_valid  = true;
    s_moved        = false;
    /* 整屏都吃：模态页底下没有任何该被点到的东西（FAB 已经藏了）。 */
    return true;
}

bool pk_apt_detail_page_drag(int x, int y)
{
    if (!pk_apt_detail_page_active()) return false;
    if (!s_press_valid) return true;   /* 仍然吃掉：模态 */
    (void)x;
    const int dy = y - s_press_y;
    if (!s_moved && (dy > APTD_DRAG_SLOP || dy < -APTD_DRAG_SLOP)) s_moved = true;
    if (!s_moved) return true;

    const int max_scroll = (s_content_h > APTD_VIEW_H)
                         ? (s_content_h - APTD_VIEW_H) : 0;
    int sy = s_press_scroll - dy;      /* 方向与手指一致 */
    if (sy < 0) sy = 0;
    if (sy > max_scroll) sy = max_scroll;
    s_scroll = sy;
    return true;
}

/*
 * 取消本次触摸：只丢状态，**不执行**动作（touch_up 会把按下结算成一次点击）。
 *
 * 现在谁在用（2026-08-02 普查）：只有模拟器 sim/main.c 的 PK_SIM_APT_SCROLL
 * 截图铺垫——滚动位置是本页内部状态没有 setter，那边用一次真实的
 * touch()+drag() 把版面滚下去，收尾必须用 cancel 而不是 touch_up，否则那次
 * 拖动会被结算成点击、把截图跳到别的页面去。
 * 真机侧原先的调用方是 touch_gt911.c 里 dock 展开时的整体让路，已随 dock
 * 一起删除（commit f560c8a）。别当死接口删掉。
 */
void pk_apt_detail_page_touch_cancel(void)
{
    s_press_valid = false;
    s_moved       = false;
}

void pk_apt_detail_page_touch_up(void)
{
    const bool click = pk_apt_detail_page_active() && s_press_valid && !s_moved;
    const int x = s_press_x, y = s_press_y;
    s_press_valid = false;
    s_moved       = false;
    if (!click) return;

    if (y < APTD_HDR_H) {
        if (x < APTD_BACK_HIT_X1)          pk_apt_detail_page_close();
        else if (x >= APTD_MAP_HIT_X0 && s_d.valid) goto_map();
    }
    /* 列表区没有可点的行——这一页是只读的。点空白什么都不做，而不是顺手
     * 关页：那会让"想滚但没滑够 12 px"变成"页面自己关了"。 */
}

/* ── 真机自检（见 apt_detail_page.h 的 PK_APT_DETAIL_SMOKE）────────
 *
 * 风格照 search_page.c 的 smoke_run / pk_aero_db.c 的 aero_smoke_check：
 * **只打日志、不断言真值**——跑道与频率的条数随 AIRAC 周期变，硬断言只会
 * 把"换了数据"误报成"坏了"。 */
#if PK_APT_DETAIL_SMOKE && !defined(PK_SIM_BUILD)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void smoke_task(void *arg)
{
    (void)arg;
    /* 等库加载完（懒加载定案：开机静默数秒后才开始读卡，全量库约 2 s）。 */
    for (int i = 0; i < 600 && pk_aero_db_state() != PK_AERO_DB_READY; ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (pk_aero_db_state() != PK_AERO_DB_READY) {
        ESP_LOGW(TAG, "smoke: aero DB never became READY — skipped");
        vTaskDelete(NULL);
        return;
    }

    static const char *const kCodes[] = { "ZGGG", "KJFK", "ZGOW" };
    for (size_t k = 0; k < sizeof(kCodes) / sizeof(kCodes[0]); ++k) {
        const int32_t idx = pk_aero_db_airport_by_icao(kCodes[k]);
        if (idx < 0) {
            ESP_LOGW(TAG, "smoke: %s not in this pack", kCodes[k]);
            continue;
        }
        /* load() 只碰 pk_aero_db + own_ship，两者都是线程安全的；它写的是
         * s_d，而页面此刻必然没打开（开机自检），不会与渲染抢。 */
        load((uint32_t)idx);
        ESP_LOGI(TAG, "smoke: %s \"%s\" elev %d ctrl %u  rwy %d/%lu  freq %d/%lu",
                 s_d.icao, s_d.name, (int)s_d.elev_ft, (unsigned)s_d.ctrl,
                 s_d.n_rwy, (unsigned long)s_d.total_rwy,
                 s_d.n_freq, (unsigned long)s_d.total_freq);
        /* 头两条频率：业务序真的跑了的话，第一条应当是 TWR（有塔台的场）。 */
        for (int i = 0; i < s_d.n_freq && i < 2; ++i) {
            char mhz[16];
            pk_apt_format_freq(mhz, sizeof(mhz), s_d.freq[i].freq_khz);
            ESP_LOGI(TAG, "smoke:   freq[%d] %-6s %-8s %s", i,
                     pk_apt_service_tag(s_d.freq[i].service), mhz,
                     s_d.freq[i].callsign);
        }
        /* 头两条跑道方向：缺字段要看得出来是"没显示"而不是"显示成 0"。 */
        for (int i = 0; i < s_d.n_rwy && i < 2; ++i) {
            const pk_apt_rwy_item_t *r = &s_d.rwy[i];
            int mag = 0;
            const bool has_mag = pk_apt_rwy_bearing_deg(r, &mag);
            ESP_LOGI(TAG, "smoke:   rwy[%d] %-4s %ux%u ft  surf %s  mag %s  thr %s",
                     i, r->desig, (unsigned)r->length_ft, (unsigned)r->width_ft,
                     pk_apt_surface_tag(r->surface) ? pk_apt_surface_tag(r->surface)
                                                    : "n/a",
                     has_mag ? "yes" : "n/a", r->has_coord ? "yes" : "n/a");
            if (has_mag) ESP_LOGI(TAG, "smoke:          mag = %03d", mag);
        }
    }
    /* 自检写脏了 s_d，清掉：否则用户点开某个机场之前，页面里躺着 KJFK 的数据。
     * open() 每次都会 memset + 重载，这一步只是不留悬念。 */
    memset(&s_d, 0, sizeof(s_d));
    vTaskDelete(NULL);
}

void pk_apt_detail_smoke_init(void)
{
    if (xTaskCreatePinnedToCore(smoke_task, "aptd_smoke", 4096, NULL, 2,
                                NULL, 0) != pdTRUE)
        ESP_LOGE(TAG, "aptd_smoke task create failed");
}

#else

void pk_apt_detail_smoke_init(void) { }

#endif /* PK_APT_DETAIL_SMOKE */

#endif /* !PK_APT_DETAIL_HOST_TEST */
