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
