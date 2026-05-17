/*
 * boot_splash.c — render the Pilot Kit boot logo on a rounded white card.
 *
 * The 160×160 RGB565 logo blob is embedded into the firmware by
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
 * Layout (240 × 320 portrait):
 *
 *   y =   0   ╔═══════════════════════════╗
 *             ║          (BG_COLOR)        ║
 *   y =  20   ║    ╭───────────────────╮   ║  ← rounded white card,
 *             ║    │                   │   ║    192×192, r=14
 *             ║    │      [LOGO]       │   ║
 *             ║    │     160×160       │   ║
 *             ║    │                   │   ║
 *             ║    ╰───────────────────╯   ║
 *   y = 232   ║      PILOT KIT BOX         ║  scale-2 title
 *             ║      Booting abc1234 ...   ║  scale-1 version
 *   y = 320   ╚═══════════════════════════╝
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

#define PK_LOGO_W 160
#define PK_LOGO_H 160

/* Layout — keep card and logo concentric so the white margin around
 * the logo is even on all sides. */
#define CARD_W              192
#define CARD_H              192
#define CARD_RADIUS         14
#define CARD_X              ((PK_DISPLAY_W - CARD_W) / 2)   /* 24 */
#define CARD_Y              20
#define LOGO_X              (CARD_X + (CARD_W - PK_LOGO_W) / 2)   /* 40 */
#define LOGO_Y              (CARD_Y + (CARD_H - PK_LOGO_H) / 2)   /* 36 */

#define TITLE_Y             (CARD_Y + CARD_H + 14)          /* 226 */
#define VERSION_Y           (TITLE_Y + PK_FONT_CELL_H(2) + 6)

/* Palette */
#define BG_COLOR             pk_rgb565( 12,  12,  16)
#define CARD_COLOR           pk_rgb565(255, 255, 255)
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

static inline void put_pixel(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= PK_DISPLAY_W || y < 0 || y >= PK_DISPLAY_H) return;
    fb[y * PK_DISPLAY_W + x] = c;
}

/* Filled rounded rectangle. Drawn as three rects (left strip, middle
 * body, right strip) for the straight portion + 4 quarter-circle
 * arcs at the corners. r should be < min(w,h)/2; we don't bother
 * validating since callers are file-local. */
static void fill_rounded_rect(uint16_t *fb,
                              int x0, int y0, int x1, int y1,
                              int r, uint16_t c)
{
    /* Body: full-width strip in the vertical centre. */
    fill_rect(fb, x0,     y0 + r, x1,     y1 - r, c);
    /* Top edge strip (between the two top corners). */
    fill_rect(fb, x0 + r, y0,     x1 - r, y0 + r, c);
    /* Bottom edge strip. */
    fill_rect(fb, x0 + r, y1 - r, x1 - r, y1,     c);

    /* Four corners — quarter-circles via simple distance test. r is
     * small (≤16 in practice), so the 4·r² inner-loop iteration count
     * is trivial. */
    const int r2 = r * r;
    for (int dy = 0; dy < r; ++dy) {
        for (int dx = 0; dx < r; ++dx) {
            int d = dx * dx + dy * dy;
            if (d <= r2) {
                /* top-left: pivot at (x0+r, y0+r), corner is r×r region
                 * at (x0..x0+r-1, y0..y0+r-1); the in-arc point is
                 * (x0+r-1-dx, y0+r-1-dy). */
                put_pixel(fb, x0 + r - 1 - dx, y0 + r - 1 - dy, c);
                put_pixel(fb, x1 - r + dx,     y0 + r - 1 - dy, c);
                put_pixel(fb, x0 + r - 1 - dx, y1 - r + dy,     c);
                put_pixel(fb, x1 - r + dx,     y1 - r + dy,     c);
            }
        }
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

    /* Rounded white card behind the logo. The logo PNG is white-
     * background and pre-cropped to its content bbox, so the card
     * just adds visual breathing room and rounded edges so it doesn't
     * look like a hard rectangle pasted on the dark background. */
    fill_rounded_rect(fb, CARD_X, CARD_Y, CARD_X + CARD_W, CARD_Y + CARD_H,
                      CARD_RADIUS, CARD_COLOR);

    /* Logo centred on the card. */
    blit_logo(fb, LOGO_X, LOGO_Y);

    /* Title (scale-2) under the card. */
    const char *title = "PILOT KIT BOX";
    int title_w = (int)strlen(title) * PK_FONT_CELL_W(2);
    int title_x = (PK_DISPLAY_W - title_w) / 2;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 title_x, TITLE_Y, title, TITLE_COLOR, 2);

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
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                 ver_x, VERSION_Y, ver, VERSION_COLOR, 1);

    /* Suppress unused-symbol warning for pk_logo_end — keeps it in
     * scope so future code can compute the blob size if needed. */
    (void)pk_logo_end;
}
