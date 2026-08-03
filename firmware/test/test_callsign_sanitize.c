/* test_callsign_sanitize.c — host proof for aircraft_state.c 的呼号规范化。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_callsign firmware/test/test_callsign_sanitize.c && \
 *      /tmp/test_callsign
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_callsign_asan \
 *      firmware/test/test_callsign_sanitize.c && /tmp/test_callsign_asan
 *
 * 同 test_pk_vib.c 惯例：把被测 .c 直接 #include 进同一 TU。呼号规范化之所以
 * 单独住在 pk_callsign.c 而不在 aircraft_state.c 里，就是为了能这么测——
 * aircraft_state.c 拖着 FreeRTOS/esp_log/demo_data，host 编不动（sim 也不编
 * 它，compat 里连 esp_log.h 都没有）。拆法照 pk_rec_store.c/pk_rec_store_fs.c。
 *
 * 被测契约（见 pk_callsign_sanitize() 头注）：
 *   1. 合法字符集只有 A-Z / 0-9 / 空格——ais_charset 里 64 个码位有 27 个
 *      是保留位、一律翻译成 '?'，CRC 过了不代表这 8 个字符合法；
 *   2. 含非法字符 -> 整条丢弃（false），**不能**剔掉问号后返回残缺呼号；
 *   3. 全空格 -> false，避免一条空报文抹掉之前收到的好呼号；
 *   4. 尾部填充空格要剥掉（ADS-B 右侧补空格是正常填充）。
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/pk_callsign.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ================================================== 正常路径 */

static void test_plain_callsign_passes(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(pk_callsign_sanitize("CES9982", out));
    CHECK(strcmp(out, "CES9982") == 0);
}

static void test_full_eight_chars_passes(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(pk_callsign_sanitize("ABCD1234", out));
    CHECK(strcmp(out, "ABCD1234") == 0);
}

static void test_trailing_pad_spaces_stripped(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(pk_callsign_sanitize("CCA101  ", out));
    CHECK(strcmp(out, "CCA101") == 0);
}

/* 机身注册号直接当呼号播（通航常见），全字母也必须放行。 */
static void test_all_letters_passes(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(pk_callsign_sanitize("BXXXX", out));
    CHECK(strcmp(out, "BXXXX") == 0);
}

/* ================================================== 拒绝路径 */

/* 这条是本次真机 bug 的复现：屏上出现 "CE?9?82"。 */
static void test_reserved_codepoint_question_mark_rejected(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(!pk_callsign_sanitize("CE?9?82", out));
}

/* 关键契约：不能"剔掉问号再显示"——那会得到看着合法、实际错误的呼号。
 * 这里直接钉死：拒绝时不产生任何形似合法的输出。 */
static void test_rejection_does_not_salvage_partial(void)
{
    char out[PK_CALLSIGN_LEN];
    memset(out, 'Z', sizeof(out));
    CHECK(!pk_callsign_sanitize("CE?9?82", out));
    /* 调用方约定：返回 false 时不看 out，所以这里不断言 out 的内容，
     * 只断言函数没有"返回 true + 残缺串"这种最危险的组合。 */
    CHECK(!pk_callsign_sanitize("?", out));
    CHECK(!pk_callsign_sanitize("CES99?2", out));
}

static void test_all_spaces_rejected(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(!pk_callsign_sanitize("        ", out));
}

static void test_empty_rejected(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(!pk_callsign_sanitize("", out));
}

/* 小写不在 ADS-B 字符集里，出现即说明解码有问题，一样拒。 */
static void test_lowercase_rejected(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(!pk_callsign_sanitize("ces9982", out));
}

static void test_punctuation_rejected(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(!pk_callsign_sanitize("CES-982", out));
    CHECK(!pk_callsign_sanitize("CES_982", out));
}

/* ================================================== 边界 */

/* 不能读过 8 字节，也不能漏掉 NUL。传一个刚好占满的串，出参必须仍是
 * 合法 C 串（strlen <= 8）。 */
static void test_output_always_nul_terminated(void)
{
    char out[PK_CALLSIGN_LEN];
    memset(out, 'Z', sizeof(out));
    CHECK(pk_callsign_sanitize("ABCD1234", out));
    CHECK(strlen(out) == 8);
}

/* 内嵌空格：ADS-B 里空格只作右侧填充，但字符集本身允许，中间出现不算
 * 非法。剥离只动尾部，中间的必须原样保留。 */
static void test_interior_space_kept(void)
{
    char out[PK_CALLSIGN_LEN];
    CHECK(pk_callsign_sanitize("N1 2AB  ", out));
    CHECK(strcmp(out, "N1 2AB") == 0);
}

/* ============================================ pk_callsign_display */

/* 出参必须**无条件**被写满，不能出现"某条分支下没写"的情况——map_page.c
 * 那份手写实现就是漏了 have_callsign=false 的分支，屏上打出栈垃圾。
 * 这里把 out 预填成不含 NUL 的图案，函数返回后必须整条被覆盖。 */
static void test_display_no_callsign_falls_back_to_icao(void)
{
    char out[10];
    memset(out, 'Z', sizeof(out));
    pk_callsign_display(false, "", 0xABCDEF, out, sizeof(out));
    CHECK(strcmp(out, "ABCDEF") == 0);
}

/* have_callsign 为真但内容是空串——同样必须回退，不能留空。 */
static void test_display_empty_callsign_falls_back(void)
{
    char out[10];
    memset(out, 'Z', sizeof(out));
    pk_callsign_display(true, "", 0x123456, out, sizeof(out));
    CHECK(strcmp(out, "123456") == 0);
}

/* 全空格呼号也算没有，回退 ICAO 而不是显示一片空白。 */
static void test_display_all_space_callsign_falls_back(void)
{
    char out[10];
    memset(out, 'Z', sizeof(out));
    pk_callsign_display(true, "        ", 0x00BEEF, out, sizeof(out));
    CHECK(strcmp(out, "00BEEF") == 0);
}

static void test_display_normal_callsign(void)
{
    char out[10];
    memset(out, 'Z', sizeof(out));
    pk_callsign_display(true, "CES9982", 0xABCDEF, out, sizeof(out));
    CHECK(strcmp(out, "CES9982") == 0);
}

static void test_display_strips_trailing_pad(void)
{
    char out[10];
    pk_callsign_display(true, "CCA101  ", 0xABCDEF, out, sizeof(out));
    CHECK(strcmp(out, "CCA101") == 0);
}

/* 中间空格保留——adsb_list 那份旧实现剔掉所有空格，会把它变成 N12AB。 */
static void test_display_keeps_interior_space(void)
{
    char out[10];
    pk_callsign_display(true, "N1 2AB", 0xABCDEF, out, sizeof(out));
    CHECK(strcmp(out, "N1 2AB") == 0);
}

/* 缓冲比呼号短：截断也必须 NUL 结尾，绝不越界。 */
static void test_display_truncates_safely(void)
{
    char out[5];
    memset(out, 'Z', sizeof(out));
    pk_callsign_display(true, "ABCD1234", 0xABCDEF, out, sizeof(out));
    CHECK(strlen(out) < sizeof(out));
    CHECK(strncmp(out, "ABCD", 4) == 0);
}

/* ICAO 回退路径在小缓冲下同样不能越界。 */
static void test_display_icao_fallback_truncates_safely(void)
{
    char out[4];
    memset(out, 'Z', sizeof(out));
    pk_callsign_display(false, NULL, 0xABCDEF, out, sizeof(out));
    CHECK(strlen(out) < sizeof(out));
}

/* callsign 指针为 NULL 也不能崩——调用方传 a->callsign 时理论上非 NULL，
 * 但这个函数是三处共用的公共入口，不该假设。 */
static void test_display_null_callsign_safe(void)
{
    char out[10];
    memset(out, 'Z', sizeof(out));
    pk_callsign_display(true, NULL, 0x0000FF, out, sizeof(out));
    CHECK(strcmp(out, "0000FF") == 0);
}

int main(void)
{
    test_plain_callsign_passes();
    test_full_eight_chars_passes();
    test_trailing_pad_spaces_stripped();
    test_all_letters_passes();

    test_reserved_codepoint_question_mark_rejected();
    test_rejection_does_not_salvage_partial();
    test_all_spaces_rejected();
    test_empty_rejected();
    test_lowercase_rejected();
    test_punctuation_rejected();

    test_output_always_nul_terminated();
    test_interior_space_kept();

    test_display_no_callsign_falls_back_to_icao();
    test_display_empty_callsign_falls_back();
    test_display_all_space_callsign_falls_back();
    test_display_normal_callsign();
    test_display_strips_trailing_pad();
    test_display_keeps_interior_space();
    test_display_truncates_safely();
    test_display_icao_fallback_truncates_safely();
    test_display_null_callsign_safe();

    if (g_fail == 0) {
        printf("test_callsign_sanitize: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_callsign_sanitize: FAIL (%d)\n", g_fail);
    return 1;
}
