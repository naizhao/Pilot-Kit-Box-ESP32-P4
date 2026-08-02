/*
 * pk_aero_reader.c — pk_aero.bin 可移植 C 参考实现
 *
 * 来源：tmp/pk_aero_bench/pk_aero_reader.c 搬入（2026-08-02，v4 版）。
 * 固件侧改动只有两处：
 *   1. 接受 v2/v3/v4 三种 bin（版本判定 + db->version，见 pk_aero_init）；
 *   2. 多一份可中断的分段子串搜索（pk_aero_search_substring_step），
 *      一口气版本改成"预算不限"地调它，两条路径共用同一个 scan_pool_range。
 * 搬入前验证状态：Mac 端与 Python 对拍 100% 一致（5,229 条用例：ICAO /
 * nearest / 聚簇 / FIX ident / v3 前缀三表 / 导航台 ident / 子串 /
 * v4 空域记录 + 环 + bbox / 航路记录 + designator + bbox）。
 *
 * 布局权威：Pilot-Kit 仓 scripts/aero_data_pipeline/export_box_bin.py。
 * 逐字节组装所有多字节字段（小端），字符串池 24-bit 偏移为大端
 * （照抄本仓 firmware/main/aircraft_db.c 的 rec_reg_off 手法）。
 * 除 init 外零堆分配；nearest 的工作区在栈上定容。
 */
#include "pk_aero_reader.h"

#include <math.h>
#include <string.h>

#include "geo.h"

/* ------------------------------------------------------------------ */
/* 逐字节取数：P4 上 payload 记录字段大量奇偏移，禁止指针强转           */
/* ------------------------------------------------------------------ */

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline int16_t rd_i16(const uint8_t *p)
{
    return (int16_t)rd_u16(p);
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

/* 字符串池 24-bit 偏移：大端（同 aircraft_db reg_off）*/
static inline uint32_t rd_u24be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* 定长 char[n] → NUL 结尾拷贝（dst 容量 n+1）*/
static void fix_str(char *dst, const uint8_t *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i] != '\0') {
        dst[i] = (char)src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* init / validate                                                     */
/* ------------------------------------------------------------------ */

uint32_t pk_aero_payload_off(const uint8_t *buf, size_t len)
{
    if (len < PK_AERO_HEADER_SIZE) return 0;
    uint16_t n_sections   = rd_u16(buf + 16);
    uint32_t sections_off = rd_u32(buf + 18);
    uint32_t off = sections_off + (uint32_t)n_sections * PK_AERO_SECTION_SIZE;
    return (off + 15u) / 16u * 16u;   /* 16 B 对齐（同 PkAeroReader）*/
}

static int parse_index(const pk_aero_db_t *db, const pk_aero_section_t *sec,
                       pk_aero_index_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sec->index_off == 0 || sec->index_size < 8) return PK_AERO_OK;
    if ((uint64_t)sec->index_off + sec->index_size > db->payload_len)
        return PK_AERO_ERR_TRUNCATED;
    const uint8_t *p = db->payload + sec->index_off;
    out->n_grid   = rd_u32(p);
    out->n_second = rd_u32(p + 4);
    out->grid_off   = sec->index_off + 8;
    out->second_off = out->grid_off + out->n_grid * 8u;
    uint64_t need = 8u + (uint64_t)out->n_grid * 8u
                       + (uint64_t)out->n_second * 4u;
    if (need > sec->index_size) return PK_AERO_ERR_TRUNCATED;
    return PK_AERO_OK;
}

int pk_aero_init(pk_aero_db_t *db, const uint8_t *buf, size_t len,
                 bool payload_is_plain)
{
    if (!db || !buf) return PK_AERO_ERR_ARG;
    memset(db, 0, sizeof(*db));

    if (len < PK_AERO_HEADER_SIZE) return PK_AERO_ERR_TRUNCATED;
    if (memcmp(buf, PK_AERO_MAGIC, 6) != 0) return PK_AERO_ERR_MAGIC;
    /* 三版兼容：v3 只加索引段、v4 只加空域/航路段，老段一个字节都没动
     * → 接受 [MIN, MAX] 区间（同 PkAeroReader）。用户换卡有窗口期，
     * 三版都得能读，缺段的查询各自退化（见头文件对照表）。 */
    uint16_t version = rd_u16(buf + 6);
    if (version < PK_AERO_VERSION_MIN || version > PK_AERO_VERSION_MAX)
        return PK_AERO_ERR_VERSION;
    db->version = version;

    fix_str(db->cycle, buf + 8, 8);
    uint16_t n_sections   = rd_u16(buf + 16);
    uint32_t sections_off = rd_u32(buf + 18);
    db->enc_algo = buf[22];
    memcpy(db->nonce,  buf + 24, 8);
    memcpy(db->sha256, buf + 32, 32);

    if (db->enc_algo != PK_AERO_ENC_NONE && !payload_is_plain)
        return PK_AERO_ERR_ENCRYPTED;

    uint64_t table_end = (uint64_t)sections_off
                       + (uint64_t)n_sections * PK_AERO_SECTION_SIZE;
    if (table_end > len) return PK_AERO_ERR_TRUNCATED;

    uint32_t payload_off = pk_aero_payload_off(buf, len);
    if (payload_off == 0 || payload_off > len) return PK_AERO_ERR_TRUNCATED;
    db->payload     = buf + payload_off;
    db->payload_len = (uint32_t)(len - payload_off);

    /* 段表：只认识下面 switch 里列出的类型，未知类型跳过（前向兼容）*/
    for (uint16_t i = 0; i < n_sections; i++) {
        const uint8_t *e = buf + sections_off
                         + (size_t)i * PK_AERO_SECTION_SIZE;
        pk_aero_section_t s;
        s.type        = rd_u16(e);
        s.rec_size    = rd_u16(e + 2);
        s.n           = rd_u32(e + 4);
        s.data_off    = rd_u32(e + 8);
        s.data_size   = rd_u32(e + 12);
        s.index_off   = rd_u32(e + 16);
        s.index_size  = rd_u32(e + 20);
        s.strings_off = rd_u32(e + 24);
        /* v3 才有 strings_size；v2 这个位置是 _reserved（实测产物恒 0）。
         * 显式按版本清零，免得哪天 v2 的保留位被写脏，被顺扫池当成长度用。 */
        s.strings_size = (version >= 3) ? rd_u32(e + 28) : 0;
        if ((uint64_t)s.data_off + s.data_size > db->payload_len)
            return PK_AERO_ERR_TRUNCATED;
        if ((uint64_t)s.rec_size * s.n > s.data_size)
            return PK_AERO_ERR_SECTION;
        if ((uint64_t)s.strings_off + s.strings_size > db->payload_len)
            return PK_AERO_ERR_TRUNCATED;
        switch (s.type) {
        case PK_AERO_SEC_AIRPORTS:      db->sec_airport = s; break;
        case PK_AERO_SEC_RUNWAY_DIRS:   db->sec_rwy     = s; break;
        case PK_AERO_SEC_FREQUENCIES:   db->sec_freq    = s; break;
        case PK_AERO_SEC_NAVAIDS:       db->sec_navaid  = s; break;
        case PK_AERO_SEC_WAYPOINTS_FIX: db->sec_fix     = s; break;
        case PK_AERO_SEC_IDX_AIRPORT_KEY:  db->sec_idx_apt_key  = s; break;
        case PK_AERO_SEC_IDX_AIRPORT_NAME: db->sec_idx_apt_name = s; break;
        case PK_AERO_SEC_IDX_NAVAID_NAME:  db->sec_idx_nav_name = s; break;
        case PK_AERO_SEC_IDX_FIX_NAME:     db->sec_idx_fix_name = s; break;
        /* v4：读 v2/v3 文件时这几个 case 一次都不会命中，结构体保持全 0 */
        case PK_AERO_SEC_AIRSPACES:       db->sec_airspace     = s; break;
        case PK_AERO_SEC_AIRSPACE_VERTS:  db->sec_asp_vtx      = s; break;
        case PK_AERO_SEC_AIRWAYS:         db->sec_airway       = s; break;
        case PK_AERO_SEC_IDX_AIRSPACE_CELL: db->sec_idx_asp_cell = s; break;
        case PK_AERO_SEC_IDX_AIRWAY_CELL:   db->sec_idx_awy_cell = s; break;
        default: break;   /* 未知类型跳过：前向兼容 */
        }
    }
    if (db->sec_airport.rec_size != PK_AERO_AIRPORT_SIZE ||
        db->sec_rwy.rec_size     != PK_AERO_RWY_DIR_SIZE ||
        db->sec_freq.rec_size    != PK_AERO_FREQ_SIZE ||
        db->sec_navaid.rec_size  != PK_AERO_NAVAID_SIZE ||
        db->sec_fix.rec_size     != PK_AERO_FIX_SIZE)
        return PK_AERO_ERR_SECTION;   /* v2 五段齐备（FIX 段可空但必须在）*/
    /* v3 索引段：存在就必须是 4 B/项（缺席 n=0，查询安全退化为无结果）*/
    const pk_aero_section_t *idx4[] = {
        &db->sec_idx_apt_key, &db->sec_idx_apt_name,
        &db->sec_idx_nav_name, &db->sec_idx_fix_name,
        /* v4 展开索引也是 4 B/项 */
        &db->sec_idx_asp_cell, &db->sec_idx_awy_cell,
    };
    for (size_t i = 0; i < sizeof(idx4) / sizeof(idx4[0]); i++) {
        if (idx4[i]->n != 0 && idx4[i]->rec_size != PK_AERO_IDX_ENTRY_SIZE)
            return PK_AERO_ERR_SECTION;
    }
    /* v4 数据段：缺席（n=0，读 v3 文件）不算错，存在就必须是约定的记录长度 */
    if ((db->sec_airspace.n != 0 &&
         db->sec_airspace.rec_size != PK_AERO_AIRSPACE_SIZE) ||
        (db->sec_asp_vtx.n != 0 &&
         db->sec_asp_vtx.rec_size != PK_AERO_AIRSPACE_VTX_SIZE) ||
        (db->sec_airway.n != 0 &&
         db->sec_airway.rec_size != PK_AERO_AIRWAY_SIZE))
        return PK_AERO_ERR_SECTION;

    int rc = parse_index(db, &db->sec_airport, &db->apt_idx);
    if (rc != PK_AERO_OK) return rc;
    rc = parse_index(db, &db->sec_navaid, &db->nav_idx);
    if (rc != PK_AERO_OK) return rc;
    rc = parse_index(db, &db->sec_fix, &db->fix_idx);
    if (rc != PK_AERO_OK) return rc;
    /* v4 展开索引段的 index 区就是 v3 的稀疏格表，解析函数原样复用 */
    rc = parse_index(db, &db->sec_idx_asp_cell, &db->asp_cell_idx);
    if (rc != PK_AERO_OK) return rc;
    rc = parse_index(db, &db->sec_idx_awy_cell, &db->awy_cell_idx);
    if (rc != PK_AERO_OK) return rc;
    if (db->apt_idx.n_grid == 0 || db->nav_idx.n_grid == 0)
        return PK_AERO_ERR_SECTION;   /* airports/navaids 必须带网格索引 */
    return PK_AERO_OK;
}

void pk_aero_stats_reset(pk_aero_db_t *db)
{
    memset(&db->stats, 0, sizeof(db->stats));
}

/* ------------------------------------------------------------------ */
/* 网格                                                                */
/* ------------------------------------------------------------------ */

uint16_t pk_aero_grid_cell(double lat, double lon)
{
    /* 与生成端 grid_cell_u16 一致：floor 语义、行钳位 [0,179]、列环绕 */
    int row = (int)floor(lat) + 90;
    if (row < 0)   row = 0;
    if (row > 179) row = 179;
    int col = ((int)floor(lon) + 180) % 360;
    if (col < 0) col += 360;
    return (uint16_t)(row * 360 + col);
}

void pk_aero_grid_lookup(pk_aero_db_t *db, const pk_aero_index_t *gi,
                         uint16_t cell, uint32_t *first, uint32_t *count)
{
    *first = 0;
    *count = 0;
    const uint8_t *base = db->payload + gi->grid_off;
    uint32_t lo = 0, hi = gi->n_grid;
    while (lo < hi) {                       /* lower_bound，同 PkAeroReader */
        uint32_t mid = (lo + hi) / 2;
        db->stats.bsearch_steps++;
        if (rd_u16(base + (size_t)mid * 8) < cell) lo = mid + 1;
        else                                       hi = mid;
    }
    db->stats.grid_lookups++;
    if (lo < gi->n_grid) {
        const uint8_t *e = base + (size_t)lo * 8;
        if (rd_u16(e) == cell) {
            *first = rd_u32(e + 2);         /* 项内偏移 2：未对齐，逐字节 */
            *count = rd_u16(e + 6);
        }
    }
}

/* ------------------------------------------------------------------ */
/* ICAO 二分                                                            */
/* ------------------------------------------------------------------ */

int32_t pk_aero_airport_by_icao(pk_aero_db_t *db, const char *code)
{
    if (!code || code[0] == '\0') return -1;
    size_t n = strlen(code);
    if (n > 4) return -1;
    uint8_t want[4] = {0, 0, 0, 0};
    memcpy(want, code, n);

    const pk_aero_index_t *gi = &db->apt_idx;
    const uint8_t *second = db->payload + gi->second_off;
    const uint8_t *data   = db->payload + db->sec_airport.data_off;

    uint32_t lo = 0, hi = gi->n_second;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        db->stats.bsearch_steps++;
        uint32_t rec = rd_u32(second + (size_t)mid * 4);
        /* 机场记录偏移 8 起是 icao[4]；memcmp 无符号比较 == Python bytes 序 */
        if (memcmp(data + (size_t)rec * PK_AERO_AIRPORT_SIZE + 8,
                   want, 4) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < gi->n_second) {
        uint32_t rec = rd_u32(second + (size_t)lo * 4);
        if (memcmp(data + (size_t)rec * PK_AERO_AIRPORT_SIZE + 8,
                   want, 4) == 0)
            return (int32_t)rec;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* nearest：目标格 ±1 圈 9 格 → 候选 → 距离排序取前 N                    */
/* ------------------------------------------------------------------ */

/* 有序插入（升序，键 (key, idx) 保证确定性）；cap 满时淘汰最差 */
static void insert_near(pk_aero_near_t *arr, int *n, int cap,
                        uint32_t idx, double key)
{
    if (*n == cap) {
        const pk_aero_near_t *last = &arr[cap - 1];
        if (key > last->dist_nm ||
            (key == last->dist_nm && idx >= last->idx))
            return;
        (*n)--;
    }
    int i = *n;
    while (i > 0 && (arr[i - 1].dist_nm > key ||
                     (arr[i - 1].dist_nm == key && arr[i - 1].idx > idx))) {
        arr[i] = arr[i - 1];
        i--;
    }
    arr[i].idx     = idx;
    arr[i].dist_nm = key;
    arr[i].brg_deg = 0.0;
    (*n)++;
}

/* 等距柱状近似：平方“度距”（只用于排序比较，不换算单位）*/
static double approx_d2(double qlat, double qlon, double rlat, double rlon)
{
    double dlat = rlat - qlat;
    double dlon = rlon - qlon;
    if (dlon > 180.0)  dlon -= 360.0;      /* 反经线环绕 */
    if (dlon < -180.0) dlon += 360.0;
    double dx = dlon * cos((qlat + rlat) * 0.5 * (M_PI / 180.0));
    return dlat * dlat + dx * dx;
}

/* 通用 nearest：sec 的记录前 8 字节必须是 lat_e7/lon_e7（airport/navaid
 * 均满足）。返回写入 out 的条数。 */
static int nearest_generic(pk_aero_db_t *db, const pk_aero_section_t *sec,
                           const pk_aero_index_t *gi,
                           double lat, double lon,
                           pk_aero_near_t *out, int max,
                           pk_aero_dist_mode_t mode)
{
    if (max <= 0) return 0;
    if (max > PK_AERO_NEAR_MAX) max = PK_AERO_NEAR_MAX;

    const uint8_t *data = db->payload + sec->data_off;
    db->stats.candidates = 0;

    /* 近似模式工作区：多留 SLACK 个，降低粗排在 top-N 边界上的误杀 */
    pk_aero_near_t scratch[PK_AERO_NEAR_MAX + PK_AERO_APPROX_SLACK];
    int cap = (mode == PK_AERO_DIST_APPROX)
            ? max + PK_AERO_APPROX_SLACK : max;
    pk_aero_near_t *sel = (mode == PK_AERO_DIST_APPROX) ? scratch : out;
    int n_sel = 0;

    int row0 = (int)floor(lat) + 90;
    if (row0 < 0)   row0 = 0;
    if (row0 > 179) row0 = 179;
    int col0 = ((int)floor(lon) + 180) % 360;
    if (col0 < 0) col0 += 360;

    for (int dr = -1; dr <= 1; dr++) {
        int row = row0 + dr;
        if (row < 0 || row > 179) continue;   /* 极区不越界（无极点环绕）*/
        for (int dc = -1; dc <= 1; dc++) {
            int col = (col0 + dc + 360) % 360; /* 反经线列环绕 */
            uint32_t first, count;
            pk_aero_grid_lookup(db, gi, (uint16_t)(row * 360 + col),
                                &first, &count);
            for (uint32_t k = 0; k < count; k++) {
                uint32_t idx = first + k;
                const uint8_t *r = data + (size_t)idx * sec->rec_size;
                int32_t lat_e7 = rd_i32(r);
                int32_t lon_e7 = rd_i32(r + 4);
                if (lat_e7 == (int32_t)PK_AERO_COORD_NONE) continue;
                db->stats.candidates++;
                double rlat = lat_e7 / 1e7, rlon = lon_e7 / 1e7;
                double key;
                if (mode == PK_AERO_DIST_APPROX) {
                    key = approx_d2(lat, lon, rlat, rlon);
                    db->stats.approx_calcs++;
                } else {
                    geo_dist_brg(lat, lon, rlat, rlon, &key, NULL);
                    db->stats.dist_calcs++;
                }
                insert_near(sel, &n_sel, cap, idx, key);
            }
        }
    }

    if (mode == PK_AERO_DIST_APPROX) {
        /* 精算：只对粗排选出的 max+SLACK 个做 Haversine，重排取前 max */
        int n_out = 0;
        for (int i = 0; i < n_sel; i++) {
            const uint8_t *r = data + (size_t)sel[i].idx * sec->rec_size;
            double d;
            geo_dist_brg(lat, lon, rd_i32(r) / 1e7, rd_i32(r + 4) / 1e7,
                         &d, NULL);
            db->stats.dist_calcs++;
            insert_near(out, &n_out, max, sel[i].idx, d);
        }
        n_sel = n_out;
    }

    /* 只对最终结果补方位（固定 ≤max 次，不计入扫描工作量统计）*/
    for (int i = 0; i < n_sel; i++) {
        const uint8_t *r = data + (size_t)out[i].idx * sec->rec_size;
        geo_dist_brg(lat, lon, rd_i32(r) / 1e7, rd_i32(r + 4) / 1e7,
                     NULL, &out[i].brg_deg);
    }
    return n_sel;
}

int pk_aero_nearest_airports(pk_aero_db_t *db, double lat, double lon,
                             pk_aero_near_t *out, int max,
                             pk_aero_dist_mode_t mode)
{
    return nearest_generic(db, &db->sec_airport, &db->apt_idx,
                           lat, lon, out, max, mode);
}

int pk_aero_nearest_navaids(pk_aero_db_t *db, double lat, double lon,
                            pk_aero_near_t *out, int max,
                            pk_aero_dist_mode_t mode)
{
    return nearest_generic(db, &db->sec_navaid, &db->nav_idx,
                           lat, lon, out, max, mode);
}

int pk_aero_nearest_fixes(pk_aero_db_t *db, double lat, double lon,
                          pk_aero_near_t *out, int max,
                          pk_aero_dist_mode_t mode)
{
    /* FIX 记录前 8 字节同样是 lat_e7/lon_e7，直接复用通用 nearest */
    return nearest_generic(db, &db->sec_fix, &db->fix_idx,
                           lat, lon, out, max, mode);
}

/* ------------------------------------------------------------------ */
/* FIX ident 二分（v2）：同名区间左端 + 线性扫                          */
/* ------------------------------------------------------------------ */

/* ident[6] 精确查找的公共骨架：second 索引按记录内 ident 定长排序，
 * 二分到同名区间左端后线性扫。返回同名总条数（可能 > max）。 */
static int norm_query(const char *s, uint8_t *out, size_t max_len);

static int ident_exact(pk_aero_db_t *db, const pk_aero_index_t *gi,
                       const pk_aero_section_t *sec, const char *ident,
                       uint32_t *out, int max)
{
    if (!db || max < 0) return PK_AERO_ERR_ARG;
    /* 查询串统一 toupper（索引里的代码类字段本应全大写；上游极少数没大写的
     * 记录因此按码搜不到，属数据问题，见 README「已知数据问题」）*/
    uint8_t want[6] = {0, 0, 0, 0, 0, 0};
    if (norm_query(ident, want, 6) <= 0) return PK_AERO_ERR_ARG;
    /* 索引缺席就是"这版数据查不了"（v2 卡的导航台第二索引是空表），
     * 返回 0 = 无结果；下面的二分本来也走不进去，这里只是把意图写明。 */
    if (gi->n_second == 0) return 0;

    const uint8_t *second = db->payload + gi->second_off;
    const uint8_t *data   = db->payload + sec->data_off;

    uint32_t lo = 0, hi = gi->n_second;
    while (lo < hi) {                       /* lower_bound，同 ICAO 二分 */
        uint32_t mid = (lo + hi) / 2;
        db->stats.bsearch_steps++;
        uint32_t rec = rd_u32(second + (size_t)mid * 4);
        /* 记录偏移 8 起是 ident[6]；memcmp 无符号比较 == Python bytes 序 */
        if (memcmp(data + (size_t)rec * sec->rec_size + 8, want, 6) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    int found = 0;
    while (lo < gi->n_second) {             /* 线性扫同名区间 */
        uint32_t rec = rd_u32(second + (size_t)lo * 4);
        if (memcmp(data + (size_t)rec * sec->rec_size + 8, want, 6) != 0)
            break;
        if (found < max) out[found] = rec;
        found++;
        lo++;
    }
    return found;
}

int pk_aero_fix_by_ident(pk_aero_db_t *db, const char *ident,
                         uint32_t *out, int max)
{
    if (!db) return PK_AERO_ERR_ARG;
    return ident_exact(db, &db->fix_idx, &db->sec_fix, ident, out, max);
}

/* v3 修 G2：导航台段第二索引在 v2 里是空表，v3 填成 ident 排序表 */
int pk_aero_navaid_by_ident(pk_aero_db_t *db, const char *ident,
                            uint32_t *out, int max)
{
    if (!db) return PK_AERO_ERR_ARG;
    return ident_exact(db, &db->nav_idx, &db->sec_navaid, ident, out, max);
}

/* ------------------------------------------------------------------ */
/* v3 搜索：前缀枚举                                                    */
/* ------------------------------------------------------------------ */

/* 查询串归一：toupper + ASCII 校验 + 长度上限。
 * 成功返回长度，失败返回 -1（非 ASCII / 超长 / 空串 = 无结果，不是错误）。
 * 与 Python PkAeroReader._norm_query 逐条对齐：str.upper() 在纯 ASCII 上
 * 就是 a–z → A–Z，其余原样。 */
static int norm_query(const char *s, uint8_t *out, size_t max_len)
{
    if (!s) return -1;
    size_t n = 0;
    while (s[n] != '\0') {
        if (n >= max_len) return -1;
        unsigned char c = (unsigned char)s[n];
        if (c >= 0x80) return -1;
        out[n] = (uint8_t)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
        n++;
    }
    return n == 0 ? -1 : (int)n;
}

/* 索引项（记录下标）→ 定长搜索键。klen 最多 6。 */
typedef void (*pk_key_fn_t)(const pk_aero_db_t *db, uint32_t rec, uint8_t *out);

/* 机场的键是**派生**的：icao 非空用 icao[4]，否则 iata[3] 右填充到 4 B。
 * 生成端 export_box_bin.py 建 SEC_IDX_AIRPORT_KEY 时用的就是这个规则。 */
static void apt_key4(const pk_aero_db_t *db, uint32_t rec, uint8_t *out)
{
    const uint8_t *r = db->payload + db->sec_airport.data_off
                     + (size_t)rec * PK_AERO_AIRPORT_SIZE;
    if (r[8] != 0) {
        memcpy(out, r + 8, 4);
    } else {
        memcpy(out, r + 12, 3);
        out[3] = 0;
    }
}

static void nav_ident6(const pk_aero_db_t *db, uint32_t rec, uint8_t *out)
{
    memcpy(out, db->payload + db->sec_navaid.data_off
                + (size_t)rec * PK_AERO_NAVAID_SIZE + 8, 6);
}

static void fix_ident6(const pk_aero_db_t *db, uint32_t rec, uint8_t *out)
{
    memcpy(out, db->payload + db->sec_fix.data_off
                + (size_t)rec * PK_AERO_FIX_SIZE + 8, 6);
}

/* 索引表第 i 项 → 记录下标。table==NULL 表示**恒等排列**（v4 航路：记录
 * 本身就按 designator 排序，Python 那边传的是 range(airway_count)）。*/
static inline uint32_t idx_at(const uint8_t *table, uint32_t i)
{
    return table ? rd_u32(table + (size_t)i * 4) : i;
}

/* 定长键排序表上的前缀枚举。
 * 查询键右填 '\0'，而 '\0' 小于任何可打印 ASCII，所以 lower_bound 恰好落在
 * 前缀区间左端；随后线性扫到前缀不再匹配 / 写满 max 为止。 */
static int prefix_generic(pk_aero_db_t *db, const uint8_t *table, uint32_t n,
                          pk_key_fn_t keyf, size_t klen,
                          const uint8_t *want, size_t plen,
                          uint32_t *out, int max)
{
    uint8_t padded[6] = {0, 0, 0, 0, 0, 0};
    uint8_t k[6];
    memcpy(padded, want, plen);

    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        db->stats.bsearch_steps++;
        db->stats.rev_derefs++;          /* 每步一次随机访存回读记录 */
        keyf(db, idx_at(table, mid), k);
        if (memcmp(k, padded, klen) < 0) lo = mid + 1;
        else                             hi = mid;
    }
    int found = 0;
    while (lo < n && found < max) {
        uint32_t rec = idx_at(table, lo);
        db->stats.rev_derefs++;
        db->stats.prefix_scanned++;
        keyf(db, rec, k);
        if (memcmp(k, want, plen) != 0) break;
        out[found++] = rec;
        lo++;
    }
    return found;
}

int pk_aero_airports_by_prefix(pk_aero_db_t *db, const char *prefix,
                               uint32_t *out, int max)
{
    if (!db || !out || max <= 0) return 0;
    uint8_t want[4];
    int plen = norm_query(prefix, want, sizeof want);
    if (plen <= 0) return 0;
    const pk_aero_section_t *s = &db->sec_idx_apt_key;
    if (s->n == 0) return 0;
    return prefix_generic(db, db->payload + s->data_off, s->n,
                          apt_key4, 4, want, (size_t)plen, out, max);
}

int pk_aero_navaids_by_prefix(pk_aero_db_t *db, const char *prefix,
                              uint32_t *out, int max)
{
    if (!db || !out || max <= 0) return 0;
    uint8_t want[6];
    int plen = norm_query(prefix, want, sizeof want);
    if (plen <= 0) return 0;
    if (db->nav_idx.n_second == 0) return 0;
    return prefix_generic(db, db->payload + db->nav_idx.second_off,
                          db->nav_idx.n_second, nav_ident6, 6,
                          want, (size_t)plen, out, max);
}

int pk_aero_fixes_by_prefix(pk_aero_db_t *db, const char *prefix,
                            uint32_t *out, int max)
{
    if (!db || !out || max <= 0) return 0;
    uint8_t want[6];
    int plen = norm_query(prefix, want, sizeof want);
    if (plen <= 0) return 0;
    if (db->fix_idx.n_second == 0) return 0;
    return prefix_generic(db, db->payload + db->fix_idx.second_off,
                          db->fix_idx.n_second, fix_ident6, 6,
                          want, (size_t)plen, out, max);
}

/* ------------------------------------------------------------------ */
/* v3 搜索：子串（顺扫字符串池 + 名称反向索引二分）                      */
/* ------------------------------------------------------------------ */

/* 查询串上限：池里最长的名称约 90 字符，256 足够；更长的查询必然无结果，
 * 直接返回 0 与"扫一遍全不匹配"等价。 */
#define PK_AERO_QUERY_MAX 256

/* 大小写不敏感的子串判定。needle 已由 norm_query 归一成大写。 */
static bool ci_contains(const char *hay, const uint8_t *needle, size_t nlen)
{
    for (size_t i = 0; hay[i] != '\0'; i++) {
        size_t j = 0;
        while (j < nlen) {
            unsigned char c = (unsigned char)hay[i + j];
            if (c == '\0') return false;    /* 剩余长度不够，后面更短 */
            if (((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c) != needle[j])
                break;
            j++;
        }
        if (j == nlen) return true;
    }
    return false;
}

/* 反向索引项 → 它所指向的池偏移（键靠回读记录现取，故表里只存 4 B 下标）*/
static uint32_t rev_key_of(const pk_aero_db_t *db, uint8_t type, uint32_t entry)
{
    uint32_t rec = entry & PK_AERO_REC_IDX_MASK;
    if (type == PK_AERO_SEC_AIRPORTS) {
        const uint8_t *r = db->payload + db->sec_airport.data_off
                         + (size_t)rec * PK_AERO_AIRPORT_SIZE;
        /* bit31=1 → 这一项索引的是 city_off（记录内 34），否则 name_off（31）*/
        return rd_u24be(r + ((entry & PK_AERO_NAME_IDX_CITY_FLAG) ? 34 : 31));
    }
    if (type == PK_AERO_SEC_NAVAIDS) {
        return rd_u24be(db->payload + db->sec_navaid.data_off
                        + (size_t)rec * PK_AERO_NAVAID_SIZE + 21);
    }
    return rd_u24be(db->payload + db->sec_fix.data_off
                    + (size_t)rec * PK_AERO_FIX_SIZE + 17);
}

/* 池偏移 → 反向索引二分 → 把引用它的记录逐条写进 out（去重）。
 * 返回 true 表示 out 已满，调用方应立即收工。 */
static bool rev_emit(pk_aero_db_t *db, uint8_t type,
                     const pk_aero_section_t *idx_sec, uint32_t pool_off,
                     pk_aero_hit_t *out, int *n, int max)
{
    const uint8_t *tbl = db->payload + idx_sec->data_off;
    uint32_t lo = 0, hi = idx_sec->n;
    db->stats.rev_lookups++;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        db->stats.bsearch_steps++;
        db->stats.rev_derefs++;
        if (rev_key_of(db, type, rd_u32(tbl + (size_t)mid * 4)) < pool_off)
            lo = mid + 1;
        else
            hi = mid;
    }
    /* 池是 intern 去重的：同一个偏移可能被多条记录引用（同名机场），
     * 也可能被同一条记录的 name 与 city 同时引用 → 线性扫 + 去重 */
    while (lo < idx_sec->n) {
        uint32_t entry = rd_u32(tbl + (size_t)lo * 4);
        db->stats.rev_derefs++;
        if (rev_key_of(db, type, entry) != pool_off) break;
        uint32_t rec = entry & PK_AERO_REC_IDX_MASK;
        bool dup = false;
        for (int i = 0; i < *n; i++) {
            if (out[i].type == type && out[i].idx == rec) { dup = true; break; }
        }
        if (!dup) {
            out[*n].type = type;
            out[*n].idx  = rec;
            (*n)++;
            if (*n >= max) return true;
        }
        lo++;
    }
    return false;
}

/* 一段池的扫描结果。 */
typedef enum {
    SCAN_BUDGET_OUT = 0,   /* 预算用完，本段还没扫完（*off 是续扫点）*/
    SCAN_SECTION_DONE,     /* 本段扫尽 */
    SCAN_FULL,             /* out 写满 max，整个搜索可以收工 */
} scan_res_t;

/* 顺扫一段字符串池的 [*off, …) 区间，最多扫 budget 字节（budget==0 = 不限）。
 * *off 由调用方跨调用持有；传 0 表示从本段开头扫起。 */
static scan_res_t scan_pool_range(pk_aero_db_t *db, uint8_t type,
                                  const pk_aero_section_t *sec,
                                  const pk_aero_section_t *idx_sec,
                                  const uint8_t *q, size_t qlen,
                                  pk_aero_hit_t *out, int *n, int max,
                                  uint32_t *off, uint32_t budget)
{
    if (sec->strings_off == 0 || sec->strings_size <= 1 || idx_sec->n == 0)
        return SCAN_SECTION_DONE;
    const char *base = (const char *)(db->payload + sec->strings_off);
    if (*off < 1) *off = 1;              /* 偏移 0 是"无"哨兵（单个 NUL）*/
    uint32_t used = 0;
    while (*off < sec->strings_size) {
        const char *s = base + *off;
        size_t len = strlen(s);
        db->stats.pool_bytes += (uint64_t)len + 1;
        db->stats.pool_strings++;
        if (ci_contains(s, q, qlen)) {
            db->stats.pool_hits++;
            if (rev_emit(db, type, idx_sec, *off, out, n, max)) return SCAN_FULL;
        }
        *off += (uint32_t)len + 1;
        used += (uint32_t)len + 1;
        /* 预算判定放在**推进游标之后**：下次进来从下一条串扫起，
         * 不会把同一条串数两遍（那会让去重之外的 stats 计数虚高）。 */
        if (budget != 0 && used >= budget) return SCAN_BUDGET_OUT;
    }
    return SCAN_SECTION_DONE;
}

bool pk_aero_search_substring_step(pk_aero_db_t *db, const char *query,
                                   pk_aero_hit_t *out, int max,
                                   pk_aero_search_cursor_t *cur,
                                   uint32_t budget_bytes)
{
    if (!db || !out || max <= 0 || !cur) return true;
    uint8_t q[PK_AERO_QUERY_MAX];
    int qlen = norm_query(query, q, sizeof q);
    if (qlen <= 0) { cur->stage = 3; return true; }

    while (cur->stage < 3) {
        /* 段顺序即结果顺序的一部分，必须与 Python SEARCH_SECTIONS 一致 */
        uint8_t type;
        const pk_aero_section_t *sec, *idx;
        if (cur->stage == 0) {
            type = PK_AERO_SEC_AIRPORTS;
            sec = &db->sec_airport; idx = &db->sec_idx_apt_name;
        } else if (cur->stage == 1) {
            type = PK_AERO_SEC_NAVAIDS;
            sec = &db->sec_navaid;  idx = &db->sec_idx_nav_name;
        } else {
            type = PK_AERO_SEC_WAYPOINTS_FIX;
            sec = &db->sec_fix;     idx = &db->sec_idx_fix_name;
        }
        scan_res_t r = scan_pool_range(db, type, sec, idx, q, (size_t)qlen,
                                       out, &cur->n, max, &cur->off,
                                       budget_bytes);
        if (r == SCAN_FULL)       { cur->stage = 3; return true; }
        if (r == SCAN_BUDGET_OUT) return false;
        cur->stage++;
        cur->off = 0;
        /* 分段模式下换段也要让渡一次：否则"这一段刚好只剩几十字节"时，
         * 单次调用会连着把下一整段扫掉，预算就形同虚设。 */
        if (budget_bytes != 0 && cur->stage < 3) return false;
    }
    return true;
}

int pk_aero_search_substring(pk_aero_db_t *db, const char *query,
                             pk_aero_hit_t *out, int max)
{
    if (!db || !out || max <= 0) return 0;
    /* 不限预算 = 一口气扫完，行为与分段版逐字节一致（同一份 scan_pool_range）。*/
    pk_aero_search_cursor_t cur = { 0, 0, 0 };
    while (!pk_aero_search_substring_step(db, query, out, max, &cur, 0)) { }
    return cur.n;
}

/* ------------------------------------------------------------------ */
/* 记录解码                                                            */
/* ------------------------------------------------------------------ */

/* 字符串池取串：off 0 = 无 → ""。返回指向 payload 的 const 指针。 */
static const char *pool_str(const pk_aero_db_t *db,
                            const pk_aero_section_t *sec, uint32_t off)
{
    if (off == PK_AERO_STR_NONE || sec->strings_off == 0) return "";
    uint64_t at = (uint64_t)sec->strings_off + off;
    if (at >= db->payload_len) return "";
    return (const char *)(db->payload + at);
}

bool pk_aero_airport_get(const pk_aero_db_t *db, uint32_t idx,
                         pk_aero_airport_t *out)
{
    const pk_aero_section_t *s = &db->sec_airport;
    if (idx >= s->n) return false;
    const uint8_t *r = db->payload + s->data_off
                     + (size_t)idx * PK_AERO_AIRPORT_SIZE;
    out->lat_e7 = rd_i32(r);
    out->lon_e7 = rd_i32(r + 4);
    out->lat = out->lat_e7 / 1e7;
    out->lon = out->lon_e7 / 1e7;
    fix_str(out->icao,    r + 8,  4);
    fix_str(out->iata,    r + 12, 3);
    out->type           = r[15];
    out->elev_ft        = rd_i16(r + 16);
    out->longest_rwy_ft = rd_u16(r + 18);
    out->ctrl           = r[20];
    fix_str(out->country, r + 21, 2);
    /* v2：first 指针 24-bit 大端（同字符串池偏移手法）*/
    out->rwy_first  = rd_u24be(r + 23);
    out->rwy_count  = r[26];
    out->freq_first = rd_u24be(r + 27);
    out->freq_count = r[30];
    out->name = pool_str(db, s, rd_u24be(r + 31));
    out->city = pool_str(db, s, rd_u24be(r + 34));
    out->grid_cell = rd_u16(r + 37);
    return true;
}

bool pk_aero_rwy_dir_get(const pk_aero_db_t *db, uint32_t idx,
                         pk_aero_rwy_dir_t *out)
{
    const pk_aero_section_t *s = &db->sec_rwy;
    if (idx >= s->n) return false;
    const uint8_t *r = db->payload + s->data_off
                     + (size_t)idx * PK_AERO_RWY_DIR_SIZE;
    out->lat_e7 = rd_i32(r);
    out->lon_e7 = rd_i32(r + 4);
    out->has_coord = (out->lat_e7 != (int32_t)PK_AERO_COORD_NONE &&
                      out->lon_e7 != (int32_t)PK_AERO_COORD_NONE);
    out->lat = out->has_coord ? out->lat_e7 / 1e7 : 0.0;
    out->lon = out->has_coord ? out->lon_e7 / 1e7 : 0.0;
    fix_str(out->designator, r + 8, 4);
    out->mag_bearing_dd = rd_u16(r + 12);
    out->has_bearing    = (out->mag_bearing_dd != PK_AERO_BEARING_NONE);
    out->length_ft      = rd_u16(r + 14);
    out->width_ft       = rd_u16(r + 16);
    out->surface        = r[18];
    out->thr_elev_ft    = rd_i16(r + 19);
    return true;
}

bool pk_aero_freq_get(const pk_aero_db_t *db, uint32_t idx,
                      pk_aero_freq_t *out)
{
    const pk_aero_section_t *s = &db->sec_freq;
    if (idx >= s->n) return false;
    const uint8_t *r = db->payload + s->data_off
                     + (size_t)idx * PK_AERO_FREQ_SIZE;
    out->freq_khz = rd_u32(r);
    out->service  = r[4];
    out->callsign = pool_str(db, s, rd_u24be(r + 5));
    return true;
}

bool pk_aero_navaid_get(const pk_aero_db_t *db, uint32_t idx,
                        pk_aero_navaid_t *out)
{
    const pk_aero_section_t *s = &db->sec_navaid;
    if (idx >= s->n) return false;
    const uint8_t *r = db->payload + s->data_off
                     + (size_t)idx * PK_AERO_NAVAID_SIZE;
    out->lat_e7 = rd_i32(r);
    out->lon_e7 = rd_i32(r + 4);
    out->lat = out->lat_e7 / 1e7;
    out->lon = out->lon_e7 / 1e7;
    fix_str(out->ident, r + 8, 6);
    out->type     = r[14];
    out->elev_ft  = rd_i16(r + 15);   /* 奇偏移：必须逐字节 */
    out->freq_khz = rd_u32(r + 17);   /* 同上 */
    out->name = pool_str(db, s, rd_u24be(r + 21));
    out->grid_cell = rd_u16(r + 24);
    return true;
}

bool pk_aero_fix_get(const pk_aero_db_t *db, uint32_t idx,
                     pk_aero_fix_t *out)
{
    const pk_aero_section_t *s = &db->sec_fix;
    if (idx >= s->n) return false;
    const uint8_t *r = db->payload + s->data_off
                     + (size_t)idx * PK_AERO_FIX_SIZE;
    out->lat_e7 = rd_i32(r);
    out->lon_e7 = rd_i32(r + 4);
    out->lat = out->lat_e7 / 1e7;
    out->lon = out->lon_e7 / 1e7;
    fix_str(out->ident, r + 8, 6);
    out->scope     = r[14];
    out->grid_cell = rd_u16(r + 15);   /* 奇偏移：必须逐字节 */
    out->name = pool_str(db, s, rd_u24be(r + 17));
    return true;
}

bool pk_aero_airport_runways(const pk_aero_db_t *db, uint32_t apt_idx,
                             uint32_t *first, uint32_t *count)
{
    pk_aero_airport_t a;
    if (!pk_aero_airport_get(db, apt_idx, &a)) return false;
    *first = a.rwy_first;
    *count = a.rwy_count;
    return (uint64_t)a.rwy_first + a.rwy_count <= db->sec_rwy.n;
}

bool pk_aero_airport_freqs(const pk_aero_db_t *db, uint32_t apt_idx,
                           uint32_t *first, uint32_t *count)
{
    pk_aero_airport_t a;
    if (!pk_aero_airport_get(db, apt_idx, &a)) return false;
    *first = a.freq_first;
    *count = a.freq_count;
    return (uint64_t)a.freq_first + a.freq_count <= db->sec_freq.n;
}

/* ================================================================== */
/* v4：空域记录 + 几何还原                                              */
/* ================================================================== */

bool pk_aero_airspace_get(const pk_aero_db_t *db, uint32_t idx,
                          pk_aero_airspace_t *out)
{
    if (!db || !out) return false;
    const pk_aero_section_t *s = &db->sec_airspace;
    if (idx >= s->n) return false;          /* 段缺席时 n=0 → 一律 false */
    const uint8_t *r = db->payload + s->data_off
                     + (size_t)idx * PK_AERO_AIRSPACE_SIZE;
    out->min_lat_e7 = rd_i32(r);
    out->min_lon_e7 = rd_i32(r + 4);
    out->max_lat_e7 = rd_i32(r + 8);
    out->max_lon_e7 = rd_i32(r + 12);
    out->min_lat = out->min_lat_e7 / 1e7;
    out->min_lon = out->min_lon_e7 / 1e7;
    out->max_lat = out->max_lat_e7 / 1e7;
    out->max_lon = out->max_lon_e7 / 1e7;
    out->type      = r[16];
    out->cls       = r[17];
    out->lower_ref = r[18];
    out->upper_ref = r[19];
    out->lower_100ft = rd_i16(r + 20);
    out->upper_100ft = rd_i16(r + 22);
    out->has_lower = (out->lower_100ft != (int16_t)PK_AERO_ALT_NONE);
    out->has_upper = (out->upper_100ft != (int16_t)PK_AERO_ALT_NONE);
    out->name       = pool_str(db, s, rd_u24be(r + 24));
    out->designator = pool_str(db, s, rd_u24be(r + 27));
    /* 顶点池下标同样是 24-bit 大端（照抄字符串池偏移手法）*/
    out->vtx_first_fine   = rd_u24be(r + 30);
    out->vtx_first_coarse = rd_u24be(r + 33);
    out->vtx_count_fine   = rd_u16(r + 36);
    out->vtx_count_coarse = rd_u16(r + 38);
    out->grid_cell        = rd_u16(r + 40);
    return true;
}

int pk_aero_airspace_ring(const pk_aero_db_t *db, uint32_t idx, bool coarse,
                          pk_aero_lonlat_t *out, int max)
{
    if (!db || !out || max < 0) return PK_AERO_ERR_ARG;
    pk_aero_airspace_t a;
    if (!pk_aero_airspace_get(db, idx, &a)) return PK_AERO_ERR_ARG;

    uint32_t first = coarse ? a.vtx_first_coarse : a.vtx_first_fine;
    uint32_t count = coarse ? a.vtx_count_coarse : a.vtx_count_fine;
    if ((uint64_t)first + count > db->sec_asp_vtx.n)
        return PK_AERO_ERR_TRUNCATED;

    /* 跨度为 0 的那一维（退化成线的空域）量化值恒 0，还原恒等于 min，误差 0 */
    double span_lat = a.max_lat - a.min_lat;
    double span_lon = a.max_lon - a.min_lon;
    const uint8_t *base = db->payload + db->sec_asp_vtx.data_off;
    int n = 0;
    for (uint32_t k = 0; k < count && n < max; k++) {
        const uint8_t *v = base
                         + (size_t)(first + k) * PK_AERO_AIRSPACE_VTX_SIZE;
        /* 运算次序与 Python 逐字对齐：span * q 先算，再除满量程 → 逐位一致 */
        double xq = (double)rd_u16(v);
        double yq = (double)rd_u16(v + 2);
        out[n].lon = a.min_lon + span_lon * xq / PK_AERO_VTX_SCALE_MAX;
        out[n].lat = a.min_lat + span_lat * yq / PK_AERO_VTX_SCALE_MAX;
        n++;
    }
    return n;
}

/* ================================================================== */
/* v4：航路记录                                                        */
/* ================================================================== */

bool pk_aero_airway_get(const pk_aero_db_t *db, uint32_t idx,
                        pk_aero_airway_t *out)
{
    if (!db || !out) return false;
    const pk_aero_section_t *s = &db->sec_airway;
    if (idx >= s->n) return false;
    const uint8_t *r = db->payload + s->data_off
                     + (size_t)idx * PK_AERO_AIRWAY_SIZE;
    out->start_lat_e7 = rd_i32(r);
    out->start_lon_e7 = rd_i32(r + 4);
    out->end_lat_e7   = rd_i32(r + 8);
    out->end_lon_e7   = rd_i32(r + 12);
    out->start_lat = out->start_lat_e7 / 1e7;
    out->start_lon = out->start_lon_e7 / 1e7;
    out->end_lat   = out->end_lat_e7   / 1e7;
    out->end_lon   = out->end_lon_e7   / 1e7;
    fix_str(out->designator,  r + 16, 6);
    fix_str(out->start_ident, r + 22, 6);
    fix_str(out->end_ident,   r + 28, 6);
    out->type  = r[34];
    out->level = r[35];
    out->mag_track_dd = rd_u16(r + 36);
    out->dist_dnm     = rd_u16(r + 38);
    out->has_mag_track = (out->mag_track_dd != PK_AERO_TRACK_NONE);
    out->has_distance  = (out->dist_dnm     != PK_AERO_DIST_NONE);
    out->min_alt_100ft = rd_i16(r + 40);
    out->max_alt_100ft = rd_i16(r + 42);
    out->has_min_alt = (out->min_alt_100ft != (int16_t)PK_AERO_ALT_NONE);
    out->has_max_alt = (out->max_alt_100ft != (int16_t)PK_AERO_ALT_NONE);
    out->seq       = rd_u16(r + 44);
    out->direction = r[46];
    return true;
}

void pk_aero_airway_lon_span(double start_lon, double end_lon,
                             double *lo_lon, double *hi_lon)
{
    /* 直接对两端点取 min/max 是错的：SYA(174.06°E) → ADK(176.67°W) 这类
     * 阿留申航段只有一两百海里长，短弧从 174° 往东跨过 180° 就到了，
     * 而 min/max 会给出 [-176.67, 174.06] 这个横跨大半个地球的假区间
     * —— 既会被亚洲的视野误查出来（假阳），又会在 179° 附近的视野里漏掉
     * （假阴）。生成端 _cell_expand_segment 建索引用的就是短弧，精筛必须
     * 同口径。返回值可能越出 ±180（本例是 174.06 / 183.33），**不夹回**，
     * 比较时由 lon_overlap 按 ±360 平移。 */
    double dlon = end_lon - start_lon;
    if (dlon > 180.0)       dlon -= 360.0;
    else if (dlon < -180.0) dlon += 360.0;
    double a = start_lon, b = start_lon + dlon;
    if (a <= b) { *lo_lon = a; *hi_lon = b; }
    else        { *lo_lon = b; *hi_lon = a; }
}

/* 航路记录**本身**按 (designator, seq) 排序 → designator 二分不需要索引表 */
static void awy_desig6(const pk_aero_db_t *db, uint32_t rec, uint8_t *out)
{
    memcpy(out, db->payload + db->sec_airway.data_off
                + (size_t)rec * PK_AERO_AIRWAY_SIZE + 16, 6);
}

int pk_aero_find_airways_by_designator(pk_aero_db_t *db, const char *name,
                                       uint32_t *out, int max)
{
    if (!db || !out || max < 0) return PK_AERO_ERR_ARG;
    uint8_t want[6] = {0, 0, 0, 0, 0, 0};
    if (norm_query(name, want, 6) <= 0) return 0;   /* 非 ASCII/超长 = 无结果 */
    uint32_t n = db->sec_airway.n;
    if (n == 0) return 0;                            /* v3 文件：段缺席 */

    const uint8_t *data = db->payload + db->sec_airway.data_off;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {                                /* lower_bound */
        uint32_t mid = (lo + hi) / 2;
        db->stats.bsearch_steps++;
        db->stats.rev_derefs++;
        if (memcmp(data + (size_t)mid * PK_AERO_AIRWAY_SIZE + 16, want, 6) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    int found = 0;
    while (lo < n) {                                 /* 线性扫同名区间 */
        db->stats.rev_derefs++;
        if (memcmp(data + (size_t)lo * PK_AERO_AIRWAY_SIZE + 16, want, 6) != 0)
            break;
        if (found < max) out[found] = lo;
        found++;
        lo++;
    }
    return found;
}

int pk_aero_find_airways_by_prefix(pk_aero_db_t *db, const char *prefix,
                                   uint32_t *out, int max)
{
    if (!db || !out || max <= 0) return 0;
    uint8_t want[6];
    int plen = norm_query(prefix, want, sizeof want);
    if (plen <= 0) return 0;
    if (db->sec_airway.n == 0) return 0;
    /* table=NULL：恒等排列（Python 传的是 range(airway_count)）*/
    return prefix_generic(db, NULL, db->sec_airway.n, awy_desig6, 6,
                          want, (size_t)plen, out, max);
}

/* ================================================================== */
/* v4：格 / bbox 空间查询                                               */
/* ================================================================== */

/* 展开索引段：cell → 记录下标列表（data 是 u32 表，index 是稀疏格表）。
 * 表内已按 (cell, 记录下标) 排序，所以同一格内的下标天然升序。 */
static int cell_expansion(pk_aero_db_t *db, const pk_aero_section_t *sec,
                          const pk_aero_index_t *gi, uint16_t cell,
                          uint32_t *out, int max)
{
    if (max <= 0 || sec->n == 0 || gi->n_grid == 0) return 0;
    uint32_t first = 0, count = 0;
    pk_aero_grid_lookup(db, gi, cell, &first, &count);
    if (count == 0) return 0;
    if ((uint64_t)first + count > sec->n) return 0;   /* 越界表：当空 */
    const uint8_t *tbl = db->payload + sec->data_off;
    int n = 0;
    for (uint32_t k = 0; k < count && n < max; k++) {
        out[n++] = rd_u32(tbl + (size_t)(first + k) * PK_AERO_IDX_ENTRY_SIZE);
        db->stats.cell_candidates++;
    }
    return n;
}

/* 有序去重插入：out[] 始终保持"记录下标升序 + 互异"，满 max 时挤掉当前
 * 最大的那个。这就是 bbox 查询的**全部**工作区 —— 零额外内存（不用位图、
 * 不用候选缓冲），代价是每次成功插入 O(max) 次搬移。
 * 语义等价于 Python "候选全收进 set → sorted() → 精筛 → 取前 max"：
 * 两边都只保留通过精筛的、最小的 max 个下标，且升序。 */
static void insert_sorted_uniq(uint32_t *out, int *n, int max, uint32_t idx)
{
    int lo = 0, hi = *n;
    while (lo < hi) {                       /* lower_bound */
        int mid = (lo + hi) / 2;
        if (out[mid] < idx) lo = mid + 1;
        else                hi = mid;
    }
    if (lo < *n && out[lo] == idx) return;  /* 已在集合里（多格命中同一条）*/
    if (*n == max) {
        if (lo == max) return;              /* 比现有最大的还大 → 丢弃 */
        (*n)--;                             /* 挤掉当前最大的那个 */
    }
    for (int i = *n; i > lo; i--) out[i] = out[i - 1];
    out[lo] = idx;
    (*n)++;
}

/* 候选记录的 bbox 相交精筛（比较全用 double，与 Python 逐项同构）*/
typedef bool (*pk_bbox_fn_t)(const pk_aero_db_t *db, uint32_t rec,
                             double qmin_lat, double qmin_lon,
                             double qmax_lat, double qmax_lon);

static bool asp_bbox_hit(const pk_aero_db_t *db, uint32_t rec,
                         double qmin_lat, double qmin_lon,
                         double qmax_lat, double qmax_lon)
{
    if (rec >= db->sec_airspace.n) return false;
    const uint8_t *r = db->payload + db->sec_airspace.data_off
                     + (size_t)rec * PK_AERO_AIRSPACE_SIZE;
    double a_min_lat = rd_i32(r)      / 1e7;
    double a_min_lon = rd_i32(r + 4)  / 1e7;
    double a_max_lat = rd_i32(r + 8)  / 1e7;
    double a_max_lon = rd_i32(r + 12) / 1e7;
    return a_min_lat <= qmax_lat && a_max_lat >= qmin_lat
        && a_min_lon <= qmax_lon && a_max_lon >= qmin_lon;
}

/* 经度区间相交：允许把被测区间按 ±360 平移去够查询区间
 * （同 PkAeroReader._lon_overlap）*/
static bool lon_overlap(double a0, double a1, double q0, double q1)
{
    static const double shift[3] = {-360.0, 0.0, 360.0};
    for (int i = 0; i < 3; i++) {
        if (a0 + shift[i] <= q1 && a1 + shift[i] >= q0) return true;
    }
    return false;
}

static bool awy_bbox_hit(const pk_aero_db_t *db, uint32_t rec,
                         double qmin_lat, double qmin_lon,
                         double qmax_lat, double qmax_lon)
{
    if (rec >= db->sec_airway.n) return false;
    const uint8_t *r = db->payload + db->sec_airway.data_off
                     + (size_t)rec * PK_AERO_AIRWAY_SIZE;
    double s_lat = rd_i32(r)      / 1e7;
    double s_lon = rd_i32(r + 4)  / 1e7;
    double e_lat = rd_i32(r + 8)  / 1e7;
    double e_lon = rd_i32(r + 12) / 1e7;
    /* 段自身 bbox = 两端点构成（线段与矩形的真实相交留给绘制时裁剪）*/
    double lo_lat = s_lat < e_lat ? s_lat : e_lat;
    double hi_lat = s_lat < e_lat ? e_lat : s_lat;
    if (lo_lat > qmax_lat || hi_lat < qmin_lat) return false;
    double lo_lon, hi_lon;
    pk_aero_airway_lon_span(s_lon, e_lon, &lo_lon, &hi_lon);
    return lon_overlap(lo_lon, hi_lon, qmin_lon, qmax_lon);
}

/* 纬度 → 行号：floor + 钳位 [0,179]（同生成端 _cell_expand_bbox）。
 * 先在 double 域钳位再取整，任意有限值都不会撞上 int 溢出的未定义行为。 */
static int bbox_row(double lat)
{
    double r = floor(lat) + 90.0;
    if (r < 0.0)   return 0;
    if (r > 179.0) return 179;
    return (int)r;
}

static int bbox_query(pk_aero_db_t *db, const pk_aero_section_t *idx_sec,
                      const pk_aero_index_t *gi, pk_bbox_fn_t hit,
                      double min_lat, double min_lon,
                      double max_lat, double max_lon,
                      uint32_t *out, int max)
{
    /* 约定：视野 bbox 自身不得跨 ±180（Python 端是 assert，C 端返回错误码）。
     * 跨经线的视野请由调用方拆成两段查 —— 把环绕逻辑收在 _cell_expand_bbox
     * 一个地方。NaN 也走这条（比较为假）。 */
    if (!(min_lon <= max_lon)) return PK_AERO_ERR_ARG;
    if (!(fabs(min_lat) < 1e15) || !(fabs(max_lat) < 1e15) ||
        !(fabs(min_lon) < 1e15) || !(fabs(max_lon) < 1e15))
        return PK_AERO_ERR_ARG;
    if (max <= 0 || idx_sec->n == 0 || gi->n_grid == 0) return 0;
    /* 纬度反了 → Python 的 range(r0, r1+1) 为空 → 无结果（不是错误）*/
    if (!(min_lat <= max_lat)) return 0;

    int r0 = bbox_row(min_lat), r1 = bbox_row(max_lat);
    double fc0 = floor(min_lon), fc1 = floor(max_lon);
    int64_t c0, c1;
    if (fc1 - fc0 >= 359.0) {
        c0 = 0; c1 = 359;    /* 已覆盖整圈：逐列枚举一次即可（结果集去重，
                              * 与 Python 把同一格重复枚举多次等价）*/
    } else {
        c0 = (int64_t)fc0; c1 = (int64_t)fc1;
    }

    const uint8_t *tbl = db->payload + idx_sec->data_off;
    int n = 0;
    for (int row = r0; row <= r1; row++) {
        for (int64_t c = c0; c <= c1; c++) {
            int64_t col = ((c + 180) % 360 + 360) % 360;   /* Python % 恒非负 */
            uint16_t cell = (uint16_t)(row * 360 + (int)col);
            db->stats.cells_visited++;
            uint32_t first = 0, count = 0;
            pk_aero_grid_lookup(db, gi, cell, &first, &count);
            if (count == 0) continue;
            if ((uint64_t)first + count > idx_sec->n) continue;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t rec = rd_u32(tbl + (size_t)(first + k)
                                          * PK_AERO_IDX_ENTRY_SIZE);
                db->stats.cell_candidates++;
                db->stats.bbox_tests++;
                if (!hit(db, rec, min_lat, min_lon, max_lat, max_lon))
                    continue;
                db->stats.bbox_inserts++;
                insert_sorted_uniq(out, &n, max, rec);
            }
        }
    }
    return n;
}

int pk_aero_airspaces_in_cell(pk_aero_db_t *db, uint16_t cell,
                              uint32_t *out, int max)
{
    if (!db || !out) return 0;
    return cell_expansion(db, &db->sec_idx_asp_cell, &db->asp_cell_idx,
                          cell, out, max);
}

int pk_aero_airways_in_cell(pk_aero_db_t *db, uint16_t cell,
                            uint32_t *out, int max)
{
    if (!db || !out) return 0;
    return cell_expansion(db, &db->sec_idx_awy_cell, &db->awy_cell_idx,
                          cell, out, max);
}

int pk_aero_airspaces_in_bbox(pk_aero_db_t *db,
                              double min_lat, double min_lon,
                              double max_lat, double max_lon,
                              uint32_t *out, int max)
{
    if (!db || !out) return 0;
    return bbox_query(db, &db->sec_idx_asp_cell, &db->asp_cell_idx,
                      asp_bbox_hit, min_lat, min_lon, max_lat, max_lon,
                      out, max);
}

int pk_aero_airways_in_bbox(pk_aero_db_t *db,
                            double min_lat, double min_lon,
                            double max_lat, double max_lon,
                            uint32_t *out, int max)
{
    if (!db || !out) return 0;
    return bbox_query(db, &db->sec_idx_awy_cell, &db->awy_cell_idx,
                      awy_bbox_hit, min_lat, min_lon, max_lat, max_lon,
                      out, max);
}
