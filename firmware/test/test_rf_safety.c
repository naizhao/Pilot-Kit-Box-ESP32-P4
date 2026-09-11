/*
 * test_rf_safety.c — F2 上电安全向量的 host 单测。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -DRF_SAFETY_HOST_TEST \
 *      -I firmware/rp2040 -o /tmp/test_rf_safety \
 *      firmware/test/test_rf_safety.c firmware/rp2040/rf_safety.c \
 *   && /tmp/test_rf_safety
 */
#include "board_pins.h"
#include "rf_safety.h"
#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* ── 运行期天线选择的真值表 ──
 * 两条硬约束：①两根选择线必须互补（同高 = 两路天线并联，同低 = 都断开）；
 * ②枚举值 0 必须落在开机安全向量的同一个电平上，否则"配置没送到"和"配置
 * 是默认值"会变成两个不同的 RF 状态。 */
static void check_ant_tables(void)
{
    bool a, b;

    rf_safety_ant_1090_levels(RF_ANT_1090_ONBOARD, &a, &b);
    CHECK(a == true && b == false, "1090 板载 IFA 应为 A=1/B=0，得到 A=%d/B=%d\n", a, b);
    rf_safety_ant_1090_levels(RF_ANT_1090_EXTERNAL, &a, &b);
    CHECK(a == false && b == true, "1090 外接 J6 应为 A=0/B=1，得到 A=%d/B=%d\n", a, b);

    rf_safety_ant_gnss_levels(RF_ANT_GNSS_EXTERNAL, &a, &b);
    CHECK(a == false && b == true, "GNSS 外接 J2 应为 A=0/B=1，得到 A=%d/B=%d\n", a, b);
    rf_safety_ant_gnss_levels(RF_ANT_GNSS_ONBOARD, &a, &b);
    CHECK(a == true && b == false, "GNSS 板载 patch 应为 A=1/B=0，得到 A=%d/B=%d\n", a, b);

    /* 互补性：四种选择都不能出现同高/同低 */
    for (int s = 0; s <= 1; s++) {
        rf_safety_ant_1090_levels((rf_ant_1090_t)s, &a, &b);
        CHECK(a != b, "1090 sel=%d 两根选择线不互补\n", s);
        rf_safety_ant_gnss_levels((rf_ant_gnss_t)s, &a, &b);
        CHECK(a != b, "GNSS sel=%d 两根选择线不互补\n", s);
    }
}

/* 枚举 0 必须与开机向量给出的电平一致——这条不成立的话，P4 还没把配置送
 * 下来的那段时间里，天线通路和 UI 上显示的默认值会对不上。 */
static void check_zero_matches_boot_vector(void)
{
    int n = 0;
    const rf_safety_pin_t *v = rf_safety_boot_vector(&n);
    bool a, b;

    rf_safety_ant_1090_levels(RF_ANT_1090_ONBOARD, &a, &b);
    for (int i = 0; i < n; i++) {
        if (v[i].gpio == PIN_ANT_SEL_1090_A)
            CHECK(v[i].level == a, "1090 枚举 0 与开机向量 A 不一致\n");
        if (v[i].gpio == PIN_ANT_SEL_1090_B)
            CHECK(v[i].level == b, "1090 枚举 0 与开机向量 B 不一致\n");
    }
    rf_safety_ant_gnss_levels(RF_ANT_GNSS_EXTERNAL, &a, &b);
    for (int i = 0; i < n; i++) {
        if (v[i].gpio == PIN_GNSS_SEL_A)
            CHECK(v[i].level == a, "GNSS 枚举 0 与开机向量 A 不一致\n");
        if (v[i].gpio == PIN_GNSS_SEL_B)
            CHECK(v[i].level == b, "GNSS 枚举 0 与开机向量 B 不一致\n");
    }
}

int main(void)
{
    check_ant_tables();
    check_zero_matches_boot_vector();

    int n = 0;
    const rf_safety_pin_t *v = rf_safety_boot_vector(&n);
    CHECK(n == 6, "count=%d\n", n);

    int bias_off = 0, a = -1, b = -1, r1090_a = -1, r1090_b = -1;
    for (int i = 0; i < n; i++) {
        if (v[i].gpio == PIN_BIAS_EN_1090 || v[i].gpio == PIN_BIAS_EN_978) {
            CHECK(v[i].level == 1, "bias gpio %d level=%d\n",
                  v[i].gpio, v[i].level);
            bias_off++;
        }
        if (v[i].gpio == PIN_GNSS_SEL_A) a = v[i].level;
        if (v[i].gpio == PIN_GNSS_SEL_B) b = v[i].level;
        if (v[i].gpio == PIN_ANT_SEL_1090_A) r1090_a = v[i].level;
        if (v[i].gpio == PIN_ANT_SEL_1090_B) r1090_b = v[i].level;
    }
    CHECK(bias_off == 2, "bias pins=%d\n", bias_off);

    CHECK(a != -1 && b != -1, "gnss select pins missing a=%d b=%d\n", a, b);
    CHECK((a ^ b) == 1, "gnss select must be complementary: a=%d b=%d\n",
          a, b);
    CHECK(a == 0 && b == 1, "gnss default = external: a=%d b=%d\n", a, b);

    CHECK(r1090_a != -1 && r1090_b != -1,
          "1090 select pins missing a=%d b=%d\n", r1090_a, r1090_b);
    CHECK((r1090_a ^ r1090_b) == 1,
          "1090 select must be complementary: a=%d b=%d\n", r1090_a, r1090_b);
    CHECK(r1090_a == 1 && r1090_b == 0,
          "1090 default = onboard IFA: a=%d b=%d\n", r1090_a, r1090_b);

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
