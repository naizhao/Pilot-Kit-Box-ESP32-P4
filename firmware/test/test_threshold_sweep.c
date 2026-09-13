/*
 * test_threshold_sweep.c — 门限扫描选点判据的 host 单测。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/rp2040 \
 *      -o /tmp/test_threshold_sweep firmware/test/test_threshold_sweep.c \
 *   && /tmp/test_threshold_sweep
 *
 * 盯两条容易想当然的分支：帧数全 0 时必须退回 pre 选点（低流量下这是唯一
 * 有信息量的指标），以及两者都全 0 时必须报"没找到"而不是默认第 0 个。
 * 用例数据取自 2026-09-13 外场实测的两张真实扫描表。
 */
#include "threshold_sweep.h"

#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

int main(void)
{
    /* 1. 常规：有帧就按帧选。取自实测外置表（400..1000）：
     *    帧 0/2/5/28/0/0/0，pre 0/9/84/347/227/80/42 → 应选 idx3（700）。 */
    {
        const uint32_t f[] = {0, 2, 5, 28, 0, 0, 0};
        const uint32_t p[] = {0, 9, 84, 347, 227, 80, 42};
        CHECK(threshold_sweep_pick(f, p, 7) == 3,
              "应按帧数选 700，得到 idx=%d\n", threshold_sweep_pick(f, p, 7));
    }

    /* 2. 关键分支：帧数全 0 时退回 pre。取自实测 IFA 表——这一张如果按
     *    帧数选就只能随机取第一个，而 pre 明确指向 idx4（800）。 */
    {
        const uint32_t f[] = {0, 0, 0, 0, 0, 0};
        const uint32_t p[] = {0, 1, 32, 119, 112, 2};
        CHECK(threshold_sweep_pick(f, p, 6) == 3,
              "帧全 0 时应按 pre 选 idx3，得到 %d\n",
              threshold_sweep_pick(f, p, 6));
    }

    /* 3. 帧数平手时用 pre 打破，而不是取先出现的那个。 */
    {
        const uint32_t f[] = {7, 7, 7};
        const uint32_t p[] = {10, 90, 50};
        CHECK(threshold_sweep_pick(f, p, 3) == 1,
              "平手应按 pre 选 idx1，得到 %d\n",
              threshold_sweep_pick(f, p, 3));
    }

    /* 4. 一点信号都没有 → -1。绝不能退化成 0，否则空表会被报成
     *    "建议用第一个门限"。 */
    {
        const uint32_t f[] = {0, 0, 0};
        const uint32_t p[] = {0, 0, 0};
        CHECK(threshold_sweep_pick(f, p, 3) == -1,
              "全 0 应返回 -1，得到 %d\n", threshold_sweep_pick(f, p, 3));
    }

    /* 5. 边界：空表 / 空指针不得越界。 */
    {
        const uint32_t f[] = {1};
        const uint32_t p[] = {1};
        CHECK(threshold_sweep_pick(f, p, 0) == -1, "n=0 应返回 -1\n");
        CHECK(threshold_sweep_pick(NULL, p, 1) == -1, "NULL frames 应返回 -1\n");
        CHECK(threshold_sweep_pick(f, NULL, 1) == -1, "NULL pre 应返回 -1\n");
    }

    /* 6. 只有一个点有帧、且它的 pre 反而更低——仍按帧优先。
     *    （pre 高但解不出，说明有形状没质量，不该盖过真解出来的那个点。） */
    {
        const uint32_t f[] = {0, 3};
        const uint32_t p[] = {500, 20};
        CHECK(threshold_sweep_pick(f, p, 2) == 1,
              "帧优先于 pre，应选 idx1，得到 %d\n",
              threshold_sweep_pick(f, p, 2));
    }

    printf(g_fail ? "test_threshold_sweep: %d FAILED\n"
                  : "test_threshold_sweep: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
