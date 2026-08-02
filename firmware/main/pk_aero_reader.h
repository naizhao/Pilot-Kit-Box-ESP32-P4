/*
 * pk_aero_reader.h — pk_aero.bin 可移植 C 参考实现（纯 C11，无 OS 依赖）
 *
 * 来源：tmp/pk_aero_bench/pk_aero_reader.h 搬入（2026-08-02，v4 版），
 * 唯一的固件侧改动是**同时接受 v2/v3/v4 三种 bin**（见下「三版兼容」）
 * 与可中断的分段子串搜索（pk_aero_search_substring_step）。
 * 对拍验证状态见 pk_aero_reader.c 文件头。
 *
 * 字节布局的权威实现是 Pilot-Kit 仓 scripts/aero_data_pipeline/
 * export_box_bin.py（PkAeroReader），本文件与其保持一致，规格文档
 * （2026-08-01-sd-aero-db-design-zh_CN.md）与脚本冲突处以脚本为准：
 *   - header 内 n_sections 为 uint16（规格误写 uint32）；
 *   - 索引区自描述 {u32 n_grid; u32 n_second} + n_grid×{u16 cell;
 *     u32 first; u16 count}（8 B 项，first 未对齐）+ n_second×u32；
 *   - AES-CTR counter 低 64 位 = payload 内偏移/16（加解密由调用方做，
 *     本模块不含任何 crypto 依赖）。
 *
 * v3（2026-08-02）新增，记录段一个字节都没动，只加索引：
 *   - 段表最后一个 u32 由 _reserved 改为 strings_size（子串搜索顺扫池要边界）；
 *   - 导航台段第二索引由空表填成 ident 排序表（修 G2）；
 *   - 新增 4 个纯索引段（type 16–19，4 B/项的 u32 数组）：机场全量 key、
 *     机场 name+city 合并反向、导航台 name 反向、FIX name 反向。
 *     反向索引项**只存记录下标**，键靠回读记录现取（同 ICAO/ident 索引手法）；
 *     机场表用 bit31 区分这一项索引的是 name_off 还是 city_off。
 *
 * v4（2026-08-02）新增空域 + 航路，**v3 的 9 个段一个字节都没动**：
 *   - header version 3 → 4；n_sections 9 → 14；payload 起点 352 → 512；
 *     读端版本判定放宽成区间 [PK_AERO_VERSION_MIN, PK_AERO_VERSION_MAX]，
 *     读 v3 文件时新段缺席（n=0）→ 空域/航路查询安全退化为 0 结果；
 *   - type=5  空域（48 B/条，带本段字符串池，无索引）；
 *   - type=6  空域顶点池（4 B/条：u16 x_q, u16 y_q，相对本空域 bbox 定点化）；
 *   - type=8  航路（48 B/条，记录**已按 (designator, seq) 排序**，
 *             所以"按航路名查"直接在记录上二分，不另建 designator 索引）；
 *   - type=20/21 空域、航路的**格→记录展开索引**：data = 按 (cell, 记录下标)
 *     排序的 u32 记录下标表，index 区沿用 v3 的稀疏格表 {cell, first, count}
 *     指向它（n_second=0）→ 格二分直接复用 pk_aero_grid_lookup。
 *
 * 三版兼容（固件侧要求：用户换卡前后不能出现"地图无数据"的窗口期。
 * 卡上现在还是 v3，v4 上卡是另一件事，所以三版都得能读）：
 *   - init 接受 version ∈ {2, 3, 4}，实际版本记在 db->version；
 *   - 老卡里没有的段根本不在段表里 → 对应 sec_* 保持全 0（n=0），不判失败：
 *     · v2 缺 4 个 v3 搜索索引段（16–19）+ strings_size（v2 那个 u32 是恒 0
 *       的 _reserved，显式按版本清零，顺扫池的 while 一步都不进）；
 *     · v2/v3 都缺 5 个 v4 段（5/6/8/20/21）→ 空域与航路查询一律返回 0。
 *   - 各查询 API 在老卡上的行为（缺段/缺索引 = 优雅返回 0，不读野指针）：
 *
 *       API                          v2 卡      v3 卡      v4 卡  依赖
 *       ---------------------------  ---------  ---------  -----  ------------
 *       airport_by_icao              正常       正常       正常   机场二级索引
 *       nearest_airports/navaids/    正常       正常       正常   各段网格索引
 *         fixes
 *       airport/rwy/freq/navaid/     正常       正常       正常   无
 *         fix_get、*_runways/_freqs
 *       fix_by_ident                 正常       正常       正常   FIX 二级索引
 *       fixes_by_prefix              正常       正常       正常   同上
 *       navaid_by_ident              **返回 0** 正常       正常   导航台二级索引
 *       navaids_by_prefix            **返回 0** 正常       正常     （v2 是空表）
 *       airports_by_prefix           **返回 0** 正常       正常   段 16
 *       search_substring             **返回 0** 正常       正常   strings_size
 *                                                                 + 段 17/18/19
 *       airspace_get/_ring           **false**  **false**  正常   段 5/6
 *       airspaces_in_cell/_in_bbox   **返回 0** **返回 0** 正常   段 20
 *       airway_get                   **false**  **false**  正常   段 8
 *       find_airways_by_designator/  **返回 0** **返回 0** 正常   段 8（记录自
 *         _by_prefix                                              身即有序）
 *       airways_in_cell/_in_bbox     **返回 0** **返回 0** 正常   段 21
 *
 *   （pk_aero_airway_lon_span 是纯函数，与版本无关，任何时候都可调。）
 *
 * 约束（面向 ESP32-P4 移植）：
 *   - 不做文件 IO：入参是已整块载入内存的 buffer + 长度；
 *   - 除 init 外零堆分配（init 本身也不分配，只填结构体）；
 *   - 所有多字节字段逐字节组装，绝不做未对齐指针强转解引用
 *     （v2 airport.name_off@31、navaid.elev@15、fix.grid_cell@15、
 *      grid 项 first@2 都不对齐）；
 *   - 记录内偏移小端，字符串池 24-bit 偏移为大端（照抄 aircraft_db）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 错误码 ---- */
enum {
    PK_AERO_OK             = 0,
    PK_AERO_ERR_TRUNCATED  = -1,   /* buffer 太短 / 段越界 */
    PK_AERO_ERR_MAGIC      = -2,   /* magic 不是 "PKAER1" */
    PK_AERO_ERR_VERSION    = -3,   /* 版本不支持 */
    PK_AERO_ERR_ENCRYPTED  = -4,   /* payload 加密且调用方未声明已解密 */
    PK_AERO_ERR_SECTION    = -5,   /* 缺必要段或段表非法 */
    PK_AERO_ERR_ARG        = -6,   /* 参数非法 */
};

/* ---- 格式常量（与 export_box_bin.py 一致）---- */
#define PK_AERO_MAGIC          "PKAER1"
/* 版本区间：v3 只加索引段、v4 只加空域/航路段，老段一个字节都没动
 * → 读端接受 [MIN, MAX] 区间，一份 reader 通吃 v2/v3/v4。
 * 老卡上新段缺席（n=0），对应查询安全退化为 0 结果，其余功能照跑。 */
#define PK_AERO_VERSION_MIN    2
#define PK_AERO_VERSION_MAX    4
#define PK_AERO_VERSION        PK_AERO_VERSION_MAX   /* 生成端当前版本 */
#define PK_AERO_HEADER_SIZE    64
#define PK_AERO_SECTION_SIZE   32

#define PK_AERO_SEC_AIRPORTS       1
#define PK_AERO_SEC_RUNWAY_DIRS    2
#define PK_AERO_SEC_FREQUENCIES    3
#define PK_AERO_SEC_NAVAIDS        4
#define PK_AERO_SEC_AIRSPACES      5   /* v4 */
#define PK_AERO_SEC_AIRSPACE_VERTS 6   /* v4 */
#define PK_AERO_SEC_WAYPOINTS_FIX  7
#define PK_AERO_SEC_AIRWAYS        8   /* v4 */

/* v3 纯索引段（record_size=4，一项一个小端 u32 记录下标）*/
#define PK_AERO_SEC_IDX_AIRPORT_KEY  16
#define PK_AERO_SEC_IDX_AIRPORT_NAME 17
#define PK_AERO_SEC_IDX_NAVAID_NAME  18
#define PK_AERO_SEC_IDX_FIX_NAME     19
/* v4 格→记录展开索引段（data = u32 记录下标表，index = 稀疏格表指向它）*/
#define PK_AERO_SEC_IDX_AIRSPACE_CELL 20
#define PK_AERO_SEC_IDX_AIRWAY_CELL   21
#define PK_AERO_IDX_ENTRY_SIZE       4
/* 机场名称反向索引项的 bit31：1=这一项索引的是 city_off，0=name_off */
#define PK_AERO_NAME_IDX_CITY_FLAG   0x80000000u
#define PK_AERO_REC_IDX_MASK         0x7FFFFFFFu

#define PK_AERO_AIRPORT_SIZE   40
#define PK_AERO_RWY_DIR_SIZE   24
#define PK_AERO_FREQ_SIZE      8
#define PK_AERO_NAVAID_SIZE    32
#define PK_AERO_FIX_SIZE       24
#define PK_AERO_AIRSPACE_SIZE     48   /* v4 */
#define PK_AERO_AIRSPACE_VTX_SIZE 4    /* v4 */
#define PK_AERO_AIRWAY_SIZE       48   /* v4 */

/* FIX scope 枚举（0=未知 1=enroute 2=terminal 3=both）*/
#define PK_AERO_FIX_SCOPE_UNKNOWN  0
#define PK_AERO_FIX_SCOPE_ENROUTE  1
#define PK_AERO_FIX_SCOPE_TERMINAL 2
#define PK_AERO_FIX_SCOPE_BOTH     3

#define PK_AERO_COORD_NONE     0x7FFFFFFF   /* int32 缺失坐标哨兵 */
#define PK_AERO_BEARING_NONE   0xFFFF       /* uint16 缺失磁航向哨兵（0.1°）*/
#define PK_AERO_STR_NONE       0            /* 字符串池偏移 0 = 无 */

/* v4 哨兵 / 定点常量 */
#define PK_AERO_ALT_NONE       (-32768)     /* i16 高度哨兵（100 ft 为单位）*/
#define PK_AERO_TRACK_NONE     0xFFFF       /* u16 磁航迹哨兵（0.1°）*/
#define PK_AERO_DIST_NONE      0xFFFF       /* u16 距离哨兵（0.1 nm）*/
#define PK_AERO_VTX_SCALE_MAX  65535.0      /* 顶点 u16 定点满量程（double）*/
/* 上/下限高度都是 i16、单位 100 ft：-32768 = 该端无限制/未知（见上），
 * 而 9999（= 999,900 ft）是生成端对 "UNL / UNLIMITED" 的编码，绘制时
 * 应当显示成 "UNL" 而不是一个荒唐的高度数字。 */
#define PK_AERO_ALT_UNLIMITED  9999

/* ---- v4 枚举（照抄 export_box_bin.py 的 *_ENUM；0 = 未知/其它）----
 *
 * 管线的字典里同一个号有别名（PROHIBITED/P、DANGER/D、FORWARD/F/TRUE…），
 * 读端只需要规范名，所以这里一个号只出现一次。分组按管线注释：
 *   1–9   管制分类（CLASS_A..G）
 *   10–19 限制类（禁/限/危/警/告警/军事活动区…）
 *   20–39 管制区（FIR/UIR/CTA/TMA/CTR…）
 *   40–49 其它活动区（跳伞/滑翔）
 */
#define PK_AERO_ASP_TYPE_OTHER        0
#define PK_AERO_ASP_TYPE_CLASS_A      1
#define PK_AERO_ASP_TYPE_CLASS_B      2
#define PK_AERO_ASP_TYPE_CLASS_C      3
#define PK_AERO_ASP_TYPE_CLASS_D      4
#define PK_AERO_ASP_TYPE_CLASS_E      5
#define PK_AERO_ASP_TYPE_CLASS_F      6
#define PK_AERO_ASP_TYPE_CLASS_G      7
#define PK_AERO_ASP_TYPE_PROHIBITED   10   /* 别名 P */
#define PK_AERO_ASP_TYPE_RESTRICTED   11   /* 别名 R */
#define PK_AERO_ASP_TYPE_DANGER       12   /* 别名 D */
#define PK_AERO_ASP_TYPE_WARNING      13
#define PK_AERO_ASP_TYPE_ALERT        14
#define PK_AERO_ASP_TYPE_MOA          15
#define PK_AERO_ASP_TYPE_TRA          16
#define PK_AERO_ASP_TYPE_TSA          17
#define PK_AERO_ASP_TYPE_ADIZ         18
#define PK_AERO_ASP_TYPE_FIR          20
#define PK_AERO_ASP_TYPE_UIR          21
#define PK_AERO_ASP_TYPE_CTA          22
#define PK_AERO_ASP_TYPE_UTA          23
#define PK_AERO_ASP_TYPE_TMA          24
#define PK_AERO_ASP_TYPE_CTR          25
#define PK_AERO_ASP_TYPE_TIA          26
#define PK_AERO_ASP_TYPE_TIZ          27
#define PK_AERO_ASP_TYPE_ACC          28
#define PK_AERO_ASP_TYPE_SECTOR       29
#define PK_AERO_ASP_TYPE_AERODROME    30
#define PK_AERO_ASP_TYPE_RMZ          31
#define PK_AERO_ASP_TYPE_TMZ          32
#define PK_AERO_ASP_TYPE_CORRIDOR     33
#define PK_AERO_ASP_TYPE_PARA_JUMPING 40
#define PK_AERO_ASP_TYPE_GLIDING      41

/* 空域等级（AIRSPACE_CLASS_ENUM）*/
#define PK_AERO_ASP_CLASS_NONE  0
#define PK_AERO_ASP_CLASS_A     1
#define PK_AERO_ASP_CLASS_B     2
#define PK_AERO_ASP_CLASS_C     3
#define PK_AERO_ASP_CLASS_D     4
#define PK_AERO_ASP_CLASS_E     5
#define PK_AERO_ASP_CLASS_F     6
#define PK_AERO_ASP_CLASS_G     7

/* 高度基准（ALT_REF_ENUM）。SFC 与 GND 同义、FL 与 STD 同义，但源库两种
 * 写法都在用，管线保留了原始区分——显示时按需归并（GND/SFC → "AGL"）。 */
#define PK_AERO_ALT_REF_UNKNOWN 0
#define PK_AERO_ALT_REF_GND     1
#define PK_AERO_ALT_REF_MSL     2
#define PK_AERO_ALT_REF_STD     3
#define PK_AERO_ALT_REF_FL      4
#define PK_AERO_ALT_REF_SFC     5

/* 航路类型（AIRWAY_TYPE_ENUM，实测全集 A/B/G/J/L/M/Q/R/T/V/W/Y）*/
#define PK_AERO_AWY_TYPE_UNKNOWN 0
#define PK_AERO_AWY_TYPE_A       1
#define PK_AERO_AWY_TYPE_B       2
#define PK_AERO_AWY_TYPE_G       3
#define PK_AERO_AWY_TYPE_J       4
#define PK_AERO_AWY_TYPE_L       5
#define PK_AERO_AWY_TYPE_M       6
#define PK_AERO_AWY_TYPE_Q       7
#define PK_AERO_AWY_TYPE_R       8
#define PK_AERO_AWY_TYPE_T       9
#define PK_AERO_AWY_TYPE_V       10
#define PK_AERO_AWY_TYPE_W       11
#define PK_AERO_AWY_TYPE_Y       12

/* 航路高度层（AIRWAY_LEVEL_ENUM）：高空 / 低空 / 两者 */
#define PK_AERO_AWY_LEVEL_UNKNOWN 0
#define PK_AERO_AWY_LEVEL_HIGH    1   /* H */
#define PK_AERO_AWY_LEVEL_LOW     2   /* L */
#define PK_AERO_AWY_LEVEL_BOTH    3   /* B */

/* 航路方向（AIRWAY_DIR_ENUM）：沿 start→end / 反向 / 双向 */
#define PK_AERO_AWY_DIR_UNKNOWN  0
#define PK_AERO_AWY_DIR_FORWARD  1
#define PK_AERO_AWY_DIR_BACKWARD 2
#define PK_AERO_AWY_DIR_BOTH     3

#define PK_AERO_ENC_NONE       0
#define PK_AERO_ENC_AES128_CTR 1

/* nearest 查询上限（out[] 最大条数；栈上工作区按此定容）*/
#define PK_AERO_NEAR_MAX       32
/* 近似粗排模式在 max 之外多保留的候选数（降低粗排误杀 top-N 的概率）*/
#define PK_AERO_APPROX_SLACK   16

/* ---- 距离算法选择 ---- */
typedef enum {
    PK_AERO_DIST_HAVERSINE = 0,   /* 全候选 Haversine（geo.c，double）*/
    PK_AERO_DIST_APPROX    = 1,   /* 等距柱状平方距离粗排 + 前 N Haversine 精算 */
} pk_aero_dist_mode_t;

/* ---- 段描述（解析自段表）---- */
typedef struct {
    uint16_t type;
    uint16_t rec_size;
    uint32_t n;
    uint32_t data_off;      /* payload 内偏移 */
    uint32_t data_size;
    uint32_t index_off;     /* 0 = 无索引 */
    uint32_t index_size;
    uint32_t strings_off;   /* 0 = 无本段字符串池 */
    uint32_t strings_size;  /* v3：池字节数（v2 该字段是恒 0 的 _reserved）*/
} pk_aero_section_t;

/* ---- 网格/第二索引定位（init 时从索引区自描述头解出）---- */
typedef struct {
    uint32_t n_grid;        /* 已占用格数 */
    uint32_t grid_off;      /* payload 内偏移：n_grid × 8 B 项 */
    uint32_t n_second;      /* 第二索引条数（airports = ICAO 排序下标）*/
    uint32_t second_off;    /* payload 内偏移：n_second × u32 */
} pk_aero_index_t;

/* ---- 算法特征计数器（平台无关，供基准统计）---- */
typedef struct {
    uint64_t dist_calcs;     /* Haversine 调用次数 */
    uint64_t approx_calcs;   /* 等距柱状近似距离计算次数 */
    uint64_t bsearch_steps;  /* 二分循环步数（网格 + ICAO + 前缀 + 反查）*/
    uint32_t grid_lookups;   /* 网格二分次数 */
    uint32_t candidates;     /* 最近一次 nearest 查询扫过的候选记录数 */
    /* v3 搜索：平台无关的"触及量"指标（Mac 绝对耗时不可外推到 P4，这些能）*/
    uint64_t pool_bytes;     /* 子串搜索顺扫过的字符串池字节数 */
    uint64_t pool_strings;   /* 顺扫过的池内字符串条数 */
    uint64_t pool_hits;      /* 池内命中（含尚未落到结果里的）条数 */
    uint64_t rev_lookups;    /* 名称反向索引二分次数 */
    uint64_t rev_derefs;     /* 反向/前缀二分中回读记录取键的次数（随机访存）*/
    uint64_t prefix_scanned; /* 前缀枚举里线性扫过的索引项数 */
    /* v4 空间查询：平台无关的"触及量" */
    uint64_t cells_visited;  /* bbox 展开出的格数（含重复格）*/
    uint64_t cell_candidates;/* 格展开表里取到的候选记录下标数（含重复）*/
    uint64_t bbox_tests;     /* 候选记录 bbox 相交测试次数 */
    uint64_t bbox_inserts;   /* 通过精筛并尝试插入结果集的次数 */
} pk_aero_stats_t;

/* ---- 数据库句柄（解析态，几十字节级，验证"常驻 = bin 本体"）---- */
typedef struct {
    const uint8_t *payload;      /* 明文 payload 起点（buffer 内）*/
    uint32_t       payload_len;
    uint16_t       version;      /* 实际 bin 版本（2/3/4），诊断/日志用 */
    char           cycle[9];     /* NUL 结尾 */
    uint8_t        enc_algo;
    uint8_t        nonce[8];     /* CTR 高 64 位（供调用方解密用）*/
    uint8_t        sha256[32];   /* payload 明文摘要（供调用方校验用）*/
    pk_aero_section_t sec_airport, sec_rwy, sec_freq, sec_navaid, sec_fix;
    /* v3 索引段（n=0 表示该索引缺席，查询会安全退化为无结果）*/
    pk_aero_section_t sec_idx_apt_key, sec_idx_apt_name;
    pk_aero_section_t sec_idx_nav_name, sec_idx_fix_name;
    pk_aero_index_t   apt_idx, nav_idx, fix_idx;
    /* v4 段（读 v3 文件时全为 0 → n=0 → 空域/航路查询返回 0 结果）*/
    pk_aero_section_t sec_airspace, sec_asp_vtx, sec_airway;
    pk_aero_section_t sec_idx_asp_cell, sec_idx_awy_cell;
    pk_aero_index_t   asp_cell_idx, awy_cell_idx;
    pk_aero_stats_t   stats;
} pk_aero_db_t;

/* ---- 解码后的记录视图（字符串是指向 payload 池的 const 指针）---- */
typedef struct {
    double      lat, lon;
    char        icao[5], iata[4], country[3];   /* NUL 结尾 */
    uint8_t     type, ctrl;
    int16_t     elev_ft;
    uint16_t    longest_rwy_ft;
    uint32_t    rwy_first, freq_first;          /* v2：记录里是 24-bit 大端 */
    uint8_t     rwy_count, freq_count;
    uint16_t    grid_cell;
    const char *name, *city;                    /* 无则 "" */
    int32_t     lat_e7, lon_e7;                 /* 原始定点值（对拍用）*/
} pk_aero_airport_t;

typedef struct {
    bool        has_coord;
    double      lat, lon;
    int32_t     lat_e7, lon_e7;
    char        designator[5];
    bool        has_bearing;
    uint16_t    mag_bearing_dd;   /* 0.1°，has_bearing=false 时无效 */
    uint16_t    length_ft, width_ft;
    uint8_t     surface;
    int16_t     thr_elev_ft;
} pk_aero_rwy_dir_t;

typedef struct {
    uint32_t    freq_khz;
    uint8_t     service;
    const char *callsign;         /* 无则 "" */
} pk_aero_freq_t;

typedef struct {
    double      lat, lon;
    int32_t     lat_e7, lon_e7;
    char        ident[7];
    uint8_t     type;
    int16_t     elev_ft;
    uint32_t    freq_khz;
    const char *name;
    uint16_t    grid_cell;
} pk_aero_navaid_t;

/* v2：航路 FIX（waypoints_fix 段，24 B/条）*/
typedef struct {
    double      lat, lon;
    int32_t     lat_e7, lon_e7;
    char        ident[7];         /* NUL 结尾 */
    uint8_t     scope;            /* PK_AERO_FIX_SCOPE_* */
    uint16_t    grid_cell;
    const char *name;             /* 无则 "" */
} pk_aero_fix_t;

/* v4：空域（airspaces 段，48 B/条）*/
typedef struct {
    int32_t     min_lat_e7, min_lon_e7, max_lat_e7, max_lon_e7;
    double      min_lat, min_lon, max_lat, max_lon;   /* = e7 / 1e7 */
    uint8_t     type, cls;            /* AIRSPACE_TYPE_ENUM / CLASS_ENUM */
    uint8_t     lower_ref, upper_ref; /* ALT_REF_ENUM（0=未知）*/
    int16_t     lower_100ft, upper_100ft;  /* PK_AERO_ALT_NONE = 无 */
    bool        has_lower, has_upper;
    const char *name, *designator;    /* 无则 ""（本段字符串池）*/
    uint32_t    vtx_first_fine, vtx_first_coarse;   /* 记录里是 24-bit 大端 */
    uint16_t    vtx_count_fine, vtx_count_coarse;
    uint16_t    grid_cell;            /* bbox 中心代表点 */
} pk_aero_airspace_t;

/* v4：几何顶点（还原后的度）。x=经度、y=纬度，与 Python (lon, lat) 同序 */
typedef struct {
    double lon, lat;
} pk_aero_lonlat_t;

/* v4：航路段（airways 段，48 B/条）*/
typedef struct {
    int32_t     start_lat_e7, start_lon_e7, end_lat_e7, end_lon_e7;
    double      start_lat, start_lon, end_lat, end_lon;
    char        designator[7], start_ident[7], end_ident[7];  /* NUL 结尾 */
    uint8_t     type, level, direction;
    uint16_t    mag_track_dd;      /* 0.1°，PK_AERO_TRACK_NONE = 无 */
    uint16_t    dist_dnm;          /* 0.1 nm，PK_AERO_DIST_NONE = 无 */
    bool        has_mag_track, has_distance;
    int16_t     min_alt_100ft, max_alt_100ft;   /* PK_AERO_ALT_NONE = 无 */
    bool        has_min_alt, has_max_alt;
    uint16_t    seq;
} pk_aero_airway_t;

/* nearest 查询结果 */
typedef struct {
    uint32_t idx;        /* 段内记录下标 */
    double   dist_nm;
    double   brg_deg;    /* 查询点 → 目标 初始方位 */
} pk_aero_near_t;

/* ---- API ---- */

/* 初始化：解析 header + 段表 + 索引区定位并做边界校验。
 * buf/len 是整个文件（含 header）。payload_is_plain=true 表示调用方已
 * 就地解密 payload（此时忽略 enc_algo）；否则 enc_algo!=0 会返回
 * PK_AERO_ERR_ENCRYPTED。本函数不做 SHA 校验（无 crypto 依赖），调用方
 * 可用 db->sha256 + pk_aero_payload() 自行校验。 */
int pk_aero_init(pk_aero_db_t *db, const uint8_t *buf, size_t len,
                 bool payload_is_plain);

/* 从文件 buffer 计算 payload 起始偏移（供调用方定位解密区间；
 * 失败返回 0）。 */
uint32_t pk_aero_payload_off(const uint8_t *buf, size_t len);

/* ICAO 精确查找：命中返回记录下标（>=0），否则 -1。code 可为 2–4 字符。 */
int32_t pk_aero_airport_by_icao(pk_aero_db_t *db, const char *code);

/* 最近机场 / 导航台：目标格 ±1 圈 9 格候选。返回写入 out 的条数。
 * max 上限 PK_AERO_NEAR_MAX。 */
int pk_aero_nearest_airports(pk_aero_db_t *db, double lat, double lon,
                             pk_aero_near_t *out, int max,
                             pk_aero_dist_mode_t mode);
int pk_aero_nearest_navaids(pk_aero_db_t *db, double lat, double lon,
                            pk_aero_near_t *out, int max,
                            pk_aero_dist_mode_t mode);
int pk_aero_nearest_fixes(pk_aero_db_t *db, double lat, double lon,
                          pk_aero_near_t *out, int max,
                          pk_aero_dist_mode_t mode);

/* v2：ident 精确查找。二分到同名区间左端后线性扫，命中下标写入 out[]
 * （最多 max 个），返回同名总条数（可能 > max，调用方按需扩容重查）；
 * 无命中返回 0，参数非法返回负错误码。ident 1–6 字符。 */
int pk_aero_fix_by_ident(pk_aero_db_t *db, const char *ident,
                         uint32_t *out, int max);

/* ---- v3 搜索 API ----
 *
 * 共同约定：
 *   - 查询串由本模块自己 toupper 归一（池里的代码类字段全大写；名称混合
 *     大小写，子串比较两边都归一），调用方不必预处理；
 *   - 非 ASCII 或超长查询返回 0（不是错误，就是无结果）；
 *   - **写满 max 就停并返回已写条数**（单字符前缀会枚举出数千条，不做
 *     截断会白扫；调用方拿不到"还有更多"这一信息，需要就加大 max 重查）；
 *   - 零堆分配，工作区全在调用方给的 out[] 与几十字节的栈变量里；
 *   - **老卡上所需索引缺席时一律返回 0**（不是错误，就是"这版数据搜不了"），
 *     唯一例外是 fixes_by_prefix：它吃的 FIX ident 索引 v2 就有，照常工作。
 *     逐 API 的可用性见文件头的三版对照表。
 */

/* 机场代码前缀枚举：走 v3 全量 key 索引（icao 优先，无 icao 用 iata），
 * 因此覆盖率高于 pk_aero_airport_by_icao 用的 ICAO 索引。prefix 1–4 字符。*/
int pk_aero_airports_by_prefix(pk_aero_db_t *db, const char *prefix,
                               uint32_t *out, int max);
/* FIX / 导航台 ident 前缀枚举（1–6 字符）*/
int pk_aero_fixes_by_prefix(pk_aero_db_t *db, const char *prefix,
                            uint32_t *out, int max);
int pk_aero_navaids_by_prefix(pk_aero_db_t *db, const char *prefix,
                              uint32_t *out, int max);
/* v3（修 G2）：导航台 ident 精确查找。同名多台（VOR/NDB 共用呼号很常见）
 * 全部返回，语义同 pk_aero_fix_by_ident：返回同名总条数（可能 > max）。*/
int pk_aero_navaid_by_ident(pk_aero_db_t *db, const char *ident,
                            uint32_t *out, int max);

/* 子串搜索的一条结果：type 是段类型（AIRPORTS / NAVAIDS / WAYPOINTS_FIX）*/
typedef struct {
    uint8_t  type;
    uint32_t idx;
} pk_aero_hit_t;

/* 子串搜索（大小写不敏感）：顺扫三段字符串池 → 命中偏移 → 名称反向索引
 * 二分回查记录号。**绝不逐记录解引用取名字**（那条慢路径是 150–300 ms）。
 * 结果顺序 = 段顺序（机场→导航台→FIX）× 池内偏移升序 × 索引表内顺序，
 * 按 (type, idx) 去重（机场 name 与 city 可能命中同一条）。 */
int pk_aero_search_substring(pk_aero_db_t *db, const char *query,
                             pk_aero_hit_t *out, int max);

/* ---- 可中断的子串搜索（分段让渡用）--------------------------------
 *
 * 为什么需要它：上面那个一口气版本在 P4 上实测 **65 ms**，而 pk_aero_db 的
 * 并发方案是"查询全程持锁"——65 ms 的持锁窗口会把地图叠加层的后台快照查询
 * 和拔卡卸载一起堵住那么久。正确做法是每扫若干 KB 就 give/take 一次锁，并
 * **在每段开头复检 state/代数**（拔卡后 payload 已被 free，拿着上一段的游标
 * 继续 strlen 就是对悬空指针取值）。
 *
 * 游标零初始化即"从头开始"。每次调用最多扫 budget_bytes 字节的字符串池
 * （budget_bytes==0 = 不限，等价于一口气版本），返回 true 表示**已扫完**
 * （三段扫尽，或 out 已写满 max）。结果条数在 cur->n。
 *
 * out[] 由调用方跨调用持有：里面只有 (type, idx)，没有任何指向 payload 的
 * 指针，所以中途拔卡也不会留下悬空引用——但**中断后的半截结果属于上一张卡**，
 * 调用方应当整体丢弃而不是拿来显示。 */
typedef struct {
    uint8_t  stage;   /* 0=机场 1=导航台 2=FIX 3=已扫完 */
    uint32_t off;     /* 当前段字符串池内的下一个待扫偏移（0=本段还没开始）*/
    int      n;       /* 已写进 out[] 的条数（跨段累计）*/
} pk_aero_search_cursor_t;

bool pk_aero_search_substring_step(pk_aero_db_t *db, const char *query,
                                   pk_aero_hit_t *out, int max,
                                   pk_aero_search_cursor_t *cur,
                                   uint32_t budget_bytes);

/* 记录读取（idx 越界返回 false）*/
bool pk_aero_airport_get(const pk_aero_db_t *db, uint32_t idx,
                         pk_aero_airport_t *out);
bool pk_aero_rwy_dir_get(const pk_aero_db_t *db, uint32_t idx,
                         pk_aero_rwy_dir_t *out);
bool pk_aero_freq_get(const pk_aero_db_t *db, uint32_t idx,
                      pk_aero_freq_t *out);
bool pk_aero_navaid_get(const pk_aero_db_t *db, uint32_t idx,
                        pk_aero_navaid_t *out);
bool pk_aero_fix_get(const pk_aero_db_t *db, uint32_t idx,
                     pk_aero_fix_t *out);

/* ---- v4 空域 / 航路 API ----
 *
 * 共同约定（沿用 v3）：
 *   - 零堆分配。bbox 查询的**唯一**工作区就是调用方给的 out[]：结果集在
 *     out[] 里维持"记录下标升序 + 互异"，满 max 时挤掉当前最大的那个，
 *     语义等价于 Python "所有格候选去重 → 升序 → 取前 max 个"。
 *     额外内存 = 0（只有几十字节栈变量），代价是每次插入 O(max) 搬移。
 *   - 段缺席（读 v2/v3 文件）时所有查询返回 0 / false，不报错。
 */

/* 空域记录读取（idx 越界 / 段缺席返回 false）*/
bool pk_aero_airspace_get(const pk_aero_db_t *db, uint32_t idx,
                          pk_aero_airspace_t *out);

/* 还原某档几何（coarse=false 取 fine 档）。u16 定点 → 度：
 *   lon = min_lon + (max_lon - min_lon) * x_q / 65535（double，同 Python）。
 * 写入 out[]，返回写入点数（<= min(顶点数, max)）；参数非法返回负错误码。*/
int pk_aero_airspace_ring(const pk_aero_db_t *db, uint32_t idx, bool coarse,
                          pk_aero_lonlat_t *out, int max);

/* 单格查询：段 20 展开表里该格的记录下标（已按下标升序）。返回写入条数。*/
int pk_aero_airspaces_in_cell(pk_aero_db_t *db, uint16_t cell,
                              uint32_t *out, int max);

/* 视野 bbox → 命中空域下标（升序、去重）。两级过滤：1° 格展开取候选 →
 * 记录 bbox 相交精筛。返回写入条数。
 * ⚠ 约定 min_lon <= max_lon：**查询 bbox 自身不支持跨 ±180**，跨经线的
 * 视野请由调用方拆成两段查（环绕逻辑只收在格展开一处）。违约返回
 * PK_AERO_ERR_ARG。纬度反了（min_lat > max_lat）则是空结果，不是错误。*/
int pk_aero_airspaces_in_bbox(pk_aero_db_t *db,
                              double min_lat, double min_lon,
                              double max_lat, double max_lon,
                              uint32_t *out, int max);

/* 航路段记录读取 */
bool pk_aero_airway_get(const pk_aero_db_t *db, uint32_t idx,
                        pk_aero_airway_t *out);

/* 按航路名精确查：记录本身已按 (designator, seq) 排序，直接在记录上二分到
 * 同名区间左端再线性扫 —— 返回的下标序列即该航路的完整有序段序列。
 * 语义同 pk_aero_fix_by_ident：返回同名总条数（可能 > max，只写前 max 个）。*/
int pk_aero_find_airways_by_designator(pk_aero_db_t *db, const char *name,
                                       uint32_t *out, int max);
/* 航路名前缀枚举（1–6 字符）：记录顺序即索引顺序，写满 max 就停。*/
int pk_aero_find_airways_by_prefix(pk_aero_db_t *db, const char *prefix,
                                   uint32_t *out, int max);

/* 航路段的**真实经度区间**（短弧口径，可能越出 ±180，不夹回）。
 * 直接对两端点取 min/max 是错的：SYA(174.06°E)→ADK(176.67°W) 这类阿留申
 * 航段短弧只有一两百海里，min/max 却会给出横跨大半个地球的假区间。
 * 生成端建格索引与读端精筛必须同口径，故公开出来供绘制端复用。*/
void pk_aero_airway_lon_span(double start_lon, double end_lon,
                             double *lo_lon, double *hi_lon);

/* 航路的格 / bbox 查询，语义与约定同空域；精筛 = 段两端点构成的 bbox
 * （纬度取 min/max、经度取上面的短弧区间并允许 ±360 平移后相交）。
 * 线段与矩形的真实相交测试留给绘制时裁剪，这里只保证不漏。*/
int pk_aero_airways_in_cell(pk_aero_db_t *db, uint16_t cell,
                            uint32_t *out, int max);
int pk_aero_airways_in_bbox(pk_aero_db_t *db,
                            double min_lat, double min_lon,
                            double max_lat, double max_lon,
                            uint32_t *out, int max);

/* 聚簇区间：机场的跑道 / 频率在各自段内的 [first, first+count) */
bool pk_aero_airport_runways(const pk_aero_db_t *db, uint32_t apt_idx,
                             uint32_t *first, uint32_t *count);
bool pk_aero_airport_freqs(const pk_aero_db_t *db, uint32_t apt_idx,
                           uint32_t *first, uint32_t *count);

/* 网格工具（与生成端 grid_cell_u16 语义一致：floor + 行钳位 + 列环绕）*/
uint16_t pk_aero_grid_cell(double lat, double lon);
/* 查某格在段内的记录区间；未占用格返回 count=0。 */
void pk_aero_grid_lookup(pk_aero_db_t *db, const pk_aero_index_t *gi,
                         uint16_t cell, uint32_t *first, uint32_t *count);

/* 计数器 */
void pk_aero_stats_reset(pk_aero_db_t *db);

/* 便捷访问 */
static inline const uint8_t *pk_aero_payload(const pk_aero_db_t *db,
                                             uint32_t *len)
{
    if (len) *len = db->payload_len;
    return db->payload;
}

#ifdef __cplusplus
}
#endif
