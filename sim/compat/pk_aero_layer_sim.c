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

/* nearest 三兄弟：桩表统共十来条，全在一屏之内，按表序返回即可——真实实现
 * 那套"网格粗排 + Haversine 精算"在这里没有任何可观察的差别（调用方
 * fill_* 只按 LOD 的 limit 截断，limit 32 > 表长，一条都不会被截掉）。 */
static int near_all(int n, pk_aero_near_t *out, int max)
{
    if (!aero_on()) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        out[i].idx = (uint32_t)i;
        out[i].dist_nm = 0.0;
        out[i].brg_deg = 0.0;
    }
    return n;
}

int pk_aero_db_nearest_airports(double lat, double lon, pk_aero_near_t *out, int max)
{
    (void)lat; (void)lon;
    return near_all(NAPT, out, max);
}

int pk_aero_db_nearest_navaids(double lat, double lon, pk_aero_near_t *out, int max)
{
    (void)lat; (void)lon;
    return near_all(NNAV, out, max);
}

int pk_aero_db_nearest_fixes(double lat, double lon, pk_aero_near_t *out, int max)
{
    (void)lat; (void)lon;
    return near_all(NFIX, out, max);
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
