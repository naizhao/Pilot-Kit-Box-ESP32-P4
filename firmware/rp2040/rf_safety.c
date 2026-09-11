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

/* ── 运行期天线选择（真值表唯一出处，见 rf_safety.h）───────────────── */

/*
 * 1090（U16）：A=1/B=0 → 板载 IFA；A=0/B=1 → 外接 J6。
 * 与 board_pins.h 的 PIN_ANT_SEL_1090_A 注释同源（netlist U8.6/U8.7）。
 */
void rf_safety_ant_1090_levels(rf_ant_1090_t sel, bool *a, bool *b)
{
    const bool ext = (sel == RF_ANT_1090_EXTERNAL);
    if (a) *a = !ext;
    if (b) *b = ext;
}

/*
 * GNSS（U17）：A=0/B=1 → 外接 J2；A=1/B=0 → 板载 patch。
 *
 * ⚠ 这两根线在 v3/v4 上**兼做偏置馈电门控**（board_pins.h: "U17 V1 / Q4
 * gate（ECO 后配对）"）。未打 ECO 的板子上配对是反的——选中哪一路就给
 * 另一路供电，有源天线拿不到电、收不到星。固件改不了这件事，UI 上必须
 * 标注，见 P4 侧设置页那一行的提示。
 */
void rf_safety_ant_gnss_levels(rf_ant_gnss_t sel, bool *a, bool *b)
{
    const bool onboard = (sel == RF_ANT_GNSS_ONBOARD);
    if (a) *a = onboard;
    if (b) *b = !onboard;
}

#ifndef RF_SAFETY_HOST_TEST
/* 先写将要变低的那根，再写将要变高的那根：SPDT 的两根选择线中间态宁可
 * 短暂"都低"（两路都断开）也不要"都高"（两路同时导通 = 天线并联）。 */
static void apply_pair(int gpio_a, int gpio_b, bool a, bool b)
{
    if (!a) gpio_put(gpio_a, 0);
    if (!b) gpio_put(gpio_b, 0);
    if (a)  gpio_put(gpio_a, 1);
    if (b)  gpio_put(gpio_b, 1);
}
#endif

void rf_safety_set_ant_1090(rf_ant_1090_t sel)
{
    bool a, b;
    rf_safety_ant_1090_levels(sel, &a, &b);
#ifndef RF_SAFETY_HOST_TEST
    apply_pair(PIN_ANT_SEL_1090_A, PIN_ANT_SEL_1090_B, a, b);
#else
    (void)a; (void)b;
#endif
}

void rf_safety_set_ant_gnss(rf_ant_gnss_t sel)
{
    bool a, b;
    rf_safety_ant_gnss_levels(sel, &a, &b);
#ifndef RF_SAFETY_HOST_TEST
    apply_pair(PIN_GNSS_SEL_A, PIN_GNSS_SEL_B, a, b);
#else
    (void)a; (void)b;
#endif
}
