/*
 * test_vbus_sense.c — USB_VBUS_SENSE 分压中点 → VBUS 换算与档位判别。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -DPK_HOST_TEST \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_vbus_sense firmware/test/test_vbus_sense.c \
 *      firmware/main/vbus_sense.c \
 *   && /tmp/test_vbus_sense
 *
 * 判据来源：PLAN.md F6 —— V4=30k/10k(4.0)、V3=10k/10k(2.0)；按 ADC Zin ≥100k
 * 算出的 V4 实际中点：5V→1163 mV、9V→2093 mV、10V→2326 mV（理想值低 7%）。
 *
 * 这里最要紧的两条不是"算得对不对"，而是：
 *   1. **板型不能弄反** —— 同一个中点电压在 V3/V4 上差整整 2 倍，弄反了读数
 *      看着完全正常（5V 会显示成 10V 或 2.5V），没有任何症状能暴露它。
 *   2. **判档不能走换算后的电压** —— 换算带 7% 系统偏差，拿它比 7V 门限
 *      等于把偏差带进判据。
 */
#include "vbus_sense.h"

#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* pk_board_profile() 由测试注入（host_stubs 只声明不定义的同款约定）。 */
static pk_board_profile_t g_profile = PK_BOARD_PROFILE_V4;
pk_board_profile_t pk_board_profile(void) { return g_profile; }

/* ── 1. 倍率：两块板差整整 2 倍 ── */
static void test_ratio_per_profile(void)
{
    /* V4 = 4.0 */
    CHECK(pk_vbus_mv_uncal(PK_BOARD_PROFILE_V4, 1163) == 4652,
          "V4 1163mV → %d，期望 4652\n", pk_vbus_mv_uncal(PK_BOARD_PROFILE_V4, 1163));
    CHECK(pk_vbus_mv_uncal(PK_BOARD_PROFILE_V4, 1250) == 5000,
          "V4 理想中点 1250mV 应换算成 5000mV\n");
    /* V3 = 2.0 */
    CHECK(pk_vbus_mv_uncal(PK_BOARD_PROFILE_V3, 1250) == 2500,
          "V3 1250mV → %d，期望 2500\n", pk_vbus_mv_uncal(PK_BOARD_PROFILE_V3, 1250));
    /* 同一个中点电压在两块板上必须差一倍——这条直接钉住"板型弄反"这种
     * 没有任何运行期症状的错误。 */
    CHECK(pk_vbus_mv_uncal(PK_BOARD_PROFILE_V4, 1000) ==
          2 * pk_vbus_mv_uncal(PK_BOARD_PROFILE_V3, 1000),
          "V4 的换算结果必须正好是 V3 的两倍\n");
}

/* ── 2. 未标定偏差如实存在：用理想倍率换算 5V 实测中点，结果必须偏低 ──
 * 这条不是在测"算错了"，而是钉住"我们没有偷偷塞校准系数把它凑成 5000"。
 * PLAN.md F6：不得把理想倍率冒充校准系数。 */
static void test_uncalibrated_bias_is_honest(void)
{
    int v = pk_vbus_mv_uncal(PK_BOARD_PROFILE_V4, 1163);   /* 实测 5V 的中点 */
    CHECK(v < 5000, "5V 的实测中点换算后应偏低（未标定），得到 %d\n", v);
    CHECK(v > 4500, "偏差不该大到离谱，得到 %d\n", v);
}

/* ── 3. 档位判别走中点电压，且门限余量足够 ── */
static void test_class_v4(void)
{
    const pk_board_profile_t P = PK_BOARD_PROFILE_V4;
    CHECK(pk_vbus_class(P, 0) == PK_VBUS_ABSENT, "0mV 应判 absent\n");
    CHECK(pk_vbus_class(P, 1163) == PK_VBUS_5V, "5V 实测中点应判 5V\n");
    CHECK(pk_vbus_class(P, 2093) == PK_VBUS_9V, "9V 实测中点应判 9V\n");
    CHECK(pk_vbus_class(P, 2326) == PK_VBUS_9V, "10V 实测中点应判 9V 档\n");

    /* 余量：两档实测值离门限都要有几百 mV，7% 的系统偏差吃不掉 */
    CHECK(pk_vbus_class(P, 1163 + 400) == PK_VBUS_5V, "5V 档上浮 400mV 仍应判 5V\n");
    CHECK(pk_vbus_class(P, 2093 - 400) == PK_VBUS_9V, "9V 档下浮 400mV 仍应判 9V\n");
}

/* ── 4. V3 判不了 9V，必须如实说，不能硬凑 ──
 * V3 倍率 2.0，9V 会把中点顶到 4186 mV，远超 3.3V IOVDD，ADC 早就饱和了。
 * 这种情况下报一个"9V"是凭空捏造。 */
static void test_class_v3_cannot_resolve_9v(void)
{
    const pk_board_profile_t P = PK_BOARD_PROFILE_V3;
    CHECK(pk_vbus_class(P, 0) == PK_VBUS_ABSENT, "V3 0mV 应判 absent\n");
    CHECK(pk_vbus_class(P, 2326) == PK_VBUS_5V, "V3 5V 中点应判 5V\n");
    /* 饱和区：不能冒充 9V */
    CHECK(pk_vbus_class(P, 3200) == PK_VBUS_UNKNOWN,
          "V3 接近满量程时应判 unknown 而不是硬报 9V\n");
}

/* ── 5. 无效输入不能被当成合法读数 ── */
static void test_invalid_inputs(void)
{
    CHECK(pk_vbus_mv_uncal(PK_BOARD_PROFILE_V4, -1) == -1, "负输入应返回 -1\n");
    CHECK(pk_vbus_class(PK_BOARD_PROFILE_V4, -1) == PK_VBUS_UNKNOWN,
          "负输入应判 unknown\n");
    CHECK(pk_vbus_mv_uncal((pk_board_profile_t)99, 1000) == -1,
          "非法板型应返回 -1\n");
    CHECK(pk_vbus_class((pk_board_profile_t)99, 1000) == PK_VBUS_UNKNOWN,
          "非法板型应判 unknown\n");
}

/* ── 6. 没收到过读数时必须说"没有"，不能报 0 ──
 * 报 0 会被上层当成"VBUS = 0V，没插电"，而真相是"RP2040 还没连上/对端是
 * 只发 40 字节的旧固件"。这两件事的处置完全不同。 */
static void test_no_reading_yet(void)
{
    int n = 123, v = 456;
    pk_vbus_class_t c = PK_VBUS_5V;
    CHECK(!pk_vbus_sense_get(&n, &v, &c), "还没喂过数据就返回了 true\n");
    CHECK(n == 123 && v == 456 && c == PK_VBUS_5V,
          "返回 false 时不该改写输出\n");

    g_profile = PK_BOARD_PROFILE_V4;
    pk_vbus_sense_update(1163);
    CHECK(pk_vbus_sense_get(&n, &v, &c), "喂过数据后应返回 true\n");
    CHECK(n == 1163, "node_mv=%d\n", n);
    CHECK(c == PK_VBUS_5V, "class=%s\n", pk_vbus_class_name(c));

    /* 同一个读数，换板型必须给出不同结果——证明 get 真的用了当前 profile，
     * 而不是把换算结果缓存下来了。 */
    g_profile = PK_BOARD_PROFILE_V3;
    int v3 = 0;
    pk_vbus_sense_get(NULL, &v3, NULL);
    CHECK(v3 == v / 2, "换到 V3 后换算结果应减半：%d vs %d\n", v3, v);
    g_profile = PK_BOARD_PROFILE_V4;
}

int main(void)
{
    test_ratio_per_profile();
    test_uncalibrated_bias_is_honest();
    test_class_v4();
    test_class_v3_cannot_resolve_9v();
    test_invalid_inputs();
    test_no_reading_yet();

    if (g_fail) { printf("test_vbus_sense: %d FAIL\n", g_fail); return 1; }
    printf("test_vbus_sense: all OK\n");
    return 0;
}
