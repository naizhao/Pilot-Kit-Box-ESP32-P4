/* test_pk_batt_model.c — host proof for 电芯 SoC 模型（电压 → 电量百分比）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main \
 *      -o /tmp/test_battmodel firmware/test/test_pk_batt_model.c \
 *      firmware/main/pk_batt_model.c && /tmp/test_battmodel
 *
 * 这份测试**先于重构存在**：曲线原本长在 power_eta6098.c 里（一颗充电芯片
 * 的文件），一个测试都没有，而 SY6970 backend 又在跨文件借它出电量。搬家
 * 之前先把行为钉死，搬完再跑一遍——否则"重构没改行为"这句话没有凭据。
 * 2026-09-13 曲线换成本机实测标定值（原始 8.37h / 502 点；剔除开头 7 分钟
 * H1 空载段后，有效放电 8.25h / 495 点），期望值随之全改；
 * 那次搬家的等价性由当时的绿灯记录背书，不靠现在这份文件复现。
 *
 * 七段，每段压一件事：
 *   1) 七个拐点的绝对值——2026-09-13 本机实测标定，改了必须有新数据支撑；
 *   2) 段内线性插值——拐点对了不代表段内斜率对；
 *   3) 上下饱和——超出量程必须钳死，不许算出 >100 或负数；
 *   4) 单调不减——SoC 对电压必须单调，否则电量会在充电过程中往回跳；
 *   5) 值域恒在 0..100——调用方（power_service 的 pct_est 是 uint8_t）
 *      按这个区间存，越界会静默截断；
 *   6) 非线性形状——带负载曲线末期陡降，低压段每毫伏崩得远比平台段快，
 *      退回线性映射这条会红。
 *   7) U19 可观测工况折算——仅电池时不重复扣负载压降；U19 检出的外部
 *      供电空载/充电端电压要折回同一条带载曲线。这里使用 2026-09-13
 *      的两组拔线实测电压作数值合同；它不能证明当前 P4 看不到的 H1 插拔。
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

/* ── 1) 七个拐点（2026-09-13 实测标定值，起点已剔除空载段）──────────────────────────
 * 这些数来自本机原始 8.37 小时 / 502 点记录中剔除 H1 空载段后的
 * 8.25 小时 / 495 个有效放电点（见 pk_batt_model.h），
 * 不是通用曲线。改动任何一个都意味着"重新标定过"，必须有新的实测数据
 * 支撑——所以钉死在这里。 */
static void test_knees(void)
{
    chk_int("4026mV = 满",    pk_batt_mv_to_pct(4026), 100);
    chk_int("3872mV = 90%",   pk_batt_mv_to_pct(3872), 90);
    chk_int("3700mV = 80%",   pk_batt_mv_to_pct(3700), 80);
    chk_int("3583mV = 60%",   pk_batt_mv_to_pct(3583), 60);
    chk_int("3380mV = 40%",   pk_batt_mv_to_pct(3380), 40);
    chk_int("3249mV = 15%",   pk_batt_mv_to_pct(3249), 15);
    chk_int("2839mV = 0%",    pk_batt_mv_to_pct(2839), 0);
}

/* ── 2) 段内线性插值 ─────────────────────────────────────────────── */
static void test_interpolation(void)
{
    chk_int("3949mV 段中点", pk_batt_mv_to_pct(3949), 90 +  77 * 10 / 154);
    chk_int("3786mV 段中点", pk_batt_mv_to_pct(3786), 80 +  86 * 10 / 172);
    chk_int("3641mV 段中点", pk_batt_mv_to_pct(3641), 60 +  58 * 20 / 117);
    chk_int("3481mV 段中点", pk_batt_mv_to_pct(3481), 40 + 101 * 20 / 203);
    chk_int("3314mV 段中点", pk_batt_mv_to_pct(3314), 15 +  65 * 25 / 131);
    chk_int("3044mV 段中点", pk_batt_mv_to_pct(3044),      205 * 15 / 410);
}

/* ── 3) 上下饱和 ─────────────────────────────────────────────────── */
static void test_saturation(void)
{
    chk_int("4200mV 仍是 100", pk_batt_mv_to_pct(4200), 100);
    chk_int("5000mV 不越界",   pk_batt_mv_to_pct(5000), 100);
    chk_int("2839mV 以下为 0", pk_batt_mv_to_pct(2800), 0);
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

/* ── 6) 形状必须是非线性的：末期陡降 ────────────────────────────
 * 这是**带负载**的实测曲线（见 pk_batt_model.h），形态与静置开路曲线
 * 不同：放电末期电池内阻上的压降迅速放大，电压崩得很快，于是 SoC 对
 * 电压的斜率在低压段远大于平台段。
 *
 * 实测：低压段 3224→3244（Δ20mV）跨 12 个百分点；
 *       平台段 3704→3844（Δ140mV）只跨 10 个百分点。
 * 单位电压的斜率差约 8 倍。
 *
 * 形状哨兵——谁把曲线改回线性映射（两段斜率相等），这条会红。
 *
 * 注：上一版曲线（静置口径）这条判据的方向是**相反**的，当时写的是
 * "平台段更陡"。换成带负载曲线后形态整个变了，不要拿旧结论套。 */
static void test_curve_is_not_linear(void)
{
    /* 归一到"每 100mV 涨多少点"再比，避免段宽不同造成误判。
     * 实测：低压段 3249~3380（131mV / 25 点）≈19 点/100mV；
     *       平台段 3700~3872（172mV / 10 点）≈ 6 点/100mV。 */
    const int low_per100  = (pk_batt_mv_to_pct(3380) - pk_batt_mv_to_pct(3249))
                            * 100 / 131;
    const int plat_per100 = (pk_batt_mv_to_pct(3872) - pk_batt_mv_to_pct(3700))
                            * 100 / 172;
    chk_true("末期每毫伏崩得比平台段快（曲线非线性）", low_per100 > plat_per100);
}

/* ── 7) U19 可观测工况折回同一条“606mA 带载”曲线 ─────────────────
 *
 * 曲线采样本身就是仅电池、满负载状态，所以 battery-only 的 BATV 已经是
 * 查表输入，不能再减一次 LOAD_NOM*R_DIS。
 *
 * 两组实测拔线电压提供了折算函数的数值合同：
 *   终充维持：插电空载 4164mV → 拔线满载 4044mV；
 *   2050mA 充电：插电 4184mV → 拔线满载 3624mV。
 * 同一瞬间的 SoC 没有突变，若调用方能正确给出外部供电状态，折算后的电压
 * 最多只容许 ADC 量化级以内差异。H1 的 USB0_5V 从 U19 下游供电，当前
 * P4 没有 H1 VBUS 信号，所以这不是 H1 插拔端到端闭环测试。
 */
static void test_operating_point_normalization(void)
{
    const int full_battery = pk_batt_equiv_loaded_mv(4044, false, false, 0);
    const int full_hold = pk_batt_equiv_loaded_mv(4164, true, false, 250);
    chk_int("仅电池 BATV 已是带载曲线口径", full_battery, 4044);
    chk_true("终充维持拔线前后连续",
             full_hold >= full_battery - 1 && full_hold <= full_battery + 1);

    const int mid_battery = pk_batt_equiv_loaded_mv(3624, false, false, 0);
    const int mid_charging = pk_batt_equiv_loaded_mv(4184, true, true, 2050);
    chk_int("仅电池中段不重复扣 121mV", mid_battery, 3624);
    chk_true("2050mA 充电拔线前后连续",
             mid_charging >= mid_battery - 1 && mid_charging <= mid_battery + 1);

    /* ICHGR 在非充电状态会保留陈值；闸门必须忽略它。 */
    chk_int("非充电时忽略陈旧 ICHGR",
            pk_batt_equiv_loaded_mv(4164, true, false, 2050), full_hold);
}

int main(void)
{
    test_knees();
    test_interpolation();
    test_saturation();
    test_monotonic();
    test_range();
    test_curve_is_not_linear();
    test_operating_point_normalization();

    if (g_fail == 0) { printf("test_pk_batt_model: all OK\n"); return 0; }
    printf("test_pk_batt_model: %d FAIL\n", g_fail);
    return 1;
}
