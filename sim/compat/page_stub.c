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
