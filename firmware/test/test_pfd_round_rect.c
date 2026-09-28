/* 圆角矩形像素级回归：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/test/host_stubs \
 *      -I firmware/main -o /tmp/test_pfd_round_rect \
 *      firmware/test/test_pfd_round_rect.c firmware/main/pfd_draw.c -lm \
 *      && /tmp/test_pfd_round_rect
 */
#include "display.h"
#include "pfd_draw.h"

#include <stdio.h>
#include <string.h>

static uint16_t s_fb[PK_DISPLAY_W * PK_DISPLAY_H];
static int s_fail;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); s_fail++; } } while (0)

static void test_high_contrast_corner_has_smooth_coverage_levels(void)
{
    const uint16_t bg = pk_rgb565(10, 15, 28);
    const uint16_t fg = pk_rgb565(46, 109, 240);
    const int x0 = 100, y0 = 80, x1 = 204, y1 = 132, radius = 10;
    for (size_t i = 0; i < sizeof(s_fb) / sizeof(s_fb[0]); ++i) s_fb[i] = bg;

    pk_pfd_fill_round_rect(s_fb, x0, y0, x1, y1, radius, fg);

    uint16_t partial[32] = {0};
    int partial_count = 0;
    for (int y = y0; y < y0 + radius; ++y) {
        for (int x = x0; x < x0 + radius; ++x) {
            const uint16_t pixel = s_fb[y * PK_DISPLAY_W + x];
            if (pixel == bg || pixel == fg) continue;
            int seen = 0;
            for (int i = 0; i < partial_count; ++i) seen |= partial[i] == pixel;
            if (!seen && partial_count < (int)(sizeof(partial) / sizeof(partial[0])))
                partial[partial_count++] = pixel;
        }
    }

    /* The old integer-sqrt transition produced one half-alpha shade only,
     * which remains visibly stair-stepped on blue/orange button fills. */
    CHECK(partial_count >= 4,
          "rounded corner has only %d partial coverage level(s)\n",
          partial_count);
    CHECK(s_fb[y0 * PK_DISPLAY_W + x0] == bg,
          "outside corner pixel was filled\n");
    CHECK(s_fb[y0 * PK_DISPLAY_W + x0 + radius] == fg,
          "straight top edge is not fully filled\n");
}

static void test_all_four_corners_are_pixel_symmetric(void)
{
    const uint16_t bg = pk_rgb565(10, 15, 28);
    const uint16_t fg = pk_rgb565(255, 180, 64);
    const int x0 = 40, y0 = 60, x1 = 144, y1 = 112, radius = 10;
    for (size_t i = 0; i < sizeof(s_fb) / sizeof(s_fb[0]); ++i) s_fb[i] = bg;

    pk_pfd_fill_round_rect(s_fb, x0, y0, x1, y1, radius, fg);
    for (int dy = 0; dy < radius; ++dy) {
        for (int dx = 0; dx < radius; ++dx) {
            const uint16_t tl = s_fb[(y0 + dy) * PK_DISPLAY_W + x0 + dx];
            const uint16_t tr = s_fb[(y0 + dy) * PK_DISPLAY_W + (x1 - 1 - dx)];
            const uint16_t bl = s_fb[(y1 - 1 - dy) * PK_DISPLAY_W + x0 + dx];
            const uint16_t br = s_fb[(y1 - 1 - dy) * PK_DISPLAY_W + (x1 - 1 - dx)];
            CHECK(tl == tr && tl == bl && tl == br,
                  "corner asymmetry at dx=%d dy=%d\n", dx, dy);
        }
    }
}

int main(void)
{
    test_high_contrast_corner_has_smooth_coverage_levels();
    test_all_four_corners_are_pixel_symmetric();
    puts(s_fail ? "FAILED" : "OK");
    return s_fail ? 1 : 0;
}
