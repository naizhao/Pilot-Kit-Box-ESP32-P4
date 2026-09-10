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
 *   utc_ok           — UTC OK flag (Status Byte 2, bit 0)
 *   uat_timestamp_s  — seconds-since-midnight UTC (0..86400)
 *   msg_count_uplink — running count of received uplink frames (0..31)
 *   msg_count_basic_long — running count of basic/long ADS-B frames (0..1023)
 *
 * Status Byte 1 bit 0 ("UAT Initialized") is set to ONE by the encoder
 * unconditionally: ICD §3.1.1 h) requires it in ALL Heartbeat messages.
 * Despite the name, the bit is the GDL90 interface-initialised talkback
 * and says nothing about UAT receiver capability. (2026-09-07: a branch
 * briefly cleared it via a `uat_initialised` parameter to "retire fake
 * UAT capability" — a misreading of the bit name; reverted, and the
 * trap-named parameter removed.)
 *
 * The Heartbeat is the EFB's keep-alive: most apps stop displaying the
 * receiver if no Heartbeat arrives for >5 s, so this MUST be emitted
 * once per second by the BLE task irrespective of traffic activity.
 */
size_t gdl90_encode_heartbeat(uint8_t *out, size_t out_cap,
                              bool gps_valid,
                              bool utc_ok,
                              uint32_t uat_timestamp_s,
                              uint8_t msg_count_uplink,
                              uint16_t msg_count_basic_long);

/*
 * Encode an Ownship (msg ID 0x0A) or Traffic (msg ID 0x14) report.
 * Both message types use the identical 27-byte payload format; pass
 * `is_ownship = true` to set the ID byte to GDL90_ID_OWNSHIP.
 *
 * `callsign` is NOT assumed NUL-terminated: only the first
 * `callsign_len` bytes are read. Bytes beyond the length (or a NUL
 * inside it) are emitted as spaces, per the 8-char field format.
 *
 * The function clamps each input to its valid range and substitutes
 * the spec's "no data" sentinels when a have_xxx flag is false:
 * horizontal velocity 0xFFF, vertical velocity 0x800, altitude 0xFFF,
 * NIC/NACp 0 with a zero lat/lon for "no position".
 *
 * 有效位是**分开的三支**，不是一个 have_velocity：
 *   have_ground_speed  地速。false → 水平速度 0xFFF（未知），不是 0 kt。
 *   have_track         地面航迹。false → Misc 的 tt 位为 00（航迹无效）且
 *                      byte16 置 0；tt=00 时接收端不得读 byte16——否则那个
 *                      0 会被读成"航迹正北"。
 *   have_vertical_rate 垂速。false → 0x800（未知），不是 0 fpm（平飞）。
 * 地面目标真的会只有其中一个（DF17 metype 5-8 的 MOV=0 或 S=0），空中速度
 * 帧的两个分量也各自会 N/A。用一个复合位串起来，等于让缺的那一个把好的
 * 那一个也带下水，或者反过来给缺的那个编一个 0 出去。
 *
 * have_air_ground / on_ground —— Misc bit3 "Airborne" 是正向断言，协议里
 * 没有"未知"编码。已知在地面 → 0；已知在空中 → 1；**未知 → 1（airborne）**：
 * 两种错法里，报 0 会让 EFB 判定这是地面目标并抑制交通告警（该看见的没看
 * 见），报 1 只是让它照常显示。本机（0x0A）不适用这条降级——未知时整帧不该
 * 发，见 own_ship.h 的 pk_own_gdl90_should_emit()。
 *
 * emitter_category —— ADS-B/GDL90 的发射器类别（0 = 无信息，1 = Light，
 * 7 = Rotorcraft …）。由调用方从目标自报的类别映射，见
 * gdl90_emitter_from_wake()；不得对没报过类别的目标硬编一个 Light。
 *
 * Misc bit2 (Report Status) 恒为 0 = "updated"：我们只发刚解出来的实测值，
 * 从不做航位外推。
 */
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
                            size_t   callsign_len);

/*
 * pk_wake_t（aircraft_state.h 的 ADS-B 类别集 A/B/C）→ GDL90 Emitter
 * Category（ICD §3.5.1.10，与 DO-260B 的类别编码同一张表）。
 *
 * 参数取 int 而不是 pk_wake_t：gdl90.h 是纯线协议头，测试单独编译它时不带
 * firmware/main 的 -I。未知类别（PK_WAKE_NONE）映射到 0 = "no aircraft type
 * information" —— 那正是规范给"没报过"准备的编码，不是 1(Light)。
 */
uint8_t gdl90_emitter_from_wake(int wake);
