/*
 * pk_aero_reader.c — pk_aero.bin 可移植 C 参考实现
 *
 * 来源：tmp/pk_aero_bench/pk_aero_reader.c 原样搬入（2026-08-01，仅加本段
 * 注释，逻辑零改动）。搬入前验证状态：Mac 端与 Python 1,200 ICAO +
 * 500 nearest + 200 聚簇 + 550 FIX ident + 600 FIX nearest 对拍 100% 一致；
 * P4 真机（p4_bench）v2 全量库 correctness=PASS（ZGGG 10/63、KJFK 8/43、
 * 抽样 40/40）。
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
    if (rd_u16(buf + 6) != PK_AERO_VERSION) return PK_AERO_ERR_VERSION;

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

    /* 段表：只认识 1–4，未知类型跳过（前向兼容）*/
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
        if ((uint64_t)s.data_off + s.data_size > db->payload_len)
            return PK_AERO_ERR_TRUNCATED;
        if ((uint64_t)s.rec_size * s.n > s.data_size)
            return PK_AERO_ERR_SECTION;
        switch (s.type) {
        case PK_AERO_SEC_AIRPORTS:      db->sec_airport = s; break;
        case PK_AERO_SEC_RUNWAY_DIRS:   db->sec_rwy     = s; break;
        case PK_AERO_SEC_FREQUENCIES:   db->sec_freq    = s; break;
        case PK_AERO_SEC_NAVAIDS:       db->sec_navaid  = s; break;
        case PK_AERO_SEC_WAYPOINTS_FIX: db->sec_fix     = s; break;
        default: break;
        }
    }
    if (db->sec_airport.rec_size != PK_AERO_AIRPORT_SIZE ||
        db->sec_rwy.rec_size     != PK_AERO_RWY_DIR_SIZE ||
        db->sec_freq.rec_size    != PK_AERO_FREQ_SIZE ||
        db->sec_navaid.rec_size  != PK_AERO_NAVAID_SIZE ||
        db->sec_fix.rec_size     != PK_AERO_FIX_SIZE)
        return PK_AERO_ERR_SECTION;   /* v2 五段齐备（FIX 段可空但必须在）*/

    int rc = parse_index(db, &db->sec_airport, &db->apt_idx);
    if (rc != PK_AERO_OK) return rc;
    rc = parse_index(db, &db->sec_navaid, &db->nav_idx);
    if (rc != PK_AERO_OK) return rc;
    rc = parse_index(db, &db->sec_fix, &db->fix_idx);
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

int pk_aero_fix_by_ident(pk_aero_db_t *db, const char *ident,
                         uint32_t *out, int max)
{
    if (!db || !ident || ident[0] == '\0' || max < 0)
        return PK_AERO_ERR_ARG;
    size_t n = strlen(ident);
    if (n > 6) return PK_AERO_ERR_ARG;
    uint8_t want[6] = {0, 0, 0, 0, 0, 0};
    memcpy(want, ident, n);

    const pk_aero_index_t *gi = &db->fix_idx;
    const uint8_t *second = db->payload + gi->second_off;
    const uint8_t *data   = db->payload + db->sec_fix.data_off;

    uint32_t lo = 0, hi = gi->n_second;
    while (lo < hi) {                       /* lower_bound，同 ICAO 二分 */
        uint32_t mid = (lo + hi) / 2;
        db->stats.bsearch_steps++;
        uint32_t rec = rd_u32(second + (size_t)mid * 4);
        /* FIX 记录偏移 8 起是 ident[6]；memcmp 无符号比较 == Python bytes 序 */
        if (memcmp(data + (size_t)rec * PK_AERO_FIX_SIZE + 8, want, 6) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    int found = 0;
    while (lo < gi->n_second) {             /* 线性扫同名区间 */
        uint32_t rec = rd_u32(second + (size_t)lo * 4);
        if (memcmp(data + (size_t)rec * PK_AERO_FIX_SIZE + 8, want, 6) != 0)
            break;
        if (found < max) out[found] = rec;
        found++;
        lo++;
    }
    return found;
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
