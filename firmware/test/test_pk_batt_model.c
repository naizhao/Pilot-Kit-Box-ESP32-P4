/* test_pk_batt_model.c — host proof for 电芯 SoC 模型（电压 → 电量百分比）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main \
 *      -o /tmp/test_battmodel firmware/test/test_pk_batt_model.c \
 *      firmware/main/pk_batt_model.c && /tmp/test_battmodel
 *
 * 这份测试**先于重构存在**：曲线原本长在 power_eta6098.c 里（一颗充电芯片
 * 的文件），一个测试都没有，而 SY6970 backend 又在跨文件借它出电量。搬家
 * 之前先把行为钉死，搬完再跑一遍——否则"重构没改行为"这句话没有凭据。
 *
 * 六段，每段压一件事：
 *   1) 五个拐点的绝对值——曲线的形状就是这五个数，改了必须是有意的；
 *   2) 段内线性插值——拐点对了不代表段内斜率对；
 *   3) 上下饱和——超出量程必须钳死，不许算出 >100 或负数；
 *   4) 单调不减——SoC 对电压必须单调，否则电量会在充电过程中往回跳；
 *   5) 值域恒在 0..100——调用方（power_service 的 pct_est 是 uint8_t）
 *      按这个区间存，越界会静默截断；
 *   6) 非线性形状——锂电 3.7~4.0V 平台段必须比低压段"每毫伏少涨几个点"，
 *      这正是当初不用线性映射的理由，退回线性这条会红。
 */
#include <stdio.h>

#include "../main/pk_batt_model.h"

static int g_fail;

static void chk_int(const char *what, int got, int want)
{
    if (got != want) {
        printf("FAIL %s: got %d want %d\n", what, got, want);
        g_fail++;
    }
}

static void chk_true(const char *what, int cond)
{
    if (!cond) { printf("FAIL %s\n", what); g_fail++; }
}

/* ── 1) 五个拐点 ─────────────────────────────────────────────────── */
static void test_knees(void)
{
    chk_int("4150mV = 满",    pk_batt_mv_to_pct(4150), 100);
    chk_int("3850mV = 75%",   pk_batt_mv_to_pct(3850), 75);
    chk_int("3700mV = 50%",   pk_batt_mv_to_pct(3700), 50);
    chk_int("3500mV = 20%",   pk_batt_mv_to_pct(3500), 20);
    chk_int("3300mV = 0%",    pk_batt_mv_to_pct(3300), 0);
}

/* ── 2) 段内线性插值 ─────────────────────────────────────────────── */
static void test_interpolation(void)
{
    /* 3850..4150 段：75 + (mv-3850)*25/300 */
    chk_int("4000mV 段中点", pk_batt_mv_to_pct(4000), 75 + 150 * 25 / 300);
    /* 3700..3850 段：50 + (mv-3700)*25/150 */
    chk_int("3775mV 段中点", pk_batt_mv_to_pct(3775), 50 + 75 * 25 / 150);
    /* 3500..3700 段：20 + (mv-3500)*30/200 */
    chk_int("3600mV 段中点", pk_batt_mv_to_pct(3600), 20 + 100 * 30 / 200);
    /* 3300..3500 段：(mv-3300)*20/200 */
    chk_int("3400mV 段中点", pk_batt_mv_to_pct(3400), 100 * 20 / 200);
}

/* ── 3) 上下饱和 ─────────────────────────────────────────────────── */
static void test_saturation(void)
{
    chk_int("4200mV 仍是 100", pk_batt_mv_to_pct(4200), 100);
    chk_int("5000mV 不越界",   pk_batt_mv_to_pct(5000), 100);
    chk_int("3000mV 是 0",     pk_batt_mv_to_pct(3000), 0);
    chk_int("0mV 不为负",      pk_batt_mv_to_pct(0),    0);
    /* 负电压：ADC/解码出错时可能传进来，不许算出负数或大数 */
    chk_int("负电压不为负",    pk_batt_mv_to_pct(-100), 0);
}

/* ── 4) 单调不减 ─────────────────────────────────────────────────
 * 电量随电压单调上升是这条曲线的**基本性质**，破了会让电量在充电过程中
 * 往回跳。整个量程逐毫伏扫一遍，比抽查拐点更有力。 */
static void test_monotonic(void)
{
    int prev = pk_batt_mv_to_pct(2500);
    for (int mv = 2500; mv <= 4500; ++mv) {
        const int cur = pk_batt_mv_to_pct(mv);
        if (cur < prev) {
            printf("FAIL 单调性在 %dmV 破了: %d → %d\n", mv, prev, cur);
            g_fail++;
            return;
        }
        prev = cur;
    }
}

/* ── 5) 值域恒在 0..100 ─────────────────────────────────────────── */
static void test_range(void)
{
    for (int mv = -500; mv <= 6000; mv += 7) {
        const int p = pk_batt_mv_to_pct(mv);
        if (p < 0 || p > 100) {
            printf("FAIL %dmV 算出 %d，越出 0..100\n", mv, p);
            g_fail++;
            return;
        }
    }
}

/* ── 6) 形状必须是非线性的 ──────────────────────────────────────
 * 锂电在 3.7~4.0V 之间"平得像条直线"——指的是**电压**平，而同一段里容量
 * 走掉了一大半。所以 SoC 对电压的斜率在平台段必须**更大**，不是更小：
 * 电压只挪一点点，电量就该跟着走一大步。
 *
 * 这正是当初不用线性映射的理由。线性映射按电压均分百分比，平台段电压
 * 走得慢、百分比就跟着停住（"还剩一半"挂很久），等掉出平台电压骤降，
 * 百分比又突然崩到 0。
 *
 * 判据：平台段（3700→3850，Δ150mV）涨的百分点必须**多于**低压段
 * （3350→3500，同样 Δ150mV）。实测 25 vs 15。
 * 形状哨兵——谁把曲线改回线性映射（两段相等），这条会红。
 *
 * 注：这条断言初版写反了（以为平台段该更平缓），被本测试当场抓出。
 * 留此注记，免得下一个人又按直觉改回去。 */
static void test_curve_is_not_linear(void)
{
    const int plateau = pk_batt_mv_to_pct(3850) - pk_batt_mv_to_pct(3700);
    const int lowend  = pk_batt_mv_to_pct(3500) - pk_batt_mv_to_pct(3350);
    chk_true("平台段每毫伏涨得比低压段快（曲线非线性）", plateau > lowend);
}

int main(void)
{
    test_knees();
    test_interpolation();
    test_saturation();
    test_monotonic();
    test_range();
    test_curve_is_not_linear();

    if (g_fail == 0) { printf("test_pk_batt_model: all OK\n"); return 0; }
    printf("test_pk_batt_model: %d FAIL\n", g_fail);
    return 1;
}
