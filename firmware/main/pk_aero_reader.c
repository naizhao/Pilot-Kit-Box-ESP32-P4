/*
 * pk_aero_reader.c — pk_aero.bin 可移植 C 参考实现
 *
 * 来源：tmp/pk_aero_bench/pk_aero_reader.c 搬入（2026-08-02，v3 版），
 * 固件侧唯一改动是接受 v2/v3 两种 bin（版本判定 + db->version，见 pk_aero_init）。
 * 搬入前验证状态：Mac 端与 Python 对拍 100% 一致（ICAO / nearest / 聚簇 /
 * FIX ident / v3 前缀三表 / 导航台 ident / 子串）；P4 真机（p4_bench）
 * v2 全量库 correctness=PASS。
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
    /* 双版本：v2/v3 记录段逐字节相同，v3 只是多了 4 个索引段 + strings_size。
     * 用户换卡有窗口期，两版都得能读，缺索引的查询各自退化（见头文件表）。 */
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
    };
    for (size_t i = 0; i < sizeof(idx4) / sizeof(idx4[0]); i++) {
        if (idx4[i]->n != 0 && idx4[i]->rec_size != PK_AERO_IDX_ENTRY_SIZE)
            return PK_AERO_ERR_SECTION;
    }

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
        keyf(db, rd_u32(table + (size_t)mid * 4), k);
        if (memcmp(k, padded, klen) < 0) lo = mid + 1;
        else                             hi = mid;
    }
    int found = 0;
    while (lo < n && found < max) {
        uint32_t rec = rd_u32(table + (size_t)lo * 4);
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

/* 顺扫一段的字符串池。返回 true 表示 out 已满。 */
static bool scan_pool(pk_aero_db_t *db, uint8_t type,
                      const pk_aero_section_t *sec,
                      const pk_aero_section_t *idx_sec,
                      const uint8_t *q, size_t qlen,
                      pk_aero_hit_t *out, int *n, int max)
{
    if (sec->strings_off == 0 || sec->strings_size <= 1 || idx_sec->n == 0)
        return false;
    const char *base = (const char *)(db->payload + sec->strings_off);
    uint32_t off = 1;                    /* 偏移 0 是"无"哨兵（单个 NUL）*/
    while (off < sec->strings_size) {
        const char *s = base + off;
        size_t len = strlen(s);
        db->stats.pool_bytes += (uint64_t)len + 1;
        db->stats.pool_strings++;
        if (ci_contains(s, q, qlen)) {
            db->stats.pool_hits++;
            if (rev_emit(db, type, idx_sec, off, out, n, max)) return true;
        }
        off += (uint32_t)len + 1;
    }
    return false;
}

int pk_aero_search_substring(pk_aero_db_t *db, const char *query,
                             pk_aero_hit_t *out, int max)
{
    if (!db || !out || max <= 0) return 0;
    uint8_t q[PK_AERO_QUERY_MAX];
    int qlen = norm_query(query, q, sizeof q);
    if (qlen <= 0) return 0;

    int n = 0;
    /* 段顺序即结果顺序的一部分，必须与 Python SEARCH_SECTIONS 一致 */
    if (scan_pool(db, PK_AERO_SEC_AIRPORTS, &db->sec_airport,
                  &db->sec_idx_apt_name, q, (size_t)qlen, out, &n, max))
        return n;
    if (scan_pool(db, PK_AERO_SEC_NAVAIDS, &db->sec_navaid,
                  &db->sec_idx_nav_name, q, (size_t)qlen, out, &n, max))
        return n;
    scan_pool(db, PK_AERO_SEC_WAYPOINTS_FIX, &db->sec_fix,
              &db->sec_idx_fix_name, q, (size_t)qlen, out, &n, max);
    return n;
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
