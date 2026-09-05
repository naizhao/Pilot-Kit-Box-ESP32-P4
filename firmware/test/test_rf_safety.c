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

int main(void)
{
    int n = 0;
    const rf_safety_pin_t *v = rf_safety_boot_vector(&n);
    CHECK(n == 4, "count=%d\n", n);

    int bias_off = 0, a = -1, b = -1;
    for (int i = 0; i < n; i++) {
        if (v[i].gpio == PIN_BIAS_EN_1090 || v[i].gpio == PIN_BIAS_EN_978) {
            CHECK(v[i].level == 1, "bias gpio %d level=%d\n",
                  v[i].gpio, v[i].level);
            bias_off++;
        }
        if (v[i].gpio == PIN_GNSS_SEL_A) a = v[i].level;
        if (v[i].gpio == PIN_GNSS_SEL_B) b = v[i].level;
    }
    CHECK(bias_off == 2, "bias pins=%d\n", bias_off);
    CHECK(a != -1 && b != -1, "select pins missing a=%d b=%d\n", a, b);
    CHECK((a ^ b) == 1, "select must be complementary: a=%d b=%d\n", a, b);

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
