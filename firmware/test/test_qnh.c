/*
 * test_qnh.c — QNH 反解纯函数的单测。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_qnh \
 *      firmware/test/test_qnh.c \
 *      firmware/main/qnh_math.c \
 *   && /tmp/test_qnh
 *
 * 合同：pk_qnh_from_pressure_alt() 是 baro_task.c 高度公式
 *   alt = 44330 * (1 - (P/QNH)^0.190295)
 * 的反解。因此最有力的判据是**往返**：用反解出的 QNH 再代回高度公式，
 * 必须还原出输入海拔（标准大气下）。此外覆盖钳制边界与非法输入。
 */
#include "qnh_math.h"

#include <math.h>
#include <stdio.h>

static int fails = 0;

static void expect_near(const char *what, float got, float want, float tol)
{
    if (fabsf(got - want) > tol) {
        printf("FAIL %s: got %.4f want %.4f (tol %.4f)\n", what, got, want, tol);
        fails++;
    } else {
        printf("ok   %s: %.4f\n", what, got);
    }
}

/* baro_task.c 的正向公式，测试独立重写一份作为判据（不复用被测实现）。 */
static float alt_from_press_qnh(float press_pa, float qnh_hpa)
{
    return 44330.0f * (1.0f - powf(press_pa / (qnh_hpa * 100.0f), 0.190295f));
}

int main(void)
{
    /* 1. 实测样点：P=100779Pa，GPS 正高 105.8m → QNH≈1020.5hPa。
     *    （2026-09-11 实机日志：baro P=100779 / GPS alt=345–351ft。） */
    float q = pk_qnh_from_pressure_alt(100779.0f, 105.8f);
    expect_near("sample QNH", q, 1020.5f, 0.5f);

    /* 2. 往返：反解出的 QNH 代回正向公式应还原输入海拔。覆盖低/中/高。 */
    const float press[3]  = { 100779.0f, 96800.0f, 84300.0f };
    const float alt_in[3] = { 105.8f,    400.0f,   1500.0f };
    for (int i = 0; i < 3; i++) {
        float qq = pk_qnh_from_pressure_alt(press[i], alt_in[i]);
        float back = alt_from_press_qnh(press[i], qq);
        expect_near("round-trip alt", back, alt_in[i], 0.5f);
    }

    /* 3. 钳制：极端输入不得越界 [950,1050]。
     *    low : P=90000 且 h=-500 → 848.5hPa → 950
     *    high: P=108000 且 h=3000 → 1560.7hPa → 1050 */
    expect_near("clamp low",  pk_qnh_from_pressure_alt(90000.0f,  -500.0f), 950.0f,  0.001f);
    expect_near("clamp high", pk_qnh_from_pressure_alt(108000.0f, 3000.0f), 1050.0f, 0.001f);

    /* 4. 非法输入（P<=0 或海拔>=44330）返回默认标准海压，不产生 NaN/Inf。 */
    expect_near("bad P=0",      pk_qnh_from_pressure_alt(0.0f,     100.0f),  1013.25f, 0.001f);
    expect_near("bad P<0",      pk_qnh_from_pressure_alt(-5.0f,    100.0f),  1013.25f, 0.001f);
    expect_near("bad alt>=44330", pk_qnh_from_pressure_alt(50000.0f, 50000.0f), 1013.25f, 0.001f);

    if (fails) { printf("\n%d FAILED\n", fails); return 1; }
    printf("\nall QNH tests passed\n");
    return 0;
}
