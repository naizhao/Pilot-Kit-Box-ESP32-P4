/*
 * touch_gt911.h — GT911 触摸接入 LVGL。
 */
#pragma once

#include "esp_err.h"

/*
 * 探测 GT911、初始化并注册为 LVGL 指针设备。须在 lv_init() 之后、且 I²C0
 * 总线已由 imu_task 建好之后调用。
 *
 * 触摸缺席不该拖垮显示：探不到芯片时返回 ESP_ERR_NOT_FOUND 并记一条 error，
 * 调用方继续跑即可——PFD 照画，只是点不动。
 */
esp_err_t pk_touch_init(void);

/*
 * 「上一次 pk_touch_init() 没成功，而 I²C0 总线刚被救回来一轮」时重试一次。
 * 每帧调开销就是一个指针比较（成功过就立刻返回），可以放在渲染循环里。
 *
 * 必须从**跑 LVGL 的那个任务**调用（内部可能 lv_indev_create()）。
 * 背景见 pk_i2c0_recover.h：2026-08-03 真机上总线在 GT911 初始化中途塌了，
 * 日志里只有「GT911 found」没有「GT911 ready」，整台机器这次开机点不动。
 * 总线恢复能把姿态和高度救回来，触摸却要靠这里补一刀。
 */
void pk_touch_retry_after_bus_recovery(void);
