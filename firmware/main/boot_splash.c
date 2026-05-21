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
 * Layout (320 × 240 landscape):
 *
 *   y =   0   ╔═══════════════════════════════════╗
 *             ║                                    ║
 *             ║                                    ║
 *   y =  50   ║              ╭────────╮            ║  ← rounded white card,
 *             ║              │ [LOGO] │            ║    100×100, r=8
 *             ║              │ 80×80  │            ║
 *             ║              ╰────────╯            ║
 *   y = 162   ║         PILOT KIT BOX              ║  scale-2 title
 *   y = 186   ║         Booting abc1234 ...        ║  scale-1 version
 *   y = 240   ╚═══════════════════════════════════╝
 *
 * The 160×160 source blob is downsampled 2:1 (nearest-neighbour) to
 * 80×80 on screen — the logo file in flash stays unchanged, only the
 * blit reads every other source pixel. Static rendering only — once
 * the PFD task starts spinning the next frame will overwrite us. No
 * animation needed.
 */

#include "boot_splash.h"

#include <string.h>

#include "esp_app_desc.h"

#include "display.h"
#include "pfd_font.h"

extern const uint8_t pk_logo_start[] asm("_binary_pk_logo_rgb565_start");
extern const uint8_t pk_logo_end[]   asm("_binary_pk_logo_rgb565_end");

#define PK_LOGO_W           160                /* source blob in flash */
#define PK_LOGO_H           160
#define LOGO_DISP_W         (PK_LOGO_W / 2)    /* 80 — displayed size */
#define LOGO_DISP_H         (PK_LOGO_H / 2)

/* The SVG that produced pk_logo.rgb565 bakes in whitespace around the
 * actual mark — without compensation, the displayed logo sits well
 * inside the card with too much margin. We crop LOGO_SRC_CROP source
 * pixels off each side so only the central content area gets sampled
 * into the 80×80 display window. With crop=24, effective source area
 * is 112×112 → effective zoom = 160/112 ≈ 1.43× vs straight 2:1
 * decimation, i.e. the logo content appears ~43% bigger on the card.
 * Bump if even more zoom is needed (cap at ~36 before the content
 * itself starts getting clipped). */
#define LOGO_SRC_CROP       24
#define LOGO_SRC_USED_W     (PK_LOGO_W - 2 * LOGO_SRC_CROP)
#define LOGO_SRC_USED_H     (PK_LOGO_H - 2 * LOGO_SRC_CROP)

/* Layout — keep card and logo concentric so the white margin around
 * the logo is even on all sides. Card sized to enclose the 80×80
 * displayed logo with a 10 px white margin on each side; the whole
 * content block (card + title + version) is vertically centered in
 * the 240-tall panel. CARD_TITLE_GAP gives the breathing room between
 * the white card and the title text — the shrunk card freed up
 * vertical space, so we hand it back to this gap instead of leaving
 * it as dead air below the version line. */
#define CARD_W              100
#define CARD_H              100
#define CARD_RADIUS         8
#define CARD_X              ((PK_DISPLAY_W - CARD_W) / 2)        /* 110 */
#define CARD_Y              38
#define LOGO_X              (CARD_X + (CARD_W - LOGO_DISP_W) / 2)/* 120 */
#define LOGO_Y              (CARD_Y + (CARD_H - LOGO_DISP_H) / 2)/* 48  */

#define CARD_TITLE_GAP      24                                    /* was 8 */
#define TITLE_VERSION_GAP   8                                     /* was 6 */

#define TITLE_Y             (CARD_Y + CARD_H + CARD_TITLE_GAP)    /* 162 */
#define VERSION_Y           (TITLE_Y + PK_FONT_CELL_H(2) + TITLE_VERSION_GAP)
                                                                  /* 186 */

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
    /* The embedded blob is PK_LOGO_W × PK_LOGO_H × 2 bytes, pre-packed
     * in the same little-endian RGB565 format the panel uses on the
     * wire. We sample the central LOGO_SRC_USED_{W,H} pixels (skipping
     * LOGO_SRC_CROP px of SVG whitespace on each side) and resample to
     * LOGO_DISP_W × LOGO_DISP_H via nearest-neighbour — gives a tighter
     * zoom into the actual logo content than a straight 2:1 decimation.
     * Step ratio is LOGO_SRC_USED_W / LOGO_DISP_W, kept in integer
     * fixed-point (×LOGO_DISP_W) to avoid floats in the hot path. */
    for (int dy = 0; dy < LOGO_DISP_H; ++dy) {
        int fy = dst_y + dy;
        if (fy < 0 || fy >= PK_DISPLAY_H) continue;
        int src_y = LOGO_SRC_CROP + (dy * LOGO_SRC_USED_H) / LOGO_DISP_H;
        uint16_t       *row_dst = fb + fy * PK_DISPLAY_W + dst_x;
        const uint16_t *row_src = src + src_y * PK_LOGO_W;
        for (int dx = 0; dx < LOGO_DISP_W; ++dx) {
            int src_x = LOGO_SRC_CROP + (dx * LOGO_SRC_USED_W) / LOGO_DISP_W;
            row_dst[dx] = row_src[src_x];
        }
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
