/* test_airline_codes.c — host proof：航司码表查询 + 表本身的不变量。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_airline \
 *      firmware/test/test_airline_codes.c && /tmp/test_airline
 *
 * 覆盖四件事：
 *   1) 表不变量：ICAO 码严格升序（二分查找的前提）、全部 3 个大写字母、
 *      无重复。表是脚本生成的，生成器改坏了排序这里立刻红；
 *   2) 命中：常见中国航司 + 国际大航司，防止解析规则调整误伤存量条目；
 *   3) 正确的不命中：注册号型呼号（N12345 / B1234）、全字母呼号、
 *      3 字符呼号、空串、NULL——这些返回 NULL 是设计意图不是 bug；
 *   4) flight_number_out 的切分位置。
 *
 * 第 2 组里的 LKE（祥鹏航空）是回归哨兵：它曾经因为维基那一行的
 * IATA 单元格里嵌了跨行 {{cite web}} 模板、参数行顶在列 0，被生成器的
 * 切列逻辑当成新单元格，整行右移后 ICAO 位取到 "website=..."，于是
 * **从来没进过表**。祥鹏的航班在屏上一直显示不出航司就是这么来的。
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/airline_codes.c"

static int g_fail;

static void chk(const char *what, bool ok)
{
    if (!ok) { printf("FAIL %s\n", what); g_fail++; }
}

/* 期望命中，且名字要匹配。 */
static void chk_hit(const char *cs, const char *want_name, const char *want_num)
{
    const char *num = NULL;
    const pk_airline_t *a = pk_airline_from_callsign(cs, &num);
    if (a == NULL) {
        printf("FAIL %s: 期望命中 \"%s\"，实际 NULL\n", cs, want_name);
        g_fail++;
        return;
    }
    if (strcmp(a->name, want_name) != 0) {
        printf("FAIL %s: 名字 got \"%s\" want \"%s\"\n", cs, a->name, want_name);
        g_fail++;
    }
    if (num == NULL || strcmp(num, want_num) != 0) {
        printf("FAIL %s: 航班号 got \"%s\" want \"%s\"\n",
               cs, num ? num : "(null)", want_num);
        g_fail++;
    }
}

/* 期望不命中，且 flight_number_out 必须被清成 NULL（调用方会直接用它）。 */
static void chk_miss(const char *what, const char *cs)
{
    const char *num = (const char *)0x1;   /* 先塞脏值，验证被覆盖 */
    const pk_airline_t *a = pk_airline_from_callsign(cs, &num);
    if (a != NULL) {
        printf("FAIL %s (\"%s\"): 期望 NULL，实际命中 \"%s\"\n",
               what, cs, a->name);
        g_fail++;
    }
    if (num != NULL) {
        printf("FAIL %s (\"%s\"): 未命中时 flight_number_out 没被清空\n",
               what, cs);
        g_fail++;
    }
}

int main(void)
{
    /* ── 1) 表不变量 ───────────────────────────────────────────── */
    chk("表非空", S_AIRLINES_N > 0);
    for (size_t i = 0; i < S_AIRLINES_N; ++i) {
        const char *c = s_airlines[i].icao3;
        if (strlen(c) != 3 || c[0] < 'A' || c[0] > 'Z' ||
            c[1] < 'A' || c[1] > 'Z' || c[2] < 'A' || c[2] > 'Z') {
            printf("FAIL 第 %zu 条 ICAO 不是 3 个大写字母: \"%s\"\n", i, c);
            g_fail++;
            break;
        }
        /* 严格升序：<= 会放过重复，而重复条目意味着二分只能查到其中一条，
         * 另一条永远取不到——那是生成器去重坏了。 */
        if (i > 0 && strcmp(s_airlines[i - 1].icao3, c) >= 0) {
            printf("FAIL 第 %zu 条破坏升序/有重复: \"%s\" 在 \"%s\" 之后\n",
                   i, c, s_airlines[i - 1].icao3);
            g_fail++;
            break;
        }
    }
    /* 二分能查到**每一条**——升序断言之外再正面走一遍，防止比较器本身写反。 */
    for (size_t i = 0; i < S_AIRLINES_N; ++i) {
        if (pk_airline_from_icao3(s_airlines[i].icao3) != &s_airlines[i]) {
            printf("FAIL 二分查不到自己的第 %zu 条: \"%s\"\n",
                   i, s_airlines[i].icao3);
            g_fail++;
            break;
        }
    }

    /* ── 2) 命中：中国航司 ─────────────────────────────────────── */
    chk_hit("CSN3341", "China Southern Airlines", "3341");
    chk_hit("CES2158", "China Eastern Airlines",  "2158");
    chk_hit("CCA1501", "Air China",               "1501");
    chk_hit("CQH8802", "Spring Airlines",         "8802");
    chk_hit("CHH7302", "Hainan Airlines",         "7302");
    chk_hit("CXA8301", "Xiamen Airlines",         "8301");
    chk_hit("CSZ9821", "Shenzhen Airlines",       "9821");
    chk_hit("GCR6543", "Tianjin Airlines",        "6543");
    chk_hit("HXA1205", "China Express Airlines",  "1205");
    chk_hit("DKH1234", "Juneyao Airlines",        "1234");   /* MANUAL_ADDITIONS */
    chk_hit("RLH5301", "Ruili Airlines",          "5301");   /* MANUAL_ADDITIONS */
    /* 回归哨兵：跨行 {{cite web}} 曾让这条整行丢失。见文件头注。 */
    chk_hit("LKE8912", "Lucky Air",               "8912");

    /* ── 2b) 命中：国际大航司（防解析改动误伤存量） ────────────── */
    chk_hit("UAL328",  "United Airlines",   "328");
    chk_hit("DLH400",  "Lufthansa",         "400");
    chk_hit("BAW117",  "British Airways",   "117");
    chk_hit("AFR83",   "Air France",        "83");
    chk_hit("JAL999",  "Japan Airlines",    "999");
    chk_hit("UAE201",  "Emirates",          "201");
    /* ABX Air 曾因备注里提到 "former Airborne Express" 被停业过滤误杀。 */
    chk_hit("ABX1234", "ABX Air",           "1234");
    /* Scoot 曾因 "Merged with Scoot"（那是 Tigerair 那一行的话）被误杀。 */
    chk_hit("TGW7",    "Scoot",             "7");

    /* DLH 必须是 "Lufthansa" 而不是 D 页那条 "Deutsche Luft Hansa"——
     * 干净行要压过带 "Became Lufthansa" 备注的历史行。上面已断言名字，
     * 这里再钉一次意图，免得有人调 tier 优先级时看不懂为什么。 */
    chk("DLH 取当代名而非历史名",
        strcmp(pk_airline_from_icao3("DLH")->name, "Lufthansa") == 0);

    /* 大小写不敏感（pk_airline_from_icao3 自己 toupper）。 */
    chk("小写 icao3 也能查到",
        pk_airline_from_icao3("csn") == pk_airline_from_icao3("CSN"));

    /* ── 3) 正确的不命中 ───────────────────────────────────────── */
    chk_miss("美籍尾号",       "N12345");
    chk_miss("中国籍尾号",     "B1234");
    chk_miss("全字母呼号",     "ABCDEF");
    chk_miss("恰好 3 字符",    "CSN");
    chk_miss("4 字符但第 4 位是字母", "CSNA");
    chk_miss("空串",           "");
    chk_miss("首位非字母",     "1CSN23");
    chk_miss("表里没有的三字码", "QQQ123");
    {   /* NULL 呼号：不能崩，且要清 out 参数。 */
        const char *num = (const char *)0x1;
        chk("NULL 呼号返回 NULL", pk_airline_from_callsign(NULL, &num) == NULL);
        chk("NULL 呼号也清 out",  num == NULL);
    }
    /* out 参数传 NULL 不能崩（调用方可能不关心航班号）。 */
    chk("out 传 NULL 不崩", pk_airline_from_callsign("CSN3341", NULL) != NULL);

    /* pk_airline_from_icao3 的边界：不足 3 字符必须返回 NULL 而不是越界读。 */
    chk("icao3 空串",   pk_airline_from_icao3("")    == NULL);
    chk("icao3 1 字符", pk_airline_from_icao3("C")   == NULL);
    chk("icao3 2 字符", pk_airline_from_icao3("CS")  == NULL);
    chk("icao3 NULL",   pk_airline_from_icao3(NULL)  == NULL);

    printf(g_fail ? "\n%d FAILED\n" : "\nOK (%d failed)\n", g_fail);
    printf("表内条目: %zu\n", S_AIRLINES_N);
    return g_fail ? 1 : 0;
}
