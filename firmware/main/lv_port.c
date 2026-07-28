/*
 * lv_port.c — LVGL 在固件侧的移植层。
 *
 * 与模拟器的对应关系
 * ------------------
 * 这是 sim/lv_backend.c 的真机版：同一套「PFD 画进全屏 lv_canvas，控件叠在
 * 其上」的结构，差别只在 flush 的去向——模拟器丢给 SDL 纹理，真机推给面板。
 * 两边的图层关系必须一致，否则模拟器验过的布局在真机上不成立。
 *
 * 字节序
 * ------
 * pk_rgb565() 在源头就把颜色转成大端（面板线序），而 LVGL 默认按主机序解释
 * RGB565。v9.5 原生支持 LV_COLOR_FORMAT_RGB565_SWAPPED 并带专门的混合实现，
 * 因此显式声明该格式即可，不必去动 pk_rgb565 的既有约定。
 *
 * 显示路径
 * --------
 * display.c 已提供 800×480 逻辑 framebuffer，并在提交时用 PPA 旋转到
 * ST7701 的 480×800 DPI 双缓冲。这里继续使用 DIRECT 模式，不做第二次旋转。
 */

#include "lv_port.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "display.h"

static const char *TAG = "lv_port";

/* 与 pk_rgb565() 的大端约定一致。 */
#define PK_LV_CF   LV_COLOR_FORMAT_RGB565_SWAPPED

static lv_display_t *s_disp;
static lv_obj_t     *s_canvas;
static uint16_t     *s_canvas_px;

/*
 * DIRECT 模式下 LVGL 已经把结果画进了 framebuffer，这里只需把整帧推给面板。
 *
 * 注意 pk_display_flush_full() 是同步的：它返回时 VSYNC 已切换，所以可以直接
 * 应答 LVGL。将来换成异步分块推送时，flush_ready 必须挪到传输完成回调里，
 * 否则 LVGL 会在 DMA 还在读的缓冲上继续画。
 */
/* 诊断计数器：flush（PPA 旋转 + 等 VSYNC）累计耗时与次数。 */
static int64_t s_flush_us;
static uint32_t s_flush_cnt;

void pk_lv_port_flush_stats(int64_t *us, uint32_t *cnt)
{
    if (us) *us = s_flush_us;
    if (cnt) *cnt = s_flush_cnt;
    s_flush_us = 0; s_flush_cnt = 0;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area; (void)px_map;
    int64_t t0 = esp_timer_get_time();
    pk_display_flush_full();
    s_flush_us += esp_timer_get_time() - t0;
    s_flush_cnt++;
    lv_display_flush_ready(disp);
}

esp_err_t pk_lv_port_init(void)
{
    uint16_t *fb = pk_display_framebuffer();
    if (fb == NULL) {
        ESP_LOGE(TAG, "no framebuffer");
        return ESP_ERR_INVALID_STATE;
    }

    lv_init();

    s_disp = lv_display_create(PK_DISPLAY_W, PK_DISPLAY_H);
    if (s_disp == NULL) return ESP_ERR_NO_MEM;

    lv_display_set_color_format(s_disp, PK_LV_CF);
    lv_display_set_flush_cb(s_disp, flush_cb);
    /* DIRECT + 直接用固件自己的 framebuffer：省掉一整块 800×480×2 的中间
     * 缓冲，也让 LVGL 与既有绘制代码写同一块内存，不存在两份画面。 */
    lv_display_set_buffers(s_disp, fb, NULL,
                           (uint32_t)PK_DISPLAY_FB_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    ESP_LOGI(TAG, "LVGL %d.%d.%d up, %dx%d",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             PK_DISPLAY_W, PK_DISPLAY_H);
    return ESP_OK;
}

uint16_t *pk_lv_port_canvas_px(void)
{
    if (s_canvas_px != NULL) return s_canvas_px;

    /* 单独一块，不复用 display 的 framebuffer：合成时 LVGL 要把 canvas 混到
     * display 缓冲上，同一块内存会源目重叠。放 PSRAM——800×480×2 = 768 KB，
     * 内部 RAM 放不下，而这块每帧顺序写、不参与 DMA，PSRAM 带宽足够。 */
    s_canvas_px = heap_caps_malloc(PK_DISPLAY_FB_BYTES, MALLOC_CAP_SPIRAM);
    if (s_canvas_px == NULL) {
        ESP_LOGE(TAG, "canvas buffer alloc failed (%d bytes)", PK_DISPLAY_FB_BYTES);
        return NULL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_px, PK_DISPLAY_W, PK_DISPLAY_H, PK_LV_CF);
    lv_obj_set_pos(s_canvas, 0, 0);
    /* canvas 是最底层：FAB / dock / Toast 都叠在它上面。 */
    lv_obj_move_background(s_canvas);
    return s_canvas_px;
}

/*
 * 为什么这里必须是「整块 canvas 标脏」，而不是只标控件
 * -----------------------------------------------------
 * 试过让 PFD 直接画进 display 缓冲、屏幕背景设 LV_OPA_TRANSP、每帧只把
 * FAB/dock 标脏，以此省掉 canvas → display 那次 768 KB 的 blit（实测 33 ms）。
 * 真机上的结果是 FAB 带一圈白底、且 FAB 与 dock 持续频闪。
 *
 * 两个原因，都是硬伤：
 *
 *   1. RGB565 没有 alpha 通道。DIRECT 模式下 LVGL 重绘控件脏区时需要一个
 *      不透明的底，屏幕透明就拿不到，圆角与半透明处填出未定义的颜色。
 *
 *   2. PFD 每帧覆盖整屏，而 LVGL 有自己的刷新周期（LV_DEF_REFR_PERIOD），
 *      并非每次 lv_timer_handler 都真的重绘。于是有的帧控件画上去、有的帧
 *      被 PFD 盖掉——正是频闪。
 *
 * 要省掉这次 blit，得让控件与 PFD 在同一次遍历里合成（例如放弃 LVGL 图层、
 * 自己叠控件像素），不是改标脏范围能解决的。
 */
void pk_lv_port_invalidate(void)
{
    /* canvas 的像素是绕过 LVGL 直接写的，必须显式告知已变脏，否则 LVGL
     * 认为无需重绘，屏幕停在上一帧。 */
    if (s_canvas) lv_obj_invalidate(s_canvas);
}

void pk_lv_port_tick(uint32_t elapsed_ms)
{
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
}
