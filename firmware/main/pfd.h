/*
 * pfd.h — Primary Flight Display rendering task.
 *
 * Reads pk_imu_sample_get() at ~30 Hz, redraws into the framebuffer
 * owned by display.c, and flushes the panel via pk_display_flush_full().
 *
 * The current PFD renders attitude, pitch ladder, bank arc, heading /
 * HSI, altitude tape, GS / VS readouts, and ADS-B status indicators.
 * It shares the UI framebuffer with ADS-B LIST, SETTINGS, ABOUT, and
 * the compass-calibration wizard.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Spawn the PFD render task. Must be called after pk_display_init()
 * has succeeded (otherwise we have no framebuffer to draw into).
 * pk_imu_init() failing is fine — the PFD just draws a level horizon
 * until an IMU sample arrives.
 */
esp_err_t pk_pfd_start(void);

/* 只有 LVGL、canvas、触摸/nav 控件均初始化完成后才为 true。任务创建成功
 * 不等于 UI 已可显示，SW3 长按关机用它作可见提示的硬闸门。 */
bool pk_pfd_ready(void);
