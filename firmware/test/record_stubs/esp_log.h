#pragma once
#include "../host_stubs/esp_log.h"
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *,va_list);
static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn) { (void)fn;return NULL; }
static inline void esp_log_level_set(const char *tag,esp_log_level_t level) { (void)tag;(void)level; }
