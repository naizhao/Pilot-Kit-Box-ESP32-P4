#pragma once
/*
 * host_stubs/hardware/adc.h — 见 pico/stdlib.h：只声明，测试自己实现。
 *
 * ADC_BASE_PIN 取真值 26（SDK platform_defs.h:37 / adc.h:109：RP2040 的
 * ADC 输入 0..3 对应 GPIO26..29）。这个常量参与被测代码的通道换算，
 * 写错了测试会绿而实板读错通道，所以不能随手编一个。
 */
#include <stdint.h>

#define ADC_BASE_PIN 26u

void     adc_init(void);
void     adc_gpio_init(unsigned int gpio);
void     adc_select_input(unsigned int input);
uint16_t adc_read(void);
