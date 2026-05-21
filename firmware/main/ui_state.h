/*
 * ui_state.h — single source of truth for which view the LCD is showing.
 *
 * Top-level views in Phase 4:
 *   PK_UI_MODE_PFD        — primary flight display (default at boot)
 *   PK_UI_MODE_ADSB_LIST  — scrollable list of currently-tracked ADS-B
 *                            aircraft + detail pane of the selected row
 *   PK_UI_MODE_ABOUT      — project name, version, build time, hardware
 *                            summary, calibration quality
 *   PK_UI_MODE_CAL_WIZARD — figure-8 calibration overlay (auto-entered
 *                            when the BNO085 magnetometer fusion has
 *                            been stuck at acc=0 for too long; auto-
 *                            exits when acc reaches 2 and stays there)
 *
 * The render task (pfd_task in firmware/main/pfd.c) checks
 * pk_ui_get_mode() once per frame and dispatches to the correct
 * renderer. Mode transitions happen in O(1) — just a flag flip — so
 * the next frame already shows the new view.
 *
 * MODE short-press cycles through the three USER-visible modes:
 *     PFD → ADSB_LIST → ABOUT → PFD …
 * CAL_WIZARD is not in the cycle — it's auto-entered/auto-exited
 * based on IMU calibration state (see pk_ui_cal_wizard_tick below)
 * and the user can also dismiss it manually by pressing MODE.
 *
 * Threading
 * ---------
 * Multiple producers/consumers (button task + render task + IMU
 * task + future BLE task) read and update mode + selection.
 * Everything goes through a small mutex; calls are short and
 * non-blocking so it's never meaningfully contended.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PK_UI_MODE_PFD = 0,
    PK_UI_MODE_ADSB_LIST,
    PK_UI_MODE_ABOUT,
    PK_UI_MODE_CAL_WIZARD,
} pk_ui_mode_t;

/* Initialise the UI state mutex. Must be called once before the
 * button task or the render task starts. */
esp_err_t pk_ui_init(void);

/* Get the current top-level view. Safe from any task. */
pk_ui_mode_t pk_ui_get_mode(void);

/* Cycle the user-visible mode forward:
 *     PFD → ADSB_LIST → ABOUT → PFD …
 * Selection is preserved across toggles (re-entering list mode
 * lands on the same highlight). If the current mode is
 * CAL_WIZARD, the cycle returns to PFD and dismisses the wizard. */
void pk_ui_toggle_mode(void);

/*
 * Drive the calibration-wizard auto-trigger state machine. Called
 * once per IMU sample (~100 Hz) from imu_task; pass the latest
 * accuracy field (0..3) and whether the sample was valid.
 *
 * The wizard auto-enters when accuracy stays at 0 for more than
 * UI_CAL_WIZARD_ENTER_MS, and auto-exits when accuracy stays at
 * ≥ UI_CAL_WIZARD_EXIT_ACCURACY for more than UI_CAL_WIZARD_EXIT_MS.
 * The user can override the wizard at any time with MODE press
 * (returns to PFD).
 */
void pk_ui_cal_wizard_tick(bool valid, uint8_t accuracy);

/* Read the current target accuracy bar used by the wizard renderer.
 * Returns 0..3. Stable across reads — updated only when imu_task
 * calls pk_ui_cal_wizard_tick(). */
uint8_t pk_ui_cal_wizard_last_accuracy(void);

/* Move the list selection by `delta` rows (negative = up, positive =
 * down). Clamping to the actual aircraft count happens in the list
 * renderer because only it knows the current count. Saturates the
 * internal "intent" cursor in the range [0, 999] so repeated UP/DOWN
 * presses don't overflow. */
void pk_ui_list_scroll(int delta);

/* Read the current (un-clamped) list cursor. The list renderer should
 * clamp this against the live aircraft snapshot count and call
 * pk_ui_list_set_index() to persist the clamp. */
int pk_ui_list_get_index(void);
void pk_ui_list_set_index(int idx);

/*
 * Runtime own-ship binding — which ADS-B aircraft drives the PFD's
 * ALT / VS / GS readouts. Volatile: lives in RAM only, cleared on
 * reboot (no NVS write). The PFD reads via pk_ui_get_own_icao(); when
 * the runtime value has never been set (or is 0), the getter falls
 * back to the compile-time CONFIG_PK_OWN_ICAO default.
 *
 * Set this from the TARE short-press handler when the user is in
 * PK_UI_MODE_ADSB_LIST — that's the gesture the kit exposes for
 * "this highlighted aircraft is me". Re-pressing TARE on another
 * aircraft replaces the binding; a power cycle wipes it.
 */
void     pk_ui_set_own_icao(uint32_t icao24);
uint32_t pk_ui_get_own_icao(void);
