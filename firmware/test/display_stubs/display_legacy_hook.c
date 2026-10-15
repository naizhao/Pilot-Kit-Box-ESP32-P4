#include "display_sdk.h"

/* A separate weak definition keeps the harness runnable against pre-fix
 * display.c. The real production hook is strong and overrides this fallback. */
extern bool pk_display_test_legacy_selected(esp_lcd_panel_handle_t, const void *);
bool __attribute__((weak)) pk_lcd_dma_frame_selected(esp_lcd_panel_handle_t panel, const void *framebuffer)
{
    return pk_display_test_legacy_selected(panel, framebuffer);
}
