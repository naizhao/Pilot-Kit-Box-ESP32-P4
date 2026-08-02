/*
 * search_page.c — 见 search_page.h。
 *
 * 版面（800×480）
 * ---------------
 *   y=0   ┌──────────────────────────────────────────────┬────────┐
 *         │ SEARCH                                       │ CLOSE  │ 48
 *   y=48  ├──────────────────────────────────────────────┴────────┤
 *         │ ┌──────────────────────────────────────┐  ┌────────┐  │ 52
 *         │ │ ZGGG▌                                │  │ CLEAR  │  │
 *   y=100 ├───────────────────────────────────────────────────────┤
 *         │ NEARBY AIRPORTS                                       │ 28
 *         │ [APT] ZGGG                              12.3NM 045    │ 76/80
 *         │       Guangzhou Baiyun International                  │
 *         │ …（滚动）                                              │
 *   y=480 └───────────────────────────────────────────────────────┘
 *
 * 为什么行高是 76：屏 800 px ≈ 95 mm → 8.4 px/mm，触摸目标通行下限 9 mm
 * ≈ 76 px（口径同 settings_draw.c 的 SEG_MIN_TOUCH_W 与 keyboard_page.c 的
 * KBD_MIN_TOUCH）。列表可视区 480−100 = 380 恰好是 5×76——先按触摸下限定
 * 行高，再让可视区整除，屏底就不会挂半行。默认视图的「附近 / 最近搜索」
 * 再放宽到 80：那两组是"不用打字就能用"的主路径，值得多给 4 px。
 *
 * 为什么每行两行文字：代码（ZGGG）与名称（Guangzhou Baiyun International）
 * 挤一行的话，名称会被右边的距离列压到只剩十来个字符。分成两行之后
 * 名称有整整 692 px，而扫视时视线本来就是"先认代码、再确认名字"。
 *
 * 触摸与 settings_draw.c 同一套约定：render 每帧把几何留在 s_hit[] 里，
 * 触摸回调查表。几何只有绘制那一刻才知道（依赖滚动偏移与分组），分开各算
 * 一次迟早会飘。
 */
#include "search_page.h"

#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * 纯函数区（无 OS / 无全局状态）——host 单测直接把本文件拉进翻译单元。
 * ═══════════════════════════════════════════════════════════════════ */

int pk_search_norm_query(const char *in, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (in == NULL) return 0;

    size_t n = 0;
    for (const char *p = in; *p != '\0' && n + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        /* 字符集与键盘一致（A-Z 0-9 - _）。别的字节**丢弃**而不是替换成
         * 占位：查询串是要拿去 memcmp 前缀的，塞一个 '_' 进去只会让本来能
         * 命中的查询查不到，而用户看不出多了个字符。 */
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            out[n++] = (char)c;
    }
    out[n] = '\0';
    return (int)n;
}

bool pk_search_result_add(pk_search_item_t *arr, int *n, int cap,
                          const pk_search_item_t *it)
{
    if (arr == NULL || n == NULL || it == NULL) return false;
    if (*n >= cap) return false;
    for (int i = 0; i < *n; ++i) {
        if (arr[i].kind == it->kind && arr[i].idx == it->idx) return false;
    }
    arr[(*n)++] = *it;
    return true;
}

void pk_search_sort_range(pk_search_item_t *arr, int from, int to)
{
    if (arr == NULL || to - from < 2) return;
    for (int i = from + 1; i < to; ++i) {
        pk_search_item_t key = arr[i];
        int j = i - 1;
        while (j >= from) {
            /* 距离只在两边都有时才比——只有一边有距离是不可能的（本机位置
             * 是整批共用的），但真出现了也不能拿 0 去比：那会把"不知道多远"
             * 排到最前面，读起来像"就在脚下"。 */
            bool swap;
            if (arr[j].have_dist && key.have_dist)
                swap = arr[j].dist_nm > key.dist_nm;
            else
                swap = strcmp(arr[j].code, key.code) > 0;
            if (!swap) break;
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
}

void pk_search_hist_push(pk_search_hist_t *h, const char *q)
{
    if (h == NULL || q == NULL || q[0] == '\0') return;
    if (h->n > PK_SEARCH_HIST_MAX) h->n = PK_SEARCH_HIST_MAX;   /* 防越界读 */

    /* 已经在表里就只是提到队首——重复条目会把 14 个格子占成同一个词，
     * 而"最近搜索"的价值全在多样性上。 */
    int at = -1;
    for (int i = 0; i < h->n; ++i) {
        if (strcmp(h->items[i], q) == 0) { at = i; break; }
    }
    if (at < 0) {
        at = (h->n < PK_SEARCH_HIST_MAX) ? h->n++ : (PK_SEARCH_HIST_MAX - 1);
    }
    for (int i = at; i > 0; --i)
        memcpy(h->items[i], h->items[i - 1], sizeof(h->items[0]));
    snprintf(h->items[0], sizeof(h->items[0]), "%s", q);
}

#ifndef PK_SEARCH_PAGE_HOST_TEST

/* ═══════════════════════════════════════════════════════════════════
 * 平台区：后台查询任务 + NVS 历史 + 渲染 + 触摸
 * ═══════════════════════════════════════════════════════════════════ */

#include "apt_detail_page.h"
#include "display.h"
#include "geo.h"
#include "i18n.h"
#include "imu_task.h"
#include "keyboard_page.h"
#include "mag_var.h"
#include "map_page.h"
#include "own_ship.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "pk_aero_db.h"
#include "pk_ui_nav.h"
#include "traffic_geom.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#ifndef PK_SIM_BUILD
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
static const char *TAG = "search";
#endif

/*
 * 真机自检开关。
 *
 * 触摸交互没法在 host 上验（模拟器只跑渲染路径），而验收人也未必总能守在盒子
 * 边上点。打开它之后开机会**自动走一遍用户那条路**：打开搜索页 → 提交
 * "ZGGG" → 读快照 → 点第一条（跳地图 + 落 PIN），全过程打进串口日志。
 * 走的全是公开入口，不是另写一份逻辑。
 *
 * 默认 0：它会在开机后自己把地图挪到查到的机场并盖一枚 PIN，正式固件里
 * 这是个莫名其妙的副作用。要验收时临时改 1、烧一次、看日志，再改回 0 重烧。
 * （pk_aero_db.c 的 PK_AERO_DB_SMOKE 敢常开，是因为它只查不改任何状态。）
 */
#define PK_SEARCH_PAGE_SMOKE  0

/* 查询串不能长过键盘的缓冲，否则会出现"屏上敲得满、提交回来短了两个字符"。
 * 与 settings_page.c 那条同款断言（keyboard_page.h 的夹是**静默**的）。 */
_Static_assert(PK_SEARCH_QUERY_MAX <= PK_KBD_TEXT_MAX,
               "查询串上限超过了键盘编辑器的缓冲，输入会被静默截断");

/* ── 版面常量 ───────────────────────────────────────────────────── */

#define SRCH_HDR_H       PFD_BAR_BOT              /* 48，与各页顶栏同高 */
#define SRCH_QRY_TOP     SRCH_HDR_H
#define SRCH_QRY_H       52
#define SRCH_LIST_TOP    (SRCH_QRY_TOP + SRCH_QRY_H)   /* 100 */
#define SRCH_VIEW_H      (PK_DISPLAY_H - SRCH_LIST_TOP)/* 380 */

#define SRCH_MIN_TOUCH   76                       /* 9 mm，同 KBD_MIN_TOUCH */
#define SRCH_ROW_H       76                       /* 结果行 */
#define SRCH_BIG_ROW_H   80                       /* 附近 / 最近搜索 */
#define SRCH_SEC_H       28                       /* 分组标题 */
_Static_assert(SRCH_ROW_H >= SRCH_MIN_TOUCH, "结果行低于 9 mm 触摸下限");
_Static_assert(SRCH_BIG_ROW_H >= SRCH_MIN_TOUCH, "默认视图行低于 9 mm 触摸下限");
_Static_assert(SRCH_VIEW_H % SRCH_ROW_H == 0, "可视区不是结果行高的整数倍");

#define SRCH_PAD         PK_UI_PAD_L              /* 16 */
#define SRCH_R           (PK_DISPLAY_W - PK_UI_PAD_L)  /* 784 */

/* 页首「关闭」。命中区用整条 48 px 的带，视觉只有 40——与 settings_draw
 * 「视觉按 spec、命中放宽到整行」同一条规矩。 */
#define SRCH_CLOSE_W     128
#define SRCH_CLOSE_X0    (SRCH_R - SRCH_CLOSE_W)
#define SRCH_CLOSE_H     40
#define SRCH_CLOSE_Y0    ((SRCH_HDR_H - SRCH_CLOSE_H) / 2)

/* 查询行：输入框 + 清除钮。清除钮只在有查询时出现（空查询没什么可清）。 */
#define SRCH_CLR_W       120
#define SRCH_BOX_H       44
#define SRCH_BOX_Y0      (SRCH_QRY_TOP + (SRCH_QRY_H - SRCH_BOX_H) / 2)
#define SRCH_BOX_Y1      (SRCH_BOX_Y0 + SRCH_BOX_H)

/* 类型徽章 */
#define SRCH_BADGE_W     60
#define SRCH_BADGE_H     28
#define SRCH_TEXT_X      (SRCH_PAD + SRCH_BADGE_W + 16)   /* 92 */
/* 方位箭头与距离数字之间的间隙。箭头按 CJK 全角出（PK_AA_S_CJK_W = 17），
 * 贴太近会读成一个词。 */
#define SRCH_ARROW_GAP   6

/* 跳地图时用哪一档 zoom。11 在 800×480 上约 1.4 NM/屏宽（中纬度），
 * 机场跑道与滑行道都看得见，又不至于一屏只剩一块灰。地图页的上限是 12，
 * 留一档给用户自己放大。 */
#define SRCH_GOTO_ZOOM   11

/* ── 屏上文案（全部走 i18n catalog）──────────────────────────────
 *
 * 词条与翻译理由写在 firmware/scripts/i18n_catalog.py 的「航空数据搜索页」
 * 那一段（含宽度账）。这里只留取词的短别名，免得每个调用点都拖一行长枚举名。
 *
 * 两条**故意借用别页的词条**，不在本页另立一份：
 *   TXT_CLEAR    → KBD_CLEAR，与键盘上那枚清除键同词同义同动作；
 *   TXT_NO_POS_HNT → TFC_NO_OWN_HINT，交通页那句已把两条来源说全。
 * 借用的代价是改那一条要顺手看一眼这里；另立一份的代价是两处迟早说岔。
 *
 * 注意本页的**内容**（机场名、navaid ident）仍然只能是 ASCII——那是数据包
 * 里的原文，不经 catalog，也不该被翻译。 */
#define TXT_TITLE        pk_i18n_text(PK_TR_SEARCH_TITLE)
#define TXT_CLOSE        pk_i18n_text(PK_TR_SEARCH_CLOSE)
#define TXT_CLEAR        pk_i18n_text(PK_TR_KBD_CLEAR)
#define TXT_PLACEHOLDER  pk_i18n_text(PK_TR_SEARCH_PLACEHOLDER)
#define TXT_SEC_NEARBY   pk_i18n_text(PK_TR_SEARCH_SEC_NEARBY)
#define TXT_SEC_RECENT   pk_i18n_text(PK_TR_SEARCH_SEC_RECENT)
#define TXT_SEC_RESULTS  pk_i18n_text(PK_TR_SEARCH_SEC_RESULTS)
#define TXT_BUSY         pk_i18n_text(PK_TR_SEARCH_BUSY)
#define TXT_NO_HISTORY   pk_i18n_text(PK_TR_SEARCH_NO_HISTORY)
#define TXT_NO_DB        pk_i18n_text(PK_TR_SEARCH_NO_DB)
#define TXT_NO_DB_ABSENT pk_i18n_text(PK_TR_SEARCH_NO_DB_ABSENT)
#define TXT_NO_DB_LOAD   pk_i18n_text(PK_TR_SEARCH_NO_DB_LOAD)
#define TXT_NO_DB_ERR    pk_i18n_text(PK_TR_SEARCH_NO_DB_ERR)
#define TXT_NO_INDEX     pk_i18n_text(PK_TR_SEARCH_NO_INDEX)
#define TXT_NO_INDEX_HNT pk_i18n_text(PK_TR_SEARCH_NO_INDEX_HINT)
#define TXT_NO_MATCH     pk_i18n_text(PK_TR_SEARCH_NO_MATCH)
#define TXT_NO_MATCH_HNT pk_i18n_text(PK_TR_SEARCH_NO_MATCH_HINT)
#define TXT_NO_POS       pk_i18n_text(PK_TR_SEARCH_NO_POS)
#define TXT_NO_POS_HNT   pk_i18n_text(PK_TR_TFC_NO_OWN_HINT)
#define TXT_NO_NEARBY    pk_i18n_text(PK_TR_SEARCH_NO_NEARBY)

/* ── 发布态 ─────────────────────────────────────────────────────── */

typedef enum {
    ST_IDLE = 0,   /* 还没查过 */
    ST_BUSY,       /* 后台在查（第 5 桶那 65 ms 用户看得见）*/
    ST_OK,
    ST_EMPTY,      /* 库正常但没命中 */
    ST_NO_DB,      /* 未 READY */
    ST_NO_INDEX,   /* v2 bin：搜索索引缺席 */
    ST_NO_POS,     /* 「附近」需要本机位置 */
} status_t;

/* 结果快照双缓冲：后台任务写非活动那份，写完翻 s_res_pub；渲染只读活动那份，
 * 全程无锁。同 pk_aero_layer.c 的手法（那边有一段更长的论证）。 */
EXT_RAM_BSS_ATTR static pk_search_item_t s_res[2][PK_SEARCH_MAX_RESULTS];
static volatile int      s_res_n[2];
static volatile int      s_res_pub;
static volatile uint8_t  s_status = ST_IDLE;
/* 快照对应的 DB 版本，用来把"没命中"与"这张卡搜不了"分开报。 */
static volatile uint16_t s_res_dbver;

#ifndef PK_SIM_BUILD
/* 作业请求。UI 线程写、后台任务读，靠 s_req_lock 串起来——不加锁的话
 * 连点两次会读到半截拼起来的查询串。sim 上没有第二个线程（submit 直接同步
 * 跑完），这三个量一个都用不上。 */
static char              s_req_query[PK_SEARCH_QUERY_MAX + 1];
static volatile uint32_t s_req_seq, s_done_seq;
#endif

/* 当前输入框里的串（UI 线程独占）。与 s_req_query 分开：用户可能敲完不提交。 */
static char s_query[PK_SEARCH_QUERY_MAX + 1];

/* 历史与命中表都进 PSRAM：内部 .bss 是本项目的硬约束（开机调度器启动前
 * 的内部堆余量有下限，见 firmware/scripts/check_early_heap.py 与
 * pk_tile_loader.c 顶部注释），而这两块都是"只有 UI 线程碰、且只在页面
 * 打开时碰"的冷数据，正是该挪出去的那一类。 */
EXT_RAM_BSS_ATTR static pk_search_hist_t s_hist;

/* ── 页面状态 ───────────────────────────────────────────────────── */

static bool s_active;
static int  s_scroll;
static int  s_content_h;      /* 上一帧的内容总高，drag 的钳位基准 */

/* 触摸：同 settings/diag——按下只记起点，位移超阈值才算拖动，
 * 松手时没拖过才当点击。三页共用同一套判定，手感才一致。 */
static int  s_press_x, s_press_y, s_press_scroll;
static bool s_press_valid, s_moved;
#define SRCH_DRAG_SLOP  12

/* 命中表：结果 12 + 历史 14 + 两个分组标题的余量。 */
#define SRCH_HIT_MAX  (PK_SEARCH_MAX_RESULTS + PK_SEARCH_HIST_MAX + 4)
typedef struct {
    int16_t y0, y1;
    uint8_t kind;    /* 0=无 1=结果/附近 2=历史 */
    int16_t arg;     /* 对应列表下标 */
} row_hit_t;
EXT_RAM_BSS_ATTR static row_hit_t s_hit[SRCH_HIT_MAX];
static int       s_nhit;

/* ── 配色（与地图叠加层同一套语言：蓝=机场、绿=导航台、紫=FIX）──── */

static uint16_t col_bg(void)     { return pk_rgb565(  7,  10,  16); }
static uint16_t col_panel(void)  { return pk_rgb565( 28,  36,  48); }
static uint16_t col_txt(void)    { return pk_rgb565(235, 240, 248); }
static uint16_t col_dim(void)    { return pk_rgb565(120, 130, 145); }
static uint16_t col_sub(void)    { return pk_rgb565(170, 182, 200); }
static uint16_t col_accent(void) { return pk_rgb565(  0, 110, 200); }
static uint16_t col_line(void)   { return pk_rgb565( 26,  33,  44); }
static uint16_t col_amber(void)  { return pk_rgb565(255, 176,   0); }
/* 方位箭头。与交通页 / ADS-B 列表页同一个青色——三处画的是同一个语义，
 * 颜色一变，用户就会去找"这个青的和那个白的有什么不同"。 */
static uint16_t col_arrow(void)  { return pk_rgb565(  0, 210, 235); }

static uint16_t col_kind(uint8_t kind)
{
    switch (kind) {
    case PK_SEARCH_KIND_NAVAID: return pk_rgb565( 40, 190, 110);
    case PK_SEARCH_KIND_FIX:    return pk_rgb565(160, 110, 235);
    default:                    return pk_rgb565( 60, 130, 235);
    }
}

static const char *kind_tag(uint8_t kind)
{
    switch (kind) {
    case PK_SEARCH_KIND_NAVAID: return "NAV";
    case PK_SEARCH_KIND_FIX:    return "FIX";
    default:                    return "APT";
    }
}

/* ═══════════════ 查询作业（后台任务里跑）═══════════════════════ */

/* 名称拷贝 + 超长截断。
 *
 * 截断以 "..." 收尾而不是硬切：硬切会在屏上留下半截词（"Zhuhai Jinwan Shequ
 * Zhili Guanli Zhihu"），读的人分不清是名字就这么长还是渲染坏了。不做
 * pk_aero_layer 那套词边界回退——那一层的标签只有几十像素、切在词中间很常见；
 * 这里有 62 个字符的预算，真被截到的是极少数超长音译名，三个点足够表意。 */
static void copy_name(char *dst, size_t cap, const char *src)
{
    if (src == NULL) src = "";
    const size_t n = strlen(src);
    if (n < cap) { memcpy(dst, src, n + 1); return; }
    memcpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    if (cap >= 4) memcpy(dst + cap - 4, "...", 4);
}

static void fill_pos(pk_search_item_t *it, double lat, double lon,
                     bool own_ok, double olat, double olon)
{
    it->lat = lat;
    it->lon = lon;
    it->have_dist = own_ok;
    if (own_ok) geo_dist_brg(olat, olon, lat, lon, &it->dist_nm, &it->brg_deg);
}

static bool add_airport(pk_search_item_t *arr, int *n, uint32_t idx,
                        uint8_t bucket, bool own_ok, double olat, double olon)
{
    pk_aero_airport_t a;
    if (!pk_aero_db_airport_get(idx, &a)) return false;
    pk_search_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = PK_SEARCH_KIND_AIRPORT;
    it.bucket = bucket;
    it.idx = idx;
    /* 无 ICAO 的小场用 IATA 顶上（v3 的全量 key 索引本来就是这么建的）。
     * 两个都没有就摆 "----"：留空会让那一行看着像渲染坏了。 */
    snprintf(it.code, sizeof(it.code), "%s",
             a.icao[0] ? a.icao : (a.iata[0] ? a.iata : "----"));
    /* 字符串必须当场拷走：a.name 指向 PSRAM 池，拔卡即悬空。 */
    copy_name(it.name, sizeof(it.name), a.name[0] ? a.name : a.city);
    fill_pos(&it, a.lat, a.lon, own_ok, olat, olon);
    return pk_search_result_add(arr, n, PK_SEARCH_MAX_RESULTS, &it);
}

static bool add_navaid(pk_search_item_t *arr, int *n, uint32_t idx,
                       uint8_t bucket, bool own_ok, double olat, double olon)
{
    pk_aero_navaid_t v;
    if (!pk_aero_db_navaid_get(idx, &v)) return false;
    pk_search_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = PK_SEARCH_KIND_NAVAID;
    it.bucket = bucket;
    it.idx = idx;
    snprintf(it.code, sizeof(it.code), "%s", v.ident);
    if (v.freq_khz > 0) {
        /* 导航台没名字的很多，频率反而是飞行员真正要核对的那一项。 */
        char tmp[PK_SEARCH_NAME_MAX + 16];
        snprintf(tmp, sizeof(tmp), "%s%s%.2f MHz",
                 v.name ? v.name : "", (v.name && v.name[0]) ? "  " : "",
                 v.freq_khz / 1000.0);
        copy_name(it.name, sizeof(it.name), tmp);
    } else {
        copy_name(it.name, sizeof(it.name), v.name);
    }
    fill_pos(&it, v.lat, v.lon, own_ok, olat, olon);
    return pk_search_result_add(arr, n, PK_SEARCH_MAX_RESULTS, &it);
}

static bool add_fix(pk_search_item_t *arr, int *n, uint32_t idx,
                    uint8_t bucket, bool own_ok, double olat, double olon)
{
    pk_aero_fix_t f;
    if (!pk_aero_db_fix_get(idx, &f)) return false;
    pk_search_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = PK_SEARCH_KIND_FIX;
    it.bucket = bucket;
    it.idx = idx;
    snprintf(it.code, sizeof(it.code), "%s", f.ident);
    copy_name(it.name, sizeof(it.name), f.name);
    fill_pos(&it, f.lat, f.lon, own_ok, olat, olon);
    return pk_search_result_add(arr, n, PK_SEARCH_MAX_RESULTS, &it);
}

/*
 * 分桶查询（设计文档 §3.3）。
 *
 *   桶1 机场 ICAO 精确      二分，µs
 *   桶2 机场 key 前缀       二分前缀扫，µs（v3 全量 key，覆盖无 ICAO 的场）
 *   桶3 导航台 ident 前缀   同上
 *   桶4 FIX ident 前缀      同上
 *   桶5 名称/城市子串       顺扫字符串池，65 ms —— 只在前 4 桶不够时才跑
 *
 * 关键取舍：**前 4 桶全是 µs 级**。只有前缀无果时才付那 65 ms，且它已经
 * 是分段让渡的（pk_aero_db_search_substring），不会把地图叠加层堵住。
 */
static int run_buckets(pk_search_item_t *arr, const char *q,
                       bool own_ok, double olat, double olon)
{
    int n = 0;
    uint32_t ids[PK_SEARCH_MAX_RESULTS];

    /* 桶1 */
    const int32_t exact = pk_aero_db_airport_by_icao(q);
    if (exact >= 0) add_airport(arr, &n, (uint32_t)exact, 1, own_ok, olat, olon);

    /* 桶2–4：每桶各自排序。桶序编码的是匹配质量，不能跨桶重排。 */
    struct {
        uint8_t bucket;
        int (*enumerate)(const char *, uint32_t *, int);
        bool (*add)(pk_search_item_t *, int *, uint32_t, uint8_t, bool,
                    double, double);
    } const kBuckets[] = {
        { 2, pk_aero_db_airports_by_prefix, add_airport },
        { 3, pk_aero_db_navaids_by_prefix,  add_navaid  },
        { 4, pk_aero_db_fixes_by_prefix,    add_fix     },
    };
    for (size_t b = 0; b < sizeof(kBuckets) / sizeof(kBuckets[0]); ++b) {
        if (n >= PK_SEARCH_MAX_RESULTS) break;
        const int from = n;
        /* max 必须截断：单字母前缀会枚举出几千条（设计文档 §6 坑 7），
         * 不截就是白扫几千次随机访存去填 12 个格子。 */
        const int k = kBuckets[b].enumerate(q, ids, PK_SEARCH_MAX_RESULTS - n);
        for (int i = 0; i < k; ++i)
            kBuckets[b].add(arr, &n, ids[i], kBuckets[b].bucket,
                            own_ok, olat, olon);
        pk_search_sort_range(arr, from, n);
    }

    /*
     * 桶5：**前 4 桶一条都没有**时才跑，且要求查询串 ≥ 3 字符。
     *
     * 门槛卡在"零结果"而不是"没写满 12 条"，是设计文档 §3.3 的原话：
     * 「只有前缀无果时才付线扫的代价」。这不是抠性能——真机实测这一趟要
     * **好几秒**（顺扫三段共约 3 MB PSRAM 字符串池，而本任务只有 prio 2，
     * 还要给 12 FPS 的渲染让路）。按"没写满就跑"写的话，敲 ZGGG 明明 µs 级
     * 就查到了，用户还要再等几秒才看到那一条，纯亏。
     *
     * 长度门槛同理：子串是"包含"语义，"A" 几乎命中池里每一条串，扫完那几秒
     * 换回来的 12 条与随机取样无异，还会把用户真正想要的前缀命中挤掉。
     */
    if (n == 0 && strlen(q) >= 3) {
        pk_aero_hit_t hits[PK_SEARCH_MAX_RESULTS];
        const int h = pk_aero_db_search_substring(q, hits,
                                                  PK_SEARCH_MAX_RESULTS);
        const int from = n;
        for (int i = 0; i < h && n < PK_SEARCH_MAX_RESULTS; ++i) {
            switch (hits[i].type) {
            case PK_AERO_SEC_AIRPORTS:
                add_airport(arr, &n, hits[i].idx, 5, own_ok, olat, olon); break;
            case PK_AERO_SEC_NAVAIDS:
                add_navaid(arr, &n, hits[i].idx, 5, own_ok, olat, olon);  break;
            case PK_AERO_SEC_WAYPOINTS_FIX:
                add_fix(arr, &n, hits[i].idx, 5, own_ok, olat, olon);     break;
            default: break;
            }
        }
        pk_search_sort_range(arr, from, n);
    }
    return n;
}

/* 「附近」：不输入直接看，飞行员 90% 的场景（设计文档 §3.2）。 */
static int fill_nearby(pk_search_item_t *arr, double lat, double lon)
{
    pk_aero_near_t near[PK_SEARCH_MAX_RESULTS];
    const int k = pk_aero_db_nearest_airports(lat, lon, near,
                                              PK_SEARCH_MAX_RESULTS);
    int n = 0;
    for (int i = 0; i < k; ++i) {
        const int before = n;
        if (!add_airport(arr, &n, near[i].idx, 1, true, lat, lon)) continue;
        /* nearest 已经把距离/方位算好了（pk_aero_near_t 自带），直接用——
         * 比再算一遍 Haversine 省，也保证与地图叠加层显示的是同一个数。 */
        if (n > before) {
            arr[n - 1].have_dist = true;
            arr[n - 1].dist_nm   = near[i].dist_nm;
            arr[n - 1].brg_deg   = near[i].brg_deg;
        }
    }
    return n;   /* nearest 本身就是距离升序，不必再排 */
}

/* 跑一次作业并发布快照。q 为空串 = 默认视图的「附近」。 */
static void run_job(const char *q)
{
    const int w = 1 - s_res_pub;
    pk_search_item_t *arr = s_res[w];
    int n = 0;
    status_t st;

    pk_aero_db_status_t db;
    pk_aero_db_status_get(&db);

    aircraft_t own;
    pk_own_src_t src;
    const bool own_ok = pk_own_ship_resolve(
        esp_timer_get_time(), (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL,
        &own, &src);

    if (db.state != PK_AERO_DB_READY) {
        st = ST_NO_DB;
    } else if (q[0] == '\0') {
        if (!own_ok) {
            st = ST_NO_POS;
        } else {
            n = fill_nearby(arr, own.lat, own.lon);
            st = (n > 0) ? ST_OK : ST_EMPTY;
        }
    } else {
        n = run_buckets(arr, q, own_ok, own.lat, own.lon);
        /* 没命中要分因：v2 卡上机场/导航台前缀与子串全都返回 0（索引缺席），
         * 那不是"没有这个机场"，是"这张卡搜不了"。混成一句会让用户反复
         * 重敲同一个代码。 */
        st = (n > 0) ? ST_OK : (db.version < 3 ? ST_NO_INDEX : ST_EMPTY);
    }

    s_res_n[w]   = n;
    s_res_dbver  = db.version;
    s_res_pub    = w;
    s_status     = (uint8_t)st;
}

/* ── 后台任务（sim 上退化成同步调用，见文件尾）──────────────────── */

#ifndef PK_SIM_BUILD

static SemaphoreHandle_t s_req_lock;
static TaskHandle_t      s_task;

/* 优先级 2 = 与 aero_db / aero_layer 同级，低于 pfd(4)：整个查询过程都可被
 * 渲染抢占。栈 6 KB——run_buckets 的栈上工作区（near[12] + hits[12] +
 * 逐条记录解码）不到 1.5 KB，snprintf 的浮点格式化是大头。 */
#define SRCH_TASK_STACK  (6 * 1024)
#define SRCH_TASK_PRIO   2

#if PK_SEARCH_PAGE_SMOKE
#include "ui_state.h"   /* pk_ui_set_mode —— 只有自检用得到 */
static void smoke_run(void);
#endif

static void search_task(void *arg)
{
    (void)arg;
#if PK_SEARCH_PAGE_SMOKE
    smoke_run();
#endif
    for (;;) {
        /* 等提交。带超时是为了"库刚加载完"这种外部变化也能自己跟上：
         * 页面开着、卡刚插上时，用户不必再点一次才看到附近机场。 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));

        uint32_t seq;
        char q[PK_SEARCH_QUERY_MAX + 1];
        xSemaphoreTake(s_req_lock, portMAX_DELAY);
        seq = s_req_seq;
        memcpy(q, s_req_query, sizeof(q));
        xSemaphoreGive(s_req_lock);

        if (seq == s_done_seq) {
            /*
             * 没有新请求，但**外部状态可能变了**：卡刚插上、GPS 刚定位。
             * 上一次因为"没库/没本机位置"空手而归的话，这里自己重跑一次——
             * 否则页面开着不动，用户得先关掉再打开才看得到附近机场，而他
             * 并不知道自己该做这个动作。只在页面开着时重跑，免得在后台空转。
             */
            if (s_active &&
                (s_status == ST_NO_DB || s_status == ST_NO_POS) &&
                pk_aero_db_state() == PK_AERO_DB_READY) {
                run_job(q);
            }
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        run_job(q);
        s_done_seq = seq;
        ESP_LOGI(TAG, "query \"%s\" -> %d hits, status %u, %lld us",
                 q[0] ? q : "(nearby)", s_res_n[s_res_pub],
                 (unsigned)s_status, (long long)(esp_timer_get_time() - t0));
    }
}

static void submit(const char *q)
{
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    snprintf(s_req_query, sizeof(s_req_query), "%s", q);
    s_req_seq++;
    xSemaphoreGive(s_req_lock);
    /* 立刻切 loading：第 5 桶那 65 ms 用户感知得到，屏上不能留"点了没反应"
     * 的空窗。真正的结果由后台任务发布时覆盖掉这一态。 */
    s_status = ST_BUSY;
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

/* ── 历史的 NVS 持久化（namespace 照 config_devname.c 的风格新开一个）── */

#define SRCH_NVS_NAMESPACE  "pk_search"
#define SRCH_NVS_KEY_HIST   "hist"

static void ensure_nvs(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

/* 整张表当一个 blob 存。逐条存 14 个键要 14 次 set + 一次 commit，而这张表
 * 每次搜索都要重写一遍——blob 是一次擦写，对 flash 更友好，也天然保住顺序
 * （LRU 的全部信息就在顺序里）。
 *
 * 为什么**仍然跑在触摸路径上**（2026-08-02 复核）：
 * 它不在任何稳态路径上——全项目只有两个调用点（键盘确定、点最近搜索），
 * 都是用户主动动作，且紧跟着 submit() 把整页内容换掉。一次 188 B 的 blob
 * set + commit 就算顶掉一帧，落点也正是用户已经在等页面变化的那一帧。
 * 挪到后台要多一个队列/任务或一个脏标志，还会开出"写之前断电就丢"的窗口，
 * 而这张表的全部价值就是"下次打开还在"。同 config_devname.c 的取舍。
 * 真要再省，省的是**没必要的那次写**——见下面 hist_remember()。 */
static void hist_save(void)
{
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(SRCH_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, SRCH_NVS_KEY_HIST, &s_hist, sizeof(s_hist));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "history save failed (%s)",
                                esp_err_to_name(err));
}

static void hist_load(void)
{
    ensure_nvs();
    nvs_handle_t h;
    if (nvs_open(SRCH_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_hist);
    /* 长度必须精确匹配才收：结构体大小随 PK_SEARCH_HIST_MAX / QUERY_MAX 变，
     * 旧固件写下的 blob 直接按新结构解读会读出半截字符串。不匹配就当没有，
     * 历史丢了无非是少几个快捷入口，比屏上冒出乱码强。 */
    if (nvs_get_blob(h, SRCH_NVS_KEY_HIST, &s_hist, &len) != ESP_OK ||
        len != sizeof(s_hist)) {
        memset(&s_hist, 0, sizeof(s_hist));
    }
    nvs_close(h);

    /* 存回来的内容仍要过一遍归一化与边界钳位：blob 可能来自别的固件版本
     * 或手工工具，屏上多一个控制字符就是一格空白。 */
    if (s_hist.n < 0 || s_hist.n > PK_SEARCH_HIST_MAX) s_hist.n = 0;
    for (int i = 0; i < s_hist.n; ++i) {
        char clean[PK_SEARCH_QUERY_MAX + 1];
        pk_search_norm_query(s_hist.items[i], clean, sizeof(clean));
        memcpy(s_hist.items[i], clean, sizeof(clean));
    }
    ESP_LOGI(TAG, "search history: %d entries", s_hist.n);
}

void pk_search_page_init(void)
{
    if (s_req_lock != NULL) return;   /* 幂等 */
    s_req_lock = xSemaphoreCreateMutex();
    hist_load();
    if (xTaskCreatePinnedToCore(search_task, "aero_search", SRCH_TASK_STACK,
                                NULL, SRCH_TASK_PRIO, &s_task, 0) != pdTRUE)
        ESP_LOGE(TAG, "aero_search task create failed");
}

#else  /* PK_SIM_BUILD：无 FreeRTOS、无 NVS —— 同步跑，历史只在内存里 */

static void submit(const char *q)
{
    s_status = ST_BUSY;
    run_job(q);
}
static void hist_save(void) { }
void pk_search_page_init(void) { }

#endif /* PK_SIM_BUILD */

/* 记一条最近搜索并落盘——**只在 blob 真会变的时候才写 flash**。
 *
 * LRU 的语义决定了"已经在队首的词再搜一次"是个空操作：pk_search_hist_push
 * 会把它从 0 号位挪到 0 号位，整张表逐字节不变。而这恰好是最常见的一下——
 * 点最近搜索的第一条、或者敲完发现结果不对再点一次同一条。不挡住的话，
 * 每一次都白擦一遍 flash，还白顶一帧。
 *
 * 判据只看队首而不是全表 memcmp：不在队首的词一定会引起顺序变化，比出来
 * 也还是要写。 */
static void hist_remember(const char *q)
{
    if (s_hist.n > 0 && strcmp(s_hist.items[0], q) == 0) return;
    pk_search_hist_push(&s_hist, q);
    hist_save();
}

/* ═══════════════ 绘制 ═══════════════════════════════════════════ */

/* 一枚按钮：圆角底 + 居中文字。与 keyboard_page.c 的 draw_button 同款
 * （那份是 static，照抄写法而不是导出——同一套视觉语言，两页各留一份）。 */
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

static void hit_push(int y0, int h, uint8_t kind, int arg)
{
    if (s_nhit >= SRCH_HIT_MAX) return;
    s_hit[s_nhit].y0   = (int16_t)y0;
    s_hit[s_nhit].y1   = (int16_t)(y0 + h);
    s_hit[s_nhit].kind = kind;
    s_hit[s_nhit].arg  = (int16_t)arg;
    s_nhit++;
}

/* 分组标题。返回本段占的高度，好让调用点把 y 推下去。 */
static int draw_section(uint16_t *fb, int y, const char *label)
{
    if (y > SRCH_LIST_TOP - SRCH_SEC_H && y < PK_DISPLAY_H) {
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_PAD,
                   y + (SRCH_SEC_H - PK_AA_XS_H) / 2, label, col_dim(),
                   PK_AA_XS);
        pk_pfd_fill_rect(fb, SRCH_PAD, y + SRCH_SEC_H - 1, SRCH_R,
                         y + SRCH_SEC_H, col_line());
    }
    return SRCH_SEC_H;
}

/* 两行的空态块：一句结论 + 一句"该怎么办"。
 * 只画结论会让用户盯着一行英文猜下一步，这一页三种空态成因完全不同
 * （没卡 / 卡太旧 / 真没这个机场），提示必须各说各的。 */
static int draw_empty(uint16_t *fb, int y, const char *title, const char *hint,
                      uint16_t col)
{
    const int h = 96;
    if (y > SRCH_LIST_TOP - h && y < PK_DISPLAY_H) {
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_PAD, y + 20, title,
                   col, PK_AA_S);
        if (hint != NULL)
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_PAD, y + 52, hint,
                       col_dim(), PK_AA_XS);
    }
    return h;
}

static void draw_item_row(uint16_t *fb, int y, int h,
                          const pk_search_item_t *it, float hdg_ref_true)
{
    if (y <= SRCH_LIST_TOP - h || y >= PK_DISPLAY_H) return;

    /* 类型徽章：形状一致、只有颜色与三个字母不同——扫视时靠颜色分类，
     * 停下来靠字母确认。与地图叠加层同一套配色（蓝机场/绿导航台/紫 FIX）。 */
    const uint16_t kc = col_kind(it->kind);
    const int by = y + (h - SRCH_BADGE_H) / 2;
    pk_pfd_fill_round_rect(fb, SRCH_PAD, by, SRCH_PAD + SRCH_BADGE_W,
                           by + SRCH_BADGE_H, SRCH_BADGE_H / 2, kc);
    {
        const char *tag = kind_tag(it->kind);
        const int tw = pk_aa_text_width(tag, PK_AA_XS);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   SRCH_PAD + (SRCH_BADGE_W - tw) / 2,
                   by + (SRCH_BADGE_H - PK_AA_XS_H) / 2, tag,
                   pk_rgb565(10, 14, 20), PK_AA_XS);
    }

    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_TEXT_X, y + 10,
               it->code, col_txt(), PK_AA_M);
    if (it->name[0])
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_TEXT_X, y + 42,
                   it->name, col_sub(), PK_AA_S);

    /* 距离/方位。**没有本机位置时整列不画**，不是画 0.0NM——0 NM 会被读成
     * "就在脚下"，那是比缺一列信息危险得多的谎。 */
    if (it->have_dist) {
        char d[24];
        snprintf(d, sizeof(d), "%.1fNM %03d", it->dist_nm,
                 ((int)(it->brg_deg + 0.5) % 360 + 360) % 360);
        const int dw = pk_aa_text_width(d, PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_R - dw, y + 14, d,
                   col_txt(), PK_AA_S);

        /* 八向箭头，与交通页 / ADS-B 列表页同一个 pk_bearing_arrow()。
         *
         * 放在数字左边而不是右边：视线从左往右先接到"大概哪个方向"，再落到
         * 精确度数，与 ADS-B 列表页 BRG 列的次序一致。放右边则要先读完
         * "12.3NM 045" 才看到方向，箭头那点"扫一眼就知道"的价值就没了。
         *
         * 只挤在第一行（代码那一行）：名称在 y+42 的第二行，两者不同行，
         * 多占的 17+6 px 不会压到名字。 */
        const char *ar = pk_bearing_arrow((float)it->brg_deg - hdg_ref_true);
        const int aw = pk_aa_text_width(ar, PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   SRCH_R - dw - SRCH_ARROW_GAP - aw, y + 14, ar,
                   col_arrow(), PK_AA_S);
    }

    pk_pfd_fill_rect(fb, SRCH_PAD, y + h - 1, SRCH_R, y + h, col_line());
}

static void draw_hist_row(uint16_t *fb, int y, int h, const char *q)
{
    if (y <= SRCH_LIST_TOP - h || y >= PK_DISPLAY_H) return;
    /* 历史项的触摸目标就是**整行**：屏上画一个小胶囊、命中却是整行，是本项目
     * 各页的通行做法（settings_draw.c 的 hit_set 那段）。 */
    pk_pfd_fill_round_rect(fb, SRCH_PAD, y + 8, SRCH_R, y + h - 8, 12,
                           col_panel());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_TEXT_X - 40,
               y + (h - PK_AA_M_H) / 2, q, col_txt(), PK_AA_M);
}

/*
 * 本帧的「箭头参考方向」——真北系下的本机机头朝向。箭头角 = 目标真方位 − 它。
 *
 * 为什么每帧算而不是塞进后台查询作业：航向一直在变，而查询作业几百毫秒才跑
 * 一次；算在那边箭头会明显滞后。距离/方位（dist_nm/brg_deg）正相反——它们只
 * 随位置变，作业里算一次就够，还能与地图叠加层保证是同一个数。
 *
 * 口径照抄 adsb_list.c 里同一段：pk_own_heading_resolve 给出的角度，ADS-B /
 * GPS 来源是真北参考，IMU 来源是磁北参考，后者要加回磁偏角（东偏为正）才回到
 * 真北系——brg_deg 是 geo_dist_brg 算的真方位，两边必须同系。
 *
 * 没有有效航向时返回 0，箭头退化成「正北朝上」：ADS-B 列表页在同一情形下也是
 * 这个表现（那边 own_heading 保持 0，rel_bearing 恒等于 abs_bearing），三页
 * 一致比在这里另发明一个"方向不可用"的空态更重要。
 */
static float arrow_ref_true_deg(bool own_ok, pk_own_src_t own_src,
                                const aircraft_t *own)
{
    pk_imu_sample_t imu;
    const bool have_imu = pk_imu_sample_get(&imu);

    float hdg = 0.0f;
    pk_hdg_src_t hsrc;
    if (!pk_own_heading_resolve(own_ok, own_src, own, have_imu,
                                have_imu ? imu.yaw_deg : 0.0f, &hdg, &hsrc))
        return 0.0f;
    if (hsrc == PK_HDG_SRC_IMU && own_ok)
        hdg += pk_mag_var_lookup(own->lat, own->lon);
    return hdg;
}

/* 库未就绪时的提示分因（照 diag 页 AERO DB 卡片的口径）。 */
static const char *no_db_hint(void)
{
    switch (pk_aero_db_state()) {
    case PK_AERO_DB_LOADING: return TXT_NO_DB_LOAD;
    case PK_AERO_DB_ERROR:   return TXT_NO_DB_ERR;
    default:                 return TXT_NO_DB_ABSENT;
    }
}

void pk_search_page_render(uint16_t *fb)
{
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg());
    s_nhit = 0;

    const int pub = s_res_pub;
    const pk_search_item_t *res = s_res[pub];
    const int nres = s_res_n[pub];
    const status_t st = (status_t)s_status;
    const bool default_view = (s_query[0] == '\0');

    aircraft_t own = {0};
    pk_own_src_t own_src;
    const bool own_ok = pk_own_ship_resolve(
        esp_timer_get_time(), (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL,
        &own, &own_src);
    const float hdg_ref = arrow_ref_true_deg(own_ok, own_src, &own);

    int y = SRCH_LIST_TOP - s_scroll;

    if (default_view) {
        y += draw_section(fb, y, TXT_SEC_NEARBY);
        if (st == ST_BUSY) {
            y += draw_empty(fb, y, TXT_BUSY, NULL, col_sub());
        } else if (st == ST_NO_DB) {
            y += draw_empty(fb, y, TXT_NO_DB, no_db_hint(), col_amber());
        } else if (st == ST_NO_POS) {
            y += draw_empty(fb, y, TXT_NO_POS, TXT_NO_POS_HNT, col_amber());
        } else if (nres == 0) {
            y += draw_empty(fb, y, TXT_NO_NEARBY, NULL, col_dim());
        } else {
            for (int i = 0; i < nres; ++i) {
                draw_item_row(fb, y, SRCH_BIG_ROW_H, &res[i], hdg_ref);
                hit_push(y, SRCH_BIG_ROW_H, 1, i);
                y += SRCH_BIG_ROW_H;
            }
        }

        y += draw_section(fb, y, TXT_SEC_RECENT);
        if (s_hist.n == 0) {
            y += draw_empty(fb, y, TXT_NO_HISTORY, NULL, col_dim());
        } else {
            for (int i = 0; i < s_hist.n; ++i) {
                draw_hist_row(fb, y, SRCH_BIG_ROW_H, s_hist.items[i]);
                hit_push(y, SRCH_BIG_ROW_H, 2, i);
                y += SRCH_BIG_ROW_H;
            }
        }
    } else {
        y += draw_section(fb, y, TXT_SEC_RESULTS);
        switch (st) {
        case ST_BUSY:
            y += draw_empty(fb, y, TXT_BUSY, NULL, col_sub());
            break;
        case ST_NO_DB:
            y += draw_empty(fb, y, TXT_NO_DB, no_db_hint(), col_amber());
            break;
        case ST_NO_INDEX:
            y += draw_empty(fb, y, TXT_NO_INDEX, TXT_NO_INDEX_HNT, col_amber());
            break;
        case ST_OK:
            for (int i = 0; i < nres; ++i) {
                draw_item_row(fb, y, SRCH_ROW_H, &res[i], hdg_ref);
                hit_push(y, SRCH_ROW_H, 1, i);
                y += SRCH_ROW_H;
            }
            break;
        default:
            y += draw_empty(fb, y, TXT_NO_MATCH, TXT_NO_MATCH_HNT, col_dim());
            break;
        }
    }

    s_content_h = y + s_scroll - SRCH_LIST_TOP;

    /* 滚动条：贴右缘，只在超出一屏时出现（几何照 diag_page.c:656）。 */
    if (s_content_h > SRCH_VIEW_H) {
        const int tx = PK_DISPLAY_W - 6;
        const int bar_h = SRCH_VIEW_H * SRCH_VIEW_H / s_content_h;
        const int bar_y = SRCH_LIST_TOP + s_scroll * SRCH_VIEW_H / s_content_h;
        pk_pfd_fill_rect(fb, tx, SRCH_LIST_TOP, tx + 3, PK_DISPLAY_H,
                         pk_rgb565(30, 38, 50));
        pk_pfd_fill_rect(fb, tx, bar_y, tx + 3, bar_y + bar_h,
                         pk_rgb565(120, 135, 155));
    }

    /* 顶栏与查询行最后画：列表从它们底下滑过去，而不是压在上面。 */
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, SRCH_LIST_TOP, col_bg());
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_PAD, PK_UI_TITLE_Y,
               TXT_TITLE, PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
    draw_button(fb, SRCH_CLOSE_X0, SRCH_CLOSE_Y0, SRCH_CLOSE_W, SRCH_CLOSE_H,
                TXT_CLOSE, pk_rgb565(52, 40, 40), col_txt(), PK_AA_S);

    {
        const bool has_q = (s_query[0] != '\0');
        const int box_x1 = has_q ? (SRCH_R - SRCH_CLR_W - 12) : SRCH_R;
        /* 边框 = 外层圆角矩形叠一层内缩的底色，同 keyboard_page 的输入框
         * （pfd_draw 里没有描边接口，两次填充比新增一个只此一处的原语划算）。 */
        pk_pfd_fill_round_rect(fb, SRCH_PAD, SRCH_BOX_Y0, box_x1, SRCH_BOX_Y1,
                               10, col_accent());
        pk_pfd_fill_round_rect(fb, SRCH_PAD + 2, SRCH_BOX_Y0 + 2, box_x1 - 2,
                               SRCH_BOX_Y1 - 2, 8, col_panel());
        const int ty = SRCH_BOX_Y0 + (SRCH_BOX_H - PK_AA_M_H) / 2;
        if (has_q) {
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_PAD + 14, ty,
                       s_query, col_txt(), PK_AA_M);
        } else {
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, SRCH_PAD + 14,
                       SRCH_BOX_Y0 + (SRCH_BOX_H - PK_AA_S_H) / 2,
                       TXT_PLACEHOLDER, col_dim(), PK_AA_S);
        }
        if (has_q)
            draw_button(fb, SRCH_R - SRCH_CLR_W, SRCH_BOX_Y0, SRCH_CLR_W,
                        SRCH_BOX_H, TXT_CLEAR, col_panel(), col_txt(),
                        PK_AA_S);
    }
}

/* ═══════════════ 打开 / 关闭 / 触摸 ═════════════════════════════ */

#ifdef PK_SIM_BUILD
static void sim_setup_once(void);
#endif

void pk_search_page_open(void)
{
    s_query[0]    = '\0';
    s_scroll      = 0;
    s_press_valid = false;
    s_moved       = false;
    s_active      = true;
    /*
     * 藏掉 FAB，理由与 keyboard_page 完全相同：本页铺满全屏、命中判定排在
     * LVGL 之前，FAB 留着就是"它自己点不动、又盖住底下的行"。出口写在屏上：
     * 页首的 CLOSE。
     */
    pk_ui_nav_set_fab_hidden(true);
    /* 一打开就去查「附近」——无键盘设备上，默认视图有内容才是关键体验。 */
    submit("");
#ifdef PK_SIM_BUILD
    sim_setup_once();
#endif
}

bool pk_search_page_active(void) { return s_active; }

void pk_search_page_close(void)
{
    s_active      = false;
    s_press_valid = false;
    pk_ui_nav_set_fab_hidden(false);
}

/* 键盘编辑器的回调。**必须是本文件的 static 函数指针**，不能再走当年那个
 * 全局弱符号——设置页早就占着它了（见 keyboard_page.h）。 */
static void kbd_commit(const char *text)
{
    char q[PK_SEARCH_QUERY_MAX + 1];
    if (pk_search_norm_query(text, q, sizeof(q)) <= 0) {
        /* 清空 = 回默认视图。用户在编辑器里全删掉再确定，意思就是"算了"。 */
        s_query[0] = '\0';
        s_scroll = 0;
        submit("");
        return;
    }
    snprintf(s_query, sizeof(s_query), "%s", q);
    s_scroll = 0;
    hist_remember(q);
    submit(q);
}

static void kbd_cancel(void)
{
    /* 什么都不做：取消就是"保持刚才那一屏"。 */
}

#ifdef PK_SIM_BUILD
/*
 * 截图钩子（同 settings_draw.c 的 pk_settings_sim_scroll 那条先例）：
 *
 *   PK_SIM_SEARCH=ZG            直接摆到结果视图
 *   PK_SIM_SEARCH_HIST=A,B,C    预置最近搜索（A 在最上）
 *   PK_SIM_SEARCH_SCROLL=<px>   滚到指定位置，好截到屏外那几行
 *
 * 走的是与真机**同一条** kbd_commit 路径，不是另塞一份状态——截出来的就是
 * 用户敲完按确定看到的那一屏，包括历史被顺手记上这个副作用。
 */
#include <stdlib.h>
static void sim_setup_once(void)
{
    static bool done;
    if (done) return;
    done = true;

    const char *h = getenv("PK_SIM_SEARCH_HIST");
    if (h != NULL && h[0] != '\0') {
        /* 倒着压：LRU 是"后压的在最前"，而环境变量里第一个才是最想看到的。 */
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", h);
        int starts[32], ns = 0;
        starts[ns++] = 0;
        for (size_t i = 0; buf[i] != '\0'; ++i) {
            if (buf[i] == ',') { buf[i] = '\0'; if (ns < 32) starts[ns++] = (int)i + 1; }
        }
        for (int i = ns - 1; i >= 0; --i) {
            char q[PK_SEARCH_QUERY_MAX + 1];
            if (pk_search_norm_query(buf + starts[i], q, sizeof(q)) > 0)
                pk_search_hist_push(&s_hist, q);
        }
    }

    const char *q = getenv("PK_SIM_SEARCH");
    if (q != NULL && q[0] != '\0') kbd_commit(q);

    const char *s = getenv("PK_SIM_SEARCH_SCROLL");
    if (s != NULL) {
        const int v = atoi(s);
        s_scroll = v < 0 ? 0 : v;
    }
}
#endif

static void open_keyboard(void)
{
    pk_keyboard_page_open(TXT_TITLE, s_query, PK_SEARCH_QUERY_MAX,
                          kbd_commit, kbd_cancel);
}

/*
 * 点一条结果，按类型分两条路（2026-08-02 改，设计文档 D3 的"详情二期"）。
 *
 * **机场 → 详情页**：跑道、频率、标高、管制与否，全是飞行员点进一个机场时
 * 真正要的东西；"跳到地图"只回答了"它在哪"。详情页里有一枚「在地图上显示」，
 * 原来那条路一步都没少，只是从默认动作变成了显式动作。
 *
 * **导航台 / FIX → 维持跳地图**：它们在数据包里只有 ident、类型、频率、
 * 坐标——没有跑道、没有频率表，一整页详情摆出来只有两行，而这两行搜索结果
 * 那一行已经写着了（add_navaid 就把频率拼进了名称列）。为它们各开一页是在
 * 用一次额外点击换零信息。真到了要显示航路/进近程序那一天再谈。
 *
 * 详情页是模态层，**不关搜索页**：关掉详情就自然回到这一屏（见
 * apt_detail_page.h 的「三层导航是怎么解决的」）。
 */
static void goto_item(const pk_search_item_t *it)
{
    if (it->kind == PK_SEARCH_KIND_AIRPORT) {
        pk_apt_detail_page_open(it->idx, PK_APT_DETAIL_FROM_SEARCH);
        return;
    }
    pk_map_page_set_pin(it->lat, it->lon, it->code);
    pk_map_page_goto(it->lat, it->lon, SRCH_GOTO_ZOOM);
    pk_search_page_close();
}

bool pk_search_page_touch(int x, int y)
{
    if (!s_active) return false;
    (void)x;
    s_press_x      = x;
    s_press_y      = y;
    s_press_scroll = s_scroll;
    s_press_valid  = true;
    s_moved        = false;
    /* 整屏都吃：模态页底下没有任何该被点到的东西（FAB 已经藏了）。 */
    return true;
}

bool pk_search_page_drag(int x, int y)
{
    if (!s_active) return false;
    if (!s_press_valid) return true;   /* 仍然吃掉：模态 */
    (void)x;
    const int dy = y - s_press_y;
    if (!s_moved && (dy > SRCH_DRAG_SLOP || dy < -SRCH_DRAG_SLOP)) s_moved = true;
    if (!s_moved) return true;

    const int max_scroll = (s_content_h > SRCH_VIEW_H)
                         ? (s_content_h - SRCH_VIEW_H) : 0;
    int sy = s_press_scroll - dy;      /* 方向与手指一致 */
    if (sy < 0) sy = 0;
    if (sy > max_scroll) sy = max_scroll;
    s_scroll = sy;
    return true;
}

void pk_search_page_touch_cancel(void)
{
    s_press_valid = false;
    s_moved       = false;
}

void pk_search_page_touch_up(void)
{
    const bool click = s_active && s_press_valid && !s_moved;
    const int x = s_press_x, y = s_press_y;
    s_press_valid = false;
    s_moved       = false;
    if (!click) return;

    /* 页首：CLOSE 的命中区是整条右侧带（视觉 40 px，命中 48）。 */
    if (y < SRCH_HDR_H) {
        if (x >= SRCH_CLOSE_X0) pk_search_page_close();
        return;
    }

    /* 查询行：右侧 CLEAR，其余整条带都是"进键盘"。 */
    if (y < SRCH_LIST_TOP) {
        if (s_query[0] != '\0' && x >= SRCH_R - SRCH_CLR_W) {
            s_query[0] = '\0';
            s_scroll = 0;
            submit("");
        } else {
            open_keyboard();
        }
        return;
    }

    for (int i = 0; i < s_nhit; ++i) {
        const row_hit_t *h = &s_hit[i];
        if (y < h->y0 || y >= h->y1) continue;
        if (h->kind == 1) {
            const int pub = s_res_pub;
            if (h->arg >= 0 && h->arg < s_res_n[pub])
                goto_item(&s_res[pub][h->arg]);
        } else if (h->kind == 2) {
            /* 点历史项**跳过防抖直接搜**（照 App 的做法）——历史项是用户
             * 亲手选的，没有"边打边等"那回事。 */
            if (h->arg >= 0 && h->arg < s_hist.n) {
                snprintf(s_query, sizeof(s_query), "%s", s_hist.items[h->arg]);
                s_scroll = 0;
                hist_remember(s_query);
                submit(s_query);
            }
        }
        return;
    }
}

/* ── 真机自检（见文件上方 PK_SEARCH_PAGE_SMOKE）────────────────────
 *
 * 风格照 pk_aero_db.c 的 aero_smoke_check：只打日志、不断言真值——真值随
 * AIRAC 周期变，硬断言只会把"换了数据"误报成"坏了"。 */
#if PK_SEARCH_PAGE_SMOKE && !defined(PK_SIM_BUILD)
static void smoke_run(void)
{
    /* 等库加载完（懒加载定案：开机静默 5 s 后才开始读卡，全量库约 2 s）。 */
    for (int i = 0; i < 600 && pk_aero_db_state() != PK_AERO_DB_READY; ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (pk_aero_db_state() != PK_AERO_DB_READY) {
        ESP_LOGW(TAG, "smoke: aero DB never became READY — skipped");
        return;
    }

    /*
     * 这里**刻意不调** pk_search_page_open() / _close()。
     *
     * 那两个函数会碰 pk_ui_nav_set_fab_hidden()，而它是 LVGL 对象操作——本项目
     * 的 LVGL 没有开 LV_OS 锁，整个控件树由 pfd 任务独占（触摸回调也跑在那个
     * 任务里，所以真机上的用户路径没问题）。从本后台任务里调进去实测会把 LVGL
     * 卡死：2026-08-02 第一版自检就停在 "open page" 之后一行不出，随后 task_wdt
     * 每 15 s 点名 pfd。
     *
     * 所以自检只跑**数据链路**：提交查询 → 读快照 → 跳地图 + 落 PIN。页面的
     * 开关归触摸路径，那条路径本来就在正确的任务里。
     */
    ESP_LOGI(TAG, "smoke: query ZGGG (数据链路；页面开关归触摸路径)");
    /* submit() 只是置 BUSY + 通知；本函数就跑在那个后台任务自己身上，
     * 干等不会有人来做，所以直接同步跑一遍作业。 */
    run_job("ZGGG");

    const int pub = s_res_pub;
    ESP_LOGI(TAG, "smoke: status=%u hits=%d (db v%u)",
             (unsigned)s_status, s_res_n[pub], (unsigned)s_res_dbver);
    for (int i = 0; i < s_res_n[pub] && i < 3; ++i) {
        const pk_search_item_t *it = &s_res[pub][i];
        ESP_LOGI(TAG, "smoke:   [%d] %s %-4s \"%s\" (%.4f,%.4f) %s",
                 i, kind_tag(it->kind), it->code, it->name, it->lat, it->lon,
                 it->have_dist ? "dist ok" : "no own pos");
    }
    if (s_res_n[pub] > 0) {
        /* 点结果那一下真正做的两件事（goto_item 里除关页之外的全部）。 */
        const pk_search_item_t *it = &s_res[pub][0];
        pk_map_page_set_pin(it->lat, it->lon, it->code);
        pk_map_page_goto(it->lat, it->lon, SRCH_GOTO_ZOOM);
        ESP_LOGI(TAG, "smoke: goto %s (%.4f,%.4f) z%d -> map centred + PIN set",
                 it->code, it->lat, it->lon, SRCH_GOTO_ZOOM);
    } else {
        ESP_LOGW(TAG, "smoke: no hits — v2 card or regional data set?");
    }

    /* 第 5 桶（子串 + 分段让渡锁）的真机耗时，两头都量：
     *   典型：命中够 max 就提前收工；
     *   最坏：一条都不命中 → 三段字符串池全扫一遍。
     * 这两个数决定了"要不要给它 loading 态"（要）和"能不能在没写满 12 条时
     * 就顺手跑一趟"（不能，见 run_buckets 里桶 5 的门槛）。 */
    pk_aero_hit_t hits[PK_SEARCH_MAX_RESULTS];
    int64_t t0 = esp_timer_get_time();
    int nh = pk_aero_db_search_substring("GUANGZHOU", hits, PK_SEARCH_MAX_RESULTS);
    ESP_LOGI(TAG, "smoke: substring 典型 \"GUANGZHOU\" -> %d hits in %lld ms",
             nh, (long long)((esp_timer_get_time() - t0) / 1000));

    t0 = esp_timer_get_time();
    nh = pk_aero_db_search_substring("QQZZXX", hits, PK_SEARCH_MAX_RESULTS);
    ESP_LOGI(TAG, "smoke: substring 最坏 \"QQZZXX\" -> %d hits in %lld ms "
                  "(全池顺扫；期间 IDLE0 必须还能喂狗)",
             nh, (long long)((esp_timer_get_time() - t0) / 1000));

    /* 切到地图页：让验收人在屏上直接看到 PIN，也顺便量一次地图页帧率
     * （基线 8 FPS）。pk_ui_set_mode 是 ui_state 里的线程安全 setter，
     * 不碰 LVGL，可以从本任务调。 */
    pk_ui_set_mode(PK_UI_MODE_MAP);
    ESP_LOGI(TAG, "smoke: switched to MAP page — PIN should be on screen");
}
#endif /* PK_SEARCH_PAGE_SMOKE */

#endif /* !PK_SEARCH_PAGE_HOST_TEST */
