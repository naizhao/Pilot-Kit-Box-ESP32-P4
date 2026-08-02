/* test_search_page.c — host proof for search_page 的纯函数区
 * （查询串归一化 / 去重 / 桶内排序 / 最近搜索 LRU）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -DPK_SEARCH_PAGE_HOST_TEST \
 *      -o /tmp/test_sp firmware/test/test_search_page.c && /tmp/test_sp
 *
 * 同 test_pk_aero_layer.c / test_traffic_geom.c 的翻译单元惯例：把被测 .c
 * 直接拉进来。PK_SEARCH_PAGE_HOST_TEST 切掉后台任务 / NVS / 渲染三段
 * （它们要 FreeRTOS + IDF），只留纯计算——纯的部分本来就是照这个边界切的。
 * 刻意不依赖 sim/CMakeLists.txt：一行 cc 就能跑，是这批测试的价值所在。
 *
 * 四段：
 *   1) 归一化——大小写、字符集之外的字节、截断、空串。池里的代码字段全大写
 *      且比较走 memcmp，漏掉 toupper 就是"敲小写查不到"。
 *   2) 去重——(kind, idx) 为键。同一个机场会被"ICAO 精确"与"key 前缀"各命中
 *      一次，子串那桶里 name 与 city 还会再命中一次。
 *   3) 桶内排序——有本机位置按距离升序、没有按字典序；且**只动指定区间**
 *      （跨桶重排会把精确命中挤到模糊命中后面）。
 *   4) 历史 LRU——新词进队首、重复词提到队首不新增、满了挤掉队尾。
 */
#include <stdio.h>
#include <string.h>

#include "../main/search_page.c"

static int g_fail;

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
}

static void chk_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
        g_fail++;
    }
}

static void chk_true(const char *what, bool got)
{
    if (!got) { printf("FAIL %s: 期望 true\n", what); g_fail++; }
}

static void chk_false(const char *what, bool got)
{
    if (got) { printf("FAIL %s: 期望 false\n", what); g_fail++; }
}

/* ── 1. 查询串归一化 ─────────────────────────────────────────────── */
static void test_norm(void)
{
    char out[PK_SEARCH_QUERY_MAX + 1];

    chk_int("小写转大写 长度", pk_search_norm_query("zggg", out, sizeof out), 4);
    chk_str("小写转大写", out, "ZGGG");

    chk_int("已是大写", pk_search_norm_query("ZGGG", out, sizeof out), 4);
    chk_str("已是大写 内容", out, "ZGGG");

    /* 空格/斜杠/中文都不在键盘字符集里，一律丢弃而不是替换 */
    pk_search_norm_query("zg gg", out, sizeof out);
    chk_str("空格丢弃", out, "ZGGG");
    pk_search_norm_query("\xe4\xb8\xad ZG", out, sizeof out);
    chk_str("非 ASCII 丢弃", out, "ZG");
    pk_search_norm_query("A-B_1", out, sizeof out);
    chk_str("- 与 _ 保留", out, "A-B_1");

    chk_int("空串", pk_search_norm_query("", out, sizeof out), 0);
    chk_str("空串内容", out, "");
    chk_int("NULL", pk_search_norm_query(NULL, out, sizeof out), 0);
    chk_int("全是非法字符 = 无查询",
            pk_search_norm_query("!@#$", out, sizeof out), 0);

    /* 截断：cap 是含 NUL 的缓冲大小 */
    chk_int("截断长度", pk_search_norm_query("ABCDEFGHIJKLMNOP", out, sizeof out),
            PK_SEARCH_QUERY_MAX);
    chk_str("截断内容", out, "ABCDEFGHIJKL");
    chk_int("cap=1 只放得下 NUL", pk_search_norm_query("ZG", out, 1), 0);
}

/* ── 2. 去重 ─────────────────────────────────────────────────────── */
static pk_search_item_t mk(uint8_t kind, uint32_t idx, const char *code,
                           bool have_dist, double dist)
{
    pk_search_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    it.idx = idx;
    snprintf(it.code, sizeof(it.code), "%s", code);
    it.have_dist = have_dist;
    it.dist_nm = dist;
    return it;
}

static void test_dedup(void)
{
    pk_search_item_t arr[PK_SEARCH_MAX_RESULTS];
    int n = 0;

    pk_search_item_t a = mk(PK_SEARCH_KIND_AIRPORT, 7, "ZGGG", false, 0);
    chk_true("首次加入", pk_search_result_add(arr, &n, PK_SEARCH_MAX_RESULTS, &a));
    chk_int("加入后 n", n, 1);

    /* 桶不同、bucket 字段不同，但 (kind, idx) 相同 → 仍是同一个机场 */
    pk_search_item_t again = a;
    again.bucket = 2;
    chk_false("同 (kind,idx) 重复被拒",
              pk_search_result_add(arr, &n, PK_SEARCH_MAX_RESULTS, &again));
    chk_int("重复后 n 不变", n, 1);

    /* idx 相同但 kind 不同 → 是两条不同记录（段各自编号） */
    pk_search_item_t f = mk(PK_SEARCH_KIND_FIX, 7, "ZGGG", false, 0);
    chk_true("同 idx 不同 kind 可加",
             pk_search_result_add(arr, &n, PK_SEARCH_MAX_RESULTS, &f));
    chk_int("n=2", n, 2);

    /* 写满就停 */
    n = PK_SEARCH_MAX_RESULTS;
    pk_search_item_t x = mk(PK_SEARCH_KIND_NAVAID, 99, "SZA", false, 0);
    chk_false("满了拒收", pk_search_result_add(arr, &n, PK_SEARCH_MAX_RESULTS, &x));
    chk_int("满了 n 不变", n, PK_SEARCH_MAX_RESULTS);
}

/* ── 3. 桶内排序 ─────────────────────────────────────────────────── */
static void test_sort(void)
{
    pk_search_item_t arr[6];
    int n;

    /* 有本机位置：距离升序 */
    n = 0;
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 1, "ZGSZ", true, 30.0);
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 2, "ZGGG", true,  5.0);
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 3, "ZGOW", true, 12.0);
    pk_search_sort_range(arr, 0, n);
    chk_str("距离序[0]", arr[0].code, "ZGGG");
    chk_str("距离序[1]", arr[1].code, "ZGOW");
    chk_str("距离序[2]", arr[2].code, "ZGSZ");

    /* 无本机位置：字典序（**不是**按 0 排——那等于按数据库物理顺序，屏上像随机）*/
    n = 0;
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 1, "ZGSZ", false, 0);
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 2, "ZGGG", false, 0);
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 3, "ZGOW", false, 0);
    pk_search_sort_range(arr, 0, n);
    chk_str("字典序[0]", arr[0].code, "ZGGG");
    chk_str("字典序[1]", arr[1].code, "ZGOW");
    chk_str("字典序[2]", arr[2].code, "ZGSZ");

    /* 只动指定区间：桶1 的精确命中必须留在最前，哪怕它离得最远 */
    n = 0;
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 9, "ZGGG", true, 900.0);  /* 桶1 */
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 1, "ZGSZ", true,  30.0);  /* 桶2 */
    arr[n++] = mk(PK_SEARCH_KIND_AIRPORT, 2, "ZGOW", true,   5.0);
    pk_search_sort_range(arr, 1, n);
    chk_str("跨桶不重排：精确命中仍在首位", arr[0].code, "ZGGG");
    chk_str("桶2 内部已排序[0]", arr[1].code, "ZGOW");
    chk_str("桶2 内部已排序[1]", arr[2].code, "ZGSZ");

    /* 退化输入不该崩 */
    pk_search_sort_range(arr, 2, 2);
    pk_search_sort_range(NULL, 0, 3);
    chk_str("空区间无副作用", arr[0].code, "ZGGG");
}

/* ── 4. 最近搜索 LRU ─────────────────────────────────────────────── */
static void test_hist(void)
{
    pk_search_hist_t h;
    memset(&h, 0, sizeof(h));

    pk_search_hist_push(&h, "ZGGG");
    pk_search_hist_push(&h, "ZGSZ");
    chk_int("两条", h.n, 2);
    chk_str("最新在队首", h.items[0], "ZGSZ");
    chk_str("次新在其后", h.items[1], "ZGGG");

    /* 重复：提到队首，不新增 */
    pk_search_hist_push(&h, "ZGGG");
    chk_int("重复不新增", h.n, 2);
    chk_str("重复提到队首", h.items[0], "ZGGG");
    chk_str("被挤下去的还在", h.items[1], "ZGSZ");

    /* 空串忽略 */
    pk_search_hist_push(&h, "");
    pk_search_hist_push(&h, NULL);
    chk_int("空串/NULL 忽略", h.n, 2);

    /* 填满再溢出：最老的那条被挤掉 */
    memset(&h, 0, sizeof(h));
    for (int i = 0; i < PK_SEARCH_HIST_MAX; ++i) {
        char q[8];
        snprintf(q, sizeof(q), "A%03d", i);
        pk_search_hist_push(&h, q);
    }
    chk_int("填满", h.n, PK_SEARCH_HIST_MAX);
    chk_str("队首是最后压入的", h.items[0], "A013");
    chk_str("队尾是最早压入的", h.items[PK_SEARCH_HIST_MAX - 1], "A000");

    pk_search_hist_push(&h, "NEW");
    chk_int("溢出后条数不变", h.n, PK_SEARCH_HIST_MAX);
    chk_str("新词在队首", h.items[0], "NEW");
    chk_str("最老的被挤掉（原队尾 A000 已不在）",
            h.items[PK_SEARCH_HIST_MAX - 1], "A001");

    /* 表里已有的词在队尾时也要能提到队首，且不丢别的条目 */
    pk_search_hist_push(&h, "A001");
    chk_int("提队尾条目 条数不变", h.n, PK_SEARCH_HIST_MAX);
    chk_str("提队尾条目 到队首", h.items[0], "A001");
    chk_str("原队首顺延", h.items[1], "NEW");
}

int main(void)
{
    test_norm();
    test_dedup();
    test_sort();
    test_hist();
    if (g_fail == 0) printf("test_search_page: 全部通过\n");
    else             printf("test_search_page: %d 处失败\n", g_fail);
    return g_fail ? 1 : 0;
}
