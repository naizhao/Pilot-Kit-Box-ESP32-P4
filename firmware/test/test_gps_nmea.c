/* test_gps_nmea.c — host proof for gps_nmea（纯 NMEA 句法解析 + *hh checksum 门）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_gps_nmea \
 *      firmware/test/test_gps_nmea.c firmware/main/gps_nmea.c \
 *      && /tmp/test_gps_nmea
 *
 *   ASan/UBSan（超长行"不越界"靠它作证）：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_gps_nmea_asan \
 *      firmware/test/test_gps_nmea.c firmware/main/gps_nmea.c \
 *      && /tmp/test_gps_nmea_asan
 *
 * WP-B Task 1：把解析从 gps_task.c 抽成纯模块。老 handle_line 来什么吃
 * 什么、从不验 *hh——损坏句子的字段会直接进 fix/校时。本文件钉死解析器
 * 合同（brief 六条判据）：
 *
 *   1. 合法句子（checksum 现算）→ 1，type 为去 talker 消息类型，字段数正确；
 *   2. checksum 错 / 载荷损坏但 checksum 没跟着改 → 0；
 *   3. 非 $ 开头 / 空行 / 缺 *hh → 0；
 *   4. 超长行（≥ GPS_NMEA_LINE_MAX 字节）→ 0 且不越界；
 *   5. 未知/小写 talker → 仍解析，type 取 addr 尾 3 字符；
 *   6. * 后出现非 hex → 0。
 *
 * 外加 Task 2 会依赖的合同细节：f[0] 是带 talker 的原始 addr（parse_gsv 靠
 * 它映射星座）、*hh 从尾字段剥掉、f[] 的生命周期到下一次 feed、未知消息
 * 类型（如 GSA）也返回 1 由调用方自行取舍。
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/gps_nmea.h"

static int g_fail = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* body = $ 与 *hh 之间的部分 → 现算 checksum 拼成完整句子 */
static void mk_sentence(char *buf, size_t cap, const char *body)
{
    int cs = 0;
    for (const char *p = body; *p; p++) cs ^= (unsigned char)*p;
    snprintf(buf, cap, "$%s*%02X", body, cs);
}

/* AT6558 实发 RMC 的形状：13 字段（f[2]=status，f[9]=日期，尾段 mode）。 */
static const char *RMC_BODY =
    "GNRMC,063956.00,A,3150.1000,N,11711.2000,E,050.0,084.5,060926,,,A";

/* 构造总长恰为 total 字节的合法句子：尾字段补 '0'（仍是 13 字段）。 */
static bool build_sentence_len(char *buf, size_t cap, int total)
{
    char body[GPS_NMEA_LINE_MAX + 64];
    snprintf(body, sizeof(body), "%s", RMC_BODY);
    int want = total - 4;                       /* 去掉 $、*、两位 hex */
    if (want < 4) return false;
    size_t bl = strlen(body);
    if ((int)bl > want) return false;
    memset(body + bl, '0', (size_t)want - (int)bl);
    body[want] = '\0';
    mk_sentence(buf, cap, body);
    return strlen(buf) == (size_t)total;
}

/* ── 1. 合法句子：返回 1、type、字段数 ─────────────────────────────── */

static void test_valid_rmc(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;
    mk_sentence(line, sizeof(line), RMC_BODY);

    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(strcmp(m.type, "RMC") == 0);       /* 去 talker 的消息类型 */
    CHECK(m.n == 13);                        /* 空字段也计数 */
    CHECK(strcmp(m.f[0], "GNRMC") == 0);     /* f[0] = 带 talker 的原始 addr */
    CHECK(m.f[2][0] == 'A');                 /* fix 状态可读 */
    CHECK(strcmp(m.f[9], "060926") == 0);    /* 日期字段（校时用） */
    CHECK(strcmp(m.f[12], "A") == 0);        /* 尾字段不带 *hh */
}

/* checksum 两位大小写不敏感（brief Step 3 明确要求）。 */
static void test_checksum_hex_case_insensitive(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;
    mk_sentence(line, sizeof(line), RMC_BODY);
    size_t l = strlen(line);
    line[l - 2] = (char)tolower((unsigned char)line[l - 2]);
    line[l - 1] = (char)tolower((unsigned char)line[l - 1]);
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(strcmp(m.type, "RMC") == 0);
}

/* ── 2. checksum 错 / 载荷损坏 → 0 ─────────────────────────────────── */

static void test_bad_checksum(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;

    mk_sentence(line, sizeof(line), RMC_BODY);
    size_t l = strlen(line);
    line[l - 1] = (line[l - 1] == '0') ? '1' : '0';   /* 翻转一位 hex */
    CHECK(gps_nmea_feed_line(&m, line) == 0);

    /* 载荷损坏、checksum 没跟着改 → 同样拒收。 */
    mk_sentence(line, sizeof(line), RMC_BODY);
    char *t = strstr(line, "063956.00");
    CHECK(t != NULL);
    t[0] = '1';
    CHECK(gps_nmea_feed_line(&m, line) == 0);
}

/* ── 3. 非 $ 开头 / 空行 / 缺 *hh → 0 ─────────────────────────────── */

static void test_not_a_sentence(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;

    CHECK(gps_nmea_feed_line(&m, "") == 0);            /* 空行 */
    CHECK(gps_nmea_feed_line(&m, RMC_BODY) == 0);      /* 非 $ 开头 */

    mk_sentence(line, sizeof(line), RMC_BODY);
    char *star = strrchr(line, '*');
    CHECK(star != NULL);
    *star = ',';                                       /* 缺 *hh */
    CHECK(gps_nmea_feed_line(&m, line) == 0);

    CHECK(gps_nmea_feed_line(&m, NULL) == 0);          /* 防御：NULL 入参 */
    CHECK(gps_nmea_feed_line(NULL, line) == 0);
}

/* ── 4. 超长行 → 0 且不越界（边界两侧都钉住） ───────────────────────── */

static void test_line_length_boundary(void)
{
    /* 缓冲要容得下刻意超长的句子本身——超长判断在解析器里，不靠截断。 */
    char line[GPS_NMEA_LINE_MAX + 64];
    gps_nmea_msg_t m;

    /* 127 字节 = 缓冲上限，形状合法 → 照常解析。 */
    CHECK(build_sentence_len(line, sizeof(line), GPS_NMEA_LINE_MAX - 1));
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(m.n == 13);

    /* 128 字节起整句拒收，绝不截半句、不越界（ASan 变体作证）。 */
    CHECK(build_sentence_len(line, sizeof(line), GPS_NMEA_LINE_MAX));
    CHECK(gps_nmea_feed_line(&m, line) == 0);

    CHECK(build_sentence_len(line, sizeof(line), GPS_NMEA_LINE_MAX + 40));
    CHECK(gps_nmea_feed_line(&m, line) == 0);
}

/* ── 5. 未知/小写 talker → 仍解析，type 按 suf ─────────────────────── */

static void test_talker_variants(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;

    /* 未知 talker QZ（QZSS）：照常解析。 */
    mk_sentence(line, sizeof(line),
                "QZRMC,063956.00,A,3150.1000,N,11711.2000,E,050.0,084.5,060926,,,A");
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(strcmp(m.type, "RMC") == 0);

    /* 小写 addr：仍解析；type 按尾 3 字符原样（gps_task 匹配区分大小写，
     * 与老代码一致——真实模块只发大写，这里钉的是语义而非放宽）。 */
    mk_sentence(line, sizeof(line),
                "gnrmc,063956.00,A,3150.1000,N,11711.2000,E,050.0,084.5,060926,,,A");
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(strcmp(m.type, "rmc") == 0);
}

/* ── 6. * 后非 hex / 位数不对 → 0 ─────────────────────────────────── */

static void test_nonhex_checksum(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;

    mk_sentence(line, sizeof(line), RMC_BODY);
    char *star = strrchr(line, '*');
    star[1] = 'G'; star[2] = 'Z';                      /* 非 hex */
    CHECK(gps_nmea_feed_line(&m, line) == 0);

    mk_sentence(line, sizeof(line), RMC_BODY);
    star = strrchr(line, '*');
    star[2] = '\0';                                    /* 只剩 1 位 */
    CHECK(gps_nmea_feed_line(&m, line) == 0);

    mk_sentence(line, sizeof(line), RMC_BODY);
    star = strrchr(line, '*');
    memmove(star + 1, "7A5", 4);                       /* 3 位：*7A5\0 */
    CHECK(gps_nmea_feed_line(&m, line) == 0);
}

/* ── 其余消息形状 + 未知类型 + 生命周期 ────────────────────────────── */

static void test_other_messages_and_unknown_type(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;

    /* GSA：未知消息类型 → 仍返回 1、type 填好，调用方自行忽略。 */
    mk_sentence(line, sizeof(line), "GNGSA,A,3,04,05,,09,12,,,24,,,,2.5,1.3,2.1");
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(strcmp(m.type, "GSA") == 0);
    CHECK(m.n == 17);

    /* GSV：parse_gsv 消费的形状（f[0] 带 talker、snr 在 f[7]）。 */
    mk_sentence(line, sizeof(line), "GNGSV,3,1,11,04,15,100,42");
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(strcmp(m.type, "GSV") == 0);
    CHECK(m.n == 8);
    CHECK(strcmp(m.f[0], "GNGSV") == 0);
    CHECK(strcmp(m.f[7], "42") == 0);

    /* TXT：*hh 剥掉后尾字段是干净文本，strstr("ANTENNA OPEN") 命中。 */
    mk_sentence(line, sizeof(line), "GPTXT,01,01,02,ANTENNA OPEN");
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(strcmp(m.type, "TXT") == 0);
    CHECK(m.n == 5);
    CHECK(strcmp(m.f[4], "ANTENNA OPEN") == 0);
}

static void test_lifetime_until_next_feed(void)
{
    char rmc[GPS_NMEA_LINE_MAX], gsv[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;

    mk_sentence(rmc, sizeof(rmc), RMC_BODY);
    CHECK(gps_nmea_feed_line(&m, rmc) == 1);
    CHECK(strcmp(m.f[0], "GNRMC") == 0);

    mk_sentence(gsv, sizeof(gsv), "GNGSV,3,1,11,04,15,100,42");
    CHECK(gps_nmea_feed_line(&m, gsv) == 1);

    /* 老 msg 的 f[] 仍指向内部缓冲——此刻反映的是第二句（头文件写明的
     * 生命周期合同：到下一次 parse 为止）。 */
    CHECK(strcmp(m.f[0], "GNGSV") == 0);
}

/* 失败后 msg 必须回到可判断状态：n 清零，不留上一句的残影。 */
static void test_failure_clears_msg(void)
{
    char line[GPS_NMEA_LINE_MAX];
    gps_nmea_msg_t m;

    mk_sentence(line, sizeof(line), RMC_BODY);
    CHECK(gps_nmea_feed_line(&m, line) == 1);
    CHECK(m.n == 13);

    line[strlen(line) - 1] = '0';                      /* 弄坏 checksum */
    CHECK(gps_nmea_feed_line(&m, line) == 0);
    CHECK(m.n == 0);
    CHECK(m.type[0] == '\0');
}

int main(void)
{
    test_valid_rmc();
    test_checksum_hex_case_insensitive();
    test_bad_checksum();
    test_not_a_sentence();
    test_line_length_boundary();
    test_talker_variants();
    test_nonhex_checksum();
    test_other_messages_and_unknown_type();
    test_lifetime_until_next_feed();
    test_failure_clears_msg();

    if (g_fail == 0) { printf("test_gps_nmea: all OK\n"); return 0; }
    fprintf(stderr, "test_gps_nmea: %d check(s) FAILED\n", g_fail);
    return 1;
}
