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
 * 数据摆的是"最糟情况"（见 ~/.claude 记忆 feedback_ui_worst_case_first）：
 *   - 各类型齐全：管制大场 / 非管制小场 / 直升机坪 / 水上机场 /
 *     VOR / VOR-DME / VORTAC / NDB / NDB-DME / DME / 航路 FIX；
 *   - 前 5 个机场、2 个导航台、2 个 FIX **故意压在 sim 那 5 个 ADS-B 假目标
 *     的经纬度上**（mock_runtime.c 的 kSpread 表），复现用户实机反馈的
 *     "机场标签和飞机标签混在一起"；
 *   - 名字长短都有：从 "Jieyang" 到 85 字节的音译长名（触发 name_ellipsize
 *     的词边界 + "..." 那条路径）。
 */
#include "pk_aero_db.h"
#include "geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 与 sim/compat/mock_runtime.c 的 MAP_DEMO_OWN_LAT/LON 同值（珠三角，落在
 * tmp/sd-maps 的 prd_pilot 试点包高清区间内）。那两个是它的 static 宏，这里
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
