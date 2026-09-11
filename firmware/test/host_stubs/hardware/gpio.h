#pragma once
/* host_stubs/hardware/gpio.h — 见 pico/stdlib.h：只声明，测试自己实现。 */
#include <stdint.h>

#define GPIO_FUNC_PWM 4

void gpio_set_function(unsigned int gpio, int fn);
