/*
 * test_cc13_bsl_proto.c — CC13x2 ROM bootloader 纯协议层的 host 单测。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_cc13_bsl_proto firmware/test/test_cc13_bsl_proto.c \
 *      firmware/rp2040/cc13_bsl_proto.c \
 *   && /tmp/test_cc13_bsl_proto
 *
 * 判据来源：TRM SWCU185G §10.2。两处是**手册自带的已知答案**，不是我自己
 * 推的：图 10-2 的包例子、以及 §10.2.3.2/§10.2.3.8 里逐字节列出的命令布局。
 *
 * 为什么这层值得单独测透：I/O 那层要等仿真器把第一版镜像烧进去才能跑，而
 * 协议层现在就能百分之百验证。上板那天如果不通，这些用例能把「协议组错了」
 * 这一半嫌疑直接排除掉。
 */
#include "cc13_bsl_proto.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* ── 1. 手册图 10-2 的已知答案 ──
 * 图里画的是 `0x06 0x84 | 0x48 0x6F 0x6C 0x61 | 0x00` 外加下面一行 ACK。
 * ⚠ 结尾那个 0x00 **不属于包体**——它是 ACK 那一行的第一个字节（§10.2.1.1：
 * ACK 是 0x00 0xCC）。包体就 6 字节，data 是 "Hola" 四个字节，这也正好和
 * size=0x06 对得上（2 个头字节 + 4 个 data）。我第一版把那个 0x00 当成 data
 * 的一部分，算出来是 7 字节，被 size 字段当场戳穿。
 * 校验和只加 data 不加头字节：0x48+0x6F+0x6C+0x61 = 0x184 → 低字节 0x84。 */
static void test_trm_figure_10_2(void)
{
    const uint8_t params[] = { 0x6F, 0x6C, 0x61 };      /* "ola" */
    uint8_t pkt[16];
    size_t n = cc13_bsl_pkt_build(pkt, sizeof pkt, 0x48 /* 'H' */,
                                  params, sizeof params);
    CHECK(n == 6, "长度 %zu，手册是 6\n", n);
    CHECK(pkt[0] == 0x06, "size=0x%02X，手册是 0x06\n", pkt[0]);
    CHECK(pkt[1] == 0x84, "checksum=0x%02X，手册是 0x84\n", pkt[1]);
    CHECK(memcmp(pkt + 2, (const uint8_t[]){0x48,0x6F,0x6C,0x61}, 4) == 0,
          "data 段与手册不符\n");
    CHECK(cc13_bsl_pkt_valid(pkt, n), "自己组的包过不了自己的校验\n");
}

/* ── 2. PING：手册 §10.2.3.1 说 size=3、三个字节都是命令码相关 ── */
static void test_ping_packet(void)
{
    uint8_t pkt[8];
    size_t n = cc13_bsl_pkt_build(pkt, sizeof pkt, CC13_BSL_CMD_PING, NULL, 0);
    CHECK(n == 3, "PING 包长 %zu，手册是 3\n", n);
    CHECK(pkt[0] == 3 && pkt[1] == CC13_BSL_CMD_PING && pkt[2] == CC13_BSL_CMD_PING,
          "PING 包 = %02X %02X %02X\n", pkt[0], pkt[1], pkt[2]);
}

/* ── 3. DOWNLOAD：§10.2.3.2 逐字节列了布局，地址与长度都是 MSB first ── */
static void test_download_layout(void)
{
    uint8_t p[8];
    cc13_bsl_put_be32(p + 0, 0x00001234u);   /* Program Address */
    cc13_bsl_put_be32(p + 4, 0x00058000u);   /* Program Size    */
    uint8_t pkt[16];
    size_t n = cc13_bsl_pkt_build(pkt, sizeof pkt, CC13_BSL_CMD_DOWNLOAD, p, 8);

    CHECK(n == 11, "DOWNLOAD 包长 %zu，手册是 11\n", n);
    CHECK(pkt[2] == 0x21, "命令码 0x%02X\n", pkt[2]);
    /* ucCommand[3..6] = 地址 [31:24]..[7:0] */
    CHECK(pkt[3] == 0x00 && pkt[4] == 0x00 && pkt[5] == 0x12 && pkt[6] == 0x34,
          "地址字节序错：%02X %02X %02X %02X\n", pkt[3], pkt[4], pkt[5], pkt[6]);
    /* ucCommand[7..10] = 长度 [31:24]..[7:0] */
    CHECK(pkt[7] == 0x00 && pkt[8] == 0x05 && pkt[9] == 0x80 && pkt[10] == 0x00,
          "长度字节序错：%02X %02X %02X %02X\n", pkt[7], pkt[8], pkt[9], pkt[10]);
}

/* ── 4. CRC32：§10.2.3.8 说 size=15，三个 32 位参数 ── */
static void test_crc32_cmd_layout(void)
{
    uint8_t p[12];
    cc13_bsl_put_be32(p + 0, 0x00000000u);
    cc13_bsl_put_be32(p + 4, 0x00058000u);
    cc13_bsl_put_be32(p + 8, 0x00000000u);   /* 读重复次数 0 = 只读一遍 */
    uint8_t pkt[24];
    size_t n = cc13_bsl_pkt_build(pkt, sizeof pkt, CC13_BSL_CMD_CRC32, p, 12);
    CHECK(n == 15, "CRC32 包长 %zu，手册是 15\n", n);
    CHECK(pkt[2] == 0x27, "命令码 0x%02X\n", pkt[2]);
}

/* ── 5. 分片：一条 SEND_DATA 最多 252 字节 ──
 * size 是单字节，整包 ≤255，减去 size/checksum/命令码三个字节。
 * 这个上界错了会让最后一片溢出、或者把整包长度算成 256 而 size 字段回绕成 0。 */
static void test_chunking(void)
{
    CHECK(cc13_bsl_chunk_len(1) == 1, "1 字节应该一次发完\n");
    CHECK(cc13_bsl_chunk_len(252) == 252, "252 应该一次发完\n");
    CHECK(cc13_bsl_chunk_len(253) == 252, "253 应该切成 252\n");
    CHECK(cc13_bsl_chunk_len(360448) == 252, "大块应该切成 252\n");
    CHECK(cc13_bsl_chunk_len(0) == 0, "0 应该是 0\n");

    /* 252 字节正好组成 255 的整包；253 字节必须被拒绝而不是静默回绕 */
    uint8_t data[256] = {0};
    uint8_t pkt[300];
    CHECK(cc13_bsl_pkt_build(pkt, sizeof pkt, CC13_BSL_CMD_SEND_DATA, data, 252) == 255,
          "252 字节数据应组成 255 的包\n");
    CHECK(cc13_bsl_pkt_build(pkt, sizeof pkt, CC13_BSL_CMD_SEND_DATA, data, 253) == 0,
          "253 字节必须拒绝（size 字段会回绕成 0）\n");

    /* 把整个 352 KB 按这个上界切完，总和必须刚好等于镜像长度、且每片非 0，
     * 否则烧录会停在半路或死循环。 */
    size_t left = 360448, sum = 0, pieces = 0;
    while (left) {
        size_t c = cc13_bsl_chunk_len(left);
        CHECK(c > 0, "分片出现 0 长度 → 会死循环\n");
        if (!c) break;
        sum += c; left -= c; pieces++;
    }
    CHECK(sum == 360448, "分片总和 %zu ≠ 360448\n", sum);
    CHECK(pieces == 1431, "片数 %zu（360448/252 = 1430 余 88 → 1431）\n", pieces);
}

/* ── 6. cap 不足必须返回 0 且**不写输出** ── */
static void test_no_write_on_failure(void)
{
    uint8_t pkt[4];
    memset(pkt, 0xA5, sizeof pkt);
    const uint8_t p[8] = {0};
    CHECK(cc13_bsl_pkt_build(pkt, sizeof pkt, CC13_BSL_CMD_DOWNLOAD, p, 8) == 0,
          "cap 不足应返回 0\n");
    for (size_t i = 0; i < sizeof pkt; i++)
        CHECK(pkt[i] == 0xA5, "失败时改写了输出（第 %zu 字节）\n", i);
}

/* ── 7. 收包校验：长度自洽 + checksum ──
 * 这是安全项：状态码是拿来判断"flash 写成功没有"的，校验松一点就等于
 * 拿噪声当成功。 */
static void test_incoming_validation(void)
{
    /* GET_STATUS 的应答：size=3, checksum=status, data=status */
    uint8_t ok[3] = { 0x03, CC13_BSL_RET_SUCCESS, CC13_BSL_RET_SUCCESS };
    CHECK(cc13_bsl_pkt_valid(ok, 3), "合法状态包被拒\n");

    uint8_t bad_sum[3] = { 0x03, 0x41, CC13_BSL_RET_SUCCESS };
    CHECK(!cc13_bsl_pkt_valid(bad_sum, 3), "checksum 错的包被接受\n");

    uint8_t bad_len[3] = { 0x04, CC13_BSL_RET_SUCCESS, CC13_BSL_RET_SUCCESS };
    CHECK(!cc13_bsl_pkt_valid(bad_len, 3), "size 与实际长度不符的包被接受\n");

    CHECK(!cc13_bsl_pkt_valid(ok, 2), "过短的包被接受\n");
    CHECK(!cc13_bsl_pkt_valid(NULL, 3), "NULL 被接受\n");

    /* 全 0（线上没人应答时的典型样子）和全 FF 都必须被拒 */
    uint8_t zeros[3] = { 0, 0, 0 };
    CHECK(!cc13_bsl_pkt_valid(zeros, 3), "全 0 被当成合法包\n");
    uint8_t ffs[3] = { 0xFF, 0xFF, 0xFF };
    CHECK(!cc13_bsl_pkt_valid(ffs, 3), "全 FF 被当成合法包\n");
}

/* ── 8. CRC32 已知答案 ──
 * "123456789" 的 IEEE 802.3 CRC-32 是 0xCBF43926，这是通用的标准向量。
 * 另验流式与一次性两条路径给出同一个值——352 KB 镜像只能流式算。 */
static void test_crc32_known_answer(void)
{
    const char *s = "123456789";
    uint32_t one = cc13_bsl_crc32((const uint8_t *)s, 9);
    CHECK(one == 0xCBF43926u, "CRC32(\"123456789\") = 0x%08X，标准值 0xCBF43926\n", one);

    CHECK(cc13_bsl_crc32(NULL, 0) == 0x00000000u,
          "空输入的 CRC32 应为 0（0xFFFFFFFF 取反）\n");

    /* 流式分片必须与一次性一致，且与分片方式无关 */
    uint32_t c = 0xFFFFFFFFu;
    c = cc13_bsl_crc32_update(c, (const uint8_t *)s, 4);
    c = cc13_bsl_crc32_update(c, (const uint8_t *)s + 4, 5);
    CHECK(cc13_bsl_crc32_final(c) == one, "流式(4+5)与一次性不一致\n");

    c = 0xFFFFFFFFu;
    for (int i = 0; i < 9; i++)
        c = cc13_bsl_crc32_update(c, (const uint8_t *)s + i, 1);
    CHECK(cc13_bsl_crc32_final(c) == one, "流式(逐字节)与一次性不一致\n");
}

int main(void)
{
    test_trm_figure_10_2();
    test_ping_packet();
    test_download_layout();
    test_crc32_cmd_layout();
    test_chunking();
    test_no_write_on_failure();
    test_incoming_validation();
    test_crc32_known_answer();

    if (g_fail) { printf("test_cc13_bsl_proto: %d FAIL\n", g_fail); return 1; }
    printf("test_cc13_bsl_proto: all OK\n");
    return 0;
}
