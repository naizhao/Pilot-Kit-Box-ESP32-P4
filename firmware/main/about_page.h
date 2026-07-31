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

/* Renders the About page into the logical 800×480 RGB565 framebuffer. */
void pk_about_page_render(uint16_t *fb);
