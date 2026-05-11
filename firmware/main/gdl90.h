/*
 * gdl90.h — GDL 90 (Garmin Data Link 90) frame encoder.
 *
 * Implements the subset of FAA spec 560-1058-00 Rev A that EFB apps
 * (ForeFlight / Garmin Pilot / Avare / Naviator / Pilot Kit) accept
 * over a BLE notification pipe:
 *
 *   - Message ID 0x00 — Heartbeat (1 Hz)
 *   - Message ID 0x0A — Ownship Report
 *   - Message ID 0x14 — Traffic Report
 *
 * Frame layout produced by each encoder:
 *
 *     0x7E | msg_id | payload... | CRC-LSB | CRC-MSB | 0x7E
 *
 * Byte stuffing (replace each 0x7D / 0x7E by 0x7D 0x5D / 0x7D 0x5E) is
 * applied after CRC, exactly as the spec requires. The encoders return
 * the framed length so callers can pass the buffer straight into a
 * BLE notify or UDP send call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GDL90_FLAG           0x7E
#define GDL90_ESCAPE         0x7D
#define GDL90_ID_HEARTBEAT   0x00
#define GDL90_ID_OWNSHIP     0x0A
#define GDL90_ID_TRAFFIC     0x14

/* Worst-case framed sizes (fully escaped, very rare in practice). */
#define GDL90_MAX_HEARTBEAT  24   /* 1+2*(1+6+2)+1 = 20 — round up */
#define GDL90_MAX_TRAFFIC    64   /* 1+2*(1+27+2)+1 = 62 — round up */
#define GDL90_MAX_FRAME      GDL90_MAX_TRAFFIC

/*
 * Encode a Heartbeat message into `out`. Returns the number of framed
 * bytes written, or 0 if `out_cap` is too small.
 *
 *   gps_valid        — GPS Position Valid flag (Status Byte 1, bit 7)
 *   uat_initialised  — UAT Initialised flag (Status Byte 1, bit 0)
 *   utc_ok           — UTC OK flag (Status Byte 2, bit 0)
 *   uat_timestamp_s  — seconds-since-midnight UTC (0..86400)
 *   msg_count_uplink — running count of received uplink frames (0..31)
 *   msg_count_basic_long — running count of basic/long ADS-B frames (0..1023)
 *
 * The Heartbeat is the EFB's keep-alive: most apps stop displaying the
 * receiver if no Heartbeat arrives for >5 s, so this MUST be emitted
 * once per second by the BLE task irrespective of traffic activity.
 */
size_t gdl90_encode_heartbeat(uint8_t *out, size_t out_cap,
                              bool gps_valid,
                              bool uat_initialised,
                              bool utc_ok,
                              uint32_t uat_timestamp_s,
                              uint8_t msg_count_uplink,
                              uint16_t msg_count_basic_long);

/*
 * Encode an Ownship (msg ID 0x0A) or Traffic (msg ID 0x14) report.
 * Both message types use the identical 27-byte payload format; pass
 * `is_ownship = true` to set the ID byte to GDL90_ID_OWNSHIP.
 *
 * The function clamps each input to its valid range and substitutes
 * the spec's "no data" sentinels (0xFFF for altitude / speed, 0x800
 * for vertical rate, etc.) when have_xxx flags are false.
 */
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
                            const char *callsign);
