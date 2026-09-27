/*
 * test_edge_glitch_filter.c — 1090 双沿流的窄高脉冲抑制。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_edge_glitch_filter firmware/test/test_edge_glitch_filter.c \
 *   && /tmp/test_edge_glitch_filter
 */
#include "edge_glitch_filter.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("  [FAIL] " __VA_ARGS__); g_fail++; } } while (0)

static void check_vec(const uint32_t *got, size_t gn,
                      const uint32_t *want, size_t wn, const char *name)
{
    CHECK(gn == wn, "%s length=%zu want=%zu\n", name, gn, wn);
    size_t n = gn < wn ? gn : wn;
    for (size_t i = 0; i < n; i++)
        CHECK(got[i] == want[i], "%s[%zu]=%u want=%u\n",
              name, i, got[i], want[i]);
}

int main(void)
{
    edge_glitch_filter_t f;
    uint32_t out[16];

    /* 1. 48ns(3 tick) 高毛刺被删，时间合入前后低电平。 */
    edge_glitch_filter_init(&f);
    const uint32_t noisy[] = {100, 3, 200, 4, 300, 31};
    const uint32_t clean[] = {607, 31};
    size_t n = edge_glitch_filter_feed(&f, noisy, 6, out, 16);
    check_vec(out, n, clean, 2, "collapse");
    CHECK(f.rejected == 2, "rejected=%u want=2\n", f.rejected);

    /* 2. 合法 0.5us 脉冲（31/32 tick）逐字保留。 */
    edge_glitch_filter_init(&f);
    const uint32_t valid[] = {63, 31, 125, 32};
    n = edge_glitch_filter_feed(&f, valid, 4, out, 16);
    check_vec(out, n, valid, 4, "valid");
    CHECK(f.rejected == 0, "valid rejected=%u\n", f.rejected);

    /* 3. 12 tick 是保留下界；11 tick 仍属于毛刺。 */
    edge_glitch_filter_init(&f);
    const uint32_t boundary[] = {10, 11, 20, 12};
    const uint32_t boundary_want[] = {41, 12};
    n = edge_glitch_filter_feed(&f, boundary, 4, out, 16);
    check_vec(out, n, boundary_want, 2, "boundary");

    /* 4. 分块不能改变结果：块尾 low 留到下一块 high 再裁决。 */
    edge_glitch_filter_init(&f);
    const uint32_t a[] = {100, 3, 200};
    const uint32_t b[] = {31};
    n = edge_glitch_filter_feed(&f, a, 3, out, 16);
    CHECK(n == 0, "chunk-a emitted %zu\n", n);
    n = edge_glitch_filter_feed(&f, b, 1, out, 16);
    const uint32_t chunk_want[] = {303, 31};
    check_vec(out, n, chunk_want, 2, "chunk-b");

    /* 5. 断点 reset 必须丢弃未配对 low，不能跨断点拼时间。 */
    edge_glitch_filter_init(&f);
    const uint32_t orphan[] = {999};
    n = edge_glitch_filter_feed(&f, orphan, 1, out, 16);
    CHECK(n == 0, "orphan emitted %zu\n", n);
    edge_glitch_filter_resync(&f);
    const uint32_t after[] = {50, 31};
    n = edge_glitch_filter_feed(&f, after, 2, out, 16);
    check_vec(out, n, after, 2, "after-reset");

    /* 6. 丢奇数沿后相位未知；>5us 空闲只能是 low，必须据此自动重锚。 */
    edge_glitch_filter_init(&f);
    const uint32_t desync[] = {31, 500, 3, 100, 31};
    const uint32_t desync_want[] = {603, 31};
    n = edge_glitch_filter_feed(&f, desync, 5, out, 16);
    check_vec(out, n, desync_want, 2, "long-gap-resync");

    printf(g_fail ? "test_edge_glitch_filter: %d FAILED\n"
                  : "test_edge_glitch_filter: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
