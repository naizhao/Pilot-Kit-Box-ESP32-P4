/* rf_safety.c — F2 实现。向量是纯数据，host 测试直接调；
 * pico 调用全部关在 apply 里并用 RF_SAFETY_HOST_TEST 隔离。 */
#include "board_pins.h"
#include "rf_safety.h"

#ifndef RF_SAFETY_HOST_TEST
#include "pico/stdlib.h"
#endif

static rf_safety_pin_t s_boot[] = {
    { PIN_BIAS_EN_1090, 1, "bias 1090 off (PMOS active-low)" },
    { PIN_BIAS_EN_978,  1, "bias 978 off (PMOS active-low)" },
    { PIN_GNSS_SEL_A, RF_SAFETY_DEFAULT_GNSS_A, "GNSS select: legal pair" },
    { PIN_GNSS_SEL_B, RF_SAFETY_DEFAULT_GNSS_B, "GNSS select: legal pair" },
    /* 1090 天线选择：A=1/B=0 → J1-J3 板载 IFA（默认装配，与 GNSS 开关
     * 同一真值表方向：V1 高 + V2 低 = 板载通路）。 */
    { PIN_ANT_SEL_1090_A, 1, "1090 antenna select: onboard IFA" },
    { PIN_ANT_SEL_1090_B, 0, "1090 antenna select: onboard IFA" },
};

const rf_safety_pin_t *rf_safety_boot_vector(int *count)
{
    if (count) *count = (int)(sizeof(s_boot) / sizeof(s_boot[0]));
    return s_boot;
}

void rf_safety_apply_boot_state(void)
{
#ifndef RF_SAFETY_HOST_TEST
    int n = 0;
    const rf_safety_pin_t *v = rf_safety_boot_vector(&n);
    for (int i = 0; i < n; i++) {
        gpio_init(v[i].gpio);
        /* 先落电平再开输出：active-low 的 bias tee 在引脚转输出的瞬间
         * 绝不能看到意外的低（audit P1：否则上电毛刺会瞬时打开 bias）。 */
        gpio_put(v[i].gpio, v[i].level);
        gpio_set_dir(v[i].gpio, GPIO_OUT);
    }
#endif
}
