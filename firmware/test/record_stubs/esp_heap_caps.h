#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
static inline void *heap_caps_malloc(size_t size,unsigned caps) { (void)caps;return malloc(size); }
static inline void heap_caps_free(void *ptr) { free(ptr); }
/* 周期状态日志（airband_task.c 的 mem/underrun 行）在 host 上也参与编译：
 * 这里给常量 0，仅保证可编译可链接，不参与任何行为断言。 */
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps;return 0; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps;return 0; }
