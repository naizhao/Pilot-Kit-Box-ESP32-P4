/*
 * test_baro_compensate.c — BMP388 补偿数学（baro_compensate.c）的 host 单测。
 *
 * 跑法（与 firmware/test/ 下其它测试同一套路：把被测 .c 直接拉进本 TU）：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/main \
 *      -o /tmp/test_baro_compensate firmware/test/test_baro_compensate.c -lm
 *   /tmp/test_baro_compensate
 *
 * ── 向量来源（WP-B Task 5 Step 1 取证结论）──────────────────────────────
 *
 * 1. Bosch 官方 BMP3-Sensor-API（github.com/BoschSensortec/BMP3-Sensor-API，
 *    boschsensortec/BMP3_SensorAPI master）：仓库只有 bmp3.c / bmp3.h /
 *    bmp3_defs.h / examples/ / self-test/，**不附带任何参考输入→输出测试
 *    向量**（examples 走真机，self-test 是片上自检）。官方分发的判据文本
 *    是 BMP388 数据手册附录的参考实现源码，不是向量表。
 * 2. 公式判据：hardware/datasheets/BMP388.pdf（BST-BMP388-DS001-07，
 *    Rev 1.7，2020-11）§9.1 量化展开 / §9.2 温度补偿 / §9.3 气压补偿，
 *    与 Bosch API bmp3.c 的 parse_calib_data()/compensate_*() 三方一致。
 * 3. 因此采用**双实现互证**：本文件内含一份按手册 §9 结构独立转写的
 *    double 参考实现（ref_*，用 ldexp 推 2^n、手写符号位展开，刻意与
 *    固件的 float 紧凑写法不同源），对同一组 21 字节标定 + raw 读数断言
 *    两实现一致（容差 = float32 舍入的实测量级，见 TOL_*）；另加物理
 *    合理性带（海平面 ~1013 hPa、巡航 ~230 hPa/-54 °C、负温、0-Pa 邻域）
 *    与量化边界（全 0/全 FF/符号沿/P1=2^14→0）钉住绝对正确性。
 * 4. 黄金向量的 (raw_p, raw_t) 由开发期生成器以参考实现对合成标定二分
 *    产出后**冻结为常量**（表中期望值是生成时的 double 输出；测试内
 *    参考实现会重算并互证，抄写错误两头必有一头对不上）。合成标定
 *    CALIB_A/CALIB_B 的字节本身是权威——不是某颗真机的出厂值，这不
 *    削弱判据：两实现吃的是同一组字节，比的是手册公式的转写一致性。
 *
 * ── 覆盖面 ──────────────────────────────────────────────────────────────
 *   - 量化解析 baro_calib_parse：小端拼装、int8/int16 符号展开、2^n 标度、
 *     P1/P2 的 2^14 偏置、t_lin 清零；
 *   - 补偿 baro_compensate_temperature/pressure：与 double 参考一致、
 *     「先温度后气压」的 t_lin 顺序契约、负温/极值 raw/负压守卫域。
 */
#include "baro_compensate.c"   /* 连实现一起拉进来（仓库 host 测试惯例） */

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  [FAIL] " __VA_ARGS__);                                 \
            printf("        at %s:%d\n", __FILE__, __LINE__);                \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(got, want, tol, label)                                    \
    CHECK(fabs((double)(got) - (double)(want)) <= (tol),                     \
          "%s got=%.9g want=%.9g (tol=%.3g)\n",                              \
          (label), (double)(got), (double)(want), (tol))

/* ── 独立参考实现：手册 §9.1–9.3 的 double 转写（刻意不同源）──────────── */

static int ref_i8(uint8_t b) { return b < 0x80 ? b : (int)b - 256; }

static int ref_i16(uint8_t lo, uint8_t hi)
{
    unsigned v = (unsigned)lo | ((unsigned)hi << 8);
    return v < 0x8000 ? (int)v : (int)v - 65536;
}

static unsigned ref_u16(uint8_t lo, uint8_t hi)
{
    return (unsigned)lo | ((unsigned)hi << 8);
}

static double ref_pow2(int n) { return ldexp(1.0, n); }

typedef struct {
    double t1, t2, t3;
    double p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
    double t_lin;
} ref_calib_t;

static void ref_parse(const uint8_t c[21], ref_calib_t *r)
{
    r->t1  = (double)ref_u16(c[0], c[1]) * ref_pow2(8);            /* /2^-8 */
    r->t2  = (double)ref_u16(c[2], c[3]) / ref_pow2(30);
    r->t3  = (double)ref_i8(c[4])       / ref_pow2(48);
    r->p1  = ((double)ref_i16(c[5], c[6]) - ref_pow2(14)) / ref_pow2(20);
    r->p2  = ((double)ref_i16(c[7], c[8]) - ref_pow2(14)) / ref_pow2(29);
    r->p3  = (double)ref_i8(c[9])       / ref_pow2(32);
    r->p4  = (double)ref_i8(c[10])      / ref_pow2(37);
    r->p5  = (double)ref_u16(c[11], c[12]) * ref_pow2(3);          /* /2^-3 */
    r->p6  = (double)ref_u16(c[13], c[14]) / ref_pow2(6);
    r->p7  = (double)ref_i8(c[15])      / ref_pow2(8);
    r->p8  = (double)ref_i8(c[16])      / ref_pow2(15);
    r->p9  = (double)ref_i16(c[17], c[18]) / ref_pow2(48);
    r->p10 = (double)ref_i8(c[19])      / ref_pow2(48);
    r->p11 = (double)ref_i8(c[20])      / ref_pow2(65);
    r->t_lin = 0.0;
}

static double ref_temperature(ref_calib_t *r, uint32_t ut)      /* §9.2 */
{
    double pd1 = (double)ut - r->t1;
    r->t_lin = pd1 * r->t2 + (pd1 * pd1) * r->t3;
    return r->t_lin;
}

static double ref_pressure(const ref_calib_t *r, uint32_t up)   /* §9.3 */
{
    double t   = r->t_lin;
    double po1 = r->p5 + r->p6 * t + r->p7 * (t * t) + r->p8 * (t * t * t);
    double po2 = (double)up * (r->p1 + r->p2 * t + r->p3 * (t * t) + r->p4 * (t * t * t));
    double up2 = (double)up * (double)up;
    double po3 = up2 * (r->p9 + r->p10 * t) + (up2 * (double)up) * r->p11;
    return po1 + po2 + po3;
}

/* ── 黄金向量（生成器冻结；字节权威，期望值为参考 double 输出）────────── */

typedef struct {
    const char *name;
    const uint8_t calib[21];
    uint32_t up, ut;
    double want_p, want_t;    /* 参考实现在该 (calib,up,ut) 下的输出 */
    double p_lo, p_hi;        /* 物理合理性带（Pa）；等价性行不再断言区间 */
    double t_lo, t_hi;        /* °C */
} vector_t;

/* 合成标定 A：通用三阶形状（p1>0、p6<0、p8<0、p9>0、p11>0，全项非零）。
 * 合成标定 B：0-Pa 邻域专用——par_p1 = -2^-12（手算可验）、par_p2=0、
 * par_p5 = 8，压强多项式退化为线性，P(up) = 8 - 2^-12·up。 */
#define CALIB_A { 0x9E, 0x8E,  0x03, 0xDB,  0xE9,  0xB7, 0x7F,  0x64, 0xFC, \
                  0x23,  0x05,  0xF4, 0x01,  0xB4, 0xFF,  0x25,  0x9C,      \
                  0xC6, 0x59,  0xA6,  0x7F }
#define CALIB_B { 0x9E, 0x8E,  0x03, 0xDB,  0xE9,  0x00, 0x3F,  0x00, 0x40, \
                  0x00,  0x00,  0x01, 0x00,  0x00, 0x00,  0x00,  0x00,      \
                  0x00, 0x00,  0x00,  0x00 }

static const vector_t VECTORS[] = {
    /* name      calib      up        ut        want_p        want_t       p_lo      p_hi      t_lo  t_hi */
    { "sea",    CALIB_A, 0x504DD4, 0x9300A3, 101325.000538,  15.000001,  101000.0,  101650.0,  10.0,  20.0 },
    { "cruise", CALIB_A, 0x3EEDCB, 0x7EDCD8,  23000.003966, -53.999979,   22500.0,   23500.0, -60.0, -50.0 },
    { "winter", CALIB_A, 0x6D954C, 0x87512E, 102000.004371, -24.999998,  101000.0,  103000.0, -30.0, -20.0 },
    { "near0",  CALIB_B, 0x006000, 0x9300A3,       2.000000,  15.000001,       0.0,      10.0,   5.0,  25.0 },
    /* 等价性专用：极值 raw。maxraw 的物理值无意义，只比两实现；
     * zeroraw 在该标定下 P 为负——正是 baro_task `press_pa > 0` 守卫
     * 覆盖的域，纯单元照手册原样返回负值（守卫在任务层，不在这里）。 */
    { "maxraw", CALIB_A, 0xFFFFFF, 0xFFFFFF, 358189.710599, 383.490799,    -1e9,       1e9, -1e9,  1e9 },
    { "zeroraw",CALIB_A, 0x000000, 0x000000, -96489.652064, -495.182551,   -1e9,       1e9, -1e9,  1e9 },
};

/* 容差：float32（固件）vs double（参考）在 ~40 次乘加上的累计舍入。
 * 实测（-O2，2026-09-06）：max |fw-ref| = 0.0231 Pa（maxraw 行，相对 ~1 个
 * float32 ULP）/ 1.5e-5 °C。阈值取 4e-6 相对 + 地板（海平面 101 kPa 处
 * ≈0.46 Pa），对实测留 ~20 倍余量，容忍不同优化档的浮点重排差异。 */
#define TOL_P(got) (4e-6 * fabs((double)(got)) + 0.05)
#define TOL_T(got) (4e-6 * fabs((double)(got)) + 2e-3)

/* 跑一行向量：固件输出 vs 参考输出 vs 冻结期望值，加物理带。 */
static void run_vector(const vector_t *v, double *max_dp, double *max_dt)
{
    baro_calib_t cal;
    baro_calib_parse(v->calib, &cal);

    float got_t = baro_compensate_temperature(&cal, v->ut);   /* 先温度（t_lin） */
    float got_p = baro_compensate_pressure(&cal, v->up);      /* 后气压 */

    ref_calib_t r;
    ref_parse(v->calib, &r);
    double ref_t = ref_temperature(&r, v->ut);
    double ref_p = ref_pressure(&r, v->up);

    CHECK_NEAR(got_t, ref_t, TOL_T(ref_t), "T fw-vs-ref");
    CHECK_NEAR(got_t, v->want_t, TOL_T(v->want_t), "T fw-vs-frozen");
    CHECK_NEAR(got_p, ref_p, TOL_P(ref_p), "P fw-vs-ref");
    CHECK_NEAR(got_p, v->want_p, TOL_P(v->want_p), "P fw-vs-frozen");

    double dp = fabs((double)got_p - ref_p);
    double dt = fabs((double)got_t - ref_t);
    if (*max_dp < dp) *max_dp = dp;
    if (*max_dt < dt) *max_dt = dt;

    CHECK(got_t >= v->t_lo && got_t <= v->t_hi,
          "%s: T=%.4f 物理带 [%.1f,%.1f]\n", v->name, (double)got_t, v->t_lo, v->t_hi);
    CHECK(got_p >= v->p_lo && got_p <= v->p_hi,
          "%s: P=%.2f 物理带 [%.1f,%.1f]\n", v->name, (double)got_p, v->p_lo, v->p_hi);
}

/* ── 测试 ─────────────────────────────────────────────────────────────── */

static void test_golden_vectors(void)
{
    double max_dp = 0.0, max_dt = 0.0;
    for (size_t i = 0; i < sizeof VECTORS / sizeof VECTORS[0]; i++)
        run_vector(&VECTORS[i], &max_dp, &max_dt);
    printf("  向量 %zu 行：max |fw-ref| = %.4g Pa / %.4g °C\n",
           sizeof VECTORS / sizeof VECTORS[0], max_dp, max_dt);
}

/* 手算可验的锚点：CALIB_B 压强多项式退化为 P = 8 - 2^-12·up。
 * up=0 → +8.0；up=2^21 → 8 - 512 = -504.0（负压 = baro_task 守卫域）。 */
static void test_hand_anchors(void)
{
    static const uint8_t cb[21] = CALIB_B;
    baro_calib_t cal;
    baro_calib_parse(cb, &cal);

    (void)baro_compensate_temperature(&cal, 0x9300A3);
    CHECK(baro_compensate_pressure(&cal, 0) == 8.0f,
          "P(up=0) 应精确 8.0，got %.9g\n", (double)baro_compensate_pressure(&cal, 0));

    baro_calib_parse(cb, &cal);
    (void)baro_compensate_temperature(&cal, 0x9300A3);
    CHECK(baro_compensate_pressure(&cal, 1u << 21) == -504.0f,
          "P(up=2^21) 应精确 -504.0，got %.9g\n",
          (double)baro_compensate_pressure(&cal, 1u << 21));
}

/* 解析边界：全 0 / 全 FF / 符号沿 / P1、P2 的 2^14 偏置 / t_lin 清零。
 * 期望值独立按手册 §9.1 手算（-0.015625、2^-20 等在 float32 中精确）。 */
static void test_parse_boundaries(void)
{
    const double p2exp = ldexp(1.0, 29), p1exp = ldexp(1.0, 20);

    {   /* 全 0：除 P1/P2 的 -2^14 偏置外全零 */
        const uint8_t c[21] = {0};
        baro_calib_t cal;
        baro_calib_parse(c, &cal);
        CHECK(cal.t1 == 0.0f && cal.t2 == 0.0f && cal.t3 == 0.0f, "全0 T 系数\n");
        CHECK(cal.p1 == (float)(-ldexp(1.0, 14) / p1exp), "全0 p1=-0.015625, got %.9g\n", (double)cal.p1);
        CHECK(cal.p2 == (float)(-ldexp(1.0, 14) / p2exp), "全0 p2=-3.0518e-5, got %.9g", (double)cal.p2);
        CHECK(cal.p5 == 0.0f && cal.p9 == 0.0f && cal.p11 == 0.0f, "全0 P 系数\n");
        CHECK(cal.t_lin == 0.0f, "parse 后 t_lin 应清零\n");
    }
    {   /* 全 FF：u16 顶格 + int8 全负 */
        uint8_t c[21]; memset(c, 0xFF, sizeof c);
        baro_calib_t cal;
        baro_calib_parse(c, &cal);
        CHECK(cal.t1 == 16776960.0f, "t1=65535·2^8, got %.9g\n", (double)cal.t1);
        CHECK(fabs((double)cal.t2 - 65535.0 / ldexp(1.0, 30)) <= 8 * (double)ldexpf(1.0f, -24) * 65535.0 / ldexp(1.0, 30),
              "t2=65535/2^30, got %.9g\n", (double)cal.t2);
        CHECK(cal.t3 == -(float)ldexp(1.0, -48), "t3=-2^-48, got %.9g\n", (double)cal.t3);
        CHECK(cal.p1 == (float)(-16385.0 / p1exp), "p1=(-1-2^14)/2^20(P1 为 s16,全FF=-1), got %.9g\n", (double)cal.p1);
        CHECK(cal.p5 == 524280.0f, "p5=65535·2^3, got %.9g\n", (double)cal.p5);
        CHECK(cal.p9 == -(float)ldexp(1.0, -48), "p9=-2^-48, got %.9g\n", (double)cal.p9);
        CHECK(cal.p11 == -(float)ldexp(1.0, -65), "p11=-2^-65, got %.9g\n", (double)cal.p11);
    }
    {   /* 符号沿：每个 int8 槽位 0x80 → -128、0x7F → +127；int16 槽位 ±顶格 */
        uint8_t c[21]; memset(c, 0, sizeof c);
        c[4] = 0x80; c[9] = 0x80; c[10] = 0x7F; c[15] = 0x80; c[16] = 0x7F;
        c[19] = 0x80; c[20] = 0x80;
        c[17] = 0x00; c[18] = 0x80;   /* P9 = 0x8000 → -32768 */
        baro_calib_t cal;
        baro_calib_parse(c, &cal);
        CHECK(cal.t3  == -(float)ldexp(128.0, -48), "t3=-128/2^48\n");
        CHECK(cal.p3  == -(float)ldexp(128.0, -32), "p3=-128/2^32\n");
        CHECK(cal.p4  ==  (float)ldexp(127.0, -37), "p4=127/2^37\n");
        CHECK(cal.p7  == -(float)ldexp(128.0,  -8), "p7=-128/2^8\n");
        CHECK(cal.p8  ==  (float)ldexp(127.0, -15), "p8=127/2^15\n");
        CHECK(cal.p10 == -(float)ldexp(128.0, -48), "p10=-128/2^48\n");
        CHECK(cal.p11 == -(float)ldexp(128.0, -65), "p11=-128/2^65\n");
        CHECK(cal.p9  == -(float)ldexp(32768.0, -48), "p9=-32768/2^48\n");
    }
    {   /* P9=0x7FFF → +32767；P1=2^14 → par_p1 精确归零；P1=2^14+1 → 2^-20 */
        uint8_t c[21]; memset(c, 0, sizeof c);
        c[17] = 0xFF; c[18] = 0x7F;
        baro_calib_t cal;
        baro_calib_parse(c, &cal);
        CHECK(cal.p9 == (float)ldexp(32767.0, -48), "p9=32767/2^48\n");

        memset(c, 0, sizeof c); c[6] = 0x40;   /* P1 = 0x4000 = 2^14 */
        baro_calib_parse(c, &cal);
        CHECK(cal.p1 == 0.0f, "p1: 2^14 偏置应精确归零, got %.9g\n", (double)cal.p1);

        memset(c, 0, sizeof c); c[5] = 0x01; c[6] = 0x40;   /* P1 = 2^14+1 */
        baro_calib_parse(c, &cal);
        CHECK(cal.p1 == (float)ldexp(1.0, -20), "p1=2^-20, got %.9g\n", (double)cal.p1);
    }
    {   /* 温度补偿后 t_lin 非零，再 parse 必须清零 */
        const uint8_t c[21] = CALIB_A;
        baro_calib_t cal;
        baro_calib_parse(c, &cal);
        (void)baro_compensate_temperature(&cal, 0x9300A3);
        CHECK(cal.t_lin != 0.0f, "温度补偿后 t_lin 应非零\n");
        baro_calib_parse(c, &cal);
        CHECK(cal.t_lin == 0.0f, "re-parse 后 t_lin 必须清零\n");
    }
}

/* 顺序契约：气压吃的是「最近一次温度补偿」的 t_lin。
 * parse 后不调温度直接调气压 ≡ t_lin=0 路径（两实现同此口径）。 */
static void test_ordering_contract(void)
{
    const uint8_t c[21] = CALIB_A;
    const uint32_t up = 0x504DD4, ut = 0x9300A3;

    baro_calib_t cal;
    baro_calib_parse(c, &cal);
    (void)baro_compensate_temperature(&cal, ut);
    float chained = baro_compensate_pressure(&cal, up);

    baro_calib_parse(c, &cal);
    float cold = baro_compensate_pressure(&cal, up);

    ref_calib_t r;
    ref_parse(c, &r);
    (void)ref_temperature(&r, ut);
    double ref_chained = ref_pressure(&r, up);
    ref_parse(c, &r);
    double ref_cold = ref_pressure(&r, up);

    CHECK_NEAR(chained, ref_chained, TOL_P(ref_chained), "chained fw-vs-ref");
    CHECK_NEAR(cold, ref_cold, TOL_P(ref_cold), "cold fw-vs-ref");
    CHECK(fabs((double)chained - ref_chained) < fabs((double)cold - ref_cold) ||
          fabs((double)cold - ref_cold) > 1.0,
          "两条路径应可区分（否则顺序契约测了个寂寞）\n");
}

int main(void)
{
    printf("test_baro_compensate:\n");
    test_golden_vectors();
    test_hand_anchors();
    test_parse_boundaries();
    test_ordering_contract();

    if (g_fail) {
        printf("  FAIL (%d)\n", g_fail);
        return 1;
    }
    printf("  ALL PASS\n");
    return 0;
}
