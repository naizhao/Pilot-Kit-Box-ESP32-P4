/*
 * about_page.h — "About" screen for Pilot Kit Box.
 *
 * Renders project name, firmware version (git short hash from
 * esp_app_get_description()), build time, ESP-IDF version, hardware
 * summary (board, chip rev, IMU model, dongle status), and the
 * current IMU calibration accuracy (with a colour-coded indicator).
 * Reached directly from the touch FAB dock.
 */
#pragma once

#include <stdint.h>

#include <stdbool.h>

/* Renders the About page into the logical 800×480 RGB565 framebuffer. */
void pk_about_page_render(uint16_t *fb);

/* 触摸：与 adsb_list / diag / settings 同一套约定。
 *   touch()  按下，返回 true = 这一下由本页消费（顶栏与右侧 FAB 带放行）
 *   drag()   按住不放的后续帧，做滚动
 *   touch_up()      松手 */
bool pk_about_page_touch(int x, int y);
bool pk_about_page_drag(int x, int y);
void pk_about_page_touch_up(void);
