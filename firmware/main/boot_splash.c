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
 *   y = 186   ║       Booting abc1234 ...          ║  scale-1 git hash
 *   y = 198   ║     Built May 21 2026 12:34:56     ║  scale-1 build stamp
 *   y = 210   ║         ESP-IDF v6.0.1             ║  scale-1 IDF version
 *   y = 228   ║        (C) 2026 Pilot Kit          ║  scale-1 copyright footer
 *   y = 240   ╚═══════════════════════════════════╝
 *
 * The 160×160 source blob is downsampled 2:1 (nearest-neighbour) to
 * 80×80 on screen — the logo file in flash stays unchanged, only the
 * blit reads every other source pixel. Static rendering only — once
 * the PFD task starts spinning the next frame will overwrite us. No
 * animation needed.
 */

#include "boot_splash.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_idf_version.h"

#include "display.h"
#include "logo_blob.h"
#include "pfd_aa_text.h"
#include "pfd_aa_font.h"

extern const uint8_t pk_logo_start[] asm("_binary_pk_logo_rgb565_start");
extern const uint8_t pk_logo_end[]   asm("_binary_pk_logo_rgb565_end");

#define PK_LOGO_SRC_W       160                /* flash 里的源图尺寸 */
#define PK_LOGO_SRC_H       160

/* ── 布局（800×480）────────────────────────────────────────────
 *
 * 整块内容垂直居中。图标比 2.4″ 那版大一倍有余（80 → 176）——屏幕物理尺寸
 * 只有信用卡大小，但像素多了 5 倍，沿用旧尺寸会让开机画面显得空旷而寒酸。
 *
 * 图标画法与「关于」页一致：完整图案 + 内边距 + 圆角，不裁源图。旧版用
 * LOGO_SRC_CROP=24 裁掉 SVG 空白好让图案占满 80×80 的小卡片，在这个尺寸上
 * 会把六边形外框整个切掉。
 */
#define CARD_SIZE           176
#define CARD_RADIUS         (CARD_SIZE * 22 / 100)   /* app icon 惯例 */
#define CARD_PAD            16                        /* 图案与卡片边的留白 */
#define CARD_X              ((PK_DISPLAY_W - CARD_SIZE) / 2)
#define CARD_Y              96

#define TITLE_GAP           36                        /* 卡片 → 标题 */
#define TITLE_Y             (CARD_Y + CARD_SIZE + TITLE_GAP)
#define INFO_GAP            18                        /* 标题 → 信息行 */
#define INFO_LINE_GAP       6
#define INFO_Y              (TITLE_Y + PK_AA_M_H + INFO_GAP)

/* ── 配色 ─────────────────────────────────────────────────────── */
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

/* 该像素是否落在圆角矩形内。只在四个角上做圆检测。 */
static bool in_rounded(int col, int row, int size, int r)
{
    int dx = 0, dy = 0;
    if (col < r)              dx = r - col;
    else if (col >= size - r) dx = col - (size - r - 1);
    if (row < r)              dy = r - row;
    else if (row >= size - r) dy = row - (size - r - 1);
    if (dx == 0 || dy == 0) return true;
    return dx * dx + dy * dy <= r * r;
}

/*
 * 画图标：白色圆角底 + 居中的完整图案。与 about_page.c 的 draw_logo() 同构，
 * 两处显示的是同一张图、同一种取景，改一处要记得改另一处。
 */
static void draw_icon(uint16_t *fb, int x, int y, int size)
{
    int sw = 0, sh = 0;
    const uint16_t *src = pk_logo_bitmap(&sw, &sh);
    const int inner = size - 2 * CARD_PAD;

    for (int row = 0; row < size; ++row) {
        const int yy = y + row;
        if (yy < 0 || yy >= PK_DISPLAY_H) continue;
        uint16_t *dst = fb + yy * PK_DISPLAY_W;

        for (int col = 0; col < size; ++col) {
            const int xx = x + col;
            if (xx < 0 || xx >= PK_DISPLAY_W) continue;
            if (!in_rounded(col, row, size, CARD_RADIUS)) continue;

            const int ix = col - CARD_PAD;
            const int iy = row - CARD_PAD;
            if (src == NULL || ix < 0 || iy < 0 || ix >= inner || iy >= inner) {
                dst[xx] = CARD_COLOR;
                continue;
            }
            const uint16_t v = src[(iy * sh / inner) * sw + (ix * sw / inner)];
            /* blob 与 framebuffer 的字节序约定不同，见 display.h 的 pk_rgb565()。 */
            dst[xx] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

void pk_boot_splash_render(uint16_t *fb)
{
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, BG_COLOR);

    draw_icon(fb, CARD_X, CARD_Y, CARD_SIZE);

    /* 产品名。用 M 档——它是这一屏唯一的主角，S 档在 93 mm 宽的屏上撑不住。 */
    {
        static const char kTitle[] = "PILOT KIT BOX";
        const int w = (int)(sizeof(kTitle) - 1) * pk_aa_cell_w(PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, TITLE_Y, kTitle, TITLE_COLOR,
                   PK_AA_M);
    }

    /*
     * 三行信息，全部取自 app descriptor 与 IDF 编译期宏——开机画面与二进制
     * 天然一致，不需要人工同步。
     */
    const esp_app_desc_t *app = esp_app_get_description();
    char line[64];
    int y = INFO_Y;

    if (app) snprintf(line, sizeof(line), "Booting %.32s ...", app->version);
    else     snprintf(line, sizeof(line), "Booting ...");
    {
        const int w = (int)strlen(line) * pk_aa_cell_w(PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, y, line, VERSION_COLOR, PK_AA_S);
        y += PK_AA_S_H + INFO_LINE_GAP;
    }

    if (app) snprintf(line, sizeof(line), "Built %.16s %.8s", app->date, app->time);
    else     snprintf(line, sizeof(line), "Built %s %s", __DATE__, __TIME__);
    {
        const int w = (int)strlen(line) * pk_aa_cell_w(PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, y, line, VERSION_COLOR, PK_AA_S);
        y += PK_AA_S_H + INFO_LINE_GAP;
    }

    snprintf(line, sizeof(line), "ESP-IDF v%d.%d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
    {
        const int w = (int)strlen(line) * pk_aa_cell_w(PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, y, line, VERSION_COLOR, PK_AA_S);
    }
}
