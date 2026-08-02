/* test_pk_vib.c — host proof for pk_vib（振动强度：加速度模长滑动窗口 RMS）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_vib \
 *      firmware/test/test_pk_vib.c -lm && /tmp/test_vib
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_vib_asan \
 *      firmware/test/test_pk_vib.c -lm && /tmp/test_vib_asan
 *
 *   leaks（macOS）：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -I firmware/main \
 *      -o /tmp/test_vib_leaks firmware/test/test_pk_vib.c -lm && \
 *      leaks --atExit -- /tmp/test_vib_leaks
 *
 * 同 test_pk_flight_phase.c 惯例：把被测 .c 直接 #include 进同一 TU。
 *
 * 核心断言是 pk_vib.h 反复强调的那条契约：0 = "不可用"，绝不能和"真实
 * 静止"（应返回 1 或 2 这种小值）混淆——test_zero_input_fills_window_is_not_zero
 * 就是专门测这条的。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/pk_vib.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ============================================================ reset */

static void test_reset_then_unfilled_window_is_unavailable(void)
{
    pk_vib_state_t st;
    pk_vib_reset(&st);
    CHECK(pk_vib_level(&st) == 0);

    /* Push fewer than PK_VIB_WINDOW_LEN samples — still unavailable. */
    for (uint16_t i = 0; i < PK_VIB_WINDOW_LEN - 1; i++) {
        pk_vib_push(&st, 1.0f, 0.0f, 0.0f);
    }
    CHECK(pk_vib_level(&st) == 0);

    /* One more push fills the window. */
    pk_vib_push(&st, 1.0f, 0.0f, 0.0f);
    CHECK(pk_vib_level(&st) != 0);
}

/* ================================================== 静止 ≠ 不可用 */

static void test_zero_input_fills_window_is_not_zero(void)
{
    pk_vib_state_t st;
    pk_vib_reset(&st);
    for (uint16_t i = 0; i < PK_VIB_WINDOW_LEN; i++) {
        pk_vib_push(&st, 0.0f, 0.0f, 0.0f);
    }
    uint8_t lvl = pk_vib_level(&st);
    /* Window full + zero magnitude every sample → RMS is exactly 0,
     * which pk_vib_level() must floor at 1 rather than reporting the
     * "unavailable" sentinel. */
    CHECK(lvl != 0);
    CHECK(lvl == 1);
}

/* ============================================================ 正弦激励 */

static void test_sine_excitation_matches_analytic_rms(void)
{
    pk_vib_state_t st;
    pk_vib_reset(&st);

    /* ax(t) = A sin(2*pi*f*t), one full cycle spanning the window.
     * RMS of a sine wave of amplitude A is A/sqrt(2). Single-axis input
     * so vector magnitude RMS == |ax| RMS. */
    const float A = 1.0f;
    for (uint16_t i = 0; i < PK_VIB_WINDOW_LEN; i++) {
        float phase = 2.0f * (float)M_PI * (float)i / (float)PK_VIB_WINDOW_LEN;
        float ax = A * sinf(phase);
        pk_vib_push(&st, ax, 0.0f, 0.0f);
    }

    float rms_analytic = A / sqrtf(2.0f);
    float scaled = rms_analytic * (255.0f / PK_VIB_RMS_FULLSCALE_MPS2);
    int32_t want = (int32_t)(scaled + 0.5f);
    if (want < 1) want = 1;
    if (want > 255) want = 255;

    uint8_t got = pk_vib_level(&st);
    /* Quantization to an 8-bit level: allow +/-1 LSB for rounding at
     * the sample-count boundary (discrete window isn't a perfect
     * continuous-time RMS integral). */
    CHECK(abs((int)got - (int)want) <= 1);
}

/* ============================================================ 窗口滑动 */

static void test_window_slides_old_samples_evicted(void)
{
    pk_vib_state_t st;
    pk_vib_reset(&st);

    /* Fill with a large constant magnitude, saturating the level. */
    for (uint16_t i = 0; i < PK_VIB_WINDOW_LEN; i++) {
        pk_vib_push(&st, 10.0f, 0.0f, 0.0f);
    }
    uint8_t lvl_loud = pk_vib_level(&st);
    CHECK(lvl_loud == 255);

    /* Push a full window's worth of near-zero samples — every loud
     * sample must have been evicted, dropping the level back down. */
    for (uint16_t i = 0; i < PK_VIB_WINDOW_LEN; i++) {
        pk_vib_push(&st, 0.0f, 0.0f, 0.0f);
    }
    uint8_t lvl_quiet = pk_vib_level(&st);
    CHECK(lvl_quiet == 1);
    CHECK(lvl_quiet < lvl_loud);
}

static void test_window_slides_partial_eviction_tracks_value(void)
{
    pk_vib_state_t st;
    pk_vib_reset(&st);

    /* Fill entirely with a mid-scale constant magnitude. */
    const float mag = 1.0f; /* sum_sq per sample = 1.0 */
    for (uint16_t i = 0; i < PK_VIB_WINDOW_LEN; i++) {
        pk_vib_push(&st, mag, 0.0f, 0.0f);
    }
    uint8_t lvl_before = pk_vib_level(&st);

    /* Push one much larger sample — running sum must reflect exactly
     * one eviction + one insertion, not a full-window recompute drift. */
    pk_vib_push(&st, 10.0f, 0.0f, 0.0f);
    uint8_t lvl_after = pk_vib_level(&st);
    CHECK(lvl_after > lvl_before);

    /* Cross-check against a from-scratch sum over the known window
     * contents: (WINDOW_LEN - 1) samples at mag^2=1.0, plus one at
     * 100.0. */
    float sum_sq_expect = (float)(PK_VIB_WINDOW_LEN - 1) * (mag * mag) + 100.0f;
    float rms_expect = sqrtf(sum_sq_expect / (float)PK_VIB_WINDOW_LEN);
    float scaled = rms_expect * (255.0f / PK_VIB_RMS_FULLSCALE_MPS2);
    int32_t want = (int32_t)(scaled + 0.5f);
    if (want > 255) want = 255;
    if (want < 1) want = 1;
    CHECK(abs((int)lvl_after - (int)want) <= 1);
}

/* ============================================================ 饱和 */

static void test_huge_input_saturates_at_255_no_overflow(void)
{
    pk_vib_state_t st;
    pk_vib_reset(&st);
    /* Enormous but finite accel — must saturate cleanly, not wrap or
     * produce garbage via float->int overflow. */
    for (uint16_t i = 0; i < PK_VIB_WINDOW_LEN; i++) {
        pk_vib_push(&st, 1.0e6f, -1.0e6f, 1.0e6f);
    }
    uint8_t lvl = pk_vib_level(&st);
    CHECK(lvl == 255);
}

int main(void)
{
    test_reset_then_unfilled_window_is_unavailable();
    test_zero_input_fills_window_is_not_zero();
    test_sine_excitation_matches_analytic_rms();
    test_window_slides_old_samples_evicted();
    test_window_slides_partial_eviction_tracks_value();
    test_huge_input_saturates_at_255_no_overflow();

    if (g_fail == 0) {
        printf("PASS: all pk_vib tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d assertion(s) failed\n", g_fail);
    return 1;
}
