/*
 * gdl90.c — encoder implementation.
 *
 * Cross-checked against:
 *   - FAA 560-1058-00 Rev A
 *   - SoftRF's gdl90.c
 *   - cyoung/stratux gen_gdl90.go
 *
 * The CRC is computed bytewise without a lookup table — the encoder
 * only runs once per aircraft per second (plus heartbeat 1 Hz), so
 * the constant-factor cost is invisible against the BLE I/O budget,
 * and skipping the table saves ~512 B of flash + simplifies audit.
 */

#include "gdl90.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CRC-16-CCITT (poly 0x1021, init 0x0000, no reflect, no xorout).    */
/* ------------------------------------------------------------------ */

static uint16_t gdl90_crc(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
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
                              bool uat_initialised,
                              bool utc_ok,
                              uint32_t uat_timestamp_s,
                              uint8_t msg_count_uplink,
                              uint16_t msg_count_basic_long)
{
    /* Status Byte 1: GPS valid + UAT initialised. Everything else is
     * left at 0 (no maintenance request, no IDENT pressed, etc.). */
    uint8_t status1 = 0;
    if (gps_valid)        status1 |= (1 << 7);
    if (uat_initialised)  status1 |= (1 << 0);

    /* Status Byte 2: bit 7 carries the MSB of the 17-bit UAT timestamp;
     * bit 0 is UTC OK. We use 17-bit seconds-since-midnight. */
    uint8_t status2 = 0;
    if (uat_timestamp_s & (1U << 16)) status2 |= (1 << 7);
    if (utc_ok)                       status2 |= (1 << 0);

    /* UAT Time Stamp: lower 16 bits, LSB first. */
    uint8_t ts_lsb = (uint8_t)(uat_timestamp_s & 0xFF);
    uint8_t ts_msb = (uint8_t)((uat_timestamp_s >> 8) & 0xFF);

    /* Message Counts: byte 1 carries 5 bits of basic-long uplink count
     * (bits 6..2) and 5 bits of UAT uplink count (bits 4..0); byte 2
     * carries the lower 8 bits of basic-long. We approximate by packing
     * the basic-long count straight and clamping uplink to 5 bits. */
    if (msg_count_basic_long > 0x3FF) msg_count_basic_long = 0x3FF;
    if (msg_count_uplink     > 0x1F)  msg_count_uplink     = 0x1F;
    uint8_t mc1 = ((msg_count_basic_long >> 8) & 0x03) << 5 | (msg_count_uplink & 0x1F);
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

size_t gdl90_encode_traffic(uint8_t *out, size_t out_cap,
                            bool     is_ownship,
                            uint32_t icao24,
                            bool     have_position,
                            double   lat,
                            double   lon,
                            bool     have_altitude,
                            int      altitude_ft,
                            bool     have_velocity,
                            int      track_deg,
                            int      ground_speed_kt,
                            int      vert_rate_fpm,
                            const char *callsign)
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
     *   bits 19..16 → 4-bit Misc indicator (NIC-Baro etc.); set to 0x9
     *                 ("True Track Angle + airborne + report extrapolated")
     *                 for traffic reports — matches what Stratux emits. */
    uint16_t alt_enc = 0xFFF;  /* invalid */
    if (have_altitude) {
        int v = (altitude_ft + 1000) / 25;
        if (v < 0)        v = 0;
        if (v > 0xFFE)    v = 0xFFE;
        alt_enc = (uint16_t)v;
    }
    p[10] = (uint8_t)((alt_enc >> 4) & 0xFF);
    p[11] = (uint8_t)(((alt_enc & 0x0F) << 4) | 0x9);

    /* Byte 12: NIC (4 bits) | NACp (4 bits). 0x9 / 0x9 = ±30 m, both
     * fine for ADS-B-derived data. */
    p[12] = 0x99;

    /* Bytes 13..15:
     *   bits 23..12 → 12-bit horizontal velocity (kt, 1 kt res, 0xFFF=N/A)
     *   bits 11..0  → 12-bit signed vertical velocity (64 fpm res, 0x800=N/A)
     */
    uint16_t hv = 0xFFF;
    uint16_t vv = 0x800;
    if (have_velocity) {
        int v = ground_speed_kt;
        if (v < 0)        v = 0;
        if (v > 0xFFE)    v = 0xFFE;
        hv = (uint16_t)v;

        int vr = vert_rate_fpm / 64;
        if (vr >  0x1FE) vr =  0x1FE;
        if (vr < -0x1FE) vr = -0x1FE;
        vv = (uint16_t)(vr & 0xFFF);
    }
    p[13] = (uint8_t)((hv >> 4) & 0xFF);
    p[14] = (uint8_t)(((hv & 0x0F) << 4) | ((vv >> 8) & 0x0F));
    p[15] = (uint8_t)(vv & 0xFF);

    /* Byte 16: Track / Heading. 360/256 deg/LSB. */
    if (have_velocity) {
        int t = ((track_deg % 360) + 360) % 360;
        p[16] = (uint8_t)((t * 256 + 180) / 360);
    } else {
        p[16] = 0;
    }

    /* Byte 17: Emitter Category. 1 = Light aircraft (fits most GA targets). */
    p[17] = 1;

    /* Bytes 18..25: Callsign, 8 ASCII chars padded with space. */
    for (int i = 0; i < 8; ++i) {
        char c = (callsign && callsign[i]) ? callsign[i] : ' ';
        if (c == '\0') {
            /* Pad remainder with spaces. */
            for (int j = i; j < 8; ++j) p[18 + j] = ' ';
            break;
        }
        p[18 + i] = (uint8_t)toupper((unsigned char)c);
    }

    /* Byte 26: Emergency/Priority Code (4 bits) | Spare (4 bits).
     *   0x00 = no emergency. */
    p[26] = 0x00;

    uint8_t msg_id = is_ownship ? GDL90_ID_OWNSHIP : GDL90_ID_TRAFFIC;
    return gdl90_frame(out, out_cap, msg_id, p, sizeof(p));
}
