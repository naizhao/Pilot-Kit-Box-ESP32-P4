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
                       (TL_PWM_WRAP + 1) / 2);      /* 50% */
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

static int adc_mv(uint gpio)
{
    adc_select_input(gpio - ADC_BASE_PIN);   /* RP2040: GPIO26→0, 27→1 */
    return (int)(((uint32_t)adc_read() * 3300u) >> 12);
}

int  threshold_ctl_read_level_mv(void) { return adc_mv(PIN_ADC_LEVEL); }
int  threshold_ctl_read_rssi_raw(void)
{
    adc_select_input(PIN_ADC_RSSI - ADC_BASE_PIN);
    return (int)adc_read();
}
