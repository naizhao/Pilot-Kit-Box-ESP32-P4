#pragma once
#include <pthread.h>
#include <assert.h>
#define configASSERT(c) assert(c)
#define portMAX_DELAY (-1)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS pdTRUE
#define tskNO_AFFINITY (-1)
typedef int BaseType_t;
#define pdMS_TO_TICKS(ms) (ms)
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) pthread_mutex_lock(m)
#define portEXIT_CRITICAL(m) pthread_mutex_unlock(m)
