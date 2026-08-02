/* pk_rec_idx.c — pk_rec_idx 的实现。设计说明见 pk_rec_idx.h。 */
#include "pk_rec_idx.h"

#include <string.h>

static bool icao_eq(const uint8_t a[3], const uint8_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

void pk_rec_idx_reset(pk_rec_idx_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

const pk_idx_rec_t *pk_rec_idx_find(const pk_rec_idx_table_t *t,
                                     const uint8_t icao24[3])
{
    for (size_t i = 0; i < t->count; i++) {
        if (icao_eq(t->entries[i].icao24, icao24)) return &t->entries[i];
    }
    return NULL;
}

pk_idx_rec_t *pk_rec_idx_find_or_add(pk_rec_idx_table_t *t,
                                     const uint8_t icao24[3])
{
    for (size_t i = 0; i < t->count; i++) {
        if (icao_eq(t->entries[i].icao24, icao24)) return &t->entries[i];
    }
    if (t->count >= PK_REC_IDX_MAX_AIRCRAFT) return NULL;
    pk_idx_rec_t *e = &t->entries[t->count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->icao24, icao24, 3);
    return e;
}

/* first_ts_ms==0 当"尚未设置"的哨兵——本文件所有时间戳都是 epoch 毫秒，
 * 真实数据不可能是 0（同 pk_rec_store_fs.c/aircraft_state.c 一以贯之的
 * 0=未初始化约定）。 */
static void note_ts(pk_idx_rec_t *e, uint64_t ts_ms)
{
    if (e->first_ts_ms == 0 || ts_ms < e->first_ts_ms) e->first_ts_ms = ts_ms;
    if (ts_ms > e->last_ts_ms) e->last_ts_ms = ts_ms;
}

void pk_rec_idx_ingest_pos(pk_rec_idx_table_t *t, const pk_trk_pos_t *pos, bool is_own)
{
    pk_idx_rec_t *e = pk_rec_idx_find_or_add(t, pos->icao24);
    if (e == NULL) return;
    e->flags |= PK_IDX_FLAG_HAD_POSITION;
    if (pos->flags & PK_TRK_FLAG_ON_GROUND) e->flags |= PK_IDX_FLAG_HAD_GROUND;
    if (is_own) e->flags |= PK_IDX_FLAG_IS_OWN;
    note_ts(e, pos->ts_ms);
    e->point_count++;
}

void pk_rec_idx_ingest_id(pk_rec_idx_table_t *t, const pk_trk_id_t *id, bool is_own)
{
    pk_idx_rec_t *e = pk_rec_idx_find_or_add(t, id->icao24);
    if (e == NULL) return;
    e->flags |= PK_IDX_FLAG_HAD_CALLSIGN;
    if (is_own) e->flags |= PK_IDX_FLAG_IS_OWN;
    memcpy(e->callsign, id->callsign, sizeof(e->callsign));
    note_ts(e, id->ts_ms);
    e->identity_count++;
}

void pk_rec_idx_rebuild_from_records(pk_rec_idx_table_t *t, const uint8_t *buf,
                                      size_t record_count, const uint8_t own_icao[3])
{
    pk_rec_idx_reset(t);
    for (size_t i = 0; i < record_count; i++) {
        const uint8_t *rec = buf + i * PK_TRK_RECORD_LEN;
        uint8_t rec_type = pk_trk_rec_type_peek(rec);
        const uint8_t *icao = &rec[8];  /* 位置/身份记录前 12 字节布局相同 */
        bool is_own = (own_icao != NULL) && icao_eq(icao, own_icao);
        if (rec_type == PK_TRK_REC_POSITION) {
            pk_trk_pos_t pos;
            pk_trk_pos_decode(rec, &pos);
            pk_rec_idx_ingest_pos(t, &pos, is_own);
        } else if (rec_type == PK_TRK_REC_IDENTITY) {
            pk_trk_id_t id;
            pk_trk_id_decode(rec, &id);
            pk_rec_idx_ingest_id(t, &id, is_own);
        }
        /* 其它 rec_type 值（坏文件/未来扩展）直接跳过，不中断重建。 */
    }
}

size_t pk_rec_idx_encode_all(const pk_rec_idx_table_t *t, uint8_t *out, size_t cap_records)
{
    size_t n = t->count < cap_records ? t->count : cap_records;
    for (size_t i = 0; i < n; i++) {
        pk_idx_rec_encode(&t->entries[i], out + i * PK_IDX_RECORD_LEN);
    }
    return n;
}
