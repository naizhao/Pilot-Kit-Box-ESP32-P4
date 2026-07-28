/*
 * page_stub.c — 让「关于 / 设置 / 诊断」这类整页渲染能在模拟器里跑起来。
 *
 * 这些页面本身只是把运行时状态排版成像素，排版逻辑与数据来源无关。固件侧
 * 那些数据来自 NVS、芯片寄存器、各任务的实时状态，PC 上都不存在——于是这里
 * 给一组**固定但真实感的**值，让版面按真实字符串长度铺开。
 *
 * 与 mock_runtime.c 的分工：那边桩的是 PFD 要的飞行数据（姿态/交通/气压），
 * 这边桩的是整页视图要的设备信息。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = {
        .project_name = "pilot_kit_box",
        .version      = "0.9.3-4.3in",
        .date         = "Jul 28 2026",
        .time         = "21:40:00",
    };
    return &d;
}

void esp_chip_info(esp_chip_info_t *out)
{
    if (!out) return;
    /* 与手上这块 Waveshare 板一致：ESP32-P4 v1.3（revision 编码 = 主*100+次）。 */
    out->model = 0; out->features = 0; out->cores = 2; out->revision = 103;
}

/* ── ui_state 的只读部分 ────────────────────────────────────────
 * 滚动位置在模拟器里恒为 0：截图要的是「页面顶端长什么样」。 */
int     pk_ui_about_scroll_y(void)          { return 0; }
int     pk_ui_diag_scroll_y(void)           { return 0; }
uint8_t pk_ui_cal_wizard_last_accuracy(void){ return 3; }   /* 3 = 已校准 */

/* ── logo ───────────────────────────────────────────────────────
 * 固件里这张图由 EMBED_FILES 链进 .rodata，模拟器没有那套机制，直接从源文件
 * 读同一份数据——保证两边显示的是同一张图，而不是各画各的占位。 */
#define LOGO_W 160
#define LOGO_H 160

const uint16_t *pk_logo_bitmap(int *w, int *h)
{
    static uint16_t px[LOGO_W * LOGO_H];
    static int loaded;          /* 0 未试过，1 成功，-1 失败 */

    if (w) *w = LOGO_W;
    if (h) *h = LOGO_H;
    if (loaded) return loaded > 0 ? px : NULL;

    /* 相对源码树定位：模拟器总是从仓库里跑起来的。 */
    const char *paths[] = {
        "firmware/main/pk_logo.rgb565",
        "../firmware/main/pk_logo.rgb565",
        "../../firmware/main/pk_logo.rgb565",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        size_t n = fread(px, 1, sizeof(px), f);
        fclose(f);
        if (n == sizeof(px)) { loaded = 1; return px; }
    }
    fprintf(stderr, "page_stub: 找不到 pk_logo.rgb565，关于页将不画 logo\n");
    loaded = -1;
    return NULL;
}
