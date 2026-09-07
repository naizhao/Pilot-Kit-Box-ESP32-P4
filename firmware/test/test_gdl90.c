/*
 * test_gdl90.c — GDL90 Heartbeat 编码器的 host 单测（WP-F Task 1）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -o /tmp/test_gdl90 firmware/test/test_gdl90.c \
 *      firmware/main/gdl90.c -lm && /tmp/test_gdl90
 *
 * 背景：设备的 978 MHz UAT 链路固件不存在（WP-E 未开始），但 BLE 调用点
 * （ble_gatt.c）曾恒传 uat_initialised=true——对 ForeFlight 等 EFB 谎报
 * 不存在的接收能力。编码器的帧格式与位序无缺陷，缺陷在调用点实参；调用点
 * 依赖 NimBLE，没有 host 测试缝，所以这里钉死编码器的位级合同，防止将来
 * 有人"顺手修好"调用点时把位序也改坏。位序依据 gdl90.c 与 FAA 560-1058
 * ICD 派生实现（SoftRF rotobox/gdl90.c）双重取证：
 *
 *   out[0]  = 0x7E 帧界
 *   out[1]  = 0x00 msg id
 *   out[2]  = Status Byte 1（bit7 = GPS valid，bit0 = UAT initialised）
 *   out[3]  = Status Byte 2（bit7 = 17 位时间戳的 bit16，bit0 = UTC OK）
 *   out[4]  = 时间戳低 16 位的 LSB（小端在前）
 *   out[5]  = 时间戳低 16 位的 MSB
 *   out[6]  = Message Counts 字节 1（basic-long 高 2 位<<5 | uplink 低 5 位）
 *   out[7]  = Message Counts 字节 2（basic-long 低 8 位）
 *   out[8]  = 增强后 CRC 的低字节
 *   out[9]  = 增强后 CRC 的高字节
 *   out[10] = 0x7E 帧界
 *
 * CRC 合同（FAA 560-1058 §2.3）：对 msg_id+payload 算 CCITT-16
 * （poly 0x1021，init 0，MSB-first），发送前与 0xF0B8 异或（增强 CRC），
 * LSB 在前。裸 CRC 帧会被 EFB 校验静默丢弃。用例 (d) 在测试内写了一份
 * 独立的表驱动 CCITT-16 + 0xF0B8 实现（不调用 gdl90.c），并与硬编码
 * 常数比对——若两边犯同一个错，(a)/(c) 的硬编码全帧（由 Python 按上述
 * poly/init/增强独立推导）仍然能抓到。
 */

#include <stdio.h>
#include <string.h>

#include "../main/gdl90.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ── (a) uat_initialised=false：Status Byte 1 bit0 必须是 0 ────────── */
/* 全帧钉死：gps=F，uat=F，utc=F，ts=0xE1A4（57636 s），计数 0/0。
 * 期望 CRC = 0x473E（裸余数 0xB786 与 0xF0B8 异或，独立推导），
 * 全帧无 0x7D/0x7E，不需要转义。 */
static void test_heartbeat_uat_not_initialised_is_zero(void)
{
    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,                    /* 帧界 | msg id 0x00              */
        0x00,                          /* Status1: bit7=0 bit0=0          */
        0x00,                          /* Status2: bit7=0(ts bit16=0) bit0=0 */
        0xA4, 0xE1,                    /* ts 低 16 位，LSB first          */
        0x00, 0x00,                    /* message counts                  */
        0x3E, 0x47,                    /* 增强 CRC LSB/MSB                */
        0x7E                           /* 帧界                            */
    };

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/false,
                                      /*uat_initialised=*/false,
                                      /*utc_ok=*/false,
                                      0xE1A4, /*uplink=*/0, /*basic_long=*/0);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    /* 单独再点名 bit0：这条是本任务的合同核心，即使全帧比对因别的
     * 字节回归而失败，也要能看出 UAT 位是不是被翻回 1。 */
    CHECK((buf[2] & 0x01) == 0);
}

/* ── (b) 位序钉死：gps bit7 / utc bit0 / 时间戳小端 ────────────────── */
/* gps=T，uat=F，utc=F，ts=0x10FF0（bit16 置位，0x10FF0=69616 s <2^17）。
 * Status1 bit7 必须是 1、bit0 必须是 0；Status2 bit7=1（ts bit16）、
 * bit0=0（utc_ok=false）。防的是"修 UAT 位时把整个 Status Byte 清零"
 * 这类误伤。 */
static void test_heartbeat_status_bit_positions(void)
{
    uint8_t buf[64] = { 0 };

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*uat_initialised=*/false,
                                      /*utc_ok=*/false,
                                      0x10FF0, /*uplink=*/0, /*basic_long=*/0);
    CHECK(n == 11);
    CHECK(buf[0] == 0x7E && buf[1] == 0x00);
    CHECK(buf[2] == 0x80);             /* bit7(GPS)=1，bit0(UAT)=0        */
    CHECK(buf[3] == 0x80);             /* bit7(ts bit16)=1，bit0(UTC)=0   */
    CHECK(buf[4] == 0xF0 && buf[5] == 0x0F);  /* ts 低 16 位，LSB first   */
    CHECK((buf[6] & 0x1F) == 0);       /* uplink 低 5 位                  */
}

/* ── (c) CRC 字节正确：非零向量全帧比对 ────────────────────────────── */
/* gps=T，uat=F，utc=F，ts=0x12345（bit16=1），uplink=0，basic_long=0x123。
 * 裸余数 0xE958 与 0xF0B8 异或得 0x19E0，LSB 在前。
 * counts 打包：mc1=(0x123>>8&3)<<5=0x20，mc2=0x23。全帧硬编码比对，
 * 任何一个字节位序/打包/CRC 回归都会被抓到。 */
static void test_heartbeat_crc_bytes(void)
{
    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,
        0x80,                          /* Status1: GPS=1，UAT=0           */
        0x80,                          /* Status2: ts bit16=1，UTC=0      */
        0x45, 0x23,                    /* ts 0x12345 低 16 位，LSB first  */
        0x20, 0x23,                    /* counts：高 2 位 + 低 8 位       */
        0xE0, 0x19,                    /* 增强 CRC LSB/MSB                */
        0x7E
    };

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*uat_initialised=*/false,
                                      /*utc_ok=*/false,
                                      0x12345, /*uplink=*/0, /*basic_long=*/0x123);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
}

/* ── (d) 增强_crc（0xF0B8）判别用例：测试内独立 CRC 实现 ───────────── */
/* 裁决记录（WP-F Task 1 跟进）：裸 CRC 帧过不了 EFB 的校验，会被静默
 * 丢弃——FAA 560-1058 §2.3 要求发送前把余数与 0xF0B8 异或。这里在测试
 * 内写一份独立的表驱动 CCITT-16（init 0，MSB-first）+ 0xF0B8 异或，
 * 不调用 gdl90.c 的任何函数；其自身输出先与硬编码常数 0x0B07 比对
 * （Python 独立推导），再与编码器发出的 CRC 字节比对。向量：
 * gps=T，uat=F，utc=F，ts=0x12345，counts 0/0 → msg 00 80 80 45 23 00 00。
 * 裸 CRC 会得到 0xFBBF，两个 CRC 字节全不同——本用例对"忘了增强"是
 * 判别性的。 */

static uint16_t g_crc_table[256];

static void crc_table_init(void)
{
    for (int i = 0; i < 256; ++i) {
        uint16_t crc = (uint16_t)(i << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        g_crc_table[i] = crc;
    }
}

static uint16_t crc_ccitt_augmented(const uint8_t *d, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i)
        crc = (uint16_t)((crc << 8) ^ g_crc_table[(crc >> 8) ^ d[i]]);
    return (uint16_t)(crc ^ 0xF0B8);
}

static void test_heartbeat_crc_augmentation_f0b8(void)
{
    static const uint8_t msg[] = { 0x00, 0x80, 0x80, 0x45, 0x23, 0x00, 0x00 };
    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,
        0x80,                          /* Status1: GPS=1，UAT=0           */
        0x80,                          /* Status2: ts bit16=1，UTC=0      */
        0x45, 0x23,                    /* ts 0x12345 低 16 位，LSB first  */
        0x00, 0x00,                    /* counts                          */
        0x07, 0x0B,                    /* 增强 CRC 0x0B07，LSB first      */
        0x7E
    };

    /* 独立实现自身的锚：常数 0x0B07 来自 Python 独立推导，不是抄实现。 */
    CHECK(crc_ccitt_augmented(msg, sizeof(msg)) == 0x0B07);

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*uat_initialised=*/false,
                                      /*utc_ok=*/false,
                                      0x12345, /*uplink=*/0, /*basic_long=*/0);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(buf[8] == 0x07 && buf[9] == 0x0B);  /* 点名 CRC 字节位置       */
}

int main(void)
{
    crc_table_init();
    test_heartbeat_uat_not_initialised_is_zero();
    test_heartbeat_status_bit_positions();
    test_heartbeat_crc_bytes();
    test_heartbeat_crc_augmentation_f0b8();

    if (g_fail == 0) {
        printf("test_gdl90: all OK\n");
        return 0;
    }
    printf("test_gdl90: %d FAIL\n", g_fail);
    return 1;
}
