/*
 * aircraft_db_reader.c — pk_actdb.bin 解析 + ICAO24 二分查找（纯 C11）。
 *
 * 布局说明见 aircraft_db_reader.h 文件头；载荷格式的权威定义是
 * firmware/scripts/gen_aircraft_db.py 的文档字符串。
 * host 单测：firmware/test/test_aircraft_db.c。
 */
#include "aircraft_db_reader.h"

#include <string.h>

/* ---- 逐字节取数（不做未对齐强转，照 pk_aero_reader.c）------------------ */

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t rd_u24be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* 定长 char[n] → NUL 结尾拷贝（dst 容量 n+1），同 pk_aero_reader 的 fix_str */
static void fix_str(char *dst, const uint8_t *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i] != '\0') {
        dst[i] = (char)src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---- 容器 / 载荷校验 --------------------------------------------------- */

uint32_t pk_actdb_payload_off(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < PK_ACTDB_HEADER_SIZE) return 0;
    uint16_t n_sections   = rd_u16(buf + 16);
    uint32_t sections_off = rd_u32(buf + 18);
    uint32_t off = sections_off + (uint32_t)n_sections * PK_ACTDB_SECTION_SIZE;
    return (off + 15u) / 16u * 16u;   /* 16 B 对齐，同 pk_aero_payload_off */
}

/* 解析内层 PKADB1（p/plen 已确保在 buffer 内）。 */
static int payload_init(pk_actdb_t *db, const uint8_t *p, uint32_t plen)
{
    if (plen < 32) return PK_ACTDB_ERR_TRUNCATED;
    if (memcmp(p, PK_ACTDB_PAYLOAD_MAGIC, 6) != 0) return PK_ACTDB_ERR_MAGIC;
    uint16_t version = rd_u16(p + 6);
    if (version != PK_ACTDB_PAYLOAD_VER) return PK_ACTDB_ERR_VERSION;

    uint32_t n_records    = rd_u32(p + 8);
    uint32_t records_off  = rd_u32(p + 12);
    uint16_t n_types      = rd_u16(p + 16);
    /* p + 18 是 _reserved */
    uint32_t types_off    = rd_u32(p + 20);
    uint32_t strings_off  = rd_u32(p + 24);
    uint32_t strings_size = rd_u32(p + 28);

    /* 边界全用 64 位算，头被改坏也不会因为 32 位回绕而漏检。 */
    uint64_t recs_end = (uint64_t)records_off + (uint64_t)n_records * PK_ACTDB_RECORD_SIZE;
    uint64_t typs_end = (uint64_t)types_off   + (uint64_t)n_types   * PK_ACTDB_TYPE_SIZE;
    uint64_t strs_end = (uint64_t)strings_off + (uint64_t)strings_size;
    if (recs_end > plen || typs_end > plen || strs_end > plen)
        return PK_ACTDB_ERR_TRUNCATED;
    /* 字符串池必须以 NUL 收尾：str_at 之后按 C 串读，靠这一条兜住越界扫描。 */
    if (strings_size == 0 || p[strings_off + strings_size - 1] != '\0')
        return PK_ACTDB_ERR_TRUNCATED;

    db->version      = version;
    db->records      = p + records_off;
    db->types        = p + types_off;
    db->strings      = (const char *)(p + strings_off);
    db->n_records    = n_records;
    db->n_types      = n_types;
    db->strings_size = strings_size;
    return PK_ACTDB_OK;
}

int pk_actdb_init(pk_actdb_t *db, const uint8_t *buf, size_t len,
                  bool with_container)
{
    if (db == NULL || buf == NULL) return PK_ACTDB_ERR_ARG;
    memset(db, 0, sizeof(*db));

    if (!with_container) {
        if (len > UINT32_MAX) return PK_ACTDB_ERR_ARG;
        db->payload     = buf;
        db->payload_len = (uint32_t)len;
        return payload_init(db, buf, (uint32_t)len);
    }

    if (len < PK_ACTDB_HEADER_SIZE) return PK_ACTDB_ERR_TRUNCATED;
    if (memcmp(buf, PK_ACTDB_MAGIC, 6) != 0) return PK_ACTDB_ERR_MAGIC;
    uint16_t version = rd_u16(buf + 6);
    if (version != PK_ACTDB_VERSION) return PK_ACTDB_ERR_VERSION;

    fix_str(db->cycle, buf + 8, 8);
    db->container_version = version;
    db->enc_algo          = buf[22];
    memcpy(db->sha256, buf + 32, 32);

    /* 机型库是公开数据（tar1090-db），定案就是不加密——省掉整块 8 MB 的
     * 解密开销。真遇到 enc_algo != 0 的文件说明是别的东西，直接拒。 */
    if (db->enc_algo != PK_ACTDB_ENC_NONE) return PK_ACTDB_ERR_ENCRYPTED;

    uint16_t n_sections   = rd_u16(buf + 16);
    uint32_t sections_off = rd_u32(buf + 18);
    uint64_t table_end = (uint64_t)sections_off
                       + (uint64_t)n_sections * PK_ACTDB_SECTION_SIZE;
    if (table_end > len) return PK_ACTDB_ERR_TRUNCATED;

    uint32_t poff = pk_actdb_payload_off(buf, len);
    if (poff == 0 || poff >= len) return PK_ACTDB_ERR_TRUNCATED;
    db->payload     = buf + poff;
    db->payload_len = (uint32_t)(len - poff);
    return payload_init(db, db->payload, db->payload_len);
}

/* ---- 查询 -------------------------------------------------------------- */

static inline uint32_t rec_icao(const uint8_t *r)
{
    return ((uint32_t)r[0] << 16) | ((uint32_t)r[1] << 8) | (uint32_t)r[2];
}

static inline uint16_t rec_type_idx(const uint8_t *r)
{
    return (uint16_t)r[3] | ((uint16_t)r[4] << 8);
}

static inline uint32_t rec_reg_off(const uint8_t *r)
{
    return rd_u24be(r + 5);
}

/* 字符串池偏移 → C 串；偏移 0 是"无"哨兵。越界一律当"无"。 */
static const char *str_at(const pk_actdb_t *db, uint32_t off)
{
    if (off == PK_ACTDB_STR_NONE)  return NULL;
    if (off >= db->strings_size)   return NULL;
    return db->strings + off;
}

/* 池里的串拷进定容字段；截断而不是丢弃——机型名 47 字符是当前上游最长值，
 * 上游若变长，显示层本来也只放得下这么多（调用方是 char v[8][48] 的 snprintf）。 */
static void copy_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

bool pk_actdb_lookup(const pk_actdb_t *db, uint32_t icao24,
                     pk_aircraft_info_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (db == NULL || db->records == NULL || db->n_records == 0) return false;

    icao24 &= 0xFFFFFF;
    const uint8_t *rec = NULL;
    int lo = 0;
    int hi = (int)db->n_records - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *r = db->records + (size_t)mid * PK_ACTDB_RECORD_SIZE;
        uint32_t key = rec_icao(r);
        if (key == icao24) { rec = r; break; }
        if (key <  icao24) lo = mid + 1;
        else               hi = mid - 1;
    }
    if (rec == NULL) return false;

    copy_str(out->reg, sizeof(out->reg), str_at(db, rec_reg_off(rec)));

    uint16_t idx = rec_type_idx(rec);
    if (idx != PK_ACTDB_TYPE_NONE && idx < db->n_types) {
        const uint8_t *t = db->types + (size_t)idx * PK_ACTDB_TYPE_SIZE;
        copy_str(out->code,  sizeof(out->code),  str_at(db, rd_u32(t)));
        copy_str(out->model, sizeof(out->model), str_at(db, rd_u32(t + 4)));
        copy_str(out->desc,  sizeof(out->desc),  str_at(db, rd_u32(t + 8)));
    }
    return true;
}
