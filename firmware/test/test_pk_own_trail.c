/* test_pk_own_trail.c — host proof for 本机航迹 ring 逻辑（push / count / 回绕）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -o /tmp/test_trail \
 *      firmware/test/test_pk_own_trail.c && /tmp/test_trail
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -o /tmp/test_trail_asan firmware/test/test_pk_own_trail.c && \
 *      /tmp/test_trail_asan
 *
 * 可测性说明：pk_own_sampler.c 依赖 IDF（FreeRTOS、esp_timer），无法 host 直接
 * 编译。但 ring 的 push/count 逻辑是纯数据结构操作，没有外部依赖。这里把
 * trail_push + ring 结构从 pk_own_sampler.c 原样复制到独立 TU 测，验证 ring 的
 * 追加、count 递增、head 回绕取模的正确性。飞行段清空决策作为集成行为靠真机
 * 验证（不在这里测），这与项目惯例一致：测能 host 编译的纯逻辑，集成靠真机。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── 从 pk_own_sampler.c 原样复制的 ring 逻辑（不带 EXT_RAM_BSS_ATTR，
 *    host 编译不需要 PSRAM 属性）────────────────────────────────────── */

#define PK_OWN_TRAIL_CAP 8192u

typedef struct {
    uint32_t ts_1k;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint8_t  phase;
} pk_own_trail_point_t;

static struct {
    pk_own_trail_point_t pt[PK_OWN_TRAIL_CAP];
    uint32_t head;
    uint32_t count;
} s_trail;

static void trail_push(uint32_t ts_ms, int32_t lat_e7, int32_t lon_e7, uint8_t phase)
{
    s_trail.pt[s_trail.head & (PK_OWN_TRAIL_CAP - 1)] = (pk_own_trail_point_t){
        .ts_1k = ts_ms, .lat_e7 = lat_e7, .lon_e7 = lon_e7, .phase = phase,
    };
    s_trail.head++;
    if (s_trail.count < PK_OWN_TRAIL_CAP) s_trail.count++;
}

static void trail_reset(void)
{
    s_trail.head = 0;
    s_trail.count = 0;
}

/* ── 测试框架（照抄 test_pk_flight_phase.c 的 CHECK 宏范式）─────────── */

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ================================================================ 测试用例 */

/* 基本 push + count 递增：push 3 个点，count 应为 3，内容正确。 */
static void test_basic_push(void)
{
    trail_reset();

    trail_push(1000, 225000000, 1140000000, 2);
    trail_push(2000, 225000001, 1140000001, 2);
    trail_push(3000, 225000002, 1140000002, 3);

    CHECK(s_trail.count == 3);
    CHECK(s_trail.pt[0].ts_1k == 1000);
    CHECK(s_trail.pt[0].lat_e7 == 225000000);
    CHECK(s_trail.pt[0].lon_e7 == 1140000000);
    CHECK(s_trail.pt[0].phase == 2);
    CHECK(s_trail.pt[2].ts_1k == 3000);
    CHECK(s_trail.pt[2].phase == 3);
}

/* CAP=8192 必须是 2 的幂，否则 head & (CAP-1) 取模会错——编译期验证。 */
static void test_cap_is_power_of_two(void)
{
    CHECK((PK_OWN_TRAIL_CAP & (PK_OWN_TRAIL_CAP - 1)) == 0);
    CHECK(PK_OWN_TRAIL_CAP == 8192);
}

/* 小 CAP 回绕：用 4 做局部 CAP 模拟回绕行为（验证 head & (CAP-1) 取模逻辑）。
 * 写满后再写，count 不超过 CAP，head 继续递增，最新数据覆盖最老数据。 */
#define SMALL_CAP 4u

static struct {
    pk_own_trail_point_t pt[SMALL_CAP];
    uint32_t head;
    uint32_t count;
} s_small;

static void small_push(uint32_t ts_ms, int32_t lat_e7, int32_t lon_e7, uint8_t phase)
{
    s_small.pt[s_small.head & (SMALL_CAP - 1)] = (pk_own_trail_point_t){
        .ts_1k = ts_ms, .lat_e7 = lat_e7, .lon_e7 = lon_e7, .phase = phase,
    };
    s_small.head++;
    if (s_small.count < SMALL_CAP) s_small.count++;
}

static void test_wraparound_small(void)
{
    memset(&s_small, 0, sizeof(s_small));

    /* 写满 4 个槽 */
    small_push(1000, 10, 20, 2);
    small_push(2000, 11, 21, 2);
    small_push(3000, 12, 22, 3);
    small_push(4000, 13, 23, 4);

    CHECK(s_small.count == 4);
    CHECK(s_small.head == 4);
    CHECK(s_small.pt[0].ts_1k == 1000);
    CHECK(s_small.pt[3].ts_1k == 4000);

    /* 第 5 个点回绕到槽 0，覆盖最早的 1000 */
    small_push(5000, 14, 24, 4);

    CHECK(s_small.count == 4);   /* 不超过 CAP */
    CHECK(s_small.head == 5);
    CHECK(s_small.pt[0].ts_1k == 5000);  /* 槽 0 被覆盖 */
    CHECK(s_small.pt[1].ts_1k == 2000);  /* 槽 1 仍是老数据 */
    CHECK(s_small.pt[2].ts_1k == 3000);
    CHECK(s_small.pt[3].ts_1k == 4000);

    /* 第 6 个点回绕到槽 1 */
    small_push(6000, 15, 25, 4);
    CHECK(s_small.count == 4);
    CHECK(s_small.head == 6);
    CHECK(s_small.pt[0].ts_1k == 5000);
    CHECK(s_small.pt[1].ts_1k == 6000);  /* 槽 1 被覆盖 */

    /* 再写满一整圈，所有槽都是新数据 */
    small_push(7000, 16, 26, 4);
    small_push(8000, 17, 27, 4);
    CHECK(s_small.count == 4);
    CHECK(s_small.head == 8);   /* head 回到 2*CAP */
    CHECK(s_small.pt[0].ts_1k == 5000);
    CHECK(s_small.pt[1].ts_1k == 6000);
    CHECK(s_small.pt[2].ts_1k == 7000);
    CHECK(s_small.pt[3].ts_1k == 8000);
}

/* 真实 CAP（8192）回绕：写 CAP+2 个点验证 count 上限 + head 正确取模。 */
static void test_wraparound_real_cap(void)
{
    trail_reset();

    /* 写满 CAP 个点 */
    for (uint32_t i = 0; i < PK_OWN_TRAIL_CAP; i++) {
        trail_push(i * 1000, (int32_t)i, (int32_t)(i * 2), 4);
    }
    CHECK(s_trail.count == PK_OWN_TRAIL_CAP);
    CHECK(s_trail.head == PK_OWN_TRAIL_CAP);

    /* 多写 2 个点，count 不超 CAP */
    trail_push(PK_OWN_TRAIL_CAP * 1000, 999, 998, 4);
    trail_push((PK_OWN_TRAIL_CAP + 1) * 1000, 997, 996, 4);
    CHECK(s_trail.count == PK_OWN_TRAIL_CAP);
    CHECK(s_trail.head == PK_OWN_TRAIL_CAP + 2);

    /* 槽 0 被覆盖为第 CAP+1 个点（ts = CAP*1000） */
    CHECK(s_trail.pt[0].ts_1k == PK_OWN_TRAIL_CAP * 1000);
    CHECK(s_trail.pt[0].lat_e7 == 999);
    /* 槽 1 被覆盖为第 CAP+2 个点（ts = (CAP+1)*1000） */
    CHECK(s_trail.pt[1].ts_1k == (PK_OWN_TRAIL_CAP + 1) * 1000);
    CHECK(s_trail.pt[1].lat_e7 == 997);
    /* 槽 2 仍是原始第 3 个点（ts = 2*1000） */
    CHECK(s_trail.pt[2].ts_1k == 2000);
    CHECK(s_trail.pt[2].lat_e7 == 2);
}

/* 清空语义（模拟飞行段清空）：head/count 归零，后续写入从头开始。 */
static void test_clear(void)
{
    trail_reset();
    trail_push(1000, 10, 20, 2);
    trail_push(2000, 11, 21, 3);
    CHECK(s_trail.count == 2);

    /* 模拟 own_sample_task 里 s_trail.head = 0; s_trail.count = 0; */
    trail_reset();
    CHECK(s_trail.count == 0);
    CHECK(s_trail.head == 0);

    /* 清空后新写入正常 */
    trail_push(3000, 30, 40, 2);
    CHECK(s_trail.count == 1);
    CHECK(s_trail.pt[0].ts_1k == 3000);
    CHECK(s_trail.pt[0].lat_e7 == 30);
}

/* 128KB 内存占用验证（编译期 sizeof 断言，确保不会意外增大）。 */
static void test_struct_size(void)
{
    /* pk_own_trail_point_t = 4+4+4+1 = 13，但 phase 后有 3 字节 padding → 16B。
     * 8192 × 16 = 131072 = 128 KB。如果这个断言挂了，说明结构被改大了。 */
    CHECK(sizeof(pk_own_trail_point_t) == 16);
}

/* ============================================================== main */

int main(void)
{
    test_cap_is_power_of_two();
    test_basic_push();
    test_wraparound_small();
    test_wraparound_real_cap();
    test_clear();
    test_struct_size();

    if (g_fail == 0) {
        printf("PASS: all pk_own_trail tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d assertion(s) failed\n", g_fail);
    return 1;
}
