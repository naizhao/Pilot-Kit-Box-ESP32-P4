/* pk_rec_format.c — 编解码实现。设计说明见 pk_rec_format.h。
 *
 * 全部小端、逐字节手拼，不依赖宿主字节序、不用 htole*（host 侧未必有，
 * 且我们要的是"文件里恒为小端"而不是"跟随宿主"）。所有多字节字段走
 * 下面这组 put_u16/put_u32/put_u64（有符号版本直接重新解释同宽度的
 * 无符号值，二进制补码下逐字节存取是等价的）。
 */
#include "pk_rec_format.h"

#include <string.h>

/* --------------------------------------------------------- 小端读写 helper */

static void put_u8(uint8_t *buf, size_t off, uint8_t v)
{
    buf[off] = v;
}

static uint8_t get_u8(const uint8_t *buf, size_t off)
{
    return buf[off];
}

static void put_u16(uint8_t *buf, size_t off, uint16_t v)
{
    buf[off + 0] = (uint8_t)(v & 0xFFu);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *buf, size_t off)
{
    return (uint16_t)((uint16_t)buf[off + 0] | ((uint16_t)buf[off + 1] << 8));
}

static void put_u32(uint8_t *buf, size_t off, uint32_t v)
{
    buf[off + 0] = (uint8_t)(v & 0xFFu);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
    buf[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
    buf[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *buf, size_t off)
{
    return (uint32_t)buf[off + 0] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

static void put_u64(uint8_t *buf, size_t off, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        buf[off + (size_t)i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
}

static uint64_t get_u64(const uint8_t *buf, size_t off)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)buf[off + (size_t)i] << (8 * i);
    }
    return v;
}

static void put_i16(uint8_t *buf, size_t off, int16_t v) { put_u16(buf, off, (uint16_t)v); }
static int16_t get_i16(const uint8_t *buf, size_t off) { return (int16_t)get_u16(buf, off); }

static void put_i32(uint8_t *buf, size_t off, int32_t v) { put_u32(buf, off, (uint32_t)v); }
static int32_t get_i32(const uint8_t *buf, size_t off) { return (int32_t)get_u32(buf, off); }

/* ------------------------------------------------------------ 32B 文件头 */

void pk_rec_header_encode(const pk_rec_header_t *hdr, uint8_t buf[PK_REC_HEADER_LEN])
{
    memset(buf, 0, PK_REC_HEADER_LEN);
    memcpy(&buf[0], hdr->magic, sizeof(hdr->magic));           /* 0..7 */
    put_u16(buf, 8, hdr->format_version);                       /* 8..9 */
    put_u16(buf, 10, hdr->record_size);                         /* 10..11 */
    put_u32(buf, 12, hdr->endian_marker);                       /* 12..15 */
    put_u64(buf, 16, hdr->created_ts_ms);                       /* 16..23 */
    put_u8(buf, 24, hdr->file_kind);                            /* 24 */
    memcpy(&buf[25], hdr->reserved, sizeof(hdr->reserved));     /* 25..31 */
}

bool pk_rec_header_decode(const uint8_t buf[PK_REC_HEADER_LEN], pk_rec_header_t *out)
{
    memcpy(out->magic, &buf[0], sizeof(out->magic));
    out->format_version = get_u16(buf, 8);
    out->record_size    = get_u16(buf, 10);
    out->endian_marker  = get_u32(buf, 12);
    out->created_ts_ms  = get_u64(buf, 16);
    out->file_kind      = get_u8(buf, 24);
    memcpy(out->reserved, &buf[25], sizeof(out->reserved));
    return out->endian_marker == PK_REC_ENDIAN_MARKER;
}

/* ------------------------------------------------------------ traffic.trk */

void pk_trk_pos_encode(const pk_trk_pos_t *rec, uint8_t buf[PK_TRK_RECORD_LEN])
{
    memset(buf, 0, PK_TRK_RECORD_LEN);
    put_u64(buf, 0, rec->ts_ms);                 /* 0 */
    memcpy(&buf[8], rec->icao24, 3);              /* 8..10 */
    put_u8(buf, 11, PK_TRK_REC_POSITION);          /* 11 */
    put_i32(buf, 12, rec->lat_e7);                 /* 12 */
    put_i32(buf, 16, rec->lon_e7);                 /* 16 */
    put_i16(buf, 20, rec->alt_d25);                 /* 20 */
    put_u16(buf, 22, rec->gs_kt);                   /* 22 */
    put_u16(buf, 24, rec->track_deg10);             /* 24 */
    put_i16(buf, 26, rec->vs_fpm_d64);               /* 26 */
    put_u8(buf, 28, rec->flags);                     /* 28 */
    /* 29..31 reserved, already zeroed */
}

void pk_trk_pos_decode(const uint8_t buf[PK_TRK_RECORD_LEN], pk_trk_pos_t *out)
{
    out->ts_ms = get_u64(buf, 0);
    memcpy(out->icao24, &buf[8], 3);
    out->lat_e7 = get_i32(buf, 12);
    out->lon_e7 = get_i32(buf, 16);
    out->alt_d25 = get_i16(buf, 20);
    out->gs_kt = get_u16(buf, 22);
    out->track_deg10 = get_u16(buf, 24);
    out->vs_fpm_d64 = get_i16(buf, 26);
    out->flags = get_u8(buf, 28);
}

void pk_trk_id_encode(const pk_trk_id_t *rec, uint8_t buf[PK_TRK_RECORD_LEN])
{
    memset(buf, 0, PK_TRK_RECORD_LEN);
    put_u64(buf, 0, rec->ts_ms);                    /* 0 */
    memcpy(&buf[8], rec->icao24, 3);                 /* 8..10 */
    put_u8(buf, 11, PK_TRK_REC_IDENTITY);             /* 11 */
    memcpy(&buf[12], rec->callsign, 9);                /* 12..20 */
    put_u8(buf, 21, rec->emitter_category);            /* 21 */
    /* 22..31 reserved, already zeroed */
}

void pk_trk_id_decode(const uint8_t buf[PK_TRK_RECORD_LEN], pk_trk_id_t *out)
{
    out->ts_ms = get_u64(buf, 0);
    memcpy(out->icao24, &buf[8], 3);
    memcpy(out->callsign, &buf[12], 9);
    out->emitter_category = get_u8(buf, 21);
}

uint8_t pk_trk_rec_type_peek(const uint8_t buf[PK_TRK_RECORD_LEN])
{
    return get_u8(buf, 11);
}

/* ------------------------------------------------------------ traffic.idx */

void pk_idx_rec_encode(const pk_idx_rec_t *rec, uint8_t buf[PK_IDX_RECORD_LEN])
{
    memset(buf, 0, PK_IDX_RECORD_LEN);
    memcpy(&buf[0], rec->icao24, 3);                  /* 0..2 */
    put_u8(buf, 3, rec->flags);                         /* 3 */
    memcpy(&buf[4], rec->callsign, 9);                   /* 4..12 */
    /* 13..15 reserved, already zeroed */
    put_u64(buf, 16, rec->first_ts_ms);                   /* 16 */
    put_u64(buf, 24, rec->last_ts_ms);                     /* 24 */
    put_u32(buf, 32, rec->point_count);                     /* 32 */
    put_u32(buf, 36, rec->identity_count);                   /* 36 */
}

void pk_idx_rec_decode(const uint8_t buf[PK_IDX_RECORD_LEN], pk_idx_rec_t *out)
{
    memcpy(out->icao24, &buf[0], 3);
    out->flags = get_u8(buf, 3);
    memcpy(out->callsign, &buf[4], 9);
    out->first_ts_ms = get_u64(buf, 16);
    out->last_ts_ms = get_u64(buf, 24);
    out->point_count = get_u32(buf, 32);
    out->identity_count = get_u32(buf, 36);
}

/* ------------------------------------------------------------ own.trk */

void pk_own_sample_encode(const pk_own_sample_t *rec, uint8_t buf[PK_OWN_RECORD_LEN])
{
    memset(buf, 0, PK_OWN_RECORD_LEN);
    put_u64(buf, 0, rec->ts_ms);                    /* 0 */
    put_u8(buf, 8, PK_OWN_REC_SAMPLE);                /* 8 */
    put_u8(buf, 9, rec->phase);                        /* 9 */
    put_u8(buf, 10, rec->flags);                        /* 10 */
    put_u8(buf, 11, rec->sats);                          /* 11 */
    put_i32(buf, 12, rec->lat_e7);                        /* 12 */
    put_i32(buf, 16, rec->lon_e7);                         /* 16 */
    put_i32(buf, 20, rec->alt_baro_ft);                     /* 20 */
    put_i32(buf, 24, rec->alt_gnss_msl_ft);                  /* 24 */
    put_u16(buf, 28, rec->gs_kt);                             /* 28 */
    put_u16(buf, 30, rec->track_deg10);                        /* 30 */
    put_i16(buf, 32, rec->vs_fpm);                              /* 32 */
    put_i16(buf, 34, rec->roll_d10);                             /* 34 */
    put_i16(buf, 36, rec->pitch_d10);                             /* 36 */
    put_i16(buf, 38, rec->yaw_d10);                                /* 38 */
    put_u8(buf, 40, rec->hdop_x10);                                 /* 40 */
    put_u8(buf, 41, rec->vib_level);                                 /* 41 */
    put_u16(buf, 42, rec->disp_m_60s);                                /* 42 */
    put_u8(buf, 44, rec->ac_category);                                 /* 44 */
    /* 45..47 reserved, already zeroed */
}

void pk_own_sample_decode(const uint8_t buf[PK_OWN_RECORD_LEN], pk_own_sample_t *out)
{
    out->ts_ms = get_u64(buf, 0);
    out->phase = get_u8(buf, 9);
    out->flags = get_u8(buf, 10);
    out->sats = get_u8(buf, 11);
    out->lat_e7 = get_i32(buf, 12);
    out->lon_e7 = get_i32(buf, 16);
    out->alt_baro_ft = get_i32(buf, 20);
    out->alt_gnss_msl_ft = get_i32(buf, 24);
    out->gs_kt = get_u16(buf, 28);
    out->track_deg10 = get_u16(buf, 30);
    out->vs_fpm = get_i16(buf, 32);
    out->roll_d10 = get_i16(buf, 34);
    out->pitch_d10 = get_i16(buf, 36);
    out->yaw_d10 = get_i16(buf, 38);
    out->hdop_x10 = get_u8(buf, 40);
    out->vib_level = get_u8(buf, 41);
    out->disp_m_60s = get_u16(buf, 42);
    out->ac_category = get_u8(buf, 44);
}

void pk_own_time_sync_encode(const pk_own_time_sync_t *rec, uint8_t buf[PK_OWN_RECORD_LEN])
{
    memset(buf, 0, PK_OWN_RECORD_LEN);
    put_u64(buf, 0, rec->ts_ms);                 /* 0 — 校时后 */
    put_u8(buf, 8, PK_OWN_REC_TIME_SYNC);          /* 8 */
    put_u8(buf, 9, 0);                              /* 9 — phase 无意义，固定 0 */
    /* 10..11 reserved（占 own.trk 采样记录里 flags/sats 的位置），置零 */
    put_u8(buf, 12, rec->sync_source);               /* 12 */
    put_u8(buf, 13, rec->sync_reason);                /* 13 */
    /* 14..15 reserved，置零 */
    put_u64(buf, 16, rec->prev_ts_ms);                 /* 16 */
    /* 24..47 reserved，置零 */
}

void pk_own_time_sync_decode(const uint8_t buf[PK_OWN_RECORD_LEN], pk_own_time_sync_t *out)
{
    out->ts_ms = get_u64(buf, 0);
    out->sync_source = get_u8(buf, 12);
    out->sync_reason = get_u8(buf, 13);
    out->prev_ts_ms = get_u64(buf, 16);
}

uint8_t pk_own_rec_type_peek(const uint8_t buf[PK_OWN_RECORD_LEN])
{
    return get_u8(buf, 8);
}
