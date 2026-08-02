/*
 * pk_aero_layer.c — 实现说明见 pk_aero_layer.h。
 *
 * 文件结构：
 *   1) 纯函数区（LOD 表 / Web Mercator / 标签避让）——无 OS、无全局状态，
 *      host 单测 firmware/test/test_pk_aero_layer.c 直接把本文件编进去，
 *      靠 PK_AERO_LAYER_HOST_TEST 把下面两段切掉（照 traffic_geom 那种
 *      "纯计算能在 host 上证明"的路子，只是这里纯的与不纯的同住一个文件）。
 *   2) 快照区——后台任务查 pk_aero_db、写 PSRAM 双缓冲。
 *   3) 渲染区——只读快照，画符号与标签。
 */
#include "pk_aero_layer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ══ 1. 纯函数区 ══════════════════════════════════════════════════════ */

/* 枚举取值照抄 Pilot-Kit 仓 scripts/aero_data_pipeline/export_box_bin.py
 * 的 AIRPORT_TYPE_ENUM / CTRL_ENUM / NAVAID_TYPE_ENUM（"固件侧照此写死；
 * 未知值一律归 0"）。生成器改了这里就得跟着改，没有第三方能替我们发现。 */
#define APT_TYPE_OTHER      0
#define APT_TYPE_AD         1   /* 正经机场；2=直升机坪 3=水上 4=超轻 5=滑翔 6=军用 */
#define APT_CTRL_UNKNOWN    0
#define APT_CTRL_UNCTRL     1
#define APT_CTRL_CTRL       2

#define NAV_TYPE_VOR        1
#define NAV_TYPE_VOR_DME    2
#define NAV_TYPE_VORTAC     3
#define NAV_TYPE_NDB        4
#define NAV_TYPE_NDB_DME    5
#define NAV_TYPE_DME        6
#define NAV_TYPE_TACAN      7

#define FIX_SCOPE_ENROUTE   1   /* 0=未知 2=终端区（见 pk_aero_reader.h:63-67）*/
#define FIX_SCOPE_BOTH      3

/* LOD 表。与设计文档 §2.2 的表一一对应，**但 limit 全部被压到 32**：
 * pk_aero_db_nearest_* 的 max 上限是 PK_AERO_NEAR_MAX(=32)，设计表里写的
 * 40/80/120/150 一次查询根本拿不到。要拿到更多只能按九宫格分点多查几次，
 * 那是成倍的毫秒成本，而 800×480 上同时摆 32 个机场 + 32 个导航台 +
 * 32 个 FIX 已经很挤了——先按 32 落地，密度真不够再谈分点查询。 */
pk_aero_lod_t pk_aero_lod_for_zoom(uint8_t zoom)
{
    pk_aero_lod_t l = {0};
    if (zoom <= 6) {
        /* 洲际视野：只留"看得出是个大场"的——有 ICAO 码且跑道够长，
         * 管制机场豁免长度（管制本身就说明它有量）。不画点状要素：
         * 这个尺度上导航台和 FIX 会糊成一片噪点。 */
        l.airport_limit = 20;  l.airport_min_rwy_ft = 8000; l.airport_need_icao = true;
        l.label = 0;
    } else if (zoom <= 8) {
        l.airport_limit = 24;  l.airport_min_rwy_ft = 5000; l.airport_need_icao = true;
        l.navaid_limit  = 16;  l.navaid_vor_only = true;
        l.label = 1;           /* 仅代码 */
    } else if (zoom <= 11) {
        l.airport_limit = 32;  l.airport_min_rwy_ft = 0;
        l.navaid_limit  = 24;
        l.fix_limit     = 24;  l.fix_enroute_only = true;
        l.label = 2;           /* 代码 + 名称 */
    } else {
        l.airport_limit = 32;  l.airport_min_rwy_ft = 0;
        l.navaid_limit  = 32;
        l.fix_limit     = 32;
        l.label = 3;           /* 代码 + 名称 + 标高 */
    }
    return l;
}

bool pk_aero_lod_airport_pass(const pk_aero_lod_t *lod, uint8_t type, uint8_t ctrl,
                              uint16_t longest_rwy_ft, bool has_icao)
{
    if (lod->airport_limit == 0) return false;
    /* 直升机坪 / 水上机场 / 超轻 / 滑翔场只在最大两档露面：它们数量极多、
     * 对固定翼飞行没有落地价值，低 zoom 挤掉真机场就是帮倒忙。 */
    if (type != APT_TYPE_AD && type != APT_TYPE_OTHER && lod->airport_min_rwy_ft > 0)
        return false;
    if (lod->airport_need_icao && !has_icao) return false;
    if (longest_rwy_ft < lod->airport_min_rwy_ft && ctrl != APT_CTRL_CTRL) return false;
    return true;
}

bool pk_aero_lod_navaid_pass(const pk_aero_lod_t *lod, uint8_t type)
{
    if (lod->navaid_limit == 0) return false;
    if (lod->navaid_vor_only)
        return type == NAV_TYPE_VOR || type == NAV_TYPE_VOR_DME || type == NAV_TYPE_VORTAC;
    return type >= NAV_TYPE_VOR && type <= NAV_TYPE_TACAN;
}

bool pk_aero_lod_fix_pass(const pk_aero_lod_t *lod, uint8_t scope)
{
    if (lod->fix_limit == 0) return false;
    if (lod->fix_enroute_only)
        return scope == FIX_SCOPE_ENROUTE || scope == FIX_SCOPE_BOTH;
    return true;
}

/* 密度降级：LOD 表按 zoom 决定"最多能显示到哪一档"，这里再按**当前这一屏
 * 实际有多少个要素**往下压一档。
 *
 * 为什么 LOD 一档不够：LOD 只看 zoom，而同一个 zoom 下珠三角和塔克拉玛干的
 * 密度差一个数量级。Z10 的珠三角一屏 24 个要素，每条标签都是"代码+名称"，
 * 一条 "ZGSZ Shenzhen Baoan..." 就横跨 200 px——压过邻近符号、挤掉后面所有
 * 标签（包括 ADS-B 呼号）。代码是飞行员真正会念、会在频率里听到的东西，
 * 名称只是锦上添花；挤的时候先丢名称，这是航图和 EFB 的通行做法。
 *
 * 阈值取自模拟器最糟情况截图（800×480 的地图区 ≈ 800×432）：
 *   - >10 个：还塞得下代码+名称，但标高那一档（+7 字节）开始互相顶，降到 2；
 *   - >20 个：平均每个要素只剩 34×43 px 的地盘，只有代码才放得下，降到 1。
 * 计数只算**画在屏上**的那些（快照里有一半可能在视野外）。 */
uint8_t pk_aero_label_mode(uint8_t lod_label, int n_visible)
{
    if (lod_label == 0) return 0;
    if (n_visible > 20) return 1;
    if (n_visible > 10 && lod_label > 2) return 2;
    return lod_label;
}

/* 罗盘玫瑰（VOR 系导航台外圈的刻度环）画不画。
 *
 * 用户要求"VOR DME 按规范加一个外层圆圈和标记角度"。航图上那个圈是**方位
 * 基准**：VOR 的径向线从它上面读，所以它只属于 VOR/VOR-DME/VORTAC，NDB 和
 * 单独的 DME/TACAN 没有（NDB 不给方位，DME 只给距离）。
 *
 * 但它是这一层里最占地方的东西：直径 52 px，比一个 VOR 符号大 3 倍。三条闸门：
 *   1) zoom < 11 不画。低 zoom 的一屏跨几百海里，圈上的 10° 刻度对应的地面
 *      距离毫无意义，纯粹是噪点；Z11 起一屏约 30 NM，读径向才开始有用。
 *   2) 屏上 VOR 系导航台 > 4 个不画。每个圈连刻度是 36 条短线 + 一个环，
 *      4 个圈是这一层绘制成本的上限（估算见 draw_compass_rose 的注释）；
 *      更要紧的是版面——5 个圈的外接盒就吃掉地图区 1/6，标签全被顶掉。
 *   3) 屏上要素总数 > 16 不画。跟 pk_aero_label_mode 同一个思路（那边是
 *      >10 降标高、>20 只留代码），密度上来的时候先丢装饰、保代码。
 *
 * 阈值与 label_mode 的 10/20 不同是有意的：一条标签占 ~34×17 px，一个玫瑰
 * 占 52×52 px ≈ 4.7 条标签的面积，所以它的闸门必须更早关上。 */
bool pk_aero_rose_enabled(uint8_t zoom, int n_visible, int n_rose_navaid)
{
    if (zoom < 11) return false;
    if (n_rose_navaid > 4) return false;
    if (n_visible > 16) return false;
    return true;
}

void pk_aero_lonlat_to_world(double lon, double lat, uint8_t z, double *wx, double *wy)
{
    /* 与 map_page.c:110 lonlat_to_world 逐行一致——两边任何一处改了，
     * 另一处必须同步，否则叠加层会整体偏移。 */
    if (lat >  85.0511) lat =  85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double n = (double)(1u << z) * 256.0;
    double latrad = lat * M_PI / 180.0;
    *wx = (lon + 180.0) / 360.0 * n;
    *wy = (0.5 - log(tan(M_PI / 4.0 + latrad / 2.0)) / (2.0 * M_PI)) * n;
}

static int rect_overlap_area(const pk_aero_rect_t *a, const pk_aero_rect_t *b)
{
    int x0 = a->x0 > b->x0 ? a->x0 : b->x0;
    int x1 = a->x1 < b->x1 ? a->x1 : b->x1;
    if (x1 <= x0) return 0;
    int y0 = a->y0 > b->y0 ? a->y0 : b->y0;
    int y1 = a->y1 < b->y1 ? a->y1 : b->y1;
    if (y1 <= y0) return 0;
    return (x1 - x0) * (y1 - y0);
}

bool pk_aero_label_place(int sx, int sy, int sym_r, int lw, int lh,
                         const pk_aero_rect_t *occ, int nocc,
                         pk_aero_rect_t *out)
{
    const int gap = 3, pad = 2;
    const int w = lw + 2 * pad;
    pk_aero_rect_t cand[4];
    /* 顺序 bottom → right → left → top（App 同款）。bottom 排头是因为符号
     * 下方最少与其它要素抢位，且读图时视线自然向下。 */
    cand[0] = (pk_aero_rect_t){ sx - w / 2, sy + sym_r + gap,
                                sx - w / 2 + w, sy + sym_r + gap + lh };
    cand[1] = (pk_aero_rect_t){ sx + sym_r + gap, sy - lh / 2,
                                sx + sym_r + gap + w, sy - lh / 2 + lh };
    cand[2] = (pk_aero_rect_t){ sx - sym_r - gap - w, sy - lh / 2,
                                sx - sym_r - gap, sy - lh / 2 + lh };
    cand[3] = (pk_aero_rect_t){ sx - w / 2, sy - sym_r - gap - lh,
                                sx - w / 2 + w, sy - sym_r - gap };

    int best = -1, best_ov = 0;
    for (int i = 0; i < 4; i++) {
        int ov = 0;
        for (int j = 0; j < nocc; j++) ov += rect_overlap_area(&cand[i], &occ[j]);
        if (ov == 0) { *out = cand[i]; return true; }   /* 零重叠即停 */
        if (best < 0 || ov < best_ov) { best = i; best_ov = ov; }
    }
    /* 四个方向都撞：取最小重叠；重叠超过标签面积一半就整条不画——
     * 半遮的文字比没有文字更坏，读图时会误认成别的机场的代码。 */
    if (best < 0 || best_ov * 2 > w * lh) return false;
    *out = cand[best];
    return true;
}

/* 机场名在标签里的显示预算（字节）。**不是**快照 name[28] 的大小——那是存储，
 * 这两个是排版，取值靠屏宽算：
 *   PK_AA_XS 一个 ASCII 字宽 10 px（pfd_aa_font.h:8），put_label 左右各 2 px
 *   padding。单条标签的宽度上限定在屏宽 1/3 ≈ 266 px，也就是 26 个字节：
 *   再宽一条标签就横跨小半个地图，而 pk_aero_label_place 只有 4 个候选位，
 *   标签越宽越容易四向全撞、直接被整条丢掉（下面那条 >50% 隐藏规则），
 *   32 个机场挤在 800×432 的地图区里，宽标签是在拿"多几个字"换"少几条标签"。
 *   - label==3：代码 ≤4（ICAO）+ 两个空格 + 标高 "12345ft" ≤7 = 13 字节开销，
 *     名称剩 26-13 = 13。
 *   - label==2：只有代码 + 空格 = 5 字节开销，名称剩 21。
 * 顺带回答"快照 name[28] 要不要加大"：不加。两档预算都 ≤21 < 27，存着的 27
 * 字节永远吃不完；加大只是白占 PSRAM（每加 1 字节 = 2 份快照 × 32 项 = 64 B），
 * 一个字都多显示不出来。等哪天标签排版真的放宽了，再一起动。 */
#define NAME_CAP_ELEV   13
#define NAME_CAP_PLAIN  21

/* 机场名按"词边界 + 省略号"收进 dst（dst_sz 就是预算：最多 dst_sz-1 个字节）。
 *
 * 为什么非改不可：数据侧把中文机场名做了音译之后，名字从一串 '?' 变成拼音，
 * 平均 29.4 字节、最长 85（"Shenyang Shenfu Gaige Chuangxin Shifanqu Shehui
 * Zhili Guanli Zhihui Zhongxin Heliport"）。原来那种直接 snprintf 硬切，屏上
 * 出来的是 "Shenyang Shenfu Gaige Chuan" 这种半截词——半截词和半遮的文字一样
 * 坏，读图时会当成另一个地名。
 *
 * 规则：
 *   1) 右侧空格一律先修掉（数据里确实有带尾空格的名字）；
 *   2) 修完能装下就原样拷贝，不加省略号；
 *   3) 装不下：留 3 字节给 "..."，在预算内**最后一个完整单词**处断开；
 *   4) 第一个词本身就超预算：硬切 + "..."。
 * 省略号用 ASCII 三个点而不是 U+2026——字库是 catalog 驱动的子集，单字符省略号
 * 不在里面会渲染成 '?'（见 gen_i18n_assets.py 那套流程）。
 *
 * 预算按**字节**算。名称现在是纯 ASCII 拼音，1 字节 = 1 个 PK_AA_XS 字宽；
 * 万一将来又混进多字节字符，硬切那一路会退回到字符起始字节，不会切出半个
 * UTF-8 序列（切一半的后果是渲染成 '?'）。 */
static void name_ellipsize(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz == 0) return;
    dst[0] = '\0';
    if (src == NULL) return;
    /* 预算连 "..." 都放不下就没得谈，直接给空串——上限是编译期常量，
     * 走到这里说明调用方配错了，画个空标签比画个 "." 诚实。 */
    if (dst_sz < 4) return;

    const size_t cap = dst_sz - 1;
    size_t len = strlen(src);
    while (len > 0 && src[len - 1] == ' ') len--;      /* 规则 1 */

    if (len <= cap) {                                   /* 规则 2 */
        memcpy(dst, src, len);
        dst[len] = '\0';
        return;
    }

    size_t keep = cap - 3;                              /* 给 "..." 留位 */
    size_t cut = keep;
    while (cut > 0 && src[cut] != ' ') cut--;           /* 规则 3：回退到词界 */
    while (cut > 0 && src[cut - 1] == ' ') cut--;       /* 词界左边的空格也不要 */
    if (cut == 0) {                                     /* 规则 4：首词就超长 */
        cut = keep;
        while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
    }

    memcpy(dst, src, cut);
    memcpy(dst + cut, "...", 3);
    dst[cut + 3] = '\0';
}

#ifndef PK_AERO_LAYER_HOST_TEST

/* ══ 2. 快照区（后台任务）═════════════════════════════════════════════ */

#include "esp_attr.h"
#include "esp_timer.h"
#ifndef PK_AERO_LAYER_SIM_IMPL
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#endif

#include "display.h"        /* PK_DISPLAY_W/H、pk_rgb565 */
#include "pfd_layout.h"     /* PFD_BAR_BOT */
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "mag_var.h"        /* 罗盘玫瑰要按磁差旋转 */
#include "pk_aero_db.h"

#ifndef PK_AERO_LAYER_SIM_IMPL
static const char *TAG = "aero_layer";
#endif

#define LAYER_TASK_STACK   (8 * 1024)
#define LAYER_TASK_PRIO    2          /* 同 pk_aero_db 的后台任务，让路给渲染 */
#define LAYER_TASK_CORE    0
#define LAYER_POLL_MS      200
/* 两次发布之间的最短间隔。除了限流，它还是双缓冲安全性的**依据**：见下方
 * s_active 的注释。 */
#define LAYER_MIN_REQUERY_MS  500

#define SNAP_MAX  PK_AERO_NEAR_MAX    /* 32；LOD 的 limit 也都收在这个数以内 */

typedef struct {
    double   lat, lon;
    char     code[8];              /* ICAO；无 ICAO 时退 IATA，再无则 "" */
    char     name[28];
    uint8_t  type, ctrl;
    uint16_t longest_rwy_ft;
    int16_t  elev_ft;
} apt_ent_t;

typedef struct {
    double  lat, lon;
    char    ident[8];
    uint8_t type;
} nav_ent_t;

typedef struct {
    double lat, lon;
    char   ident[8];
} fix_ent_t;

typedef struct {
    uint8_t   zoom;
    double    lat, lon;            /* 生成这份快照时的视图中心 */
    int       n_apt, n_nav, n_fix;
    apt_ent_t apt[SNAP_MAX];
    nav_ent_t nav[SNAP_MAX];
    fix_ent_t fix[SNAP_MAX];
} snapshot_t;

/* 大静态一律进 PSRAM .bss——内部 RAM 是硬约束，理由见 pk_tile_loader.c:52
 * 那段（P4 rev<v3 调度器启动前只有 ~65 KB 可分配窗口，多几 KB 就 boot loop）。
 * 两份快照约 6 KB，放内部 .bss 正好是压垮那个窗口的量级。 */
EXT_RAM_BSS_ATTR static snapshot_t s_snap[2];

/* 双缓冲：后台任务只写"非活动"的那份，填完再把下标写进 s_active 发布。
 * 渲染端每帧读一次 s_active 就一直用那份。
 *
 * 为什么不用锁：渲染线程 30 FPS，等一把可能被毫秒级查询持有的锁就是掉帧。
 * 为什么这样安全：一帧的读取窗口 ≤33 ms，而后台两次发布至少隔
 * LAYER_MIN_REQUERY_MS(500 ms) + 查询耗时——渲染正在读的那份缓冲，要被覆盖
 * 得等后台连发两次，也就是 1 s 之后，窗口宽了一个数量级不止。
 * s_active 本身是 32 位对齐的 int，RV32 上单次读写不会撕裂。
 * -1 = 尚无有效快照（开机 / 拔卡后）。 */
static volatile int s_active = -1;

/* 视图状态：渲染线程写、后台任务读。存 1e-7 定点整数而不是 double，是为了
 * 让每个字段的读写都是单条 32 位访存（RV32 上 double 要两条，会撕裂成
 * "上半新下半旧"的假坐标）。精度 ~1 cm，绰绰有余。 */
static volatile int32_t s_view_lat_e7, s_view_lon_e7;
static volatile uint8_t s_view_zoom = 0xFF;   /* 0xFF = 地图页还没渲染过 */

#ifndef PK_AERO_LAYER_SIM_IMPL
void pk_aero_layer_notify_view(double center_lat, double center_lon, uint8_t zoom)
{
    s_view_lat_e7 = (int32_t)lround(center_lat * 1e7);
    s_view_lon_e7 = (int32_t)lround(center_lon * 1e7);
    s_view_zoom   = zoom;
}
#endif  /* sim 版在文件下方：存完值还要就地查一次，没有后台任务替它查 */

/* 需不需要重查：无快照 / 换 zoom / 中心移动超过视口 1/4。
 * "视口 1/4" 直接在世界像素域上量——这正是 App 那套"视口 20% 扩展 + 包含
 * 判定"的等价简化：屏幕是 800×480，移动不到 200 px 时旧结果仍然盖得住
 * 视野，没必要为此付一次毫秒级查询。 */
static bool need_requery(double lat, double lon, uint8_t zoom, const snapshot_t *cur)
{
    if (cur == NULL || cur->zoom != zoom) return true;
    double wx0, wy0, wx1, wy1;
    pk_aero_lonlat_to_world(cur->lon, cur->lat, zoom, &wx0, &wy0);
    pk_aero_lonlat_to_world(lon, lat, zoom, &wx1, &wy1);
    return fabs(wx1 - wx0) > PK_DISPLAY_W / 4.0 ||
           fabs(wy1 - wy0) > PK_DISPLAY_H / 4.0;
}

static void fill_airports(snapshot_t *s, const pk_aero_lod_t *lod,
                          double lat, double lon)
{
    pk_aero_near_t near[PK_AERO_NEAR_MAX];
    /* 一律按上限取回再过滤，而不是把 lod->airport_limit 直接当 max：过滤会
     * 淘汰掉一部分（低 zoom 尤其狠），先要满再筛才填得满一屏。 */
    int n = pk_aero_db_nearest_airports(lat, lon, near, PK_AERO_NEAR_MAX);
    for (int i = 0; i < n && s->n_apt < lod->airport_limit; i++) {
        pk_aero_airport_t a;
        if (!pk_aero_db_airport_get(near[i].idx, &a)) continue;
        if (!pk_aero_lod_airport_pass(lod, a.type, a.ctrl, a.longest_rwy_ft,
                                      a.icao[0] != '\0'))
            continue;
        apt_ent_t *e = &s->apt[s->n_apt++];
        e->lat = a.lat;  e->lon = a.lon;
        e->type = a.type; e->ctrl = a.ctrl;
        e->longest_rwy_ft = a.longest_rwy_ft;
        e->elev_ft = a.elev_ft;
        /* 字符串必须拷走：a.name/a.icao 指向 PSRAM 池，拔卡即悬空
         * （pk_aero_db.h:23-25）。跨帧留指针 = 迟早读到 free 掉的内存。 */
        snprintf(e->code, sizeof(e->code), "%s",
                 a.icao[0] ? a.icao : (a.iata[0] ? a.iata : ""));
        snprintf(e->name, sizeof(e->name), "%s", a.name ? a.name : "");
    }
}

static void fill_navaids(snapshot_t *s, const pk_aero_lod_t *lod,
                         double lat, double lon)
{
    pk_aero_near_t near[PK_AERO_NEAR_MAX];
    int n = pk_aero_db_nearest_navaids(lat, lon, near, PK_AERO_NEAR_MAX);
    for (int i = 0; i < n && s->n_nav < lod->navaid_limit; i++) {
        pk_aero_navaid_t v;
        if (!pk_aero_db_navaid_get(near[i].idx, &v)) continue;
        if (!pk_aero_lod_navaid_pass(lod, v.type)) continue;
        nav_ent_t *e = &s->nav[s->n_nav++];
        e->lat = v.lat;  e->lon = v.lon;  e->type = v.type;
        snprintf(e->ident, sizeof(e->ident), "%s", v.ident);
    }
}

static void fill_fixes(snapshot_t *s, const pk_aero_lod_t *lod,
                       double lat, double lon)
{
    pk_aero_near_t near[PK_AERO_NEAR_MAX];
    int n = pk_aero_db_nearest_fixes(lat, lon, near, PK_AERO_NEAR_MAX);
    for (int i = 0; i < n && s->n_fix < lod->fix_limit; i++) {
        pk_aero_fix_t f;
        if (!pk_aero_db_fix_get(near[i].idx, &f)) continue;
        if (!pk_aero_lod_fix_pass(lod, f.scope)) continue;
        fix_ent_t *e = &s->fix[s->n_fix++];
        e->lat = f.lat;  e->lon = f.lon;
        snprintf(e->ident, sizeof(e->ident), "%s", f.ident);
    }
}

/* 查库 → 填非活动缓冲 → 发布。返回耗时毫秒（日志用）。
 * 提出来是因为 sim 走同步路径要用同一份填充逻辑（见文件末的 sim 分支）——
 * 桩掉的只是 pk_aero_db_*，LOD 过滤与快照结构必须是真实那一份，否则在
 * 模拟器上评审出来的版面在真机上不成立。 */
static int publish_snapshot(double lat, double lon, uint8_t zoom)
{
    const int act = s_active;
    const int nb = (act == 0) ? 1 : 0;
    snapshot_t *s = &s_snap[nb];
    s->zoom = zoom;  s->lat = lat;  s->lon = lon;
    s->n_apt = s->n_nav = s->n_fix = 0;

    const pk_aero_lod_t lod = pk_aero_lod_for_zoom(zoom);
    const int64_t t0 = esp_timer_get_time();
    if (lod.airport_limit) fill_airports(s, &lod, lat, lon);
    if (lod.navaid_limit)  fill_navaids(s, &lod, lat, lon);
    if (lod.fix_limit)     fill_fixes(s, &lod, lat, lon);

    s_active = nb;                              /* 发布 */
    return (int)((esp_timer_get_time() - t0) / 1000);
}

#ifndef PK_AERO_LAYER_SIM_IMPL

static void aero_layer_task(void *arg)
{
    (void)arg;
    uint32_t last_pub_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LAYER_POLL_MS));

        /* 拔卡 / 还在加载：作废快照，render 立刻什么都不画。留着旧快照更糟
         * ——数据源已经没了，屏上却还有机场，那是在说谎。 */
        if (pk_aero_db_state() != PK_AERO_DB_READY) { s_active = -1; continue; }

        const uint8_t zoom = s_view_zoom;
        if (zoom == 0xFF) continue;            /* 地图页还没进过，不预热 */
        const double lat = (double)s_view_lat_e7 * 1e-7;
        const double lon = (double)s_view_lon_e7 * 1e-7;

        const int act = s_active;
        const snapshot_t *cur = (act >= 0) ? &s_snap[act] : NULL;
        if (!need_requery(lat, lon, zoom, cur)) continue;

        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (act >= 0 && (uint32_t)(now_ms - last_pub_ms) < LAYER_MIN_REQUERY_MS)
            continue;

        const int dt_ms = publish_snapshot(lat, lon, zoom);
        last_pub_ms = now_ms;
        const snapshot_t *s = &s_snap[s_active];
        ESP_LOGI(TAG, "snapshot: %d airports, %d navaids, %d fixes @ zoom %u "
                      "(%.4f,%.4f) in %dms",
                 s->n_apt, s->n_nav, s->n_fix, (unsigned)zoom, lat, lon, dt_ms);
    }
}

void pk_aero_layer_init(void)
{
    static bool started = false;
    if (started) return;            /* 幂等 */
    started = true;
    BaseType_t ok = xTaskCreatePinnedToCore(aero_layer_task, "aero_layer",
                                            LAYER_TASK_STACK, NULL,
                                            LAYER_TASK_PRIO, NULL, LAYER_TASK_CORE);
    if (ok != pdTRUE) ESP_LOGE(TAG, "aero_layer task create failed");
}

#else  /* PK_AERO_LAYER_SIM_IMPL */

/* sim 是单线程、跑一帧就截图退出的工具，没有 FreeRTOS，也没有"查库会卡帧"
 * 这个要解决的问题（桩数据是内存里的常量表，微秒级）。所以后台任务整个不要，
 * 改成 notify_view 就地查一次——照 compat/pk_tile_loader_sim.c 把异步实现
 * 换成同步实现的先例。渲染端读到的仍是同一套双缓冲快照。 */
void pk_aero_layer_init(void) { }

void pk_aero_layer_notify_view(double center_lat, double center_lon, uint8_t zoom)
{
    s_view_lat_e7 = (int32_t)lround(center_lat * 1e7);
    s_view_lon_e7 = (int32_t)lround(center_lon * 1e7);
    s_view_zoom   = zoom;

    if (pk_aero_db_state() != PK_AERO_DB_READY) { s_active = -1; return; }
    const int act = s_active;
    const snapshot_t *cur = (act >= 0) ? &s_snap[act] : NULL;
    if (!need_requery(center_lat, center_lon, zoom, cur)) return;
    (void)publish_snapshot(center_lat, center_lon, zoom);
}

#endif /* PK_AERO_LAYER_SIM_IMPL */

/* ══ 3. 渲染区（地图页渲染线程）═══════════════════════════════════════ */

/* 视口几何与 map_page.c:44-46 同源。那三个宏是 map_page 的私有宏，这里照抄
 * 一份而不是导出——同上，减少与地图页抢改同一个文件的面。 */
#define AERO_MAP_TOP   PFD_BAR_BOT
#define AERO_MCX       (PK_DISPLAY_W / 2)
#define AERO_MCY       (AERO_MAP_TOP + (PK_DISPLAY_H - AERO_MAP_TOP) / 2)

/* 配色。
 *
 * 2026-08-02 返工，起因是用户实机反馈"机场/导航台/FIX 的标签和 ADS-B 飞机的
 * 标签没有任何区别"。模拟器截图核实，两条根因：
 *
 *   1) **所有航空标签共用一个浅灰白 (226,232,240)**，而 map_page.c:397 的
 *      ADS-B callsign 用的是 (207,211,220)——同一族浅灰白，肉眼不可分。屏上
 *      "TAK"（DME）、"IDUMA"（FIX）、"ZGSZ Shenzhen Baoan..."（管制机场）、
 *      飞机呼号，四种完全不同的东西长得一模一样。符号那一层本来是按类型分色
 *      的，标签却把这份信息全丢了。
 *   2) 上一版这段注释写的"底图是 OSM 栅格，整体偏亮，深色符号才咬得住"
 *      **与出厂的底图包不符**：tmp/sd-maps 那 4 个 pmtiles（真机同一批）是
 *      深色主题，截图里深青导航台糊在深绿植被上、深紫 FIX 几乎看不见。
 *
 * 现在的规则（按航图/座舱显示惯例）：
 *   - 每个类型一对颜色：**符号取中亮度、标签取同色系亮化版**，标签的颜色
 *     本身就是类型标识，不用再加前缀字符（何况非 ASCII 字符会被 catalog
 *     子集字库渲染成 '?'）。
 *   - 静态设施走蓝/洋红/绿/紫，**动态交通（ADS-B）留给亮青 (0,210,235) 与
 *     浅灰白**——"会动的目标"独占一个色系。导航台因此从深青挪到翠绿，不再
 *     和飞机符号撞色系（改导航台不改 ADS-B：交通是既有功能，且 map_page.c
 *     正在被另一路改动占用）。
 *   - RGB565 只有 5/6/5 位，低饱和度会在暗底衬（darken_rect）上糊成一片，
 *     所以每个类型都取高饱和；灰色那一档是故意的（直升机坪/水上机场本来就
 *     不该抢眼），用偏蓝的浅灰与浅灰白的 ADS-B 拉开一点。 */
#define COL_APT_CTRL   pk_rgb565( 60, 130, 235)   /* 管制机场：亮蓝 */
#define COL_APT_UNCTRL pk_rgb565(225,  60, 110)   /* 非管制机场：洋红 */
#define COL_APT_OTHER  pk_rgb565(150, 165, 185)   /* 直升机坪/水上/滑翔等：灰蓝 */
#define COL_NAV        pk_rgb565( 40, 190, 110)   /* 导航台：翠绿（避开 ADS-B 青）*/
#define COL_FIX        pk_rgb565(160, 110, 235)   /* FIX：亮紫 */

/* 标签色 = 对应符号的亮化版。亮度都压在 240 以内：纯白级别的字在 130 档
 * 暗底衬上会晕开，反而不如稍暗一点锐利。 */
#define COL_LBL_APT_CTRL   pk_rgb565(140, 195, 255)
#define COL_LBL_APT_UNCTRL pk_rgb565(255, 150, 185)
/* 直升机坪/水上机场这一档最容易与 ADS-B 的浅灰白 (207,211,220) 混：截图对拍
 * 后往蓝里再压一档（去饱和的板岩蓝），既跟浅灰白拉开，又比管制机场的亮蓝暗
 * 一个档次——它本来就不该抢眼。 */
#define COL_LBL_APT_OTHER  pk_rgb565(150, 178, 210)
#define COL_LBL_NAV        pk_rgb565(120, 240, 170)
#define COL_LBL_FIX        pk_rgb565(205, 165, 255)

/* "有硬跑道"这条设计规则在数据侧落不了地：surface 字段在 runway_dirs 记录
 * 里，airport 记录只有 longest_rwy_ft（pk_aero_reader.h:130-141），为一个
 * 圆圈的实心与否再去读一遍跑道段不值当。用跑道长度当代理量：≥3000 ft 基本
 * 就是铺装跑道的量级。同理设计文档里的"关闭灰"没有实现——**数据里没有
 * closed 标志位**，宁可不画这个区分，也不要靠猜给飞行员上色。 */
#define APT_PAVED_FT   3000

static bool on_screen(int sx, int sy)
{
    return sx >= -16 && sx < PK_DISPLAY_W + 16 &&
           sy >= AERO_MAP_TOP - 16 && sy < PK_DISPLAY_H + 16;
}

/* 圆盘 / 圆环：直接扫描外接正方形，按到圆心的距离落在 [r_in, r_out] 内就上色，
 * 边缘 1 px 用 blend 做抗锯齿。r_in=0 即实心圆盘。
 *
 * 这里**不能**用 pk_pfd_draw_arc_aa 画整圈：它按 1.5° 步进拆线段，一个 360°
 * 的圆就是 240 次 draw_line_aa，填充还要一圈圈套——2026-08-02 第一版这么写，
 * 真机上地图页从 6 FPS 掉到 5 FPS（每帧 +33 ms）。换成扫描线之后 r=7 的符号
 * 只有 225 次像素判定，50 个符号也就万把次，实测帧率回到基线。
 * pfd_draw.h 里确实没有填充圆原语（map_page.c:131 的 draw_btn_plate 也是自己
 * 手写了一份），这是第三份——真要提炼该提到 pfd_draw.c，但那是另一件事。 */
static void draw_disc(uint16_t *fb, int cx, int cy, float r_out, float r_in, uint16_t c)
{
    const int ir = (int)ceilf(r_out) + 1;
    for (int dy = -ir; dy <= ir; dy++) {
        const int py = cy + dy;
        if (py < 0 || py >= PK_DISPLAY_H) continue;
        /* 每行先解出 x 的可能区间，别扫整个外接正方形。
         * 罗盘玫瑰那个 R=26 的细环，扫方框是 55×55=3025 次 sqrtf，按行求解
         * 之后只剩环本身的 ~340 个像素——细环越大省得越多，而实心盘（r_in=0）
         * 也顺带省掉四个角。 */
        const float fy2 = (float)(dy * dy);
        const float ro2 = (r_out + 0.5f) * (r_out + 0.5f) - fy2;
        if (ro2 < 0.0f) continue;
        const int dx_out = (int)ceilf(sqrtf(ro2));
        /* 这一行落在内孔里的那段可以整段跳过 */
        int dx_in = -1;
        if (r_in > 0.0f) {
            const float ri2 = (r_in - 0.5f) * (r_in - 0.5f) - fy2;
            if (ri2 > 0.0f) dx_in = (int)sqrtf(ri2);
        }
        for (int dx = -dx_out; dx <= dx_out; dx++) {
            if (dx_in > 0 && dx > -dx_in && dx < dx_in) { dx = dx_in - 1; continue; }
            const int px = cx + dx;
            if (px < 0 || px >= PK_DISPLAY_W) continue;
            const float d = sqrtf((float)(dx * dx + dy * dy));
            if (d > r_out + 0.5f || d < r_in - 0.5f) continue;
            /* 内外两条边各留 1 px 的软过渡，取二者较小的覆盖率。 */
            float cov = r_out + 0.5f - d;
            const float cov_in = d - (r_in - 0.5f);
            if (r_in > 0.0f && cov_in < cov) cov = cov_in;
            if (cov >= 1.0f) pk_pfd_put_pixel(fb, px, py, c);
            else             pk_pfd_blend_pixel(fb, px, py, c, (uint8_t)(cov * 255.0f));
        }
    }
}

/* 机场的符号色与标签色是同一条分支算出来的——两者必须永远配对，
 * 分成两处 if 迟早会走偏（比如加一个机场类型只改了一处）。 */
static void apt_colors(const apt_ent_t *e, uint16_t *sym, uint16_t *lbl)
{
    if (e->type != APT_TYPE_AD && e->type != APT_TYPE_OTHER) {
        *sym = COL_APT_OTHER;  *lbl = COL_LBL_APT_OTHER;
    } else if (e->ctrl == APT_CTRL_CTRL) {
        *sym = COL_APT_CTRL;   *lbl = COL_LBL_APT_CTRL;
    } else {
        *sym = COL_APT_UNCTRL; *lbl = COL_LBL_APT_UNCTRL;
    }
}

static void draw_airport(uint16_t *fb, int sx, int sy, int r, const apt_ent_t *e)
{
    uint16_t col, lbl;
    apt_colors(e, &col, &lbl);
    (void)lbl;                  /* 符号这一路只用 col，标签色在 render 里取 */

    if (e->longest_rwy_ft >= APT_PAVED_FT) draw_disc(fb, sx, sy, (float)r, 0.0f, col);
    else                                   draw_disc(fb, sx, sy, (float)r, r - 2.0f, col);

    /* 管制机场加 4 个 tick（正北/东/南/西各一根短须）——形状=种类、
     * tick=是否管制、实心=跑道等级，三个维度正交，互不干扰。 */
    if (e->ctrl == APT_CTRL_CTRL) {
        for (int k = 0; k < 4; k++) {
            const float a = (float)k * (float)M_PI / 2.0f;
            const float dx = sinf(a), dy = -cosf(a);
            pk_pfd_draw_line_aa(fb, sx + dx * r, sy + dy * r,
                                sx + dx * (r + 4), sy + dy * (r + 4), 2.0f, col);
        }
    }
}

static void draw_polygon(uint16_t *fb, int sx, int sy, int r, int sides,
                         float rot_deg, float lw, uint16_t col)
{
    float px = 0, py = 0;
    for (int k = 0; k <= sides; k++) {
        const float a = (rot_deg + 360.0f * (float)k / (float)sides) * (float)M_PI / 180.0f;
        const float x = sx + r * sinf(a), y = sy - r * cosf(a);
        if (k > 0) pk_pfd_draw_line_aa(fb, px, py, x, y, lw, col);
        px = x;  py = y;
    }
}

/* 轴对齐正方形外框，半边长 h。**不能**用 draw_polygon(sides=4, rot=45)：那画
 * 出来的方形半边长只有 r/√2 ≈ 0.71r，边会横穿六角形（2026-08-02 前的画法就是
 * 这样，六角与方形互相切割，是"糊成一个点"的第二个根因）。 */
static void draw_square(uint16_t *fb, int sx, int sy, float h, float w, uint16_t col)
{
    const float x0 = sx - h, x1 = sx + h, y0 = sy - h, y1 = sy + h;
    pk_pfd_draw_line_aa(fb, x0, y0, x1, y0, w, col);
    pk_pfd_draw_line_aa(fb, x1, y0, x1, y1, w, col);
    pk_pfd_draw_line_aa(fb, x1, y1, x0, y1, w, col);
    pk_pfd_draw_line_aa(fb, x0, y1, x0, y0, w, col);
}

#define NAV_LW      2.0f    /* 导航台符号线宽。1.8 在 12 px 的六角上会把六个角
                             * 抹平成圆点，放大到 r=9 之后 2.0 才咬得住底图 */

/* 罗盘玫瑰。外圈 + 每 10° 一根刻度（30° 的加长）+ 磁北那根旁边一个 "N"。
 *
 * 磁差：直接用导航台自己的经纬度查 pk_mag_var_lookup（WMM 5° 网格双线性插值，
 * 东偏为正）。地图是**正北朝上**（瓦片按 Web Mercator 轴对齐贴，没有旋转），
 * 所以屏幕上方 = 真北；磁北在真北**东侧** var 度，即整圈顺时针转 +var。
 * 这正是罗盘玫瑰存在的意义——航图上 VOR 的径向是磁方位，圈不按磁差转就读错。
 *
 * 性能：外圈走 draw_disc 的环路径（R=26 的细环 ≈ 340 个像素判定），刻度是
 * 36 条长 3~6 px 的短线。**绝不能**用 pk_pfd_draw_arc_aa 画整圈：它按 1.5°
 * 步进拆成 240 次 draw_line_aa，2026-08-02 实测让地图页从 6 掉到 5 FPS。
 * 一个玫瑰 = 1 环 + 36 短线 + 1 个字，闸门（pk_aero_rose_enabled）把同屏
 * 上限压在 4 个，即最坏 144 条短线——与一屏 32 个机场的圆盘同量级。 */
#define R_ROSE          26
#define ROSE_TICK_LONG   7.0f
#define ROSE_TICK_SHORT  3.0f

static void draw_compass_rose(uint16_t *fb, int sx, int sy, float magvar_deg)
{
    const float r_out = (float)R_ROSE;
    draw_disc(fb, sx, sy, r_out + 0.9f, r_out - 0.9f, COL_NAV);

    /* 刻度朝内画：朝外的话玫瑰的外接盒还要再涨一圈，标签更没地方摆。 */
    for (int k = 0; k < 36; k++) {
        const float a = ((float)(k * 10) + magvar_deg) * (float)M_PI / 180.0f;
        const float dx = sinf(a), dy = -cosf(a);
        /* 长短刻度还要**分粗细**：R=26 的圈上相邻两根只隔 4.5 px，光靠 6 与 3
         * 的长度差在 1 px 网格上分不出来（v1 截图上整圈像一排锯齿）。 */
        const bool major = (k % 3 == 0);
        const float len = major ? ROSE_TICK_LONG : ROSE_TICK_SHORT;
        const float r1 = r_out - 1.0f, r0 = r1 - len;
        pk_pfd_draw_line_aa(fb, sx + dx * r0, sy + dy * r0,
                            sx + dx * r1, sy + dy * r1, major ? 2.0f : 1.1f, COL_NAV);
    }

    /* 只标一个 "N"（磁北），不标 3/6/9/12。
     * 直径 52 px 的圈上摆 12 个两位数，每个数字 10 px 宽——相邻两个 30° 刻度
     * 之间的弧长只有 13 px，数字必然叠字。ASCII 单字符是唯一放得下的标注，
     * 而 "N" 已经把"这圈是按磁北定向的"这句话说完了（其余方位靠刻度数）。
     * 字符必须是 ASCII：CJK 与特殊符号走 catalog 子集字库，会渲染成 '?'。 */
    const float an = magvar_deg * (float)M_PI / 180.0f;
    const int nx = sx + (int)lroundf(sinf(an) * (r_out + 8.0f));
    const int ny = sy - (int)lroundf(cosf(an) * (r_out + 8.0f));
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               nx - pk_aa_text_width("N", PK_AA_XS) / 2, ny - PK_AA_XS_H / 2,
               "N", COL_LBL_NAV, PK_AA_XS);
}

/* 导航台符号。按 ICAO/FAA 航图惯例逐类画（尺寸以 800×480 上肉眼可辨为准，
 * 见 2026-08-02 的截图迭代）：
 *   VOR       正六角形，尖角朝上
 *   VOR-DME   六角形 + **外接**正方形（半边长 = 六角外接圆半径，方形正好
 *             切在上下两个尖角上，这是航图的画法）
 *   VORTAC    六角形 + 三片 TACAN 叶：叶片贴在交替的三条边中点外侧，用一根
 *             宽 5 px 的短线表达。真航图的叶是带缺口的梯形，r=9 上画不出缺口
 *             （每片只有 4×5 px），所以退化成实心短块——保留"三片、位置在
 *             交替边上"这两个可辨识特征，不退化成 VOR-DME 那样丢掉信息。
 *   NDB       点圈 + 中心实心点（航图上 NDB 就是一圈点）
 *   NDB-DME   点圈 + 外接正方形
 *   DME/TACAN 单独一个正方形
 *
 * 点圈换掉了原来的 8 段 draw_arc_aa 虚线圆：那是 8×16=128 次 draw_line_aa，
 * 在 r=6 上还糊成一个实心圆环（ZUH 那个"破图标"就是它）。12 个小圆点既是
 * 规范画法，成本也只有 12×(4×4) 像素判定。 */
static void draw_navaid(uint16_t *fb, int sx, int sy, int r, uint8_t type, bool rose,
                        float magvar_deg)
{
    const bool vor = (type == NAV_TYPE_VOR || type == NAV_TYPE_VOR_DME ||
                      type == NAV_TYPE_VORTAC);
    const bool ndb = (type == NAV_TYPE_NDB || type == NAV_TYPE_NDB_DME);
    const bool box = (type == NAV_TYPE_VOR_DME || type == NAV_TYPE_NDB_DME ||
                      type == NAV_TYPE_DME || type == NAV_TYPE_TACAN);

    if (rose) draw_compass_rose(fb, sx, sy, magvar_deg);

    if (vor) draw_polygon(fb, sx, sy, r, 6, 0.0f, NAV_LW, COL_NAV);
    if (type == NAV_TYPE_VORTAC) {
        /* 三片叶贴在 60°/180°/300° 这三条边的中点外（边中点距圆心 r·cos30°）。 */
        for (int k = 0; k < 3; k++) {
            const float a = (60.0f + 120.0f * (float)k) * (float)M_PI / 180.0f;
            const float dx = sinf(a), dy = -cosf(a);
            /* 从边中点**再往外一点**起画，长一点、瘦一点：v1 用 4 长 5 宽，
             * 叶根埋进六角形里，三片连着边框糊成一坨绿疙瘩。 */
            const float r0 = (float)r * 0.866f + 1.0f, r1 = r0 + 5.0f;
            pk_pfd_draw_line_aa(fb, sx + dx * r0, sy + dy * r0,
                                sx + dx * r1, sy + dy * r1, 3.6f, COL_NAV);
        }
    }
    if (ndb) {
        /* 有外接方框时点圈要收进框内：两者同半径的话点正好压在框边上，
         * v1 截图里 ZUH 的点圈和方框粘成一圈毛刺（比返工前还糟）。 */
        const float rr = box ? ((float)r - 3.0f) : ((float)r - 0.5f);
        /* 点数跟着半径走：点直径 3 px，圈上要留得出空隙才叫"点圈"而不是
         * 一圈毛边。r=8.5 的外圈周长 53 px 放 12 个（间距 4.4），r=6 的内圈
         * 周长 38 px 只放得下 8 个（间距 4.7）。 */
        const int ndot = box ? 8 : 12;
        for (int k = 0; k < ndot; k++) {
            const float a = (float)k * (360.0f / (float)ndot) * (float)M_PI / 180.0f;
            draw_disc(fb, sx + (int)lroundf(sinf(a) * rr),
                          sy - (int)lroundf(cosf(a) * rr), 1.5f, 0.0f, COL_NAV);
        }
        draw_disc(fb, sx, sy, box ? 2.0f : 2.6f, 0.0f, COL_NAV);
    }
    if (box) draw_square(fb, sx, sy, (float)r, NAV_LW, COL_NAV);
}

static void draw_fix(uint16_t *fb, int sx, int sy, int r)
{
    pk_pfd_draw_triangle(fb, sx, sy - r, sx - r, sy + r - 1, sx + r, sy + r - 1, COL_FIX);
}

/* 标签统一走这里：宽度必须用 pk_aa_text_width（strlen×cell_w 数的是字节，
 * 见 pfd_aa_text.h:47-51），底衬用 darken_rect 保证任何底图上都读得出。
 * col 由调用方按要素类型给（见上面那组 COL_LBL_*）——这一层不再有"统一的
 * 标签色"，颜色本身就是类型标识。 */
static void put_label(uint16_t *fb, int sx, int sy, int sym_r, const char *txt,
                      uint16_t col, pk_aero_rect_t *occ, int *nocc, int occ_max)
{
    if (!txt[0] || *nocc >= occ_max) return;
    const int lw = pk_aa_text_width(txt, PK_AA_XS);
    const int lh = PK_AA_XS_H;
    pk_aero_rect_t r;
    if (!pk_aero_label_place(sx, sy, sym_r, lw, lh, occ, *nocc, &r)) return;
    occ[(*nocc)++] = r;
    pk_pfd_darken_rect(fb, r.x0, r.y0, r.x1, r.y1, 130);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, r.x0 + 2, r.y0, txt, col, PK_AA_XS);
}

/* 把一个符号的包围盒登记进占位池，后面的标签就不会压在它身上。
 *
 * 2026-08-02 起因：截图里 "ZGSZ Shenzhen Baoan..." 这种长条横穿画面、正好
 * 盖住旁边的 VOR 和 FIX。原来的避让只让**标签之间**互斥，符号自己不是障碍物
 * ——于是"没有标签的地方"就被当成空地，其实那里画着要素。符号丢了位置就丢了，
 * 比丢一条标签严重得多。
 *
 * 只在标签趟调用（不在符号趟）：占位池三方共用，登记早了 ADS-B 呼号也得躲
 * 机场符号，目标压在机场上时呼号会被整批挤掉——那正是要修的另一个问题。
 *
 * pad=1 是给抗锯齿的软边留的一像素，不然字会贴着符号边缘糊在一起。 */
static void occ_add_symbol(pk_aero_rect_t *occ, int *nocc, int occ_max,
                           int sx, int sy, int r)
{
    if (*nocc >= occ_max) return;
    const int pad = 1;
    occ[(*nocc)++] = (pk_aero_rect_t){ sx - r - pad, sy - r - pad,
                                       sx + r + pad + 1, sy + r + pad + 1 };
}

/* 两趟渲染之间共享的状态。
 *
 * 为什么要拆成两趟：map_page 要把 ADS-B 目标插在中间——符号层在飞机**之下**
 * （静态设施本来就该垫底），标签层在飞机呼号**之后**（航空安全上交通优先于
 * 机场位置：机场不会撞你，飞机会；机场标签没了符号还在，位置不丢）。
 *
 * 为什么用静态量传快照而不是各查一次 s_active：后台任务随时可能翻缓冲，
 * 两趟各读一次就可能读到不同代的数据，符号画的是 A 代、标签写的是 B 代，
 * 屏上就是"标签指向隔壁那个点"。渲染是单线程按序调用（每帧 symbols →
 * labels），把符号趟当时的快照下标记下来给标签趟用即可，不需要加锁。
 * -1 = 符号趟没画（未 READY / 无快照），标签趟直接返回。 */
static int s_pass_act = -1;
static int s_pass_nvis = 0;      /* 符号趟数出来的"屏上要素数"，喂密度降级 */
/* 本帧画不画罗盘玫瑰。符号趟判定一次存在这里，标签趟拿它算包围盒——两趟
 * 各判一次的话，条件里任何一项（zoom/密度）算法走偏，标签就会压在刻度上。 */
static bool s_pass_rose = false;

/* 符号半径。**符号趟与标签趟必须用同一份**：2026-08-02 之前两处各写一遍
 * `const int r_nav = 6`，是那种"改一处漏一处、标签悄悄压住符号"的经典写法。
 * r_nav 从 6 提到 9：12×12 px 里六角形的六个角表现不出来，用户实机反馈
 * "ZUH 之类的 icon 破"。18×18 之后六角/方框/点圈都分得清（截图为证）。 */
#define R_NAV   9
#define R_FIX   4
static int r_apt_for_zoom(uint8_t zoom) { return (zoom >= 10) ? 7 : 5; }

/* 导航台占位盒的半径：带玫瑰时按玫瑰外圈算（"N" 那个字也在里面）。
 * 玫瑰只属于 VOR 系（见 pk_aero_rose_enabled 的注释）。 */
static bool nav_has_rose(uint8_t type, bool rose_on)
{
    return rose_on && (type == NAV_TYPE_VOR || type == NAV_TYPE_VOR_DME ||
                       type == NAV_TYPE_VORTAC);
}

static int nav_occ_r(uint8_t type, bool rose_on)
{
    return nav_has_rose(type, rose_on) ? (R_ROSE + 8 + PK_AA_XS_H / 2) : R_NAV;
}

void pk_aero_layer_render_symbols(uint16_t *fb, double center_lat, double center_lon,
                                  uint8_t zoom)
{
    s_pass_act = -1;
    s_pass_nvis = 0;
    s_pass_rose = false;
    const int act = s_active;
    if (act < 0 || act > 1) return;          /* 未 READY / 无快照：静默返回 */
    const snapshot_t *s = &s_snap[act];
    s_pass_act = act;

    /* 注意用的是**当前帧**的 zoom 与中心投影，不是快照里那份：快照只决定
     * "画哪些要素"，位置永远按当前视图算，所以刚缩放/平移完的那 200 ms 里
     * 符号是跟手的，只是 LOD 集合还是上一档的——比整层闪一下好得多。 */
    double cwx, cwy;
    pk_aero_lonlat_to_world(center_lon, center_lat, zoom, &cwx, &cwy);
    const int r_apt = r_apt_for_zoom(zoom);
    int nvis = 0;

    /* 先数一遍上屏个数，再开始画：罗盘玫瑰的启用要看密度，而它是**画第一个
     * 导航台时就要知道**的（画完再数就晚了）。数一遍的成本是 ≤96 次
     * lonlat_to_world（几十微秒），相对绘制可忽略——标签趟本来也要重投影一遍。 */
    int nvor = 0;
    for (int i = 0; i < s->n_fix; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->fix[i].lon, s->fix[i].lat, zoom, &wx, &wy);
        if (on_screen(AERO_MCX + (int)lround(wx - cwx), AERO_MCY + (int)lround(wy - cwy)))
            nvis++;
    }
    for (int i = 0; i < s->n_nav; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->nav[i].lon, s->nav[i].lat, zoom, &wx, &wy);
        if (!on_screen(AERO_MCX + (int)lround(wx - cwx), AERO_MCY + (int)lround(wy - cwy)))
            continue;
        nvis++;
        if (nav_has_rose(s->nav[i].type, true)) nvor++;
    }
    for (int i = 0; i < s->n_apt; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->apt[i].lon, s->apt[i].lat, zoom, &wx, &wy);
        if (on_screen(AERO_MCX + (int)lround(wx - cwx), AERO_MCY + (int)lround(wy - cwy)))
            nvis++;
    }
    s_pass_nvis = nvis;
    s_pass_rose = pk_aero_rose_enabled(zoom, nvis, nvor);

    /* 符号自下而上：FIX → 导航台 → 机场。机场压在最上面，它是这一层里
     * 最要紧的信息。这一趟**只画不占位**——占位在标签趟统一登记，理由见那边。 */
    for (int i = 0; i < s->n_fix; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->fix[i].lon, s->fix[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        if (!on_screen(sx, sy)) continue;
        draw_fix(fb, sx, sy, R_FIX);
    }
    for (int i = 0; i < s->n_nav; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->nav[i].lon, s->nav[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        if (!on_screen(sx, sy)) continue;
        const bool rose = nav_has_rose(s->nav[i].type, s_pass_rose);
        /* 磁差按**导航台自己的位置**查，不是视图中心：一屏可能横跨几度经度，
         * 高纬度上两个 VOR 的磁差能差好几度，圈转错就把径向读歪了。 */
        const float mv = rose ? pk_mag_var_lookup(s->nav[i].lat, s->nav[i].lon) : 0.0f;
        draw_navaid(fb, sx, sy, R_NAV, s->nav[i].type, rose, mv);
    }
    for (int i = 0; i < s->n_apt; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->apt[i].lon, s->apt[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        if (!on_screen(sx, sy)) continue;
        draw_airport(fb, sx, sy, r_apt, &s->apt[i]);
    }
}

void pk_aero_layer_render_labels(uint16_t *fb, double center_lat, double center_lon,
                                 uint8_t zoom, pk_aero_rect_t *occ, int *nocc,
                                 int occ_max)
{
    const int act = s_pass_act;
    if (act < 0 || act > 1) return;          /* 符号趟没画：标签也不画 */
    const snapshot_t *s = &s_snap[act];

    const pk_aero_lod_t lod = pk_aero_lod_for_zoom(zoom);
    const uint8_t label = pk_aero_label_mode(lod.label, s_pass_nvis);
    if (label == 0) return;

    double cwx, cwy;
    pk_aero_lonlat_to_world(center_lon, center_lat, zoom, &cwx, &cwy);
    const int r_apt = r_apt_for_zoom(zoom);

    /* 先把本层所有上屏符号登记成障碍物，再摆标签。
     *
     * 为什么登记放在**标签趟**而不是符号趟：占位池是三方共用的，登记时机
     * 决定了"谁躲谁"。放在符号趟，ADS-B 呼号就得躲机场符号——而 sim 那 5 个
     * 假目标正压在机场上，呼号被一个不剩地挤掉（v1 截图实测只剩 1 个），
     * 又回到了要修的那个问题。放在标签趟，顺序就是：ADS-B 呼号谁都不用躲
     * （最高优先级，只避同类），本层标签躲呼号 + 躲所有符号。
     * 代价是重投影一遍（≤96 个点、几十次三角函数），相对绘制本身可忽略。
     *
     * 重投影用的是与符号趟同一份快照（s_pass_act）和同一套投影参数，
     * 所以盒子一定盖在刚才画出来的符号上。 */
    for (int i = 0; i < s->n_fix; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->fix[i].lon, s->fix[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        if (on_screen(sx, sy)) occ_add_symbol(occ, nocc, occ_max, sx, sy, R_FIX);
    }
    for (int i = 0; i < s->n_nav; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->nav[i].lon, s->nav[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        /* 带罗盘玫瑰的导航台占位盒按玫瑰外圈算（含 "N" 那个字），否则标签
         * 会压在刻度上——符号一大，包围盒必须跟着大。 */
        if (on_screen(sx, sy))
            occ_add_symbol(occ, nocc, occ_max, sx, sy,
                           nav_occ_r(s->nav[i].type, s_pass_rose));
    }
    for (int i = 0; i < s->n_apt; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->apt[i].lon, s->apt[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        /* 管制机场那 4 根 tick 伸出 r+4，包围盒要跟着涨，否则标签压住须尖。 */
        if (on_screen(sx, sy))
            occ_add_symbol(occ, nocc, occ_max, sx, sy,
                           r_apt + (s->apt[i].ctrl == APT_CTRL_CTRL ? 4 : 0));
    }

    /* 标签分三轮：机场 → 导航台 → FIX，重要的先占位。
     * 整层排在 map_page 的 ADS-B callsign **之后**——飞机呼号先占，机场标签
     * 让路。上一版是反过来的（设计文档 §2.4"静态先占"），实测一屏 5 个飞机
     * 一个呼号都挤不出来，判定为错：交通信息的优先级高于机场位置。
     * "让动的去躲静的、否则每帧抖"这个担心也不成立——ADS-B 目标每秒才更新
     * 一两次位置，抖的是它自己躲不躲得开，与谁先占位无关。 */
    for (int i = 0; i < s->n_apt; i++) {
        const apt_ent_t *e = &s->apt[i];
        double wx, wy;
        pk_aero_lonlat_to_world(e->lon, e->lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        if (!on_screen(sx, sy)) continue;
        /* txt 装得下最坏情况：代码 7 + 空格 + 名称 21 + 空格 + "-32768ft" 8
         * + NUL = 39 < 64，这三条 snprintf 都不会截断。 */
        char txt[64], nm[NAME_CAP_PLAIN + 1];
        if (label >= 3 && e->name[0]) {
            name_ellipsize(nm, NAME_CAP_ELEV + 1, e->name);
            snprintf(txt, sizeof(txt), "%s %s %dft", e->code, nm, (int)e->elev_ft);
        } else if (label >= 2 && e->name[0]) {
            name_ellipsize(nm, sizeof(nm), e->name);
            snprintf(txt, sizeof(txt), "%s %s", e->code, nm);
        } else {
            snprintf(txt, sizeof(txt), "%s", e->code);
        }
        uint16_t sym_col, lbl_col;
        apt_colors(e, &sym_col, &lbl_col);
        (void)sym_col;
        put_label(fb, sx, sy, r_apt + (e->ctrl == APT_CTRL_CTRL ? 4 : 0),
                  txt, lbl_col, occ, nocc, occ_max);
    }
    for (int i = 0; i < s->n_nav; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->nav[i].lon, s->nav[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        if (on_screen(sx, sy))
            put_label(fb, sx, sy, nav_occ_r(s->nav[i].type, s_pass_rose),
                      s->nav[i].ident, COL_LBL_NAV,
                      occ, nocc, occ_max);
    }
    if (label < 2) return;   /* 仅代码档不给 FIX 加标签，太挤 */
    for (int i = 0; i < s->n_fix; i++) {
        double wx, wy;
        pk_aero_lonlat_to_world(s->fix[i].lon, s->fix[i].lat, zoom, &wx, &wy);
        int sx = AERO_MCX + (int)lround(wx - cwx), sy = AERO_MCY + (int)lround(wy - cwy);
        if (on_screen(sx, sy))
            put_label(fb, sx, sy, R_FIX, s->fix[i].ident, COL_LBL_FIX,
                      occ, nocc, occ_max);
    }
}

#endif /* !PK_AERO_LAYER_HOST_TEST */
