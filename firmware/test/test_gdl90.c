/*
 * test_gdl90.c — GDL90 Heartbeat 编码器的 host 单测（WP-F Task 1）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -o /tmp/test_gdl90 firmware/test/test_gdl90.c \
 *      firmware/main/gdl90.c -lm && /tmp/test_gdl90
 *
 * 背景：本任务的"清退假 UAT 能力"在终审被推翻了一半——真正的假能力
 * 表述只在文档里（位名 misleading），**线上行为从来不该改**：ICD
 * §3.1.1 h) 原文 "UAT Initialized: This bit is set to ONE in all
 * Heartbeat messages"——bit0 是接口初始化 talkback，与 UAT 接收能力
 * 无关，ICD 自己的 golden heartbeat（§2.2.4，status1=0x81）bit0 也是
 * 1。本分支曾按位名把调用点改成 false，产出不合规心跳；现回退：参数
 * 从编码器签名里整体删除（名字即陷阱），bit0 由编码器无条件置 1。
 * 调用点依赖 NimBLE，没有 host 测试缝，所以这里钉死编码器的位级合同。
 *
 * 帧字节布局（依据 gdl90.c 与 ICD §2.2.4 golden vector 取证）：
 *
 *   out[0]  = 0x7E 帧界
 *   out[1]  = 0x00 msg id
 *   out[2]  = Status Byte 1（bit7 = GPS valid；bit0 恒 1，§3.1.1 h)）
 *   out[3]  = Status Byte 2（bit7 = 17 位时间戳的 bit16，bit0 = UTC OK）
 *   out[4]  = 时间戳低 16 位的 LSB（小端在前）
 *   out[5]  = 时间戳低 16 位的 MSB
 *   out[6]  = Message Counts 字节 1（uplink 5 位在 [7:3]，bit2 保留 0，
 *             basic/long 高 2 位在 [1:0]——ICD §3.1.4）
 *   out[7]  = Message Counts 字节 2（basic-long 低 8 位）
 *   out[8]  = FCS 低字节
 *   out[9]  = FCS 高字节
 *   out[10] = 0x7E 帧界
 *
 * FCS 合同（FAA 560-1058-00 Rev A §2.2.3 参考算法，语义照抄）：
 * 256 项表（表初始化也按 ICD 原文）+ 更新行
 * `crc = Table[crc >> 8] ^ (crc << 8) ^ block[i]`（初值 0），对
 * msg_id + payload 计算，LSB 在前。两个坑都有前科，都有测试钉着：
 *
 *   1. 这条更新行与常见逐位循环 `crc ^= b << 8; 8 次移位` **不是**
 *      代数等价的——原编码器用的就是逐位循环，FCS 不合规（已被
 *      golden vector 证实）；
 *   2. GDL90 **不做** 0xF0B8 增强（那是 HDLC/X.25 的常数）。2026-09-07
 *      曾按错误裁决加过增强，后被 §2.2.4 golden vector 推翻回退：
 *      [7E 00 81 41 DB D0 08 02 B3 8B 7E]（FCS 0x8BB3，LSB first）。
 *
 * (d)/(e) 在测试内写了一份**独立照抄 ICD 更新行**的实现（不调用
 * gdl90.c），自身先与硬编码常数锚定；(a)/(c)/(e) 的硬编码全帧由
 * Python 按 ICD 算法独立推导后写入——两边若犯同一个错，锚常数仍能抓到。
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

/* ── 测试内独立的 ICD §2.2.3 参考实现（不调用 gdl90.c）──────────────── */

static uint16_t g_crc_table[256];

static void crc_table_init(void)
{
    /* ICD §2.2.3 表初始化，逐字语义。 */
    for (int i = 0; i < 256; ++i) {
        uint16_t crc = (uint16_t)(i << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        g_crc_table[i] = crc;
    }
}

/* 注意是 ICD 的更新行 `Table[crc>>8] ^ (crc<<8) ^ b`，不是教科书
 * 标准形 `Table[(crc>>8) ^ b] ^ (crc<<8)`，也不是逐位循环——三者互
 * 不等价，只有 ICD 这条与真实设备一致（§2.2.4 golden vector 判定）。 */
static uint16_t crc_icd_reference(const uint8_t *d, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i)
        crc = (uint16_t)(g_crc_table[crc >> 8] ^ (uint16_t)(crc << 8) ^ d[i]);
    return crc;
}

/* ── (a) Status Byte 1 默认位：bit0 恒 1，bit7 随 gps_valid ────────── */
/* 全帧钉死：gps=F，utc=F，ts=0xE1A4（57636 s），计数 0/0。
 * Status1 = 0x01：bit7(GPS)=0，bit0 恒 1（ICD §3.1.1 h)）。
 * 期望 FCS = 0x4A01（ICD §2.2.3 算法独立推导），
 * 全帧无 0x7D/0x7E，不需要转义。 */
static void test_heartbeat_status1_defaults(void)
{
    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,                    /* 帧界 | msg id 0x00              */
        0x01,                          /* Status1: bit7(GPS)=0，bit0 恒 1  */
        0x00,                          /* Status2: bit7=0(ts bit16=0) bit0=0 */
        0xA4, 0xE1,                    /* ts 低 16 位，LSB first          */
        0x00, 0x00,                    /* message counts                  */
        0x01, 0x4A,                    /* FCS 0x4A01，LSB first           */
        0x7E                           /* 帧界                            */
    };

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/false,
                                      /*utc_ok=*/false,
                                      0xE1A4, /*uplink=*/0, /*basic_long=*/0);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    /* 单独点名 bit0：合同核心。谁把它清回 0（哪怕是"诚实"理由），
     * 每秒的心跳就是不合规帧。 */
    CHECK((buf[2] & 0x01) == 1);
}

/* ── (b) 位序钉死：gps bit7 / utc bit0 / 时间戳小端 ────────────────── */
/* gps=T，uat 位恒 1，utc=F，ts=0x10FF0（bit16 置位，0x10FF0=69616 s）。
 * Status1 = 0x81（bit7=1、bit0=1）；Status2 bit7=1（ts bit16）、
 * bit0=0（utc_ok=false）。防的是"修某一位时把整个 Status Byte 清零"
 * 这类误伤。 */
static void test_heartbeat_status_bit_positions(void)
{
    uint8_t buf[64] = { 0 };

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*utc_ok=*/false,
                                      0x10FF0, /*uplink=*/0, /*basic_long=*/0);
    CHECK(n == 11);
    CHECK(buf[0] == 0x7E && buf[1] == 0x00);
    CHECK(buf[2] == 0x81);             /* bit7(GPS)=1，bit0 恒 1           */
    CHECK(buf[3] == 0x80);             /* bit7(ts bit16)=1，bit0(UTC)=0   */
    CHECK(buf[4] == 0xF0 && buf[5] == 0x0F);  /* ts 低 16 位，LSB first   */
    CHECK((buf[6] & 0x1F) == 0);       /* uplink 低 5 位                  */
}

/* ── (c) FCS 字节正确：非零计数向量全帧比对 ────────────────────────── */
/* gps=T，utc=F，ts=0x12345（bit16=1），uplink=0，basic_long=0x123。
 * counts 打包（§3.1.4）：mc1=(0<<3)|((0x123>>8)&3)=0x01，mc2=0x23。
 * Status1=0x81。FCS 0x77CD（ICD 算法独立推导），LSB 在前。全帧硬编码
 * 比对，任何一个字节位序/打包/FCS 回归都会被抓到。 */
static void test_heartbeat_crc_bytes(void)
{
    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,
        0x81,                          /* Status1: GPS=1，bit0 恒 1       */
        0x80,                          /* Status2: ts bit16=1，UTC=0      */
        0x45, 0x23,                    /* ts 0x12345 低 16 位，LSB first  */
        0x01, 0x23,                    /* counts §3.1.4：uplink[7:3]+b1[1:0] */
        0xCD, 0x77,                    /* FCS 0x77CD，LSB first           */
        0x7E
    };

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*utc_ok=*/false,
                                      0x12345, /*uplink=*/0, /*basic_long=*/0x123);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
}

/* ── (d) 编码器 FCS == ICD 参考实现（非零向量，测试内独立实现）─────── */
/* 向量：gps=T，utc=F，ts=0x12345，counts 0/0 →
 * msg 00 81 80 45 23 00 00（Status1=0x81：bit0 恒 1），ICD 算法
 * FCS = 0x76EE。独立实现与编码器对同一 message 必须给出同一 FCS；
 * 任何一边改用别的 CRC 族（逐位循环 / 增强版）都会让比对或锚常数
 * 变红。 */
static void test_heartbeat_fcs_matches_icd_reference(void)
{
    static const uint8_t msg[] = { 0x00, 0x81, 0x80, 0x45, 0x23, 0x00, 0x00 };
    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,
        0x81,                          /* Status1: GPS=1，bit0 恒 1       */
        0x80,                          /* Status2: ts bit16=1，UTC=0      */
        0x45, 0x23,                    /* ts 0x12345 低 16 位，LSB first  */
        0x00, 0x00,                    /* counts（钉当前打包行为，见
                                        * gdl90.c 的 §3.1.4 偏差注）       */
        0xEE, 0x76,                    /* FCS 0x76EE，LSB first           */
        0x7E
    };

    /* 独立实现自身的锚：常数 0x76EE 来自 Python 独立推导，不是抄实现。 */
    CHECK(crc_icd_reference(msg, sizeof(msg)) == 0x76EE);

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*utc_ok=*/false,
                                      0x12345, /*uplink=*/0, /*basic_long=*/0);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(buf[8] == 0xEE && buf[9] == 0x76);  /* 点名 FCS 字节位置       */
}

/* ── (e) ICD §2.2.4 golden heartbeat：规格自己发布的测试向量 ────────── */
/* golden message = msg id + payload = 00 81 41 DB D0 08 02，ICD 原文
 * 发布的 FCS = 0x8BB3（帧 [7E 00 81 41 DB D0 08 02 B3 8B 7E]）。
 *
 * 复现说明：golden payload 分解为 status1=0x81(GPS+UAT 位)、
 * status2=0x41(bit6 CSA Requested + bit0 UTC)、ts=0xD0DB、uplink=8、
 * basic=2。bit0=1 本编码器现在无条件置位 ✓；status2 bit6 = CSA
 * Requested 不实现（恒 0，参数表里也没有它，签名是任务合同），
 * golden 帧无法逐字节从公共 API 产出；能产出的最近向量只差 status2
 * 一个字节（0x01 vs 0x41）。所以钉两层：
 *   1. 测试内 ICD 参考实现对 **完整 golden message** 的输出 == ICD
 *      发布的 0x8BB3——常数照抄规格原文，是全文件最硬的锚；
 *   2. 编码器对可产出向量（其余字节全同 golden）的全帧输出，且其
 *      FCS 与参考实现对同一 message 的输出一致——证明线上 FCS 就是
 *      ICD §2.2.3 算法。注意该向量同时从侧面印证 bit0 合同：status1
 *      的 0x81 与 golden 完全一致（GPS+UAT 位）。 */
static void test_heartbeat_icd_golden_vector(void)
{
    static const uint8_t golden_msg[] = { 0x00, 0x81, 0x41, 0xDB, 0xD0, 0x08, 0x02 };
    CHECK(crc_icd_reference(golden_msg, sizeof(golden_msg)) == 0x8BB3);

    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,
        0x81,                          /* Status1: GPS=1，bit0=1（同 golden）*/
        0x01,                          /* Status2: UTC=1；bit6 CSA 不实现   */
        0xDB, 0xD0,                    /* ts 0xD0DB，LSB first（同 golden） */
        0x08, 0x02,                    /* counts §3.1.4：uplink=1、basic=2
                                        * 恰好产出 golden 的 08 02——合规
                                        * 打包下对得上，不是巧合           */
        0x1E, 0x96,                    /* FCS 0x961E，LSB first             */
        0x7E
    };
    static const uint8_t produced_msg[] = { 0x00, 0x81, 0x01, 0xDB, 0xD0, 0x08, 0x02 };
    CHECK(crc_icd_reference(produced_msg, sizeof(produced_msg)) == 0x961E);

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*utc_ok=*/true,
                                      0xD0DB, /*uplink=*/1, /*basic_long=*/2);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(buf[8] == 0x1E && buf[9] == 0x96);  /* 点名 FCS 字节位置       */
}

/* ── (f) Message Counts §3.1.4 打包：非零 uplink + 跨字节 basic/long ── */
/* gps=T，utc=F，ts=0（ts bit16=0），uplink=0x15（21），basic_long=0x2AB
 * （683，10 位跨字节）。§3.1.4：byte1=(0x15<<3)|((0x2AB>>8)&3)
 * =0xA8|0x02=0xAA（bit2 保留 0），byte2=0xAB。FCS 0x0127（ICD 算法
 * 独立推导）。锁死打包公式：谁把 uplink/basic 挪回别的位序，这里红。 */
static void test_heartbeat_message_counts_packing(void)
{
    static const uint8_t msg[] = { 0x00, 0x81, 0x00, 0x00, 0x00, 0xAA, 0xAB };
    uint8_t buf[64] = { 0 };
    static const uint8_t expect[] = {
        0x7E, 0x00,
        0x81,                          /* Status1: GPS=1，bit0 恒 1       */
        0x00,                          /* Status2: ts bit16=0，UTC=0      */
        0x00, 0x00,                    /* ts 0 低 16 位                   */
        0xAA, 0xAB,                    /* counts §3.1.4（见上）           */
        0x27, 0x01,                    /* FCS 0x0127，LSB first           */
        0x7E
    };

    CHECK(crc_icd_reference(msg, sizeof(msg)) == 0x0127);

    size_t n = gdl90_encode_heartbeat(buf, sizeof(buf),
                                      /*gps_valid=*/true,
                                      /*utc_ok=*/false,
                                      0, /*uplink=*/0x15, /*basic_long=*/0x2AB);
    CHECK(n == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(buf[6] == 0xAA && buf[7] == 0xAB);  /* 点名 counts 字节位置    */
}

int main(void)
{
    crc_table_init();
    test_heartbeat_status1_defaults();
    test_heartbeat_status_bit_positions();
    test_heartbeat_crc_bytes();
    test_heartbeat_fcs_matches_icd_reference();
    test_heartbeat_icd_golden_vector();
    test_heartbeat_message_counts_packing();

    if (g_fail == 0) {
        printf("test_gdl90: all OK\n");
        return 0;
    }
    printf("test_gdl90: %d FAIL\n", g_fail);
    return 1;
}
