/* threshold_ctl.c — TL_PWM（F5）+ LEVEL/RSSI ADC 回读。
 *
 * clkdiv 修正（对任务简稿的偏离，依据 SDK 2.1.1 源码验证）：
 * 简稿按 wrap=9999 算出 clkdiv=125e6/(20e3*10000)=0.625，但 RP2040 PWM
 * 分频器为 8.4 定点、整数域下限为 1——hardware/pwm.h 的
 * pwm_config_set_clkdiv() 明确断言 `div >= 1.f`（pwm.h:162
 * valid_params_if），DIV_INT=0 不是合法分频。故改用 div=1.0、
 * wrap=6249：125 MHz / (6249+1) / 1.0 = 20 kHz 整，精度无损，
 * 且 (wrap+1)=6250 可被 2 整除，50% 占空比（3125）精确。
 *
 * ADC 通道编号（hardware/adc.h:109 + platform_defs.h:37，SDK 2.1.1）：
 * RP2040 ADC 输入 0..3 对应 GPIO26..29（ADC_BASE_PIN=26）——
 * PIN_ADC_LEVEL=26→通道 0，PIN_ADC_RSSI=27→通道 1。
 */
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "threshold_ctl.h"
#include "board_pins.h"

#define TL_PWM_FREQ_HZ 20000u
#define TL_PWM_WRAP    6249u   /* div=1.0：125e6/(6249+1)=20kHz 精确 */
#define TL_PWM_CLKDIV  1.0f    /* RP2040 PWM 分频下限即 1.0（见文件头） */

void threshold_ctl_init(void)
{
    uint slice = pwm_gpio_to_slice_num(PIN_TL_PWM);
    gpio_set_function(PIN_TL_PWM, GPIO_FUNC_PWM);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, TL_PWM_CLKDIV);
    pwm_config_set_wrap(&c, TL_PWM_WRAP);
    pwm_init(slice, &c, true);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(PIN_TL_PWM),
                       (uint16_t)((uint32_t)THRESHOLD_CTL_DEFAULT_PERMILLE *
                                  (TL_PWM_WRAP + 1) / 1000));  /* 实测定值 */
    adc_init();
    adc_gpio_init(PIN_ADC_LEVEL);
    adc_gpio_init(PIN_ADC_RSSI);
}

void threshold_ctl_set_permille(int pml)
{
    if (pml < 0) pml = 0;
    if (pml > 1000) pml = 1000;
    pwm_set_chan_level(pwm_gpio_to_slice_num(PIN_TL_PWM),
                       pwm_gpio_to_channel(PIN_TL_PWM),
                       (uint16_t)((uint32_t)pml * (TL_PWM_WRAP + 1) / 1000));
}

/*
 * 切通道后丢弃一次转换再读。
 *
 * RP2040 的 ADC 是单个采样保持电容 + 模拟多路开关：切到新通道后，第一次
 * 转换的采样期要把电容从上一通道的电压拉过来，源阻抗稍高就拉不到位。丢一
 * 次转换等于多给电容一个采样期，是 RP2040 多通道采样的常规做法。
 *
 * ⚠ 诚实记录：加这一条**并没有**解决 2026-09-11 观察到的 rssi_raw 跟着门限
 * 走的问题——加之前加之后实测一模一样。原因见 threshold_ctl.h 顶部的
 * 「rssi_raw 不可信」一节：那是板级的直流通路问题，不是采样电容没充到位。
 * 留着它是因为它本身是对的、且有 test_threshold_ctl.c 的用例兜着，不是因为
 * 它修好了那个现象。
 */
static uint16_t adc_read_settled(uint chan)
{
    adc_select_input(chan);
    (void)adc_read();        /* 丢弃：让采样电容从上一通道充到本通道 */
    return adc_read();
}

static int adc_mv(uint gpio)
{
    /* RP2040: GPIO26→通道 0, 27→通道 1 */
    return (int)(((uint32_t)adc_read_settled(gpio - ADC_BASE_PIN) * 3300u) >> 12);
}

int  threshold_ctl_read_level_mv(void) { return adc_mv(PIN_ADC_LEVEL); }
int  threshold_ctl_read_rssi_raw(void)
{
    return (int)adc_read_settled(PIN_ADC_RSSI - ADC_BASE_PIN);
}

/*
 * 诊断：连读同一通道 n 次，把每次的原始码都给出来。
 *
 * 判据——被真正驱动的节点，连读的值应当立刻稳定在真值上；而**悬空**的
 * 高阻节点会被 ADC 的采样电容反复充电，读数从"上一通道的电压"开始往别处
 * 漂。所以「连读会不会漂」能把"这一路没人驱动"和"驱动了但读错通道"分开，
 * 这是不接仪器时唯一能做的区分。
 */
void threshold_ctl_adc_burst(uint gpio, uint16_t *out, int n)
{
    adc_select_input(gpio - ADC_BASE_PIN);
    for (int i = 0; i < n; i++) out[i] = adc_read();
}
