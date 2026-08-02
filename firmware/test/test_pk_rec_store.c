/* test_pk_rec_store.c — host proof for pk_rec_store 的纯逻辑部分（序号
 * 分配/回绕、保留策略选谁删、SD 降级档位判定）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_recstore \
 *      firmware/test/test_pk_rec_store.c && /tmp/test_recstore
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_recstore_asan \
 *      firmware/test/test_pk_rec_store.c && /tmp/test_recstore_asan
 *
 * 只 #include pk_rec_store.c（纯逻辑，不依赖 IDF）——真机专属的文件
 * 系统 / FreeRTOS 胶水在 pk_rec_store_fs.c，那部分 host 编不过（依赖
 * dirent/FreeRTOS/esp_log/pk_sdcard 等），本测试不覆盖，同文件头部注释
 * 说明的分层。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/pk_rec_store.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ================================================================ 序号分配 */

static void test_alloc_seq_empty(void)
{
    static bool used[PK_REC_STORE_SEQ_SPACE];
    memset(used, 0, sizeof(used));
    CHECK(pk_rec_store_alloc_seq(used) == 0);
}

static void test_alloc_seq_normal_increment(void)
{
    static bool used[PK_REC_STORE_SEQ_SPACE];
    memset(used, 0, sizeof(used));
    used[0] = true;
    used[1] = true;
    used[2] = true;
    CHECK(pk_rec_store_alloc_seq(used) == 3);
}

static void test_alloc_seq_ignores_gaps(void)
{
    /* max+1 语义：中间有空洞也不回填，只看最大值——回填是回绕专属行为。 */
    static bool used[PK_REC_STORE_SEQ_SPACE];
    memset(used, 0, sizeof(used));
    used[0] = true;
    used[5] = true;   /* 5 是当前最大值，1-4 是空洞 */
    CHECK(pk_rec_store_alloc_seq(used) == 6);
}

static void test_alloc_seq_wraparound_reuses_smallest_free(void)
{
    static bool used[PK_REC_STORE_SEQ_SPACE];
    memset(used, 1, sizeof(used));   /* 1 = 合法的 true 字节表示，0xFF 对 _Bool 是 UB；全部占用 */
    used[7] = false;                    /* 唯一空位 */
    CHECK(pk_rec_store_alloc_seq(used) == 7);
}

static void test_alloc_seq_wraparound_picks_smallest_of_several(void)
{
    static bool used[PK_REC_STORE_SEQ_SPACE];
    memset(used, 1, sizeof(used));   /* 1 = 合法的 true 字节表示，0xFF 对 _Bool 是 UB */
    used[0] = false;
    used[3] = false;
    used[9999] = true;   /* 保持顶到头，触发回绕分支 */
    CHECK(pk_rec_store_alloc_seq(used) == 0);
}

static void test_alloc_seq_full_degrades_to_zero(void)
{
    static bool used[PK_REC_STORE_SEQ_SPACE];
    memset(used, 1, sizeof(used));   /* 1 = 合法的 true 字节表示，0xFF 对 _Bool 是 UB；10000 个全占用，无空位 */
    CHECK(pk_rec_store_alloc_seq(used) == 0);
}

/* ================================================================ 保留策略 */

static void test_prune_below_keep_selects_nothing(void)
{
    uint16_t seqs[] = {1, 2, 3};
    uint16_t out[8];
    size_t n = pk_rec_store_select_prune(seqs, 3, 32, out);
    CHECK(n == 0);
}

static void test_prune_exactly_keep_selects_nothing(void)
{
    uint16_t seqs[] = {1, 2, 3};
    uint16_t out[8];
    size_t n = pk_rec_store_select_prune(seqs, 3, 3, out);
    CHECK(n == 0);
}

static void test_prune_selects_smallest_first(void)
{
    /* 乱序输入，5 个里保留 3 个——应删掉数值最小的两个（1, 2）。 */
    uint16_t seqs[] = {5, 1, 4, 2, 3};
    uint16_t out[8] = {0};
    size_t n = pk_rec_store_select_prune(seqs, 5, 3, out);
    CHECK(n == 2);
    CHECK(out[0] == 1);
    CHECK(out[1] == 2);
}

static void test_prune_keep_zero_selects_all(void)
{
    uint16_t seqs[] = {30, 10, 20};
    uint16_t out[8] = {0};
    size_t n = pk_rec_store_select_prune(seqs, 3, 0, out);
    CHECK(n == 3);
    CHECK(out[0] == 10);
    CHECK(out[1] == 20);
    CHECK(out[2] == 30);
}

static void test_prune_negative_keep_treated_as_zero(void)
{
    uint16_t seqs[] = {2, 1};
    uint16_t out[8] = {0};
    size_t n = pk_rec_store_select_prune(seqs, 2, -5, out);
    CHECK(n == 2);
}

static void test_prune_wraparound_boundary_values(void)
{
    /* 回绕后新分配的序号可能比"更旧"的 session 数值更小（例如新的是
     * 0007、还在保留期内的老 session 是 9990）——select_prune 只看数值
     * 大小,这正是 pk_rec_store.h 里点名的"简化假设",这条用例把它钉死,
     * 免得日后有人"优化"成看不出问题的错误版本。 */
    uint16_t seqs[] = {9990, 9991, 7};
    uint16_t out[8] = {0};
    size_t n = pk_rec_store_select_prune(seqs, 3, 2, out);
    CHECK(n == 1);
    CHECK(out[0] == 7);   /* 数值最小的被选中删除,即使它其实是"最新"的 */
}

/* ================================================================ 降级档位 */

static void test_degrade_tier_boundaries(void)
{
    const uint64_t MB = 1024ull * 1024ull;

    CHECK(pk_rec_store_degrade_tier(501ull * MB) == PK_REC_DEGRADE_FULL);
    CHECK(pk_rec_store_degrade_tier(500ull * MB) == PK_REC_DEGRADE_NO_RAW);  /* 边界含在降级档 */
    CHECK(pk_rec_store_degrade_tier(101ull * MB) == PK_REC_DEGRADE_NO_RAW);
    CHECK(pk_rec_store_degrade_tier(100ull * MB) == PK_REC_DEGRADE_OWN_ONLY); /* 边界含在最严档 */
    CHECK(pk_rec_store_degrade_tier(1ull * MB) == PK_REC_DEGRADE_OWN_ONLY);
    CHECK(pk_rec_store_degrade_tier(0) == PK_REC_DEGRADE_OWN_ONLY);
}

/* ================================================================ 连续失败阈值 */

static void test_sink_should_disable_threshold(void)
{
    CHECK(pk_rec_store_sink_should_disable(0) == false);
    CHECK(pk_rec_store_sink_should_disable(PK_REC_STORE_FAIL_THRESHOLD - 1) == false);
    CHECK(pk_rec_store_sink_should_disable(PK_REC_STORE_FAIL_THRESHOLD) == true);
    CHECK(pk_rec_store_sink_should_disable(PK_REC_STORE_FAIL_THRESHOLD + 100) == true);
}

/* ================================================================ bounds（阶段 5b） */

static void test_bounds_reset_has_no_data(void)
{
    pk_rec_bounds_t b;
    pk_rec_bounds_reset(&b);
    CHECK(b.has_any == false);
}

static void test_bounds_first_update_sets_min_eq_max(void)
{
    pk_rec_bounds_t b;
    pk_rec_bounds_reset(&b);
    pk_rec_bounds_update(&b, 377749000, 1206194000);   /* 37.7749, 120.6194 */
    CHECK(b.has_any == true);
    CHECK(b.min_lat_e7 == 377749000);
    CHECK(b.max_lat_e7 == 377749000);
    CHECK(b.min_lon_e7 == 1206194000);
    CHECK(b.max_lon_e7 == 1206194000);
}

static void test_bounds_expands_with_each_point(void)
{
    pk_rec_bounds_t b;
    pk_rec_bounds_reset(&b);
    pk_rec_bounds_update(&b, 100, 200);
    pk_rec_bounds_update(&b, 50, 300);     /* 更小的 lat，更大的 lon */
    pk_rec_bounds_update(&b, 150, 100);    /* 更大的 lat，更小的 lon */
    CHECK(b.min_lat_e7 == 50);
    CHECK(b.max_lat_e7 == 150);
    CHECK(b.min_lon_e7 == 100);
    CHECK(b.max_lon_e7 == 300);
}

static void test_bounds_single_point_degenerate_box(void)
{
    /* 只见过一个目标一次：外包框退化成一个点，不是"没有 bounds"。 */
    pk_rec_bounds_t b;
    pk_rec_bounds_reset(&b);
    pk_rec_bounds_update(&b, 42, 42);
    CHECK(b.has_any == true);
    CHECK(b.min_lat_e7 == b.max_lat_e7);
    CHECK(b.min_lon_e7 == b.max_lon_e7);
}

static void test_bounds_negative_coordinates(void)
{
    /* 南半球/西经：负数比较不能想当然用无符号或字符串序，钉住一条用例。 */
    pk_rec_bounds_t b;
    pk_rec_bounds_reset(&b);
    pk_rec_bounds_update(&b, -338700000, 1512100000);  /* 悉尼 */
    pk_rec_bounds_update(&b, -370000000, 1440000000);  /* 更南、更西 */
    CHECK(b.min_lat_e7 == -370000000);
    CHECK(b.max_lat_e7 == -338700000);
    CHECK(b.min_lon_e7 == 1440000000);
    CHECK(b.max_lon_e7 == 1512100000);
}

/* ================================================================ own_icao_changes（阶段 5b） */

static void test_icao_changes_reset_is_empty(void)
{
    pk_rec_own_icao_changes_t c;
    pk_rec_own_icao_changes_reset(&c);
    CHECK(c.count == 0);
}

static void test_icao_changes_append_bind(void)
{
    pk_rec_own_icao_changes_t c;
    pk_rec_own_icao_changes_reset(&c);
    const uint8_t icao[3] = {0xA1, 0xB2, 0xC3};
    CHECK(pk_rec_own_icao_changes_append(&c, 1000, true, icao) == true);
    CHECK(c.count == 1);
    CHECK(c.entries[0].ts_ms == 1000);
    CHECK(c.entries[0].bound == true);
    CHECK(memcmp(c.entries[0].icao24, icao, 3) == 0);
}

static void test_icao_changes_append_unbind_zeroes_icao(void)
{
    pk_rec_own_icao_changes_t c;
    pk_rec_own_icao_changes_reset(&c);
    CHECK(pk_rec_own_icao_changes_append(&c, 2000, false, NULL) == true);
    CHECK(c.entries[0].bound == false);
    static const uint8_t zero[3] = {0, 0, 0};
    CHECK(memcmp(c.entries[0].icao24, zero, 3) == 0);
}

static void test_icao_changes_full_array_rejects_further_appends(void)
{
    pk_rec_own_icao_changes_t c;
    pk_rec_own_icao_changes_reset(&c);
    const uint8_t icao[3] = {1, 2, 3};
    for (unsigned i = 0; i < PK_REC_OWN_ICAO_CHANGES_MAX; i++) {
        CHECK(pk_rec_own_icao_changes_append(&c, (int64_t)i, true, icao) == true);
    }
    CHECK(c.count == PK_REC_OWN_ICAO_CHANGES_MAX);
    /* 满了之后：丢新不丢旧——追加失败，已有条目原样保留。 */
    CHECK(pk_rec_own_icao_changes_append(&c, 99999, true, icao) == false);
    CHECK(c.count == PK_REC_OWN_ICAO_CHANGES_MAX);
    CHECK(c.entries[0].ts_ms == 0);   /* 最早那条没被顶掉 */
}

/* ================================================================ main */

int main(void)
{
    test_alloc_seq_empty();
    test_alloc_seq_normal_increment();
    test_alloc_seq_ignores_gaps();
    test_alloc_seq_wraparound_reuses_smallest_free();
    test_alloc_seq_wraparound_picks_smallest_of_several();
    test_alloc_seq_full_degrades_to_zero();

    test_prune_below_keep_selects_nothing();
    test_prune_exactly_keep_selects_nothing();
    test_prune_selects_smallest_first();
    test_prune_keep_zero_selects_all();
    test_prune_negative_keep_treated_as_zero();
    test_prune_wraparound_boundary_values();

    test_degrade_tier_boundaries();

    test_sink_should_disable_threshold();

    test_bounds_reset_has_no_data();
    test_bounds_first_update_sets_min_eq_max();
    test_bounds_expands_with_each_point();
    test_bounds_single_point_degenerate_box();
    test_bounds_negative_coordinates();

    test_icao_changes_reset_is_empty();
    test_icao_changes_append_bind();
    test_icao_changes_append_unbind_zeroes_icao();
    test_icao_changes_full_array_rejects_further_appends();

    if (g_fail == 0) {
        printf("OK (all pk_rec_store host tests passed)\n");
        return 0;
    }
    fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
}
