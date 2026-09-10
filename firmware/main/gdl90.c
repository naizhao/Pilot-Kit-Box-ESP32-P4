/*
 * gdl90.c — encoder implementation.
 *
 * Cross-checked against:
 *   - FAA 560-1058-00 Rev A
 *   - SoftRF's gdl90.c
 *   - cyoung/stratux gen_gdl90.go
 *
 * The FCS is the ICD's own reference algorithm (FAA 560-1058-00 Rev A
 * §2.2.3): a 256-entry table built once at first use — the table costs
 * 512 B of RAM but the byte-update line the ICD specifies is the
 * table form, which is NOT algebraically equal to a bitwise loop
 * (that mismatch was the original encoder bug; see the gdl90_crc
 * comment below).
 */

#include "gdl90.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

/* pk_wake_t 的取值定义。gdl90_emitter_from_wake() 按枚举名而不是裸数字做
 * 映射：类别表若有增补，改一处就够，而抄一份数字过来的版本会静默漂移。
 * aircraft_state.h 只依赖 stdbool/stdint，不把 IDF 拖进这个纯协议 TU。 */
#include "aircraft_state.h"

/* ------------------------------------------------------------------ */
/* FCS — FAA 560-1058-00 Rev A §2.2.3, reference algorithm, verbatim   */
/* semantics.  Two traps, both cost us a round trip:                   */
/*                                                                     */
/*   1. The ICD's byte update is `crc = Table[crc >> 8] ^ (crc << 8)   */
/*      ^ block[i]` (init 0).  This is NOT algebraically equal to the  */
/*      common bitwise `crc ^= b << 8; 8x shift` loop — our original   */
/*      encoder used that loop and produced non-compliant FCS bytes    */
/*      on the wire.                                                   */
/*                                                                     */
/*   2. NO 0xF0B8 augmentation.  That constant belongs to HDLC/X.25,   */
/*      not GDL90.  An augmentation attempt (2026-09-07) was reverted  */
/*      after golden-vector verification against §2.2.4:               */
/*         [7E 00 81 41 DB D0 08 02 B3 8B 7E]  (FCS 0x8BB3, LSB first) */
/*                                                                     */
/* The table is read-only once built; the BLE emitter task is the      */
/* only caller today, so the lazy init below has no race in practice.  */
/* ------------------------------------------------------------------ */

static uint16_t crc_table[256];
static bool     crc_table_ready;

static void gdl90_crc_init(void)
{
    /* ICD §2.2.3 table init, verbatim. */
    for (int i = 0; i < 256; ++i) {
        uint16_t crc = (uint16_t)(i << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        crc_table[i] = crc;
    }
    crc_table_ready = true;
}

static uint16_t gdl90_crc(const uint8_t *data, size_t len)
{
    if (!crc_table_ready) gdl90_crc_init();
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i)
        crc = (uint16_t)(crc_table[crc >> 8] ^ (uint16_t)(crc << 8) ^ data[i]);
    return crc;
}

/* ------------------------------------------------------------------ */
/* Framing: 0x7E | msg_id | payload | CRC-LSB | CRC-MSB | 0x7E         */
/* with 0x7D / 0x7E inside the frame escaped as 0x7D <byte^0x20>.      */
/* ------------------------------------------------------------------ */

static size_t gdl90_frame(uint8_t *out, size_t out_cap,
                          uint8_t msg_id,
                          const uint8_t *payload, size_t payload_len)
{
    uint8_t tmp[1 + 27 + 2];  /* worst case: traffic = 1 id + 27 payload + 2 CRC */
    if (1 + payload_len + 2 > sizeof(tmp)) return 0;

    size_t tmp_len = 0;
    tmp[tmp_len++] = msg_id;
    memcpy(tmp + tmp_len, payload, payload_len);
    tmp_len += payload_len;

    uint16_t crc = gdl90_crc(tmp, tmp_len);
    tmp[tmp_len++] = (uint8_t)(crc & 0xFF);          /* LSB first */
    tmp[tmp_len++] = (uint8_t)((crc >> 8) & 0xFF);

    size_t n = 0;
    if (n >= out_cap) return 0;
    out[n++] = GDL90_FLAG;
    for (size_t i = 0; i < tmp_len; ++i) {
        uint8_t b = tmp[i];
        if (b == GDL90_FLAG || b == GDL90_ESCAPE) {
            if (n + 2 > out_cap) return 0;
            out[n++] = GDL90_ESCAPE;
            out[n++] = b ^ 0x20;
        } else {
            if (n + 1 > out_cap) return 0;
            out[n++] = b;
        }
    }
    if (n >= out_cap) return 0;
    out[n++] = GDL90_FLAG;
    return n;
}

/* ------------------------------------------------------------------ */
/* Heartbeat (msg ID 0x00, 6-byte payload)                             */
/* ------------------------------------------------------------------ */

size_t gdl90_encode_heartbeat(uint8_t *out, size_t out_cap,
                              bool gps_valid,
                              bool utc_ok,
                              uint32_t uat_timestamp_s,
                              uint8_t msg_count_uplink,
                              uint16_t msg_count_basic_long)
{
    /* Status Byte 1: bit 0 is ONE in ALL Heartbeat messages — ICD
     * §3.1.1 h): "UAT Initialized: This bit is set to ONE in all
     * Heartbeat messages." It is the interface-initialised talkback;
     * despite the name it says nothing about UAT receiver capability.
     * (2026-09-07: a branch briefly cleared this bit to "retire fake
     * UAT capability" — that was a misreading of the bit name;
     * reverted, and the trap-named parameter removed.) Everything else
     * is left at 0 (no maintenance request, no IDENT pressed, etc.). */
    uint8_t status1 = 0x01;
    if (gps_valid) status1 |= (1 << 7);

    /* Status Byte 2: bit 7 carries the MSB of the 17-bit UAT timestamp;
     * bit 0 is UTC OK. We use 17-bit seconds-since-midnight. */
    uint8_t status2 = 0;
    if (uat_timestamp_s & (1U << 16)) status2 |= (1 << 7);
    if (utc_ok)                       status2 |= (1 << 0);

    /* UAT Time Stamp: lower 16 bits, LSB first. */
    uint8_t ts_lsb = (uint8_t)(uat_timestamp_s & 0xFF);
    uint8_t ts_msb = (uint8_t)((uat_timestamp_s >> 8) & 0xFF);

    /* Message Counts (ICD §3.1.4): byte 1 = uplink count (5 bits) in
     * [7:3], bit 2 reserved 0, and the two MSBs of the basic/long
     * count in [1:0]; byte 2 = the lower 8 bits of basic/long. */
    if (msg_count_basic_long > 0x3FF) msg_count_basic_long = 0x3FF;
    if (msg_count_uplink     > 0x1F)  msg_count_uplink     = 0x1F;
    uint8_t mc1 = (uint8_t)(((msg_count_uplink & 0x1F) << 3)
                            | ((msg_count_basic_long >> 8) & 0x03));
    uint8_t mc2 = (uint8_t)(msg_count_basic_long & 0xFF);

    uint8_t payload[6] = { status1, status2, ts_lsb, ts_msb, mc1, mc2 };
    return gdl90_frame(out, out_cap, GDL90_ID_HEARTBEAT, payload, sizeof(payload));
}

/* ------------------------------------------------------------------ */
/* Traffic / Ownship Report (msg ID 0x14 / 0x0A, 27-byte payload)      */
/* ------------------------------------------------------------------ */

/* Pack a signed 24-bit integer in big-endian into out[0..2]. */
static void pack_24(uint8_t *out, int32_t v)
{
    uint32_t u = (uint32_t)v & 0xFFFFFF;
    out[0] = (uint8_t)((u >> 16) & 0xFF);
    out[1] = (uint8_t)((u >> 8)  & 0xFF);
    out[2] = (uint8_t)(u         & 0xFF);
}

/* Encode latitude as 24-bit signed, 180/2^23 deg/LSB. */
static int32_t encode_lat(double deg)
{
    if (deg >  90.0) deg =  90.0;
    if (deg < -90.0) deg = -90.0;
    double v = deg * ((double)(1 << 23) / 180.0);
    int32_t i = (int32_t)floor(v + (v >= 0 ? 0.5 : -0.5));
    if (i >  0x7FFFFF) i =  0x7FFFFF;
    if (i < -0x800000) i = -0x800000;
    return i;
}

static int32_t encode_lon(double deg)
{
    if (deg >  180.0) deg =  180.0;
    if (deg < -180.0) deg = -180.0;
    double v = deg * ((double)(1 << 23) / 180.0);
    int32_t i = (int32_t)floor(v + (v >= 0 ? 0.5 : -0.5));
    if (i >  0x7FFFFF) i =  0x7FFFFF;
    if (i < -0x800000) i = -0x800000;
    return i;
}

uint8_t gdl90_emitter_from_wake(int wake)
{
    /* ICD §3.5.1.10 的 Emitter Category 表。留空的档（8/13/16）在规范里就是
     * unassigned，本项目的类别枚举里也没有对应项。 */
    switch (wake) {
    case PK_WAKE_LIGHT:            return 1;   /* A1 Light                  */
    case PK_WAKE_SMALL:            return 2;   /* A2 Small                  */
    case PK_WAKE_LARGE:            return 3;   /* A3 Large                  */
    case PK_WAKE_HIGH_VORTEX:      return 4;   /* A4 High Vortex Large      */
    case PK_WAKE_HEAVY:            return 5;   /* A5 Heavy                  */
    case PK_WAKE_HIGH_PERF:        return 6;   /* A6 Highly Maneuverable    */
    case PK_WAKE_ROTOR:            return 7;   /* A7 Rotorcraft             */
    case PK_WAKE_GLIDER:           return 9;   /* B1 Glider/sailplane       */
    case PK_WAKE_LTA:              return 10;  /* B2 Lighter than air       */
    case PK_WAKE_PARACHUTE:        return 11;  /* B3 Parachutist            */
    case PK_WAKE_ULTRALIGHT:       return 12;  /* B4 Ultralight/hang glider */
    case PK_WAKE_UAV:              return 14;  /* B6 UAV                    */
    case PK_WAKE_SPACE:            return 15;  /* B7 Space/transatmospheric */
    case PK_WAKE_SURFACE_EMERG:    return 17;  /* C1 Surface — emergency    */
    case PK_WAKE_SURFACE_SERVICE:  return 18;  /* C3 Surface — service      */
    case PK_WAKE_SURFACE_OBSTACLE: return 19;  /* C4..C7 Point obstacle     */
    case PK_WAKE_NONE:
    default:                       return 0;   /* 没报过类别 = 没有信息     */
    }
}

size_t gdl90_encode_traffic(uint8_t *out, size_t out_cap,
                            bool     is_ownship,
                            uint32_t icao24,
                            bool     have_position,
                            double   lat,
                            double   lon,
                            bool     have_altitude,
                            int      altitude_ft,
                            bool     have_ground_speed,
                            int      ground_speed_kt,
                            bool     have_track,
                            int      track_deg,
                            bool     have_vertical_rate,
                            int      vert_rate_fpm,
                            bool     have_air_ground,
                            bool     on_ground,
                            uint8_t  emitter_category,
                            const char *callsign,
                            size_t   callsign_len)
{
    uint8_t p[27];
    memset(p, 0, sizeof(p));

    /* Byte 0: Alert Status (4 bits) | Address Type (4 bits).
     *   alert=0 (no alert), addr=0 (ADS-B with ICAO 24-bit address). */
    p[0] = 0x00;

    /* Bytes 1..3: 24-bit Participant Address (ICAO). */
    p[1] = (uint8_t)((icao24 >> 16) & 0xFF);
    p[2] = (uint8_t)((icao24 >> 8)  & 0xFF);
    p[3] = (uint8_t)(icao24         & 0xFF);

    /* Bytes 4..6: Latitude. */
    if (have_position) {
        pack_24(&p[4], encode_lat(lat));
    } else {
        pack_24(&p[4], 0x000000);  /* spec: 0 means no position */
    }

    /* Bytes 7..9: Longitude. */
    if (have_position) {
        pack_24(&p[7], encode_lon(lon));
    } else {
        pack_24(&p[7], 0x000000);
    }

    /* Bytes 10..11:
     *   bits 31..20 → 12-bit altitude (25 ft resolution, -1000 ft offset)
     *   bits 19..16 → 4-bit Misc indicator
     *
     * Misc (ICD §3.5.1.5) —— 每一位都由入参决定，没有一个常量：
     *   bit 3  Airborne : 1 = airborne, 0 = on ground
     *   bit 2  Report   : 0 = updated, 1 = extrapolated
     *   bits1-0 tt      : 00 = tt 无效, 01 = True Track Angle,
     *                     10 = Magnetic Heading, 11 = True Heading
     *
     * 原实现恒写 0x9（airborne + extrapolated + true track），三处都是凭空
     * 断言：地面上的目标被报成在空中；每一帧实测数据被标成外推；没有航迹的
     * 目标让接收端去读 byte16 里的那个 0，读成"航迹正北"。 */
    uint16_t alt_enc = 0xFFF;  /* invalid */
    if (have_altitude) {
        int v = (altitude_ft + 1000) / 25;
        if (v < 0)        v = 0;
        if (v > 0xFFE)    v = 0xFFE;
        alt_enc = (uint16_t)v;
    }
    uint8_t misc = 0;
    if (have_track) misc |= 0x1;                        /* tt = True Track  */
    /* 空地未知按 airborne 编：见 gdl90.h 的取舍说明（本机不走这条降级）。 */
    if (!have_air_ground || !on_ground) misc |= 0x8;
    p[10] = (uint8_t)((alt_enc >> 4) & 0xFF);
    p[11] = (uint8_t)(((alt_enc & 0x0F) << 4) | misc);

    /* Byte 12: NIC (4 bits) | NACp (4 bits). 有位置时 0x9 / 0x9 = ±30 m，
     * 对 ADS-B/GNSS 来源都合适；没有位置时必须是 0（"unknown"）——给一份
     * 空位置配上 ±30 m 的完好性声明，是在替接收端担保一个不存在的点。 */
    p[12] = have_position ? 0x99 : 0x00;

    /* Bytes 13..15:
     *   bits 23..12 → 12-bit horizontal velocity (kt, 1 kt res, 0xFFF=N/A)
     *   bits 11..0  → 12-bit signed vertical velocity (64 fpm res, 0x800=N/A)
     * 两者各自独立：只有地速没有垂速（ADS-B 地面帧、GPS 兜底本机）是常态。
     */
    uint16_t hv = 0xFFF;
    uint16_t vv = 0x800;
    if (have_ground_speed) {
        int v = ground_speed_kt;
        if (v < 0)        v = 0;
        if (v > 0xFFE)    v = 0xFFE;
        hv = (uint16_t)v;
    }
    if (have_vertical_rate) {
        int vr = vert_rate_fpm / 64;
        if (vr >  0x1FE) vr =  0x1FE;
        if (vr < -0x1FE) vr = -0x1FE;
        vv = (uint16_t)(vr & 0xFFF);
    }
    p[13] = (uint8_t)((hv >> 4) & 0xFF);
    p[14] = (uint8_t)(((hv & 0x0F) << 4) | ((vv >> 8) & 0x0F));
    p[15] = (uint8_t)(vv & 0xFF);

    /* Byte 16: Track / Heading. 360/256 deg/LSB. tt=00 时接收端本不该读它，
     * 但仍然置 0，免得别人的宽容解析读到上一帧的残留。 */
    if (have_track) {
        int t = ((track_deg % 360) + 360) % 360;
        p[16] = (uint8_t)((t * 256 + 180) / 360);
    } else {
        p[16] = 0;
    }

    /* Byte 17: Emitter Category —— 由调用方给出，见 gdl90_emitter_from_wake。
     * 原来恒写 1(Light)：没报过类别的目标、直升机、重型机在 EFB 上全成了
     * 轻型机，尾流间隔与图标都跟着错。 */
    p[17] = emitter_category;

    /* Bytes 18..25: Callsign, 8 ASCII chars padded with space.  Only
     * the first callsign_len bytes of callsign are readable — it is
     * NOT guaranteed NUL-terminated (ble_gatt.c passes a 1-byte ""),
     * so a length bound is mandatory here; a NUL inside the range
     * simply pads the rest with spaces. */
    for (int i = 0; i < 8; ++i) {
        char c = (callsign && (size_t)i < callsign_len) ? callsign[i] : ' ';
        if (c == '\0') c = ' ';
        p[18 + i] = (uint8_t)toupper((unsigned char)c);
    }

    /* Byte 26: Emergency/Priority Code (4 bits) | Spare (4 bits).
     *   0x00 = no emergency. */
    p[26] = 0x00;

    uint8_t msg_id = is_ownship ? GDL90_ID_OWNSHIP : GDL90_ID_TRAFFIC;
    return gdl90_frame(out, out_cap, msg_id, p, sizeof(p));
}
