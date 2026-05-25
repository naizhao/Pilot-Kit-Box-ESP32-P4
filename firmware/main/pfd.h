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

#include "esp_err.h"

/*
 * Spawn the PFD render task. Must be called after pk_display_init()
 * has succeeded (otherwise we have no framebuffer to draw into).
 * pk_imu_init() failing is fine — the PFD just draws a level horizon
 * until an IMU sample arrives.
 */
esp_err_t pk_pfd_start(void);
