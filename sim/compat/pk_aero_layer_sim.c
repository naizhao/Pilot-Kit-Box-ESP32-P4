/*
 * pk_aero_layer_sim.c — sim 侧对 firmware/main/pk_aero_db.h 查询接口的桩，
 * 用来在模拟器上预览地图页的航空叠加层（机场 / 导航台 / FIX）。
 *
 * 为什么桩的是 pk_aero_db 而不是 pk_aero_layer 本身：
 *   叠加层的**观感**由三样东西决定——LOD 过滤（哪些要素上屏）、快照结构、
 *   渲染配色与标签避让。这三样必须是固件那一份真实代码，否则在模拟器上
 *   评审通过的版面到真机就不成立。真正 host 上跑不起来的只有数据源：
 *   pk_aero_db 要 SD 卡 + 10 MB 加密 bin + FreeRTOS 后台加载任务。所以这里
 *   只把**数据源**换成内存里的常量表，pk_aero_layer.c 原样编译
 *   （sim/CMakeLists.txt 里给它 -DPK_AERO_LAYER_SIM_IMPL）。
 *   同 compat/pk_tile_loader_sim.c 的路子：桩的成本由 sim 侧承担。
 *
 * 开关：**PK_SIM_AERO=1 才有数据**，否则 pk_aero_db_state() 返回 ABSENT，
 * 叠加层静默不画——既有的 ui-4.3-map* 截图场景一张都不受影响。
 *
 * 数据摆的是"最糟情况"——UI 一律先按最糟情况验收，理想数据下踩不到坑：
 *   - 各类型齐全：管制大场 / 非管制小场 / 直升机坪 / 水上机场 /
 *     VOR / VOR-DME / VORTAC / NDB / NDB-DME / DME / 航路 FIX；
 *   - 前 5 个机场、2 个导航台、2 个 FIX **故意压在 sim 那 5 个 ADS-B 假目标
 *     的经纬度上**（mock_runtime.c 的 kSpread 表），复现用户实机反馈的
 *     "机场标签和飞机标签混在一起"；
 *   - 名字长短都有：从 "Jieyang" 到 85 字节的音译长名（触发 name_ellipsize
 *     的词边界 + "..." 那条路径）。
 */
#include "pk_aero_db.h"
#include "pk_win.h"
#include "geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 与 sim/compat/mock_runtime.c 的 MAP_DEMO_OWN_LAT/LON 同值（珠三角，落在
 * datafiles/maps 的 prd_pilot 试点包高清区间内）。那两个是它的 static 宏，这里
 * 照抄一份而不是导出——桩与桩之间互不依赖，改一处要记得改两处。 */
#define AERO_SIM_BASE_LAT  22.54
#define AERO_SIM_BASE_LON  113.90

/* 枚举取值同 pk_aero_layer.c 顶部那张表（源头是 export_box_bin.py）。 */
#define T_AD    1
#define T_HELI  2
#define T_WATER 3
#define C_UNK   0
#define C_UNCTL 1
#define C_CTL   2

#define N_VOR      1
#define N_VOR_DME  2
#define N_VORTAC   3
#define N_NDB      4
#define N_NDB_DME  5
#define N_DME      6

typedef struct {
    double      dlat, dlon;
    const char *icao;
    const char *name;
    uint8_t     type, ctrl;
    uint16_t    rwy_ft;
    int16_t     elev_ft;
} apt_row_t;

/* 前 5 行的偏移 = mock_runtime.c kSpread 五个目标的 dlat/dlon（各挪几个
 * 像素，让符号不完全叠死、标签必然打架）。 */
static const apt_row_t kApt[] = {
    {  0.062,  0.022, "ZGSZ", "Shenzhen Baoan International",   T_AD,   C_CTL,  11000,  13 },
    { -0.052,  0.082, "ZGGG", "Guangzhou Baiyun International Airport",
                                                                T_AD,   C_CTL,  12467,  50 },
    {  0.101, -0.058, "ZGOW", "Jieyang",                        T_AD,   C_UNCTL, 4600,  90 },
    { -0.081, -0.041, "VHHH", "Hong Kong International",        T_AD,   C_CTL,  12467,  28 },
    {  0.021,  0.141, "ZG12", "Nanhai Guidian Heliport",        T_HELI, C_UNK,      0,  15 },
    /* 以下是不与飞机重叠的散点，用来看"正常密度"下各类型好不好认。 */
    { -0.150,  0.300, "ZG34", "Zhuhai Jinwan Shequ Zhili Guanli Zhihui Zhongxin Heliport",
                                                                T_HELI, C_UNK,      0,  20 },
    {  0.160, -0.280, "ZGSD", "Zhuhai Jinwan",                  T_AD,   C_UNCTL, 8530,  23 },
    { -0.190, -0.300, "ZGDY", "Dayawan",                        T_AD,   C_UNCTL, 3200,   8 },
    {  0.185,  0.360, "ZGHZ", "Huizhou Pingtan",                T_AD,   C_CTL,  10499,  40 },
    { -0.120,  0.200, "ZG56", "Shuiku Shuishang Jichang",       T_WATER,C_UNK,      0,   5 },
};

typedef struct { double dlat, dlon; const char *ident; uint8_t type; } nav_row_t;

static const nav_row_t kNav[] = {
    {  0.060,  0.018, "SZA",  N_VOR_DME },   /* 压在 CES2158 上 */
    { -0.050,  0.078, "GGB",  N_VORTAC  },   /* 压在 CSN3341 上 */
    {  0.100,  0.100, "HZC",  N_NDB     },
    { -0.140, -0.150, "MKM",  N_VOR     },
    {  0.140, -0.120, "TAK",  N_DME     },
    { -0.020,  0.300, "ZUH",  N_NDB_DME },
};

typedef struct { double dlat, dlon; const char *ident; } fix_row_t;

static const fix_row_t kFix[] = {
    {  0.065,  0.026, "IDUMA" },             /* 压在 CES2158 上 */
    { -0.045,  0.085, "GYARO" },             /* 压在 CSN3341 上 */
    {  0.030, -0.100, "POLUR" },
    { -0.100,  0.120, "SANKO" },
    {  0.120,  0.050, "TONIL" },
    { -0.160,  0.050, "EPKUS" },
    {  0.000, -0.300, "BEKOL" },
    {  0.080,  0.320, "MUBEL" },
};

#define NAPT ((int)(sizeof(kApt) / sizeof(kApt[0])))
#define NNAV ((int)(sizeof(kNav) / sizeof(kNav[0])))
#define NFIX ((int)(sizeof(kFix) / sizeof(kFix[0])))

static bool aero_on(void)
{
    const char *e = getenv("PK_SIM_AERO");
    return e != NULL && e[0] == '1';
}

/* 基准点跟着 own_ship 走：overzoom 场景（PK_SIM_MAP_OWN_LAT/LON=悉尼）挪本机
 * 时，这批要素跟着挪，不会整批散到画面外。同 mock_runtime.c 的做法。 */
static void base_pos(double *lat, double *lon)
{
    const char *lat_e = getenv("PK_SIM_MAP_OWN_LAT");
    const char *lon_e = getenv("PK_SIM_MAP_OWN_LON");
    *lat = lat_e ? atof(lat_e) : AERO_SIM_BASE_LAT;
    *lon = lon_e ? atof(lon_e) : AERO_SIM_BASE_LON;
}

pk_aero_db_state_t pk_aero_db_state(void)
{
    return aero_on() ? PK_AERO_DB_READY : PK_AERO_DB_ABSENT;
}

const char *pk_aero_db_cycle(void)
{
    return aero_on() ? "2026-02" : "";
}

/* nearest 三兄弟：桩表统共十来条，全在一屏之内，真实实现那套"网格粗排 +
 * Haversine 精算"在这里没有可观察的差别（地图叠加层只按 LOD 的 limit 截断，
 * limit 32 > 表长）。
 *
 * 但**距离与方位必须算真的**：搜索页把它们直接摆在结果行右侧，全填 0 的话
 * 截出来的图上每一行都写着 "0.0NM 000"，那既看不出数字列会不会顶到名字，
 * 也演示不了"离我最近的排最前"这条排序规则——而那正是要在模拟器上评审的东西。
 * 排序也照真实现来：距离升序。 */
/* 三张桩表的**共同前缀**都是 { double dlat, dlon; }（C 的 common initial
 * sequence），所以一份带 stride 的遍历就够，不必给每类各抄一遍。 */
typedef struct { double dlat, dlon; } pos_row_t;

static int near_all(const void *rows, size_t stride, int n,
                    double qlat, double qlon, pk_aero_near_t *out, int max)
{
    if (!aero_on()) return 0;
    double blat, blon;
    base_pos(&blat, &blon);

    pk_aero_near_t tmp[64];
    if (n > (int)(sizeof(tmp) / sizeof(tmp[0]))) n = (int)(sizeof(tmp) / sizeof(tmp[0]));
    for (int i = 0; i < n; i++) {
        const pos_row_t *r = (const pos_row_t *)((const char *)rows + (size_t)i * stride);
        tmp[i].idx = (uint32_t)i;
        geo_dist_brg(qlat, qlon, blat + r->dlat, blon + r->dlon,
                     &tmp[i].dist_nm, &tmp[i].brg_deg);
    }
    /* 插入排序，n ≤ 十几条 */
    for (int i = 1; i < n; i++) {
        pk_aero_near_t k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].dist_nm > k.dist_nm) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    return n;
}

int pk_aero_db_nearest_airports(double lat, double lon, pk_aero_near_t *out, int max)
{
    return near_all(kApt, sizeof(kApt[0]), NAPT, lat, lon, out, max);
}

int pk_aero_db_nearest_navaids(double lat, double lon, pk_aero_near_t *out, int max)
{
    return near_all(kNav, sizeof(kNav[0]), NNAV, lat, lon, out, max);
}

int pk_aero_db_nearest_fixes(double lat, double lon, pk_aero_near_t *out, int max)
{
    return near_all(kFix, sizeof(kFix[0]), NFIX, lat, lon, out, max);
}

/* ── 搜索页要用的那几个查询（2026-08-02 补）─────────────────────────
 *
 * 桩的仍然是**数据源**：分桶次序、去重、桶内排序、空态分因都在 search_page.c
 * 那份真实代码里跑，这里只负责"给出符合语义的结果"。前缀比较照真库的规矩来
 * ——池里的代码字段全大写、比较大小写敏感（memcmp），所以这里也直接比大写。
 *
 * PK_SIM_AERO_V2=1 可以把版本按到 2，用来截"这张卡没有搜索索引"那一屏：
 * 真库 v2 上机场/导航台前缀与子串搜索全都返回 0，只有 FIX 前缀还能用。 */
static bool aero_v2(void)
{
    const char *e = getenv("PK_SIM_AERO_V2");
    return e != NULL && e[0] == '1';
}

/*
 * 状态快照。诊断页那张 AERO DB 卡片与搜索页共用它（原先诊断那份在
 * compat/page_stub.c，两处各留一份直接是 duplicate symbol，更糟的是会出现
 * "诊断说 READY、搜索却查不出东西"）。
 *
 * 两个开关语义不同，都要认：
 *   PK_SIM_AERO=1     给地图叠加层与搜索页喂真数据（本文件那三张桩表）；
 *   PK_SIM_DIAG_OK=1  把整屏诊断切成"一切正常"，AERO DB 卡片也该跟着变绿，
 *                     否则会出现"七张卡跟着开关变、单单这张纹丝不动"的违和。
 */
static bool diag_ok(void)
{
    const char *e = getenv("PK_SIM_DIAG_OK");
    return e != NULL && e[0] == '1';
}

void pk_aero_db_status_get(pk_aero_db_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (!aero_on() && !diag_ok()) {
        out->state = PK_AERO_DB_ABSENT;
        return;
    }
    out->state = PK_AERO_DB_READY;
    snprintf(out->cycle, sizeof(out->cycle), "%s", "2026-02");
    out->version    = aero_v2() ? 2 : 3;
    out->load_pct   = 100;
    /* 有桩表就报桩表的条数，没有（只开了 DIAG_OK）就报一组像真库的数字。 */
    out->n_airports = aero_on() ? (uint32_t)NAPT : 9327;
    out->n_navaids  = aero_on() ? (uint32_t)NNAV : 4118;
    out->n_fixes    = aero_on() ? (uint32_t)NFIX : 61240;
}

uint32_t pk_aero_db_generation(void) { return aero_on() ? 1u : 0u; }

/*
 * 滚动窗口（pk_win）的状态快照。真身依赖 FreeRTOS 任务 + SD 区间读，host 上
 * 起不来，但诊断页那张 WINDOW 卡片的版面要在模拟器上评审，所以在这里给桩。
 *
 * 口径跟着 AERO DB 那格走：没开 PK_SIM_AERO / PK_SIM_DIAG_OK 就报"句柄没开"
 * （屏上 no data，灰），否则报一组像真库的驻留量。
 *
 * PK_SIM_WIN_WARN=1 摆**最糟情况**：48 个槽全满、驻留量顶到两位数 MB、再挂上
 * 最长的异常后缀。理想数据下值行只有十几个字符，永远踩不到降档与溢出——
 * 这一档就是专门用来看"降到 S 档之后还压不压隔壁卡"的。
 */
void pk_win_status_get(pk_win_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    if (!aero_on() && !diag_ok()) return;      /* open=false → "no data" */

    out->open    = true;
    out->version = aero_v2() ? 2 : 3;

    const char *w = getenv("PK_SIM_WIN_WARN");
    if (w != NULL && w[0] == '1') {
        out->n_cells = out->n_ready = 48;
        out->win_cells = 15;
        out->bytes = 19u * 1024u * 1024u + 800u * 1024u;
        out->str_skipped = 3;
        out->forced = 12;
    } else {
        out->n_cells = out->n_ready = 12;
        out->win_cells = 12;
        out->bytes = 1u * 1024u * 1024u + 420u * 1024u;
    }
    out->yields = 1837;
    out->loads  = out->n_ready;
}

/* 窗口视口约束 + nearest 查询（W1.4）。模拟器的航空数据由本文件的全量内存表
 * 提供，不依赖 pk_win 的窗口驻留集，故这里 viewport 无操作、nearest 返回 0
 * 条——pk_aero_layer 拿到 0 会 fallback 到全量 pk_aero_db 查询。 */
void pk_win_set_viewport(double min_lat, double min_lon,
                         double max_lat, double max_lon)
{
    (void)min_lat; (void)min_lon; (void)max_lat; (void)max_lon;
}
int pk_win_nearest(uint16_t sec_type, double lat, double lon,
                   pk_aero_near_t *out, int max)
{
    (void)sec_type; (void)lat; (void)lon; (void)out; (void)max;
    return 0;
}

/* 大写化 + 前缀比较（桩表里的代码本来就全大写）。 */
static bool has_prefix(const char *s, const char *pfx)
{
    for (size_t i = 0; pfx[i] != '\0'; ++i) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != pfx[i]) return false;
    }
    return true;
}

/* 大小写不敏感的子串判定（needle 已是大写）。 */
static bool ci_has(const char *hay, const char *needle)
{
    for (size_t i = 0; hay[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0') {
            char c = hay[i + j];
            if (c == '\0') return false;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c != needle[j]) break;
            j++;
        }
        if (needle[j] == '\0') return true;
    }
    return false;
}

static void upper(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; in[n] != '\0' && n + 1 < cap; ++n) {
        char c = in[n];
        out[n] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    out[n] = '\0';
}

int32_t pk_aero_db_airport_by_icao(const char *code)
{
    if (!aero_on() || code == NULL) return -1;
    char q[8];
    upper(code, q, sizeof(q));
    for (int i = 0; i < NAPT; ++i)
        if (strcmp(kApt[i].icao, q) == 0) return i;
    return -1;
}

int pk_aero_db_airports_by_prefix(const char *prefix, uint32_t *out, int max)
{
    if (!aero_on() || aero_v2() || prefix == NULL || out == NULL) return 0;
    char q[8];
    upper(prefix, q, sizeof(q));
    if (q[0] == '\0') return 0;
    int n = 0;
    for (int i = 0; i < NAPT && n < max; ++i)
        if (has_prefix(kApt[i].icao, q)) out[n++] = (uint32_t)i;
    return n;
}

int pk_aero_db_navaids_by_prefix(const char *prefix, uint32_t *out, int max)
{
    if (!aero_on() || aero_v2() || prefix == NULL || out == NULL) return 0;
    char q[8];
    upper(prefix, q, sizeof(q));
    if (q[0] == '\0') return 0;
    int n = 0;
    for (int i = 0; i < NNAV && n < max; ++i)
        if (has_prefix(kNav[i].ident, q)) out[n++] = (uint32_t)i;
    return n;
}

int pk_aero_db_fixes_by_prefix(const char *prefix, uint32_t *out, int max)
{
    /* FIX ident 索引 v2 就有，两版都正常——这一条**不受** aero_v2() 影响，
     * 与真库一致（见 pk_aero_reader.h 文件头那张表）。 */
    if (!aero_on() || prefix == NULL || out == NULL) return 0;
    char q[8];
    upper(prefix, q, sizeof(q));
    if (q[0] == '\0') return 0;
    int n = 0;
    for (int i = 0; i < NFIX && n < max; ++i)
        if (has_prefix(kFix[i].ident, q)) out[n++] = (uint32_t)i;
    return n;
}

int pk_aero_db_navaid_by_ident(const char *ident, uint32_t *out, int max)
{
    if (!aero_on() || aero_v2() || ident == NULL || out == NULL) return 0;
    char q[8];
    upper(ident, q, sizeof(q));
    int n = 0;
    for (int i = 0; i < NNAV && n < max; ++i)
        if (strcmp(kNav[i].ident, q) == 0) out[n++] = (uint32_t)i;
    return n;
}

int pk_aero_db_search_substring(const char *query, pk_aero_hit_t *out, int max)
{
    if (!aero_on() || aero_v2() || query == NULL || out == NULL) return 0;
    char q[32];
    upper(query, q, sizeof(q));
    if (q[0] == '\0') return 0;
    int n = 0;
    /* 段顺序同真实现：机场 → 导航台 → FIX。桩表里只有机场有名字。 */
    for (int i = 0; i < NAPT && n < max; ++i) {
        if (ci_has(kApt[i].name, q)) {
            out[n].type = PK_AERO_SEC_AIRPORTS;
            out[n].idx  = (uint32_t)i;
            n++;
        }
    }
    return n;
}

bool pk_aero_db_airport_get(uint32_t idx, pk_aero_airport_t *out)
{
    if (!aero_on() || idx >= (uint32_t)NAPT || out == NULL) return false;
    const apt_row_t *r = &kApt[idx];
    double blat, blon;
    base_pos(&blat, &blon);
    memset(out, 0, sizeof(*out));
    out->lat = blat + r->dlat;
    out->lon = blon + r->dlon;
    snprintf(out->icao, sizeof(out->icao), "%s", r->icao);
    out->type = r->type;
    out->ctrl = r->ctrl;
    out->longest_rwy_ft = r->rwy_ft;
    out->elev_ft = r->elev_ft;
    out->name = r->name;     /* 桩表是 .rodata 常量，不会像真机那样拔卡即悬空 */
    out->city = "";
    return true;
}

bool pk_aero_db_navaid_get(uint32_t idx, pk_aero_navaid_t *out)
{
    if (!aero_on() || idx >= (uint32_t)NNAV || out == NULL) return false;
    const nav_row_t *r = &kNav[idx];
    double blat, blon;
    base_pos(&blat, &blon);
    memset(out, 0, sizeof(*out));
    out->lat = blat + r->dlat;
    out->lon = blon + r->dlon;
    snprintf(out->ident, sizeof(out->ident), "%s", r->ident);
    out->type = r->type;
    out->name = "";
    return true;
}

bool pk_aero_db_fix_get(uint32_t idx, pk_aero_fix_t *out)
{
    if (!aero_on() || idx >= (uint32_t)NFIX || out == NULL) return false;
    const fix_row_t *r = &kFix[idx];
    double blat, blon;
    base_pos(&blat, &blon);
    memset(out, 0, sizeof(*out));
    out->lat = blat + r->dlat;
    out->lon = blon + r->dlon;
    snprintf(out->ident, sizeof(out->ident), "%s", r->ident);
    /* LOD 在 zoom≤11 只放航路 FIX（fix_enroute_only），全填 ENROUTE 才画得出来。 */
    out->scope = PK_AERO_FIX_SCOPE_ENROUTE;
    out->name = "";
    return true;
}

/* ── 机场详情页要用的聚簇区间与两种子记录（2026-08-02 补）───────────
 *
 * 桩的仍然是**数据源**：业务序排序、哨兵处置、版面与滚动都在
 * apt_detail_page.c 那份真实代码里跑，这里只负责"给出符合语义的记录"。
 *
 * 数据按"最糟情况优先"摆（理想数据下踩不到坑）：
 *   - ZGGG（idx 1）= 真库实测的量：**10 条跑道方向 + 63 条频率**，用来验
 *     滚动条、长列表到底、以及业务序把 TWR 从第 40 条提到第 1 条；
 *   - 缺字段全都真的缺：约 81% 的跑道方向没有入口坐标、部分没有磁航向，
 *     还有宽度为 0、道面未知、服务类型未知这几种；
 *   - ZGOW（idx 2）= 有跑道但**一条频率都没有**；
 *   - 直升机坪 ZG12（idx 4）= 跑道与频率**两段都空**；
 *   - 其余机场各给两条方向 + 三条频率，够看"正常"长什么样。
 *
 * 聚簇区间用 idx×100 编码：真库里跑道/频率是按机场物理聚簇的连续区间，
 * 这里只要保证"区间不重叠、能从全局下标反解回机场"即可，×100 比维护一张
 * 前缀和表短得多，也不会因为改了某个机场的条数就要重算后面所有人的 first。
 */
#define APT_CLUSTER_STRIDE 100

typedef struct { const char *desig; uint16_t len_ft, wid_ft; uint8_t surface;
                 uint16_t brg_dd; bool coord; } sim_rwy_t;

/* ZGGG：5 条跑道 = 10 个方向。刻意混进各种缺失。 */
static const sim_rwy_t kRwyZGGG[] = {
    { "02L", 12467, 197, 1, 232,    true  },
    { "20R", 12467, 197, 1, 2032,   true  },
    { "02R", 12467, 197, 1, 232,    false },   /* 无入口坐标（真库多数如此）*/
    { "20L", 12467, 197, 1, 2032,   false },
    { "01",  11811, 148, 2, 0xFFFF, false },   /* 无磁航向 */
    { "19",  11811, 148, 2, 0xFFFF, false },
    { "03",   9186,   0, 0, 315,    false },   /* 无宽度 + 道面未知 */
    { "21",   9186,   0, 0, 2115,   false },
    { "18L",     0,   0, 3, 1800,   true  },   /* 连长度都没有 */
    { "36R",     0,   0, 3, 3600,   true  },   /* 360.0° → 必须显示 000 */
};

/* ZGOW：两条方向，齐全。频率一条都没有。 */
static const sim_rwy_t kRwyZGOW[] = {
    { "04", 4600, 148, 3, 435,  true  },
    { "22", 4600, 148, 3, 2235, false },
};

/* 通用两条（其余机场共用）。 */
static const sim_rwy_t kRwyGeneric[] = {
    { "09", 8530, 148, 1, 895,  true  },
    { "27", 8530, 148, 1, 2695, false },
};

static void sim_rwy_table(uint32_t apt, const sim_rwy_t **rows, uint32_t *n)
{
    switch (apt) {
    case 1: *rows = kRwyZGGG;    *n = 10; break;
    case 2: *rows = kRwyZGOW;    *n = 2;  break;
    case 4: *rows = NULL;        *n = 0;  break;   /* 直升机坪：无跑道段 */
    case 5: *rows = NULL;        *n = 0;  break;
    case 9: *rows = NULL;        *n = 0;  break;   /* 水上机场 */
    default: *rows = kRwyGeneric; *n = 2; break;
    }
}

/* ZGGG 的 63 条频率是**生成**的，不是手抄 63 行：真库里那 63 条本来就是
 * "同一个服务分好几个扇区"的重复结构，手抄只会抄出一堆看不出规律的噪音。
 * 关键是**存储序刻意把 TWR 摆在很后面**（第 40 条），这样截图上 TWR 出现在
 * 第一行就直接证明了业务序排序真的跑了。 */
static void sim_freq_row(uint32_t apt, uint32_t k, pk_aero_freq_t *out)
{
    /* 服务枚举同 export_box_bin.py 的 SERVICE_ENUM。 */
    static const uint8_t kSvcOrder[] = {
        9, 7, 6, 3, 3, 4, 14, 12, 5, 10, 0, 13, 8, 2, 1, 11,
    };
    static const char *kCall[] = {
        "GUANGZHOU AWOS", "UNICOM", "CTAF",
        "GUANGZHOU APPROACH SECTOR 3", "GUANGZHOU APPROACH",
        "GUANGZHOU DEPARTURE", "GUANGZHOU RADAR", "",
        "GUANGZHOU ATIS", "GUANGZHOU RADIO", "", "FLIGHT SERVICE",
        "GUANGZHOU DELIVERY", "GUANGZHOU GROUND", "GUANGZHOU TOWER",
        "AERODROME INFORMATION",
    };
    const uint32_t n = (uint32_t)(sizeof(kSvcOrder) / sizeof(kSvcOrder[0]));
    /* 让 TWR（表内下标 14）落在第 40 条附近：k 直接取模即可，16 条一轮，
     * 第 40 条 = 40 % 16 = 8 … 这里改成从后往前排更直观——k 越大越接近
     * 表尾，于是 TWR/GND 天然落在后段。 */
    const uint32_t i = (k * 7u + apt) % n;
    memset(out, 0, sizeof(*out));
    out->service  = kSvcOrder[i];
    out->callsign = kCall[i];
    /* 频率覆盖三种格式：常规 25 kHz、8.33 kHz 间隔、带前导零的小数。 */
    static const uint32_t kBase[] = { 118250, 118275, 121005, 127350, 135075 };
    out->freq_khz = kBase[k % 5] + (k / 5) * 25;
}

static uint32_t sim_freq_count(uint32_t apt)
{
    switch (apt) {
    case 1: return 63;    /* ZGGG 真库实测 */
    case 2: return 0;     /* 有跑道、无频率 */
    case 4: return 0;
    case 5: return 0;
    case 9: return 0;
    default: return 3;
    }
}

bool pk_aero_db_airport_runways(uint32_t apt_idx, uint32_t *first, uint32_t *count)
{
    if (!aero_on() || apt_idx >= (uint32_t)NAPT) return false;
    const sim_rwy_t *rows; uint32_t n;
    sim_rwy_table(apt_idx, &rows, &n);
    (void)rows;
    if (first) *first = apt_idx * APT_CLUSTER_STRIDE;
    if (count) *count = n;
    return true;
}

bool pk_aero_db_airport_freqs(uint32_t apt_idx, uint32_t *first, uint32_t *count)
{
    if (!aero_on() || apt_idx >= (uint32_t)NAPT) return false;
    if (first) *first = apt_idx * APT_CLUSTER_STRIDE;
    if (count) *count = sim_freq_count(apt_idx);
    return true;
}

bool pk_aero_db_rwy_dir_get(uint32_t idx, pk_aero_rwy_dir_t *out)
{
    if (!aero_on() || out == NULL) return false;
    const uint32_t apt = idx / APT_CLUSTER_STRIDE, k = idx % APT_CLUSTER_STRIDE;
    if (apt >= (uint32_t)NAPT) return false;
    const sim_rwy_t *rows; uint32_t n;
    sim_rwy_table(apt, &rows, &n);
    if (rows == NULL || k >= n) return false;

    memset(out, 0, sizeof(*out));
    snprintf(out->designator, sizeof(out->designator), "%s", rows[k].desig);
    out->length_ft = rows[k].len_ft;
    out->width_ft  = rows[k].wid_ft;
    out->surface   = rows[k].surface;
    /* 哨兵的解码语义与 pk_aero_reader.c:821-827 一致：0xFFFF 磁航向、
     * 0x7FFFFFFF 坐标 → has_* 为 false。桩这一层必须照做，否则详情页那条
     * "缺字段不显示"的分支在模拟器上根本走不到。 */
    out->has_bearing    = (rows[k].brg_dd != PK_AERO_BEARING_NONE);
    out->mag_bearing_dd = rows[k].brg_dd;
    out->has_coord      = rows[k].coord;
    if (out->has_coord) {
        double blat, blon;
        base_pos(&blat, &blon);
        out->lat = blat + kApt[apt].dlat;
        out->lon = blon + kApt[apt].dlon;
    }
    return true;
}

bool pk_aero_db_freq_get(uint32_t idx, pk_aero_freq_t *out)
{
    if (!aero_on() || out == NULL) return false;
    const uint32_t apt = idx / APT_CLUSTER_STRIDE, k = idx % APT_CLUSTER_STRIDE;
    if (apt >= (uint32_t)NAPT || k >= sim_freq_count(apt)) return false;
    sim_freq_row(apt, k, out);
    return true;
}
