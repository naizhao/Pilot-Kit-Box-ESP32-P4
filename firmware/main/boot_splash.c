/*
 * boot_splash.c — render the Pilot Kit boot logo.
 *
 * The 128×128 RGB565 logo blob is embedded into the firmware by
 * CMakeLists.txt:
 *
 *     idf_component_register(
 *         ...
 *         EMBED_FILES "pk_logo.rgb565"
 *     )
 *
 * which produces two linker-defined symbols giving the start/end of
 * the data in flash. We declare them via `asm()` labels (no C
 * preprocessor name mangling) and read them as raw bytes; cast to
 * uint16_t* when blitting since the binary is pre-packed in little-
 * endian RGB565 by tools/png_to_rgb565.py.
 *
 * Static rendering only — once the PFD task starts spinning the next
 * frame will overwrite us. No animation needed.
 */

#include "boot_splash.h"

#include <string.h>

#include "esp_app_desc.h"

#include "display.h"
#include "pfd_font.h"

extern const uint8_t pk_logo_start[] asm("_binary_pk_logo_rgb565_start");
extern const uint8_t pk_logo_end[]   asm("_binary_pk_logo_rgb565_end");

#define PK_LOGO_W 128
#define PK_LOGO_H 128

/* Layout */
#define BG_COLOR             pk_rgb565( 12,  12,  16)
#define TITLE_COLOR          pk_rgb565(240, 240, 240)
#define VERSION_COLOR        pk_rgb565(140, 140, 160)

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

static void blit_logo(uint16_t *fb, int dst_x, int dst_y)
{
    const uint16_t *src = (const uint16_t *)pk_logo_start;
    /* The embedded blob is exactly PK_LOGO_W × PK_LOGO_H × 2 bytes,
     * pre-packed in the same little-endian RGB565 format the panel
     * uses on the wire, so memcpy of one source row into the
     * destination row is exact. */
    for (int y = 0; y < PK_LOGO_H; ++y) {
        int fy = dst_y + y;
        if (fy < 0 || fy >= PK_DISPLAY_H) continue;
        memcpy(fb + fy * PK_DISPLAY_W + dst_x,
               src + y * PK_LOGO_W,
               PK_LOGO_W * sizeof(uint16_t));
    }
}

void pk_boot_splash_render(uint16_t *fb)
{
    /* Solid background */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, BG_COLOR);

    /* Logo: horizontally centred, vertically biased up to leave room
     * for the title + version line below it. */
    int logo_x = (PK_DISPLAY_W - PK_LOGO_W) / 2;       /* (240-128)/2 = 56 */
    int logo_y = 40;
    blit_logo(fb, logo_x, logo_y);

    /* Title (scale-2) under the logo */
    const char *title = "PILOT KIT BOX";
    int title_w = (int)strlen(title) * PK_FONT_CELL_W(2);
    int title_x = (PK_DISPLAY_W - title_w) / 2;
    int title_y = logo_y + PK_LOGO_H + 18;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 title_x, title_y, title, TITLE_COLOR, 2);

    /* Version line — pulls the git short-hash baked into the app
     * descriptor at build time. esp_app_desc_t.version is a fixed
     * 32-byte field, so reserve enough room here and bound the
     * format specifier to keep gcc -Wformat-truncation happy. */
    const esp_app_desc_t *app = esp_app_get_description();
    char ver[64];
    if (app) {
        snprintf(ver, sizeof(ver), "Booting %.32s ...", app->version);
    } else {
        snprintf(ver, sizeof(ver), "Booting ...");
    }
    int ver_w = (int)strlen(ver) * PK_FONT_CELL_W(1);
    int ver_x = (PK_DISPLAY_W - ver_w) / 2;
    int ver_y = title_y + PK_FONT_CELL_H(2) + 6;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ver_x, ver_y, ver, VERSION_COLOR, 1);

    /* Suppress unused-symbol warning for pk_logo_end — keeps it in
     * scope so future code can compute the blob size if needed. */
    (void)pk_logo_end;
}
