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
