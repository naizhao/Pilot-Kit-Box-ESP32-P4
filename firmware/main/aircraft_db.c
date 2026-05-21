/*
 * aircraft_db.c — runtime side of the ICAO24 → type/model/reg lookup.
 *
 * Maps directly over the EMBED_FILES blob `aircraft_db.bin` which lives
 * in .rodata (flash-mapped). Header layout + record / type / string
 * encoding match firmware/scripts/gen_aircraft_db.py — keep the two in
 * sync.
 *
 * No allocation, no copies — every returned string is a const pointer
 * into flash. Binary search over a sorted array of 8-byte packed records.
 */

#include "aircraft_db.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "acdb";

/* Symbols emitted by EMBED_FILES "aircraft_db.bin". */
extern const uint8_t aircraft_db_start[] asm("_binary_aircraft_db_bin_start");
extern const uint8_t aircraft_db_end[]   asm("_binary_aircraft_db_bin_end");

/* On-disk header: matches gen_aircraft_db.py HEADER_FMT exactly. Little-
 * endian fields throughout (ESP32-P4 native byte order). */
typedef struct __attribute__((packed)) {
    char     magic[6];        /* "PKADB1" */
    uint16_t version;         /* == 2 */
    uint32_t n_records;
    uint32_t records_off;
    uint16_t n_types;
    uint16_t _reserved;
    uint32_t types_off;
    uint32_t strings_off;
    uint32_t strings_size;
} db_header_t;

/* Sentinel values. Must match gen_aircraft_db.py. */
#define DB_TYPE_NONE   0xFFFFu     /* type_idx for aircraft w/o type code */
#define DB_STR_NONE    0u          /* string offset for "no string" */

/* Records are 8 bytes packed: 3 ICAO24 (big-endian), 2 type_idx (little-
 * endian), 3 reg_off (big-endian). We hand-decode them byte-by-byte
 * rather than declaring a struct because the mixed-endian 3-byte fields
 * are easier to express as explicit byte loads than as packed-struct
 * accessors that the compiler has to lower to byte loads anyway. */
#define DB_RECORD_SIZE   8

/* Each type entry: three little-endian uint32 offsets into the string pool.
 * Tail-packed and natively aligned because 12 % 4 == 0. */
typedef struct __attribute__((packed)) {
    uint32_t code_off;
    uint32_t model_off;
    uint32_t desc_off;
} db_type_t;

static struct {
    bool                  ok;
    const db_header_t    *header;
    const uint8_t        *records;       /* records_off start */
    const db_type_t      *types;
    const char           *strings;
    uint32_t              n_records;
    uint16_t              n_types;
    uint32_t              strings_size;
} s_db;

static inline uint32_t rec_icao(const uint8_t *r)
{
    return ((uint32_t)r[0] << 16)
         | ((uint32_t)r[1] <<  8)
         |  (uint32_t)r[2];
}

static inline uint16_t rec_type_idx(const uint8_t *r)
{
    return (uint16_t)r[3] | ((uint16_t)r[4] << 8);
}

static inline uint32_t rec_reg_off(const uint8_t *r)
{
    return ((uint32_t)r[5] << 16)
         | ((uint32_t)r[6] <<  8)
         |  (uint32_t)r[7];
}

void pk_aircraft_db_init(void)
{
    size_t blob = (size_t)(aircraft_db_end - aircraft_db_start);
    if (blob < sizeof(db_header_t)) {
        ESP_LOGE(TAG, "aircraft_db.bin too small (%zu B) — disabled", blob);
        return;
    }
    const db_header_t *h = (const db_header_t *)aircraft_db_start;
    if (memcmp(h->magic, "PKADB1", 6) != 0) {
        ESP_LOGE(TAG, "aircraft_db.bin bad magic — disabled");
        return;
    }
    if (h->version != 2) {
        ESP_LOGE(TAG, "aircraft_db.bin version %u unsupported "
                       "(expected 2) — disabled",
                 (unsigned)h->version);
        return;
    }
    /* Bounds-check offsets against blob size so a corrupted header can't
     * dereference out-of-flash pointers later. */
    uint32_t recs_end = h->records_off + (uint32_t)h->n_records * DB_RECORD_SIZE;
    uint32_t typs_end = h->types_off   + (uint32_t)h->n_types   * sizeof(db_type_t);
    uint32_t strs_end = h->strings_off + h->strings_size;
    if (recs_end > blob || typs_end > blob || strs_end > blob) {
        ESP_LOGE(TAG, "aircraft_db.bin header out of range — disabled");
        return;
    }

    s_db.header       = h;
    s_db.records      = aircraft_db_start + h->records_off;
    s_db.types        = (const db_type_t *)(aircraft_db_start + h->types_off);
    s_db.strings      = (const char      *)(aircraft_db_start + h->strings_off);
    s_db.n_records    = h->n_records;
    s_db.n_types      = h->n_types;
    s_db.strings_size = h->strings_size;
    s_db.ok           = true;

    ESP_LOGI(TAG,
             "aircraft DB ready (v%u): %lu records, %u types, "
             "%.1f KB strings, %.2f MB total",
             (unsigned)h->version,
             (unsigned long)s_db.n_records,
             (unsigned)s_db.n_types,
             s_db.strings_size / 1024.0,
             blob / 1024.0 / 1024.0);
}

/* Return a pointer to the 8-byte record for icao24, or NULL if absent. */
static const uint8_t *find_icao(uint32_t icao24)
{
    if (!s_db.ok) return NULL;
    icao24 &= 0xFFFFFF;
    int lo = 0;
    int hi = (int)s_db.n_records - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *r = s_db.records + (size_t)mid * DB_RECORD_SIZE;
        uint32_t key = rec_icao(r);
        if (key == icao24) return r;
        if (key <  icao24) lo = mid + 1;
        else               hi = mid - 1;
    }
    return NULL;
}

/* Resolve a string-pool offset to a C string pointer. Offset 0 is the
 * "no string" sentinel and returns NULL. */
static const char *str_at(uint32_t off)
{
    if (off == DB_STR_NONE)      return NULL;
    if (off >= s_db.strings_size) return NULL;
    return s_db.strings + off;
}

/* Look up the per-type string at index `which`:
 *   0 → code, 1 → model, 2 → desc.
 * Returns NULL when the record has no type code, the type_idx is out of
 * range, or the looked-up offset is the "no string" sentinel. */
static const char *type_field(const uint8_t *rec, int which)
{
    uint16_t idx = rec_type_idx(rec);
    if (idx == DB_TYPE_NONE)  return NULL;
    if (idx >= s_db.n_types)  return NULL;
    const db_type_t *t = &s_db.types[idx];
    uint32_t off = (which == 0) ? t->code_off
                : (which == 1) ? t->model_off
                :                t->desc_off;
    return str_at(off);
}

const char *pk_aircraft_type_code(uint32_t icao24)
{
    const uint8_t *r = find_icao(icao24);
    return r ? type_field(r, 0) : NULL;
}

const char *pk_aircraft_type_model(uint32_t icao24)
{
    const uint8_t *r = find_icao(icao24);
    return r ? type_field(r, 1) : NULL;
}

const char *pk_aircraft_type_desc(uint32_t icao24)
{
    const uint8_t *r = find_icao(icao24);
    return r ? type_field(r, 2) : NULL;
}

const char *pk_aircraft_registration(uint32_t icao24)
{
    const uint8_t *r = find_icao(icao24);
    if (!r) return NULL;
    return str_at(rec_reg_off(r));
}
