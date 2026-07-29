/* FreeRTOS.h 桩 —— 诊断页经 pilot_kit.h 间接引到它，只需要类型与几个宏。
 * 模拟器是单线程，同步原语全部退化成空操作。 */
#pragma once

#include <stdint.h>

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *EventGroupHandle_t;
typedef void *QueueHandle_t;
typedef int   BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE            1
#define pdFALSE           0
#define pdPASS            1
#define portMAX_DELAY     0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)

typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(mux)      ((void)(mux))
#define portEXIT_CRITICAL(mux)       ((void)(mux))
