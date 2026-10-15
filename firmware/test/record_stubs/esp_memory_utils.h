#pragma once
#include <stdbool.h>
#include <stdint.h>
/* P4 domains from soc/soc.h and soc/ext_mem_defs.h. Native SDK dispatch is
 * additionally compiled in test_sd_dma_buffer.py; these serve lifecycle tests. */
static inline bool esp_ptr_dma_capable(const void *p)
{ uintptr_t a=(uintptr_t)p;return a>=0x4ff00000&&a<0x4ffc0000; }
static inline bool esp_ptr_dma_ext_capable(const void *p)
{ uintptr_t a=(uintptr_t)p;return a>=0x48000000&&a<0x4c000000; }
