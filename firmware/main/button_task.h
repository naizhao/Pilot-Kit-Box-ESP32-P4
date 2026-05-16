/*
 * button_task.h — Phase 4b config-UI tact-button polling task.
 *
 * Owns BTN1 (GPIO 26 on the Waveshare ESP32-P4-WIFI6 right header).
 * The button is wired as an active-low momentary switch between
 * GPIO 26 and GND; the ESP32-P4 internal pull-up is enabled by this
 * driver, so no external pull is required.
 *
 * Press semantics — mirrors a real PFD's "DG sync" + "erect & cage"
 * combo on a single switch:
 *
 *   Short press (release within  < 3 s)  →  pk_imu_tare_yaw()
 *                                            heading reset to 0°
 *
 *   Long  press (held         ≥ 3 s)     →  pk_imu_full_reorient()
 *                                            tare all 3 axes,
 *                                            persist to BNO085 flash,
 *                                            save dynamic calibration
 *
 * Polled at 50 Hz (20 ms tick); 40 ms debounce. Implementation in
 * button_task.c is a small 4-state machine.
 */
#pragma once

#include "esp_err.h"

/*
 * Configure GPIO 26 as input + pull-up, then spawn the button polling
 * task on CPU 0 at priority 3 (well below IMU @ 5 and PFD @ 4 so it
 * never preempts the render path). Call once after pk_imu_init()
 * succeeds — calling this without a live IMU is harmless but every
 * press will just return ESP_ERR_INVALID_STATE.
 */
esp_err_t pk_button_init(void);
