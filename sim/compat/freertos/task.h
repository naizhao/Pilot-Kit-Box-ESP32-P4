/* task.h 桩：诊断页不起任务，只是被间接 include 到。 */
#pragma once
#include "freertos/FreeRTOS.h"
static inline void vTaskDelay(TickType_t t) { (void)t; }
