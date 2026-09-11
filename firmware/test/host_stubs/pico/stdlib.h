#pragma once
/*
 * host_stubs/pico/stdlib.h — RP2040 模块在 host 上的注入点。
 *
 * 与 host_stubs/esp_timer.h 同一套约定：**只声明不定义**，实现由每个测试
 * 自己给，测试因此能把外设行为造成任意样子（比如让 ADC 表现出采样保持电容
 * 没充到位）。真外设的行为拿真板测，这里测的是驱动逻辑对外设行为的处理。
 */
#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

#include "hardware/gpio.h"
