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
#include "pk_ui_nav.h"

static const char *TAG = "lv_port";

/* 与 pk_rgb565() 的大端约定一致。 */
#define PK_LV_CF   LV_COLOR_FORMAT_RGB565_SWAPPED

static lv_display_t *s_disp;

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
    /*
     * PFD 直接画进 LVGL 的显示缓冲，不再单独开一块 canvas。
     *
     * 原先的结构是：PFD 画 canvas（PSRAM 768 KB）→ LVGL 把整块 canvas blit
     * 到 display 缓冲 → PPA 旋转推屏。中间那次 blit 是 768 KB 读 + 768 KB 写，
     * 实测占每帧 33 ms，而它搬运的内容与源一模一样——纯粹是为了让 PFD 与
     * 控件分层。
     *
     * DIRECT 渲染模式下 LVGL 的缓冲就是 pk_display_framebuffer() 本身，所以
     * 让 PFD 直接画在上面，控件叠着画即可，分层关系不变，只是少搬一趟。
     *
     * 前提是屏幕背景**透明**：否则 LVGL 每次重绘脏区都会先用背景色刷一遍，
     * 把 PFD 刚画好的内容抹掉。透明之后它只画控件本身，控件底下露出的就是
     * PFD 的像素。
     *
     * 控件被 PFD 覆盖的问题由 pk_lv_port_invalidate() 解决：PFD 每帧重画整屏，
     * 所以每帧都要让控件重绘一次。
     */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return pk_display_framebuffer();
}

void pk_lv_port_invalidate(void)
{
    /* 谁该重绘由导航层决定（它知道哪些控件此刻可见），这里不再整屏标脏——
     * invalidate(screen) 会让 LVGL 按 800×480 去遍历，而真正要重画的只有
     * FAB 与 dock 那点面积。见 pk_ui_nav_refresh()。 */
    pk_ui_nav_refresh();
}

void pk_lv_port_tick(uint32_t elapsed_ms)
{
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
}
