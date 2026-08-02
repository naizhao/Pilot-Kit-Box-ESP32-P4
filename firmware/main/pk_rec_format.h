/*
 * pk_rec_format.h — ADS-B / 本机航迹落盘的二进制记录格式：编解码。
 *
 * 设计依据 docs/internal/2026-08-02-adsb-data-persistence-design-zh_CN.md
 * （v5）「记录格式」一节。字段偏移严格照 spec 的表格，不得自行调整。
 *
 * 本文件刻意**不依赖任何 IDF 头**——固件与 host 单测共用同一份源码
 * （firmware/test/test_pk_rec_format.c 直接 #include 本 .c 进同一 TU）。
 *
 * 非对齐访问红线（ESP32-P4 是 RISC-V，非对齐访问会 panic）：
 *   - 本文件下面这些 pk_trk_pos_t / pk_own_sample_t 等结构体是**逻辑值
 *     容器**，不是线格式本身——它们不 __attribute__((packed))，因为
 *     从来不会被整体 memcpy 进/出磁盘缓冲区；
 *   - 真正落盘的线格式只存在于 pk_rec_format.c 里，编解码一律逐字段
 *     手动搬运单个字节（put_u16/32/64 系列 helper），不对缓冲区做任何
 *     整体 memcpy、更不把 uint8_t* 缓冲区指针 cast 成结构体指针——比
 *     "packed 结构体 + 整体 memcpy" 更严格，从根上不存在对齐假设；
 *   - 全部小端固定：编码时自己拼字节（不依赖宿主字节序），不用 htole* 之类
 *     的库调用（host 侧未必有）。
 *
 * sentinel 语义见 spec：alt_d25 / vs_fpm_d64 用 INT16_MIN 表示无效；
 * gs_kt / track_deg10 用 0xFFFF 表示无效。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ sentinel */

#define PK_REC_ALT_INVALID    ((int16_t)INT16_MIN)   /* alt_d25 */
#define PK_REC_VS_INVALID     ((int16_t)INT16_MIN)   /* vs_fpm_d64 / own.trk vs_fpm */
#define PK_REC_GS_INVALID     ((uint16_t)0xFFFFu)    /* gs_kt */
#define PK_REC_TRACK_INVALID  ((uint16_t)0xFFFFu)    /* track_deg10 */

/* ------------------------------------------------------------ 32B 文件头 */

#define PK_REC_HEADER_LEN 32u

/* endian_marker 固定写入这个值；解码端读回后必须原样比对，
 * 不等则说明字节序假设不成立（本格式设计上恒为小端，正常情况下
 * 不会不等，这里只是防坏文件/防误用）。 */
#define PK_REC_ENDIAN_MARKER ((uint32_t)0x01020304u)

/* file_kind 取值。 */
typedef enum {
    PK_REC_FILE_KIND_TRAFFIC_TRK = 0,
    PK_REC_FILE_KIND_TRAFFIC_IDX = 1,
    PK_REC_FILE_KIND_OWN_TRK     = 2,
} pk_rec_file_kind_t;

/* 三种文件各自的 magic —— 8 字节，不含 NUL 终止（凑满 8 字节）。 */
#define PK_REC_MAGIC_TRAFFIC_TRK "PKTRFTRK"
#define PK_REC_MAGIC_TRAFFIC_IDX "PKTRFIDX"
#define PK_REC_MAGIC_OWN_TRK     "PKOWNTRK"

typedef struct {
    char     magic[8];          /* 不含 NUL；写入/比对固定 8 字节 */
    uint16_t format_version;
    uint16_t record_size;
    uint32_t endian_marker;     /* 恒为 PK_REC_ENDIAN_MARKER */
    uint64_t created_ts_ms;
    uint8_t  file_kind;         /* pk_rec_file_kind_t */
    uint8_t  reserved[7];
} pk_rec_header_t;

/* 编码到 buf[0..PK_REC_HEADER_LEN)。magic 必须恰好 8 字节
 * （调用方传入的字符串字面量若短于 8 字节，剩余部分补 0）。 */
void pk_rec_header_encode(const pk_rec_header_t *hdr, uint8_t buf[PK_REC_HEADER_LEN]);

/* 从 buf 解码。返回 false 表示 endian_marker 不匹配（坏文件/非本格式）。 */
bool pk_rec_header_decode(const uint8_t buf[PK_REC_HEADER_LEN], pk_rec_header_t *out);

/* ------------------------------------------------------------ traffic.trk */

#define PK_TRK_RECORD_LEN 32u

typedef enum {
    PK_TRK_REC_POSITION = 0,
    PK_TRK_REC_IDENTITY = 1,
} pk_trk_rec_type_t;

/* flags 位。 */
#define PK_TRK_FLAG_TIME_SYNCED   (1u << 0)
#define PK_TRK_FLAG_ON_GROUND     (1u << 1)
#define PK_TRK_FLAG_ALT_GEOMETRIC (1u << 2) /* 0=baro 1=geometric，见 altitude_datum */
#define PK_TRK_FLAG_SURFACE_CPR   (1u << 3) /* 由 surface CPR 解出 */

/* 位置记录（rec_type=0）。 */
typedef struct {
    uint64_t ts_ms;
    uint8_t  icao24[3];
    int32_t  lat_e7;
    int32_t  lon_e7;
    int16_t  alt_d25;         /* 25 ft/LSB；PK_REC_ALT_INVALID = 无效 */
    uint16_t gs_kt;           /* PK_REC_GS_INVALID = 无效 */
    uint16_t track_deg10;     /* 度 ×10；PK_REC_TRACK_INVALID = 无效 */
    int16_t  vs_fpm_d64;      /* ft/min ÷64；PK_REC_VS_INVALID = 无效 */
    uint8_t  flags;
} pk_trk_pos_t;

/* 身份记录（rec_type=1）。callsign 未必 NUL 结尾满 9 字节时——按 spec
 * "char callsign[9]" 存 9 字节，调用方保证不超长（这里只做定长搬运，
 * 不额外做截断/校验，那是上游 dsp_task 的职责）。 */
typedef struct {
    uint64_t ts_ms;
    uint8_t  icao24[3];
    char     callsign[9];
    uint8_t  emitter_category; /* pk_wake_t，见 aircraft_state.h，本文件不依赖该头 */
} pk_trk_id_t;

void pk_trk_pos_encode(const pk_trk_pos_t *rec, uint8_t buf[PK_TRK_RECORD_LEN]);
void pk_trk_pos_decode(const uint8_t buf[PK_TRK_RECORD_LEN], pk_trk_pos_t *out);

void pk_trk_id_encode(const pk_trk_id_t *rec, uint8_t buf[PK_TRK_RECORD_LEN]);
void pk_trk_id_decode(const uint8_t buf[PK_TRK_RECORD_LEN], pk_trk_id_t *out);

/* 只读出偏移 11 的 rec_type，用于分派 decode 到 pos/id —— 两种记录
 * 前 12 字节布局相同（ts_ms/icao24/rec_type），调用方据此路由。 */
uint8_t pk_trk_rec_type_peek(const uint8_t buf[PK_TRK_RECORD_LEN]);

/* ------------------------------------------------------------ traffic.idx */

#define PK_IDX_RECORD_LEN 40u

#define PK_IDX_FLAG_HAD_POSITION (1u << 0)
#define PK_IDX_FLAG_HAD_CALLSIGN (1u << 1)
#define PK_IDX_FLAG_HAD_GROUND   (1u << 2)
#define PK_IDX_FLAG_IS_OWN       (1u << 3)

typedef struct {
    uint8_t  icao24[3];
    uint8_t  flags;
    char     callsign[9];
    uint64_t first_ts_ms;
    uint64_t last_ts_ms;
    uint32_t point_count;
    uint32_t identity_count;
} pk_idx_rec_t;

void pk_idx_rec_encode(const pk_idx_rec_t *rec, uint8_t buf[PK_IDX_RECORD_LEN]);
void pk_idx_rec_decode(const uint8_t buf[PK_IDX_RECORD_LEN], pk_idx_rec_t *out);

/* ------------------------------------------------------------ own.trk */

#define PK_OWN_RECORD_LEN 48u

typedef enum {
    PK_OWN_REC_SAMPLE    = 0,
    PK_OWN_REC_TIME_SYNC = 1,
} pk_own_rec_type_t;

#define PK_OWN_FLAG_TIME_SYNCED (1u << 0)
#define PK_OWN_FLAG_GPS_FIX     (1u << 1)
#define PK_OWN_FLAG_IMU_VALID   (1u << 2)
#define PK_OWN_FLAG_BARO_VALID  (1u << 3)

/* 采样记录（rec_type=0）。 */
typedef struct {
    uint64_t ts_ms;
    uint8_t  phase;             /* 相位状态机输出，见 pk_flight_phase.h */
    uint8_t  flags;
    uint8_t  sats;
    int32_t  lat_e7;
    int32_t  lon_e7;
    int32_t  alt_baro_ft;       /* QNH 修正后 */
    int32_t  alt_gnss_msl_ft;   /* GGA 正高 MSL，非椭球高 */
    uint16_t gs_kt;
    uint16_t track_deg10;
    int16_t  vs_fpm;            /* 来自 baro 平滑高度 */
    int16_t  roll_d10;
    int16_t  pitch_d10;
    int16_t  yaw_d10;
    uint8_t  hdop_x10;
    uint8_t  vib_level;         /* 0 = 不可用 */
    uint16_t disp_m_60s;
    uint8_t  ac_category;       /* 0 = unknown，枚举从 1 开始 */
} pk_own_sample_t;

/* 时间修正记录（rec_type=1）。ts_ms 是**校时后**的时间戳；prev_ts_ms
 * 是校时前的时钟读数。phase 字段无意义，编码时固定写 0。 */
typedef enum {
    PK_OWN_SYNC_SOURCE_GPS = 1,
    PK_OWN_SYNC_SOURCE_BLE_CTS = 2,
} pk_own_sync_source_t;

typedef struct {
    uint64_t ts_ms;             /* 校时后 */
    uint8_t  sync_source;       /* pk_own_sync_source_t */
    uint8_t  sync_reason;
    uint64_t prev_ts_ms;        /* 校时前 */
} pk_own_time_sync_t;

void pk_own_sample_encode(const pk_own_sample_t *rec, uint8_t buf[PK_OWN_RECORD_LEN]);
void pk_own_sample_decode(const uint8_t buf[PK_OWN_RECORD_LEN], pk_own_sample_t *out);

void pk_own_time_sync_encode(const pk_own_time_sync_t *rec, uint8_t buf[PK_OWN_RECORD_LEN]);
void pk_own_time_sync_decode(const uint8_t buf[PK_OWN_RECORD_LEN], pk_own_time_sync_t *out);

/* 偏移 8 的 rec_type，own.trk 版本（同 pk_trk_rec_type_peek 但偏移不同，
 * 不能共用）。 */
uint8_t pk_own_rec_type_peek(const uint8_t buf[PK_OWN_RECORD_LEN]);

#ifdef __cplusplus
}
#endif
