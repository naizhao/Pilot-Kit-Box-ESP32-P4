#pragma once
/* host_stubs/hardware/pwm.h — 见 pico/stdlib.h：只声明，测试自己实现。 */
#include <stdint.h>

typedef struct { float clkdiv; uint32_t wrap; } pwm_config;

unsigned int pwm_gpio_to_slice_num(unsigned int gpio);
unsigned int pwm_gpio_to_channel(unsigned int gpio);
pwm_config   pwm_get_default_config(void);
void         pwm_config_set_clkdiv(pwm_config *c, float div);
void         pwm_config_set_wrap(pwm_config *c, uint32_t wrap);
void         pwm_init(unsigned int slice, pwm_config *c, int start);
void         pwm_set_chan_level(unsigned int slice, unsigned int chan,
                                uint16_t level);
