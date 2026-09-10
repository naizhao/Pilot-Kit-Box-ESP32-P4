/*
 * mode_s.c — Mode-S 帧解码（56/112-bit → 字段）。
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

#include "mode_s.h"
#include <time.h>  /* time() — used by the ICAO address cache TTL */

#define MODE_S_PREAMBLE_US 8 // microseconds
#define MODE_S_LONG_MSG_BITS 112
#define MODE_S_SHORT_MSG_BITS 56
#define MODE_S_FULL_LEN (MODE_S_PREAMBLE_US + MODE_S_LONG_MSG_BITS)

#define MODE_S_ICAO_CACHE_TTL 60 // Time to live of cached addresses.

// =============================== Initialization ===========================

void mode_s_init(mode_s_t *self)
{
    self->fix_errors = 1;
    self->check_crc = 1;
    self->aggressive = 0;

    // Allocate the ICAO address cache. We use two uint32_t for every entry
    // because it's a addr / timestamp pair for every entry
    memset(&self->icao_cache, 0, sizeof(self->icao_cache));

}

// ===================== Mode S detection and decoding  =====================

// Parity table for MODE S Messages.
//
// The table contains 112 elements, every element corresponds to a bit set in
// the message, starting from the first bit of actual data after the preamble.
//
// For messages of 112 bit, the whole table is used. For messages of 56 bits
// only the last 56 elements are used.
//
// The algorithm is as simple as xoring all the elements in this table for
// which the corresponding bit on the message is set to 1.
//
// The latest 24 elements in this table are set to 0 as the checksum at the end
// of the message should not affect the computation.
//
// Note: this function can be used with DF11 and DF17, other modes have the CRC
// xored with the sender address as they are reply to interrogations, but a
// casual listener can't split the address from the checksum.
uint32_t mode_s_checksum_table[] = {
    0x3935ea, 0x1c9af5, 0xf1b77e, 0x78dbbf, 0xc397db, 0x9e31e9, 0xb0e2f0, 0x587178,
    0x2c38bc, 0x161c5e, 0x0b0e2f, 0xfa7d13, 0x82c48d, 0xbe9842, 0x5f4c21, 0xd05c14,
    0x682e0a, 0x341705, 0xe5f186, 0x72f8c3, 0xc68665, 0x9cb936, 0x4e5c9b, 0xd8d449,
    0x939020, 0x49c810, 0x24e408, 0x127204, 0x093902, 0x049c81, 0xfdb444, 0x7eda22,
    0x3f6d11, 0xe04c8c, 0x702646, 0x381323, 0xe3f395, 0x8e03ce, 0x4701e7, 0xdc7af7,
    0x91c77f, 0xb719bb, 0xa476d9, 0xadc168, 0x56e0b4, 0x2b705a, 0x15b82d, 0xf52612,
    0x7a9309, 0xc2b380, 0x6159c0, 0x30ace0, 0x185670, 0x0c2b38, 0x06159c, 0x030ace,
    0x018567, 0xff38b7, 0x80665f, 0xbfc92b, 0xa01e91, 0xaff54c, 0x57faa6, 0x2bfd53,
    0xea04ad, 0x8af852, 0x457c29, 0xdd4410, 0x6ea208, 0x375104, 0x1ba882, 0x0dd441,
    0xf91024, 0x7c8812, 0x3e4409, 0xe0d800, 0x706c00, 0x383600, 0x1c1b00, 0x0e0d80,
    0x0706c0, 0x038360, 0x01c1b0, 0x00e0d8, 0x00706c, 0x003836, 0x001c1b, 0xfff409,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000};

uint32_t mode_s_checksum(unsigned char *msg, int bits)
{
    uint32_t crc = 0;
    int offset = (bits == 112) ? 0 : (112 - 56);
    int j;

    for (j = 0; j < bits; j++)
    {
        int byte = j / 8;
        int bit = j % 8;
        int bitmask = 1 << (7 - bit);

        // If bit is set, xor with corresponding table entry.
        if (msg[byte] & bitmask)
            crc ^= mode_s_checksum_table[j + offset];
    }
    return crc; // 24 bit checksum.
}

// Given the Downlink Format (DF) of the message, return the message length in
// bits.
//
// DF18（TIS-B / ADS-R / 非应答机 ADS-B）与 DF24-31（Comm-D ELM）原本漏在
// 短帧一侧：112 bit 的帧按 56 bit 去算校验和，CRC 永远对不上，整类报文在
// modes_ingest 的 CRC 门那里被静默丢掉——地面站转播的 TIS-B 目标一架都收
// 不到，而计数器只会显示"坏 CRC 帧数"在涨。
int mode_s_msg_len_by_type(int type)
{
    if (type == 16 || type == 17 || type == 18 ||
        type == 19 || type == 20 ||
        type == 21 || type >= 24)
        return MODE_S_LONG_MSG_BITS;
    else
        return MODE_S_SHORT_MSG_BITS;
}

// Try to fix single bit errors using the checksum. On success modifies the
// original buffer with the fixed version, and returns the position of the
// error bit. Otherwise if fixing failed -1 is returned.
int fix_single_bit_errors(unsigned char *msg, int bits)
{
    int j;
    unsigned char aux[MODE_S_LONG_MSG_BITS / 8];

    for (j = 0; j < bits; j++)
    {
        int byte = j / 8;
        int bitmask = 1 << (7 - (j % 8));
        uint32_t crc1, crc2;

        memcpy(aux, msg, bits / 8);
        aux[byte] ^= bitmask; // Flip j-th bit.

        crc1 = ((uint32_t)aux[(bits / 8) - 3] << 16) |
               ((uint32_t)aux[(bits / 8) - 2] << 8) |
               (uint32_t)aux[(bits / 8) - 1];
        crc2 = mode_s_checksum(aux, bits);

        if (crc1 == crc2)
        {
            // The error is fixed. Overwrite the original buffer with the
            // corrected sequence, and returns the error bit position.
            memcpy(msg, aux, bits / 8);
            return j;
        }
    }
    return -1;
}

// Similar to fix_single_bit_errors() but try every possible two bit
// combination. This is very slow and should be tried only against DF17
// messages that don't pass the checksum, and only in Aggressive Mode.
int fix_two_bits_errors(unsigned char *msg, int bits)
{
    int j, i;
    unsigned char aux[MODE_S_LONG_MSG_BITS / 8];

    for (j = 0; j < bits; j++)
    {
        int byte1 = j / 8;
        int bitmask1 = 1 << (7 - (j % 8));

        // Don't check the same pairs multiple times, so i starts from j+1
        for (i = j + 1; i < bits; i++)
        {
            int byte2 = i / 8;
            int bitmask2 = 1 << (7 - (i % 8));
            uint32_t crc1, crc2;

            memcpy(aux, msg, bits / 8);

            aux[byte1] ^= bitmask1; // Flip j-th bit.
            aux[byte2] ^= bitmask2; // Flip i-th bit.

            crc1 = ((uint32_t)aux[(bits / 8) - 3] << 16) |
                   ((uint32_t)aux[(bits / 8) - 2] << 8) |
                   (uint32_t)aux[(bits / 8) - 1];
            crc2 = mode_s_checksum(aux, bits);

            if (crc1 == crc2)
            {
                // The error is fixed. Overwrite the original buffer with the
                // corrected sequence, and returns the error bit position.
                memcpy(msg, aux, bits / 8);
                // We return the two bits as a 16 bit integer by shifting 'i'
                // on the left. This is possible since 'i' will always be
                // non-zero because i starts from j+1.
                return j | (i << 8);
            }
        }
    }
    return -1;
}

// Hash the ICAO address to index our cache of MODE_S_ICAO_CACHE_LEN elements,
// that is assumed to be a power of two.
uint32_t icao_cache_has_addr(uint32_t a)
{
    // The following three rounds wil make sure that every bit affects every
    // output bit with ~ 50% of probability.
    a = ((a >> 16) ^ a) * 0x45d9f3b;
    a = ((a >> 16) ^ a) * 0x45d9f3b;
    a = ((a >> 16) ^ a);
    return a & (MODE_S_ICAO_CACHE_LEN - 1);
}

// Add the specified entry to the cache of recently seen ICAO addresses. Note
// that we also add a timestamp so that we can make sure that the entry is only
// valid for MODE_S_ICAO_CACHE_TTL seconds.
void add_recently_seen_icao_addr(mode_s_t *self, uint32_t addr)
{
    uint32_t h = icao_cache_has_addr(addr);
    self->icao_cache[h * 2] = addr;
    self->icao_cache[h * 2 + 1] = (uint32_t)time(NULL);
}

// Returns 1 if the specified ICAO address was seen in a DF format with proper
// checksum (not xored with address) no more than * MODE_S_ICAO_CACHE_TTL
// seconds ago. Otherwise returns 0.
int icao_addr_was_recently_seen(mode_s_t *self, uint32_t addr)
{
    uint32_t h = icao_cache_has_addr(addr);
    uint32_t a = self->icao_cache[h * 2];
    int32_t t = self->icao_cache[h * 2 + 1];

    return a && a == addr && time(NULL) - t <= MODE_S_ICAO_CACHE_TTL;
}

// If the message type has the checksum xored with the ICAO address, try to
// brute force it using a list of recently seen ICAO addresses.
//
// Do this in a brute-force fashion by xoring the predicted CRC with the
// address XOR checksum field in the message. This will recover the address: if
// we found it in our cache, we can assume the message is ok.
//
// This function expects mm->msgtype and mm->msgbits to be correctly populated
// by the caller.
//
// On success the correct ICAO address is stored in the mode_s_msg structure in
// the aa3, aa2, and aa1 fiedls.
//
// If the function successfully recovers a message with a correct checksum it
// returns 1. Otherwise 0 is returned.
int brute_force_ap(mode_s_t *self, unsigned char *msg, struct mode_s_msg *mm)
{
    unsigned char aux[MODE_S_LONG_MSG_BYTES];
    int msgtype = mm->msgtype;
    int msgbits = mm->msgbits;

    if (msgtype == 0 ||  // Short air surveillance
        msgtype == 4 ||  // Surveillance, altitude reply
        msgtype == 5 ||  // Surveillance, identity reply
        msgtype == 16 || // Long Air-Air survillance
        msgtype == 20 || // Comm-A, altitude request
        msgtype == 21 || // Comm-A, identity request
        msgtype == 24)   // Comm-C ELM
    {
        uint32_t addr;
        uint32_t crc;
        int lastbyte = (msgbits / 8) - 1;

        // Work on a copy.
        memcpy(aux, msg, msgbits / 8);

        // Compute the CRC of the message and XOR it with the AP field so that
        // we recover the address, because:
        //
        // (ADDR xor CRC) xor CRC = ADDR.
        crc = mode_s_checksum(aux, msgbits);
        aux[lastbyte] ^= crc & 0xff;
        aux[lastbyte - 1] ^= (crc >> 8) & 0xff;
        aux[lastbyte - 2] ^= (crc >> 16) & 0xff;

        // If the obtained address exists in our cache we consider the message
        // valid.
        addr = aux[lastbyte] | (aux[lastbyte - 1] << 8) | (aux[lastbyte - 2] << 16);
        if (icao_addr_was_recently_seen(self, addr))
        {
            mm->aa1 = aux[lastbyte - 2];
            mm->aa2 = aux[lastbyte - 1];
            mm->aa3 = aux[lastbyte];
            return 1;
        }
    }
    return 0;
}

// ─────────────── Gillham（Mode-C，Q=0 的 100 ft 编码）───────────────
//
// 上游 dump1090 在这两处只留了 "TODO: Implement altitude where Q=0"，直接
// return 0。后果不是"少一个字段"而是"错一个字段"：Q=0 的帧在国内空域并不
// 罕见（100 ft 分辨率的老应答机），返回 0 之后调用方无法把它与"合法的 0
// 英尺"区分开，只能靠 `altitude != 0` 这种哨兵去猜。补齐解码 + 显式的
// altitude_valid 才能让"解不出来"和"就是 0 英尺"是两件事。
//
// 13 bit ID 字段 → Gillham 位序（C1 A1 C2 A2 C4 A4 M B1 D1 B2 D2 B4 D4），
// 与 Squawk 用的是同一张交织表，只是解释不同。
static int gillham_id13_to_hex(int id13)
{
    int hex = 0;
    if (id13 & 0x1000) hex |= 0x0010; // C1
    if (id13 & 0x0800) hex |= 0x1000; // A1
    if (id13 & 0x0400) hex |= 0x0020; // C2
    if (id13 & 0x0200) hex |= 0x2000; // A2
    if (id13 & 0x0100) hex |= 0x0040; // C4
    if (id13 & 0x0080) hex |= 0x4000; // A4
    /* bit 6 是 M（单位位），不参与高度格雷码 */
    if (id13 & 0x0020) hex |= 0x0100; // B1
    if (id13 & 0x0010) hex |= 0x0001; // D1（高度里不用，保留位序）
    if (id13 & 0x0008) hex |= 0x0200; // B2
    if (id13 & 0x0004) hex |= 0x0002; // D2
    if (id13 & 0x0002) hex |= 0x0400; // B4
    if (id13 & 0x0001) hex |= 0x0004; // D4
    return hex;
}

#define GILLHAM_INVALID (-9999)

// Gillham 位 → 100 ft 为单位的高度码。非法组合返回 GILLHAM_INVALID：
// C 组必须落在 1..5（0/6/7 是非法码），全 0 的 C 组表示该帧根本没带高度。
static int gillham_hex_to_alt100(int hex)
{
    int five_hundreds = 0;
    int one_hundreds = 0;

    if (((unsigned)hex & 0xFFFF8889u) || ((hex & 0x000000F0) == 0))
        return GILLHAM_INVALID;

    if (hex & 0x0010) one_hundreds ^= 0x007; // C1
    if (hex & 0x0020) one_hundreds ^= 0x003; // C2
    if (hex & 0x0040) one_hundreds ^= 0x001; // C4

    // 去掉 7（7↔5 互换）
    if ((one_hundreds & 5) == 5) one_hundreds ^= 2;
    if (one_hundreds > 5) return GILLHAM_INVALID;

    // D1 在高度编码里恒不用
    if (hex & 0x0002) five_hundreds ^= 0x0FF; // D2
    if (hex & 0x0004) five_hundreds ^= 0x07F; // D4
    if (hex & 0x1000) five_hundreds ^= 0x03F; // A1
    if (hex & 0x2000) five_hundreds ^= 0x01F; // A2
    if (hex & 0x4000) five_hundreds ^= 0x00F; // A4
    if (hex & 0x0100) five_hundreds ^= 0x007; // B1
    if (hex & 0x0200) five_hundreds ^= 0x003; // B2
    if (hex & 0x0400) five_hundreds ^= 0x001; // B4

    if (five_hundreds & 1) one_hundreds = 6 - one_hundreds;

    return (five_hundreds * 5) + one_hundreds - 13;
}

// 13 bit（含 M/Q）的 Gillham 高度 → 英尺，失败返回 GILLHAM_INVALID。
static int gillham_ac13_to_feet(int ac13)
{
    int alt100 = gillham_hex_to_alt100(gillham_id13_to_hex(ac13));
    if (alt100 == GILLHAM_INVALID) return GILLHAM_INVALID;
    return alt100 * 100;
}

// Decode the 13 bit AC altitude field (in DF 20 and others). Returns the
// altitude, and set 'unit' to either MODE_S_UNIT_METERS or MODE_S_UNIT_FEET.
// *valid 为 0 时返回值无意义。
int decode_ac13_field(unsigned char *msg, int *unit, int *valid)
{
    int ac13 = ((msg[2] & 0x1F) << 8) | msg[3];
    int m_bit = msg[3] & (1 << 6);
    int q_bit = msg[3] & (1 << 4);

    *unit = MODE_S_UNIT_FEET;
    *valid = 0;
    if (ac13 == 0) return 0; // 全 0 = 本帧不带高度

    if (m_bit)
    {
        // 公制单位：DO-260B 允许但实际空域里没有应答机在用；解不出来就
        // 老实说不可用，不要返回一个假的英尺值。
        *unit = MODE_S_UNIT_METERS;
        return 0;
    }
    if (q_bit)
    {
        // N is the 11 bit integer resulting from the removal of bit Q and M
        int n = ((msg[2] & 31) << 6) |
                ((msg[3] & 0x80) >> 2) |
                ((msg[3] & 0x20) >> 1) |
                (msg[3] & 15);
        *valid = 1;
        // The final altitude is due to the resulting number multiplied by
        // 25, minus 1000.
        return n * 25 - 1000;
    }
    int ft = gillham_ac13_to_feet(ac13);
    if (ft == GILLHAM_INVALID) return 0;
    *valid = 1;
    return ft;
}

// Decode the 12 bit AC altitude field (in DF 17 and others). *valid 为 0 时
// 返回值无意义（**不要**再用 "== 0" 当哨兵，0 英尺是合法高度）。
int decode_ac12_field(unsigned char *msg, int *unit, int *valid)
{
    int ac12 = (msg[5] << 4) | (msg[6] >> 4);
    int q_bit = ac12 & 0x10;

    *unit = MODE_S_UNIT_FEET;
    *valid = 0;
    if (ac12 == 0) return 0; // 全 0 = 本帧不带高度

    if (q_bit)
    {
        // N is the 11 bit integer resulting from the removal of bit Q
        int n = ((ac12 & 0x0FE0) >> 1) | (ac12 & 0x000F);
        *valid = 1;
        // The final altitude is due to the resulting number multiplied by 25,
        // minus 1000.
        return n * 25 - 1000;
    }
    // Q=0：把 M 位（bit 6）插回去还原成 13 bit 字段，再走 Gillham。
    int ft = gillham_ac13_to_feet(((ac12 & 0x0FC0) << 1) | (ac12 & 0x003F));
    if (ft == GILLHAM_INVALID) return 0;
    *valid = 1;
    return ft;
}

static const char *ais_charset = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";

// Decode the 7-bit "movement" (ground speed) field of a DF17 ME type 5-8
// Surface Position Message into knots.
//
// Table sourced from antirez/dump1090 (upstream of this vendored decoder,
// https://github.com/antirez/dump1090/blob/master/dump1090.c,
// decodeMovementField()), which itself follows the RTCA DO-260B Table 2-14
// piecewise-linear encoding:
//   0        -> no information available
//   1        -> stopped, < 0.125 kt
//   2..8     -> 0.125 kt steps,  0.125 .. 1    kt
//   9..12    -> 0.25  kt steps,  1     .. 2    kt
//   13..38   -> 0.5   kt steps,  2     .. 15   kt
//   39..93   -> 1     kt steps,  15    .. 70   kt
//   94..108  -> 2     kt steps,  70    .. 100  kt
//   109..123 -> 5     kt steps,  100   .. 175  kt
//   124      -> >= 175 kt
//   125..127 -> reserved (treated as unavailable)
static double decode_movement_field(int movement)
{
    if (movement == 0)
        return -1; // Not available.
    if (movement == 1)
        return 0; // Stopped (< 0.125 kt).
    if (movement <= 8)
        return (movement - 2) * 0.125 + 0.125;
    if (movement <= 12)
        return (movement - 9) * 0.25 + 1;
    if (movement <= 38)
        return (movement - 13) * 0.5 + 2;
    if (movement <= 93)
        return (movement - 39) + 15;
    if (movement <= 108)
        return (movement - 94) * 2 + 70;
    if (movement <= 123)
        return (movement - 109) * 5 + 100;
    if (movement <= 124)
        return 175; // >= 175 kt.
    return -1;      // 125-127 reserved.
}

// Decode a raw Mode S message demodulated as a stream of bytes by
// mode_s_detect(), and split it into fields populating a mode_s_msg structure.
void mode_s_decode(mode_s_t *self, struct mode_s_msg *mm, unsigned char *msg)
{
    uint32_t crc2; // Computed CRC, used to verify the message CRC.
    unsigned char raw[MODE_S_LONG_MSG_BYTES];

    // Work on our local copy.
    //
    // 先整体清零再填：本函数只显式写"本帧真的带了"的字段，其余一律留 0/
    // false。上游实现不清零，于是 DF21 的 mm->altitude 是上一次解码残留在
    // 栈上的**别人家飞机的高度**（mode_s_msg 在调用方通常是未初始化的局部
    // 变量）。中转的 raw[] 是为了让 msg == mm->msg 的调用也安全。
    memcpy(raw, msg, sizeof(raw));
    memset(mm, 0, sizeof(*mm));
    memcpy(mm->msg, raw, sizeof(raw));
    msg = mm->msg;

    // Get the message type ASAP as other operations depend on this
    mm->msgtype = msg[0] >> 3; // Downlink Format
    mm->msgbits = mode_s_msg_len_by_type(mm->msgtype);

    // CRC is always the last three bytes.
    mm->crc = ((uint32_t)msg[(mm->msgbits / 8) - 3] << 16) |
              ((uint32_t)msg[(mm->msgbits / 8) - 2] << 8) |
              (uint32_t)msg[(mm->msgbits / 8) - 1];
    crc2 = mode_s_checksum(msg, mm->msgbits);

    // Check CRC and fix single bit errors using the CRC when possible (DF 11 and 17).
    mm->errorbit = -1; // No error
    mm->crcok = (mm->crc == crc2);

    if (!mm->crcok && self->fix_errors && (mm->msgtype == 11 || mm->msgtype == 17))
    {
        if ((mm->errorbit = fix_single_bit_errors(msg, mm->msgbits)) != -1)
        {
            mm->crc = mode_s_checksum(msg, mm->msgbits);
            mm->crcok = 1;
        }
        else if (self->aggressive && mm->msgtype == 17 &&
                 (mm->errorbit = fix_two_bits_errors(msg, mm->msgbits)) != -1)
        {
            mm->crc = mode_s_checksum(msg, mm->msgbits);
            mm->crcok = 1;
        }
    }

    // Note that most of the other computation happens *after* we fix the
    // single bit errors, otherwise we would need to recompute the fields
    // again.
    mm->ca = msg[0] & 7; // Responder capabilities.

    // ICAO address
    mm->aa1 = msg[1];
    mm->aa2 = msg[2];
    mm->aa3 = msg[3];

    // DF 17 type (assuming this is a DF17, otherwise not used)
    mm->metype = msg[4] >> 3; // Extended squitter message type.
    mm->mesub = msg[4] & 7;   // Extended squitter message subtype.

    // Fields for DF4,5,20,21
    mm->fs = msg[0] & 7;           // Flight status for DF4,5,20,21
    mm->dr = msg[1] >> 3 & 31;     // Request extraction of downlink request.
    mm->um = ((msg[1] & 7) << 3) | // Request extraction of downlink request.
             msg[2] >> 5;

    // In the squawk (identity) field bits are interleaved like that (message
    // bit 20 to bit 32):
    //
    // C1-A1-C2-A2-C4-A4-ZERO-B1-D1-B2-D2-B4-D4
    //
    // So every group of three bits A, B, C, D represent an integer from 0 to
    // 7.
    //
    // The actual meaning is just 4 octal numbers, but we convert it into a
    // base ten number tha happens to represent the four octal numbers.
    //
    // For more info: http://en.wikipedia.org/wiki/Gillham_code
    {
        int a, b, c, d;

        a = ((msg[3] & 0x80) >> 5) |
            ((msg[2] & 0x02) >> 0) |
            ((msg[2] & 0x08) >> 3);
        b = ((msg[3] & 0x02) << 1) |
            ((msg[3] & 0x08) >> 2) |
            ((msg[3] & 0x20) >> 5);
        c = ((msg[2] & 0x01) << 2) |
            ((msg[2] & 0x04) >> 1) |
            ((msg[2] & 0x10) >> 4);
        d = ((msg[3] & 0x01) << 2) |
            ((msg[3] & 0x04) >> 1) |
            ((msg[3] & 0x10) >> 4);
        mm->identity = a * 1000 + b * 100 + c * 10 + d;
    }

    // DF18 的 Control Field 与地址语义。必须在"要不要把地址塞进最近可见
    // 缓存"之前定下来：一个 Mode-A 航迹文件号被当成 ICAO 记进缓存，之后任何
    // 一条 AP 帧都可能被它误"解"出来并当成合法帧。
    mm->aa_is_icao = 1;
    if (mm->msgtype == 18)
    {
        mm->cf = msg[0] & 7;
        // IMF 的位置随报文类型变：细格式位置报文（TC 5-18）在 ME bit 8，
        // 速度报文（TC 19）在 ME bit 9。只有 CF=2/6 才有 IMF 这个字段，
        // 其它 CF 下同一位是 ADS-B 的单天线标志，读了就是读错。
        if (mm->cf == 2 || mm->cf == 6)
        {
            if (mm->metype >= 5 && mm->metype <= 18)
                mm->imf = msg[4] & 1;
            else if (mm->metype == 19)
                mm->imf = (msg[5] & 0x80) ? 1 : 0;
        }
        switch (mm->cf)
        {
        case 0:            mm->aa_is_icao = 1;        break;
        case 2: case 6:    mm->aa_is_icao = !mm->imf; break;
        default:           mm->aa_is_icao = 0;        break; // CF 1/3/4/5/7
        }
    }

    // DF 11 / 17 / 18: 校验和是"裸"的，可以直接校验并把地址收进白名单。
    // 其余 DF 的校验和与 ICAO 地址异或过（AP 字段），只能靠最近可见地址
    // 暴力反解。DF18 早先漏在这里，连同 56/112 长度那处一起，整类 TIS-B /
    // ADS-R 报文都进不来。
    if (mm->msgtype != 11 && mm->msgtype != 17 && mm->msgtype != 18)
    {
        // Check if we can check the checksum for the Downlink Formats where
        // the checksum is xored with the aircraft ICAO address. We try to
        // brute force it using a list of recently seen aircraft addresses.
        if (brute_force_ap(self, msg, mm))
        {
            // We recovered the message, mark the checksum as valid.
            mm->crcok = 1;
        }
        else
        {
            mm->crcok = 0;
        }
    }
    else
    {
        // If this is DF 11 / 17 / 18 and the checksum was ok, we can add this
        // address to the list of recently seen addresses — but only when it
        // really is an ICAO address (see aa_is_icao above).
        if (mm->crcok && mm->errorbit == -1 && mm->aa_is_icao)
        {
            uint32_t addr = (mm->aa1 << 16) | (mm->aa2 << 8) | mm->aa3;
            add_recently_seen_icao_addr(self, addr);
        }
    }

    // Decode 13 bit altitude for DF0, DF4, DF16, DF20
    if (mm->msgtype == 0 || mm->msgtype == 4 ||
        mm->msgtype == 16 || mm->msgtype == 20)
    {
        mm->altitude = decode_ac13_field(msg, &mm->unit, &mm->altitude_valid);
        mm->altitude_source = MODE_S_ALTITUDE_BARO;
    }

    // Decode extended squitter specific stuff.
    // DF18 的 ME 字段与 DF17 同构，但只有 CF 0/1/2/5/6 携带标准 ME；CF=3 是
    // 另一种（粗格式）布局，CF=4 是管理报文，CF=7 保留——按标准 ME 去解会
    // 得到一堆像模像样的假字段。
    if (mm->msgtype == 17 ||
        (mm->msgtype == 18 && mm->cf != 3 && mm->cf != 4 && mm->cf != 7))
    {
        // Decode the extended squitter message.

        if (mm->metype >= 1 && mm->metype <= 4)
        {
            // Aircraft Identification and Category
            mm->aircraft_type = mm->metype - 1;
            mm->flight[0] = (ais_charset)[msg[5] >> 2];
            mm->flight[1] = ais_charset[((msg[5] & 3) << 4) | (msg[6] >> 4)];
            mm->flight[2] = ais_charset[((msg[6] & 15) << 2) | (msg[7] >> 6)];
            mm->flight[3] = ais_charset[msg[7] & 63];
            mm->flight[4] = ais_charset[msg[8] >> 2];
            mm->flight[5] = ais_charset[((msg[8] & 3) << 4) | (msg[9] >> 4)];
            mm->flight[6] = ais_charset[((msg[9] & 15) << 2) | (msg[10] >> 6)];
            mm->flight[7] = ais_charset[msg[10] & 63];
            mm->flight[8] = '\0';
        }
        else if (mm->metype >= 5 && mm->metype <= 8)
        {
            // Surface Position Message. No altitude on the ground, those
            // bits are reused for movement (ground speed) and track instead.
            // ME field layout (offsets from the first ME bit, 0-indexed):
            //   TC[0:5) MOV[5:12) S[12] TRK[13:20) T[20] F[21] LAT[22:39) LON[39:56)
            // F/LAT/LON sit at the exact same bit offsets as the airborne
            // branch below (both header sections are 22 bits), so those
            // extractions are copied verbatim.
            mm->fflag = msg[6] & (1 << 2);
            mm->raw_latitude = ((msg[6] & 3) << 15) |
                               (msg[7] << 7) |
                               (msg[8] >> 1);
            mm->raw_longitude = ((msg[8] & 1) << 16) |
                                (msg[9] << 8) |
                                msg[10];

            // MOV: msg[4] low 3 bits << 4 | msg[5] high 4 bits.
            mm->surface_movement_raw = ((msg[4] & 0x07) << 4) | (msg[5] >> 4);
            mm->surface_ground_speed = decode_movement_field(mm->surface_movement_raw);

            // S: msg[5] bit 3.
            mm->surface_track_valid = (msg[5] >> 3) & 1;

            // TRK: msg[5] low 3 bits << 4 | msg[6] high 4 bits, step 360/128 deg.
            mm->surface_track_raw = ((msg[5] & 0x07) << 4) | (msg[6] >> 4);
            mm->surface_track = mm->surface_track_raw * (360.0 / 128);
        }
        else if (mm->metype >= 9 && mm->metype <= 18)
        {
            // Airborne position Message（气压高度）
            mm->fflag = msg[6] & (1 << 2);
            mm->tflag = msg[6] & (1 << 3);
            mm->altitude = decode_ac12_field(msg, &mm->unit,
                                             &mm->altitude_valid);
            mm->altitude_source = MODE_S_ALTITUDE_BARO;
            mm->raw_latitude = ((msg[6] & 3) << 15) |
                               (msg[7] << 7) |
                               (msg[8] >> 1);
            mm->raw_longitude = ((msg[8] & 1) << 16) |
                                (msg[9] << 8) |
                                msg[10];
        }
        else if (mm->metype >= 20 && mm->metype <= 22)
        {
            // Airborne position Message（GNSS 椭球高 HAE）。CPR 位置字段与
            // TC9-18 同一偏移；高度那 12 bit 没有 Q 位，直接是**米**。
            // 早先整段没解：TC20-22 的位置帧连 CPR 都拿不到，一架只发
            // GNSS 高度帧的飞机在图上没有位置。
            mm->fflag = msg[6] & (1 << 2);
            mm->tflag = msg[6] & (1 << 3);
            mm->raw_latitude = ((msg[6] & 3) << 15) |
                               (msg[7] << 7) |
                               (msg[8] >> 1);
            mm->raw_longitude = ((msg[8] & 1) << 16) |
                                (msg[9] << 8) |
                                msg[10];
            int hae_m = (msg[5] << 4) | (msg[6] >> 4);
            mm->altitude = hae_m;
            mm->unit = MODE_S_UNIT_METERS;
            mm->altitude_valid = (hae_m != 0);
            mm->altitude_source = MODE_S_ALTITUDE_GNSS;
        }
        else if (mm->metype == 19 && mm->mesub >= 1 && mm->mesub <= 4)
        {
            // Airborne Velocity Message.
            //
            // DO-260B Table 2-69：每个速度类字段都是"编码值 0 = 不可用，
            // 否则真实幅值 = (编码值 - 1) × 步进"，超音速子类型（2/4）的
            // 步进是 4 kt。上游把编码值直接当 kt 用，于是每个速度都偏大
            // 1 kt、超音速帧偏 4 倍，而"不可用"被当成 0 kt——一架没报速度
            // 的飞机在 GDL90 上是"地速 0"而不是"未知"。
            const int scale = (mm->mesub == 2 || mm->mesub == 4) ? 4 : 1;

            mm->vert_rate_source = (msg[8] & 0x10) >> 4;
            mm->vert_rate_sign = (msg[8] & 0x8) >> 3;
            mm->vert_rate = ((msg[8] & 7) << 6) | ((msg[9] & 0xfc) >> 2);
            mm->vert_rate_valid = (mm->vert_rate != 0);

            if (mm->mesub == 1 || mm->mesub == 2)
            {
                // 地速矢量：E/W 与 N/S 两个分量各自带"不可用"编码。缺一个
                // 分量就既合不出地速也合不出航迹——两者必须一起判无效，
                // 只清速度会留下一个方向笃定、大小为 0 的假矢量。
                int ew_raw = ((msg[5] & 3) << 8) | msg[6];
                int ns_raw = ((msg[7] & 0x7f) << 3) | ((msg[8] & 0xe0) >> 5);
                mm->ew_dir = (msg[5] & 4) >> 2;
                mm->ns_dir = (msg[7] & 0x80) >> 7;
                if (ew_raw != 0 && ns_raw != 0)
                {
                    mm->ew_velocity = (ew_raw - 1) * scale;
                    mm->ns_velocity = (ns_raw - 1) * scale;
                    mm->velocity = (int)sqrt(
                        (double)mm->ns_velocity * mm->ns_velocity +
                        (double)mm->ew_velocity * mm->ew_velocity);
                    mm->velocity_valid = 1;

                    int ewv = mm->ew_dir ? -mm->ew_velocity : mm->ew_velocity;
                    int nsv = mm->ns_dir ? -mm->ns_velocity : mm->ns_velocity;
                    if (ewv != 0 || nsv != 0)
                    {
                        double heading = atan2((double)ewv, (double)nsv) *
                                         180.0 / M_PI;
                        if (heading < 0) heading += 360.0;
                        mm->heading = heading;
                        mm->heading_is_valid = 1;
                    }
                }
            }
            else
            {
                // subtype 3/4：磁/真**空中航向** + 空速（IAS 或 TAS）。
                // 这不是地速矢量，不能顶替地面航迹（有风时差十几度），也
                // 不能当 GDL90 的地速用。
                mm->heading_is_valid = (msg[5] & (1 << 2)) ? 1 : 0;
                if (mm->heading_is_valid)
                {
                    // 10 bit 航向，步进 360/1024。上游按 7 bit / (360/128)
                    // 解，解出来永远是错的角度。
                    int hdg_raw = ((msg[5] & 3) << 8) | msg[6];
                    mm->heading = hdg_raw * (360.0 / 1024.0);
                }
                int as_raw = ((msg[7] & 0x7f) << 3) | ((msg[8] & 0xe0) >> 5);
                if (as_raw != 0)
                {
                    mm->velocity = (as_raw - 1) * scale;
                    mm->velocity_valid = 1;
                }
            }
        }
    }
    mm->phase_corrected = 0; // Set to 1 by the caller if needed.
}


// Return -1 if the message is out of fase left-side
// Return  1 if the message is out of fase right-size
// Return  0 if the message is not particularly out of phase.
//
// Note: this function will access mag[-1], so the caller should make sure to
// call it only if we are not at the start of the current buffer.
int detect_out_of_phase(uint16_t *mag)
{
    if (mag[3] > mag[2] / 3)
        return 1;
    if (mag[10] > mag[9] / 3)
        return 1;
    if (mag[6] > mag[7] / 3)
        return -1;
    if (mag[-1] > mag[1] / 3)
        return -1;
    return 0;
}

// This function does not really correct the phase of the message, it just
// applies a transformation to the first sample representing a given bit:
//
// If the previous bit was one, we amplify it a bit.
// If the previous bit was zero, we decrease it a bit.
//
// This simple transformation makes the message a bit more likely to be
// correctly decoded for out of phase messages:
//
// When messages are out of phase there is more uncertainty in sequences of the
// same bit multiple times, since 11111 will be transmitted as continuously
// altering magnitude (high, low, high, low...)
//
// However because the message is out of phase some part of the high is mixed
// in the low part, so that it is hard to distinguish if it is a zero or a one.
//
// However when the message is out of phase passing from 0 to 1 or from 1 to 0
// happens in a very recognizable way, for instance in the 0 -> 1 transition,
// magnitude goes low, high, high, low, and one of of the two middle samples
// the high will be *very* high as part of the previous or next high signal
// will be mixed there.
//
// Applying our simple transformation we make more likely if the current bit is
// a zero, to detect another zero. Symmetrically if it is a one it will be more
// likely to detect a one because of the transformation. In this way similar
// levels will be interpreted more likely in the correct way.
void apply_phase_correction(uint16_t *mag)
{
    int j;

    mag += 16; // Skip preamble.
    for (j = 0; j < (MODE_S_LONG_MSG_BITS - 1) * 2; j += 2)
    {
        if (mag[j] > mag[j + 1])
        {
            // One
            mag[j + 2] = (mag[j + 2] * 5) / 4;
        }
        else
        {
            // Zero
            mag[j + 2] = (mag[j + 2] * 4) / 5;
        }
    }
}
