/*
 * lv_backend.c — 见 lv_backend.h。
 */

#include "lv_backend.h"

#include <string.h>

#include "lvgl.h"

#include "display.h"

/* 本项目全程使用大端 RGB565（pk_rgb565 在源头就交换好了）。display 与
 * canvas 都必须声明成这个格式，LVGL 才会走 swapped 的混合路径。 */
#define PK_LV_CF   LV_COLOR_FORMAT_RGB565_SWAPPED

/* LVGL 直接渲染到这里（RENDER_MODE_DIRECT），因此它就是最终画面。 */
static uint16_t s_screen[PK_DISPLAY_W * PK_DISPLAY_H];

/* PFD 的绘制目标 —— 即原来的那块 framebuffer，现在是 canvas 的 buffer。 */
static uint16_t s_canvas_px[PK_DISPLAY_W * PK_DISPLAY_H];

static lv_obj_t *s_canvas;

/*
 * DIRECT 模式下 LVGL 已经把结果画进了我们给的缓冲，flush 无事可做，
 * 只需应答。真机上这里才是把帧推给面板的地方。
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area; (void)px_map;
    lv_display_flush_ready(disp);
}

uint16_t *pk_sim_lv_init(void)
{
    lv_init();

    lv_display_t *disp = lv_display_create(PK_DISPLAY_W, PK_DISPLAY_H);
    lv_display_set_color_format(disp, PK_LV_CF);
    lv_display_set_flush_cb(disp, flush_cb);
    /* DIRECT：LVGL 直接画进 s_screen，省掉一次整屏拷贝。代价是缓冲必须
     * 是全屏大小，而这正是真机的做法（PSRAM 里一整块 framebuffer）。 */
    lv_display_set_buffers(disp, s_screen, NULL, sizeof(s_screen),
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    /* 屏幕底色取黑：PFD 的 canvas 铺满全屏，正常情况下看不到它；
     * 一旦哪里露出黑色，就说明 canvas 没覆盖到，是个有用的信号。 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_px,
                         PK_DISPLAY_W, PK_DISPLAY_H, PK_LV_CF);
    lv_obj_set_pos(s_canvas, 0, 0);
    /* canvas 是最底层：后续的 FAB / dock / Toast 都叠在它上面。 */
    lv_obj_move_background(s_canvas);

    return s_canvas_px;
}

uint16_t *pk_sim_lv_render(uint32_t dt_ms)
{
    /* canvas 的像素是我们绕过 LVGL 直接写的，必须显式告知它已变脏，
     * 否则 LVGL 认为无需重绘，屏幕会停在上一帧。 */
    lv_obj_invalidate(s_canvas);

    lv_tick_inc(dt_ms);
    lv_timer_handler();

    return s_screen;
}
