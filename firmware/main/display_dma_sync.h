#pragma once

#include <stdbool.h>
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called in the DMA ISR after the sampled frame buffer has been adopted.
 * Return true when a higher priority task was woken. */
bool pk_lcd_dma_frame_selected(esp_lcd_panel_handle_t panel, const void *framebuffer);

#ifdef __cplusplus
}
#endif
