#pragma once

/* uat_decode.h —— 978 MHz UAT 解码纯单元（WP-E Task 1）。
 *
 * 职责：dump978 边界之后的整条软件链——上行 552 B 交织帧的解交织 +
 * RS(92,72)×6 纠错 + 432 B 消息层（站点坐标/标志/信息帧/FIS-B APDU 头），
 * 以及下行 48 B 帧的 RS 长/短帧判别纠错 + 头部字段。每个参数与判据的
 * 出处见 docs/internal/firmware-v3v4/UAT-PROTOCOL-FACTS.md（下称"事实卡"，
 * 引用其 §N）；本文件与 .c 里的 [MUT]/[FA]/[STX] 代号同事实卡 §0。
 *
 * 纯单元合同（同 gps_nmea.c / power_sy6970.c 模式）：
 *   - 无 malloc / 无 OS / 无 ESP-IDF 依赖，host 单测
 *     firmware/test/test_uat_decode.c 直接编译本模块；
 *   - 无内部静态可写状态跨帧残留（uat_fec_init 的表只写一次），
 *     可在任意线程/裸机环境调用；
 *   - 失败路径绝不改写输出缓冲（dump978 对下行"长帧失败重试短帧"
 *     依赖的同一契约，事实卡 §2）。
 *
 * 边界裁决（事实卡 §7）：CC1312R 交付 552 B 含 RS 校验字的交织帧，
 * RS/解交织/消息层全在本单元。T3/T4 想把调用点挪到 slave 侧时，
 * 本合同不变。
 *
 * 全零容错（事实卡 §8）：全 0x00 输入一律拒绝——SPI 链路的合法
 * "无帧"事务就是全零（PROTOCOL_RP2040_CC1312R_SPI.md §2.3 规则 5），
 * 而全零在 RS 数学上恰是合法码字，这道闸必须在 RS 之前。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── 帧尺寸（事实卡 §1；单位字节）─────────────────────────────── */
#define UAT_UPLINK_FRAME_BYTES 552 /* 6 块 × 92（含各块 20 B RS 校验） */
#define UAT_UPLINK_DATA_BYTES 432  /* 6 块 × 72 净数据                  */
#define UAT_UPLINK_APP_BYTES 424   /* 432 − 8 B 帧头                    */
#define UAT_UPLINK_MAX_INFO_FRAMES (UAT_UPLINK_APP_BYTES / 6) /* 70，[MUT] uat_decode.h:60 */

#define UAT_DL_SHORT_FRAME_BYTES 30 /* 下行短帧总长（18 B 数据 + 12 B RS） */
#define UAT_DL_SHORT_DATA_BYTES 18
#define UAT_DL_LONG_FRAME_BYTES 48 /* 下行长帧总长（34 B 数据 + 14 B RS） */
#define UAT_DL_LONG_DATA_BYTES 34

/* ── 上行解码输出 ──────────────────────────────────────────────── */

/* FIS-B APDU 头字段（事实卡 §4；仅 type==0 且长度足够时有效）。 */
typedef struct {
    uint16_t product_id;         /* 11 bit */
    uint8_t flags;               /* bit3=A bit2=G bit1=P bit0=S */
    bool monthday_valid;         /* t_opt 2/3 时为真               */
    bool seconds_valid;          /* t_opt 1/3 时为真               */
    uint8_t month, day;          /* monthday_valid 才有意义         */
    uint8_t hours, minutes;
    uint8_t seconds;             /* seconds_valid 才有意义          */
    const uint8_t *payload;      /* 指向 data_out 内部，随调用方    */
    uint16_t payload_len;        /* 载荷 = 信息帧去 APDU 头         */
} uat_fisb_t;

/* 一个信息帧：length/type + 原始字节 + （若是 FIS-B）APDU 头解包。
 * data 指向本次调用 data_out 缓冲内部，生命周期同该缓冲。 */
typedef struct {
    uint16_t length;   /* 9 bit，信息帧载荷字节数（不含 2 B 头） */
    uint8_t type;      /* 4 bit：0=FIS-B APDU，15=TIS-B/ADS-R 状态 */
    const uint8_t *data;
    bool is_fisb;
    uat_fisb_t fisb;
} uat_info_frame_t;

typedef struct {
    const uint8_t *raw_data; /* 完整纠错后 432 B，生命周期同 data_out */
    /* 坐标以 24 bit 原始码给出（整数，换算不丢精度）：
     * lat = raw·360/2²⁴，>90 再 −180；lon 同式，>180 再 −360
     * （事实卡 §4）。deg_e6 为换算后的百万分之一度。 */
    uint32_t raw_lat, raw_lon;
    int32_t lat_deg_e6, lon_deg_e6;
    bool position_valid;
    bool utc_coupled;
    bool app_data_valid;
    uint8_t slot_id;      /* 5 bit */
    uint8_t tisb_site_id; /* 4 bit  */
    uint8_t num_info_frames;
    uat_info_frame_t info[UAT_UPLINK_MAX_INFO_FRAMES];
} uat_uplink_t;

/* 下行头部（事实卡 §5）。 */
typedef struct {
    uint8_t mdb_type;       /* d0>>3，5 bit；==0 即短帧族 */
    uint8_t addr_qualifier; /* d0&7                      */
    uint32_t address;       /* d1..d3，24 bit            */
} uat_dl_hdr_t;

/* 下行地址限定符。只有 ADS-B ICAO 与 TIS-B ICAO 能安全并入以 ICAO24
 * 为键的现有 aircraft_state；track-file/车辆/固定信标地址不能伪装成
 * ICAO，否则地面站复用 track id 时会把不同目标写进同一个槽。 */
typedef enum {
    UAT_ADDR_ADSB_ICAO = 0,
    UAT_ADDR_NATIONAL = 1,
    UAT_ADDR_TISB_ICAO = 2,
    UAT_ADDR_TISB_TRACK = 3,
    UAT_ADDR_VEHICLE = 4,
    UAT_ADDR_FIXED_BEACON = 5,
    UAT_ADDR_RESERVED_6 = 6,
    UAT_ADDR_RESERVED_7 = 7,
} uat_addr_qualifier_t;

typedef enum {
    UAT_ALT_NONE = 0,
    UAT_ALT_BARO,
    UAT_ALT_GEO,
} uat_altitude_source_t;

typedef enum {
    UAT_AG_SUBSONIC = 0,
    UAT_AG_SUPERSONIC = 1,
    UAT_AG_GROUND = 2,
    UAT_AG_RESERVED = 3,
} uat_air_ground_t;

/* 已纠错的 UAT ADS-B/TIS-B Message Data Block。坐标保留为 e-6 度整数，
 * 避免 P4 与 host 因 double 舍入产生不同测试结果。每个数值都有独立 valid，
 * 0 高度/0 速度不能拿来充当“无数据”哨兵。 */
typedef struct uat_aircraft_msg {
    uint8_t mdb_type;
    uat_addr_qualifier_t addr_qualifier;
    uint32_t address;
    bool address_is_icao;

    bool has_state_vector;
    bool has_mode_status;
    bool has_aux_state;
    uint8_t nic;
    bool position_valid;
    int32_t lat_deg_e6;
    int32_t lon_deg_e6;
    bool altitude_valid;
    int32_t altitude_ft;
    uat_altitude_source_t altitude_source;
    uat_air_ground_t air_ground;
    bool ns_velocity_valid;
    int16_t ns_velocity_kt;
    bool ew_velocity_valid;
    int16_t ew_velocity_kt;
    bool speed_valid;
    uint16_t speed_kt;
    bool track_valid;
    uint16_t track_deg;
    bool track_is_heading;
    bool track_is_magnetic;
    bool vertical_rate_valid;
    int16_t vertical_rate_fpm;
    uat_altitude_source_t vertical_rate_source;
    bool utc_coupled;
    uint8_t tisb_site_id;

    uint8_t emitter_category;
    char callsign[9];
    bool callsign_valid;
    bool callsign_is_flight_id;
    uint8_t emergency_status;
    uint8_t uat_version;
    uint8_t sil;
    uint8_t nac_p;
    uint8_t nac_v;
    bool nic_baro;
    bool has_cdti;
    bool has_acas;
    bool acas_ra_active;
    bool ident_active;
    bool atc_services;

    bool aux_altitude_valid;
    int32_t aux_altitude_ft;
    uat_altitude_source_t aux_altitude_source;
} uat_aircraft_msg_t;

/* ── API ───────────────────────────────────────────────────────── */

/* 构建 GF(256)/RS 表。幂等；必须在任何 decode 前调用一次
 * （host 测试与目标端各自在首个消费者处调；事实卡 §2）。 */
void uat_fec_init(void);

/* 解码一帧上行。
 * frame：552 B 解调帧（交织、含 RS 校验字，事实卡 §1/§3）。
 * data_out：接收 432 B 净数据（信息帧的 data 指针指向它）。
 * out：消息层输出，可为 NULL（只要 FEC+数据）。
 * rs_corrected：可选，回写全帧纠正的字节总数（≤60）。
 * 返回 false：RS 不可纠 / 全零帧。此时 data_out 不被改写。 */
bool uat_uplink_decode(const uint8_t frame[UAT_UPLINK_FRAME_BYTES],
                       uint8_t data_out[UAT_UPLINK_DATA_BYTES],
                       uat_uplink_t *out, uint8_t *rs_corrected);

/* 解码一帧下行（先试长帧再试短帧，判据事实卡 §2）。
 * frame：48 B——即使短帧也必须给满 48 B（短帧 RS 只消费前 30 B，
 * 但长帧先试会读全 48 B）。
 * data_out：接收净数据；短帧时仅前 18 B 有效，长帧 34 B。
 * 返回：0=拒绝（含全零），1=短帧，2=长帧。帧型编号沿用 dump978
 * （fec.c:38-53 的 1/2），但失败值是本仓有意偏离：上游返回 -1，
 * 本仓以 0 为失败哨兵，与 uat_uplink_decode 的 false 失败语义对齐，
 * 调用方判 0 即拒、判真值即帧型。拒绝时 data_out/hdr 不被改写。 */
int uat_downlink_decode(const uint8_t frame[UAT_DL_LONG_FRAME_BYTES],
                        uint8_t data_out[UAT_DL_LONG_DATA_BYTES],
                        uat_dl_hdr_t *hdr, uint8_t *rs_corrected);

/* 解析已完成 RS 纠错的 18 B short / 34 B long Message Data Block。
 * 长度必须与 MDB type 所需字段相容；失败时 out 保持不变。 */
bool uat_downlink_parse_aircraft(const uint8_t *data, size_t data_len,
                                 uat_aircraft_msg_t *out);
