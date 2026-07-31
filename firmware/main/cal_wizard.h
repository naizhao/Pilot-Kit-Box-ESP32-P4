/*
 * cal_wizard.h — magnetometer-calibration figure-8 overlay screen.
 *
 * Drawn whenever pk_ui_get_mode() == PK_UI_MODE_CAL_WIZARD. The
 * wizard auto-enters from PFD when the BNO085 has reported acc=0 for
 * UI_CAL_WIZARD_ENTER_MS milliseconds and auto-exits back to PFD
 * when acc has been ≥ 2 for UI_CAL_WIZARD_EXIT_MS. The user can also
 * dismiss it with the on-screen “Later” action.
 *
 * UX
 * --
 * - Top: "COMPASS CALIBRATION" header in cyan
 * - Middle: animated dot tracing a Bernoulli lemniscate (the iconic
 *   figure-8 shape) so the user knows what motion to do
 * - Lower middle: textual instructions
 * - Bottom: acc quality bar (0..3) updated live from
 *   pk_ui_cal_wizard_last_accuracy()
 */
#pragma once

#include <stdint.h>

/* Renders one frame of the calibration wizard into the logical 800×480
 * framebuffer. Animation phase is derived from esp_timer_get_time()
 * so consecutive calls produce smooth motion without internal state. */
void pk_cal_wizard_render(uint16_t *fb);
