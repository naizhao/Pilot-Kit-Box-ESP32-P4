/*
 * about_page.h — "About" screen for Pilot Kit Box.
 *
 * Renders project name, firmware version (git short hash from
 * esp_app_get_description()), build time, ESP-IDF version, hardware
 * summary (board, chip rev, IMU model, dongle status), and the
 * current IMU calibration accuracy (with a colour-coded indicator
 * so the user knows when it's safe to TARE long-press).
 *
 * Reached via MODE short-press cycle:
 * PFD → ADSB_LIST → SETTINGS → ABOUT → PFD.
 */
#pragma once

#include <stdint.h>

/* Renders the About page into the 240×320 RGB565 framebuffer. */
void pk_about_page_render(uint16_t *fb);
