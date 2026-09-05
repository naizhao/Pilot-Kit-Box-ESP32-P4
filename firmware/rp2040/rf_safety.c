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
        gpio_set_dir(v[i].gpio, GPIO_OUT);
        gpio_put(v[i].gpio, v[i].level);
    }
#endif
}
