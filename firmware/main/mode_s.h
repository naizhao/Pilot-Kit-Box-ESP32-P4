/*
 * mode_s.h — Mode-S 帧解码（56/112-bit → 字段）的公共接口。
 *
 * 归属链：antirez/Malcolm-Robb dump1090 → naizhao/esp32-rtl-sdr 的
 * mode-s.{c,h}（本仓库 components/esp32-rtl-sdr，2026-09-05 复制到 main/）。
 *
 * 上游许可（2026-09-05 审计，见 docs/internal/firmware-v3v4/PLAN.md §11 R5）：
 *   Copyright (c) 2012, Salvatore Sanfilippo <antirez@gmail.com>
 *   All rights reserved.
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are met:
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 *
 * 1090 数据入口已改 RP2040 解调 + UART（PLAN.md §6.6）：本文件只保留
 * mode_s_decode / mode_s_checksum 路径。IQ 时代的 mode_s_detect /
 * mode_s_compute_magnitude_vector / maglut（66,564 B 静态表）已随
 * esp32-rtl-sdr 目录删除一并移除（2026-09-05，R10 闭环）。
 */
#pragma once

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>

#define MODE_S_ICAO_CACHE_LEN 1024 // Power of two required
#define MODE_S_LONG_MSG_BYTES (112 / 8)
#define MODE_S_UNIT_FEET 0
#define MODE_S_UNIT_METERS 1

// Program state
typedef struct
{
    // Internal state
    uint32_t icao_cache[sizeof(uint32_t) * MODE_S_ICAO_CACHE_LEN * 2]; // Recently seen ICAO addresses cache

    // Configuration
    int fix_errors; // Single bit error correction if true
    int aggressive; // Aggressive detection algorithm
    int check_crc;  // Only display messages with good CRC
} mode_s_t;

// The struct we use to store information about a decoded message
struct mode_s_msg
{
    // Generic fields
    unsigned char msg[MODE_S_LONG_MSG_BYTES]; // Binary message
    int msgbits;                              // Number of bits in message
    int msgtype;                              // Downlink format #
    int crcok;                                // True if CRC was valid
    uint32_t crc;                             // Message CRC
    int errorbit;                             // Bit corrected. -1 if no bit corrected.
    int aa1, aa2, aa3;                        // ICAO Address bytes 1 2 and 3
    int phase_corrected;                      // True if phase correction was applied.

    // DF 11
    int ca; // Responder capabilities.

    // DF 17
    int metype; // Extended squitter message type.
    int mesub;  // Extended squitter message subtype.
    int heading_is_valid;
    int heading;
    int aircraft_type;
    int fflag;            // 1 = Odd, 0 = Even CPR message.
    int tflag;            // UTC synchronized?
    int raw_latitude;     // Non decoded latitude
    int raw_longitude;    // Non decoded longitude
    char flight[9];       // 8 chars flight number.
    int ew_dir;           // 0 = East, 1 = West.
    int ew_velocity;      // E/W velocity.
    int ns_dir;           // 0 = North, 1 = South.
    int ns_velocity;      // N/S velocity.
    int vert_rate_source; // Vertical rate source.
    int vert_rate_sign;   // Vertical rate sign.
    int vert_rate;        // Vertical rate.
    int velocity;         // Computed from EW and NS velocity.

    // DF 17, ME type 5-8: Surface Position Message. Ground frames carry no
    // altitude (those bits are reused for movement/track); fflag/raw_latitude/
    // raw_longitude above are shared with the airborne branch (same bit
    // offsets in the ME field) and are populated for surface frames too.
    int surface_movement_raw;   // Raw 7-bit MOV field, 0-127.
    double surface_ground_speed; // Decoded ground speed in knots, -1 if the
                                  // MOV field is 0 (no information available).
    int surface_track_valid;    // Status bit S: 1 = surface_track is valid.
    int surface_track_raw;      // Raw 7-bit TRK field, 0-127.
    double surface_track;       // Decoded ground track, degrees (0-360, true
                                 // north = 0), meaningful only if
                                 // surface_track_valid is set.

    // DF4, DF5, DF20, DF21
    int fs;       // Flight status for DF4,5,20,21
    int dr;       // Request extraction of downlink request.
    int um;       // Request extraction of downlink request.
    int identity; // 13 bits identity (Squawk).

    // Fields used by multiple message types.
    int altitude, unit;
};

void mode_s_init(mode_s_t *self);
void mode_s_decode(mode_s_t *self, struct mode_s_msg *mm, unsigned char *msg);
uint32_t mode_s_checksum(unsigned char *msg, int bits);   /* 尾 24 位不计入 */