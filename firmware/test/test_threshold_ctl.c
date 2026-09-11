/*
 * test_threshold_ctl.c — 门限 PWM 占空比换算 + ADC 换通道后的读数正确性。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 -I firmware/test/host_stubs \
 *      -o /tmp/test_threshold_ctl firmware/test/test_threshold_ctl.c \
 *      firmware/rp2040/threshold_ctl.c \
 *   && /tmp/test_threshold_ctl
 *
 * 为什么值得为两行修复写这个测试
 * ------------------------------
 * 这个缺陷不会让任何东西崩，它只是让 `H` 命令报出**说得通但完全错误**的
 * 数字：2026-09-11 实测 rssi_raw 全程跟着门限 PWM 走（六个扫描点换算后都
 * 落在 LEVEL 读数的 1–2% 以内），看上去像"RSSI 随门限变化"这种能自圆其说
 * 的结论。这类读数一旦被当成判据，后面所有基于它的排查都是错的。
 *
 * 模型：RP2040 的 ADC 是**单个采样保持电容 + 模拟多路开关**。切到新通道后，
 * 第一次转换的采样期要把电容从上一通道的电压拉过来，源阻抗稍高就拉不到位，
 * 读数偏向上一通道。这里把它建模成"换通道后第一次读回 (旧+新)/2，之后才
 * 是真值"——只要驱动在切通道后丢弃一次转换，这个模型就还原成真值。
 */
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"

#include "threshold_ctl.h"
#include "board_pins.h"

#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK_EQ(got, want, what) do { \
    long g_ = (long)(got), w_ = (long)(want); \
    if (g_ != w_) { \
        printf("  [FAIL] %s: 得到 %ld，期望 %ld  (%s:%d)\n", \
               what, g_, w_, __FILE__, __LINE__); \
        g_fail++; \
    } \
} while (0)

/* ── 外设桩 ───────────────────────────────────────────────────── */

static uint16_t s_truth[4];     /* 每个通道真实电压（ADC 码） */
static unsigned s_sel;          /* 当前选中通道 */
static uint16_t s_held;         /* 采样保持电容上的残留 */
static int      s_reads;        /* 总转换次数，用来确认确实多读了一次 */

void adc_init(void) { }
void adc_gpio_init(unsigned int gpio) { (void)gpio; }
void adc_select_input(unsigned int input) { s_sel = input; }

uint16_t adc_read(void)
{
    /* 换通道后的第一次转换只把电容拉到一半；再读一次才是真值。 */
    uint16_t out = (uint16_t)(((uint32_t)s_held + s_truth[s_sel]) / 2u);
    s_held = s_truth[s_sel];
    s_reads++;
    return out;
}

static uint16_t s_pwm_level;
unsigned int pwm_gpio_to_slice_num(unsigned int gpio) { (void)gpio; return 0; }
unsigned int pwm_gpio_to_channel(unsigned int gpio)   { (void)gpio; return 0; }
pwm_config   pwm_get_default_config(void) { pwm_config c = {1.0f, 0}; return c; }
void pwm_config_set_clkdiv(pwm_config *c, float d) { c->clkdiv = d; }
void pwm_config_set_wrap(pwm_config *c, uint32_t w) { c->wrap = w; }
void pwm_init(unsigned int s, pwm_config *c, int go) { (void)s; (void)c; (void)go; }
void pwm_set_chan_level(unsigned int s, unsigned int ch, uint16_t lv)
{ (void)s; (void)ch; s_pwm_level = lv; }
void gpio_set_function(unsigned int gpio, int fn) { (void)gpio; (void)fn; }

/* ── 用例 ─────────────────────────────────────────────────────── */

/* 两个通道差得很远时，先读 LEVEL 再读 RSSI 必须各读各的真值。
 * 缺陷版本下 RSSI 会读到 (LEVEL+RSSI)/2，正好长成"RSSI 跟着门限走"。 */
static void test_channel_switch_returns_own_channel(void)
{
    s_truth[PIN_ADC_LEVEL - ADC_BASE_PIN] = 1000;
    s_truth[PIN_ADC_RSSI  - ADC_BASE_PIN] = 3000;
    s_held = 0;

    /* 先读 LEVEL：1000 码 × 3300 / 4096 = 805 mV */
    CHECK_EQ(threshold_ctl_read_level_mv(), (1000 * 3300) >> 12,
             "LEVEL 读数");
    /* 紧接着读 RSSI：必须是 3000，不能是 (1000+3000)/2 = 2000 */
    CHECK_EQ(threshold_ctl_read_rssi_raw(), 3000, "换通道后的 RSSI 读数");
    /* 反过来再来一遍，证明不是"恰好 RSSI 那次对" */
    CHECK_EQ(threshold_ctl_read_level_mv(), (1000 * 3300) >> 12,
             "换回 LEVEL 的读数");
}

/* RSSI 不该随门限变化：把门限从 0 拉到 1000 permille，RSSI 通道真值不变，
 * 读数就必须一动不动。这条直接对应实测现场看到的假象。 */
static void test_rssi_does_not_track_threshold(void)
{
    s_truth[PIN_ADC_RSSI - ADC_BASE_PIN] = 2500;
    int first = 0;
    for (int pml = 0; pml <= 1000; pml += 200) {
        threshold_ctl_set_permille(pml);
        /* 门限那一路的电压跟着 PWM 走 */
        s_truth[PIN_ADC_LEVEL - ADC_BASE_PIN] = (uint16_t)(pml * 4u);
        (void)threshold_ctl_read_level_mv();       /* 现场就是这个读序 */
        int rssi = threshold_ctl_read_rssi_raw();
        if (pml == 0) first = rssi;
        CHECK_EQ(rssi, first, "RSSI 随门限变化了");
        CHECK_EQ(rssi, 2500, "RSSI 读数偏离真值");
    }
}

/* 占空比换算：800 permille → (6249+1)*800/1000 = 5000。
 * 夹取也要成立，否则越界值会被截断成一个看起来正常的占空比。 */
static void test_permille_to_duty(void)
{
    threshold_ctl_set_permille(800);
    CHECK_EQ(s_pwm_level, 5000, "800 permille 的占空比");
    threshold_ctl_set_permille(0);
    CHECK_EQ(s_pwm_level, 0, "0 permille");
    threshold_ctl_set_permille(1000);
    CHECK_EQ(s_pwm_level, 6250, "1000 permille");
    threshold_ctl_set_permille(5000);          /* 越界 → 夹到 1000 */
    CHECK_EQ(s_pwm_level, 6250, "越界值夹取");
    threshold_ctl_set_permille(-7);
    CHECK_EQ(s_pwm_level, 0, "负值夹取");
}

/* 开机默认必须真的被写进 PWM——头文件里那组实测数据（800→808 帧）只有在
 * 默认值确实生效时才有意义。 */
static void test_init_applies_default(void)
{
    s_pwm_level = 0xFFFF;
    threshold_ctl_init();
    CHECK_EQ(s_pwm_level,
             (uint32_t)THRESHOLD_CTL_DEFAULT_PERMILLE * 6250u / 1000u,
             "init 落下的默认占空比");
}

int main(void)
{
    test_channel_switch_returns_own_channel();
    test_rssi_does_not_track_threshold();
    test_permille_to_duty();
    test_init_applies_default();

    if (g_fail) { printf("test_threshold_ctl: %d FAIL\n", g_fail); return 1; }
    printf("test_threshold_ctl: all OK (%d 次 ADC 转换)\n", s_reads);
    return 0;
}
