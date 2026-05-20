/*
 * button_task.h — Pilot Kit Box tact-button driver.
 *
 * Polls 4 active-low momentary switches on the Waveshare ESP32-P4-WIFI6
 * 2×20 user-facing header. Internal pull-ups are enabled by this
 * driver, so each switch only needs to short its GPIO to GND when
 * pressed — no external resistor required.
 *
 * Button layout (mirrors docs/hardware/board_pinout.md §3):
 *
 *   PK_BTN_TARE  →  GPIO 26   "TARE"   IMU heading reset / full reorient
 *   PK_BTN_MODE  →  GPIO 27   "MODE"   PFD ↔ ADS-B list toggle
 *   PK_BTN_UP    →  GPIO 22   "UP"     list scroll up / brightness up
 *   PK_BTN_DOWN  →  GPIO 23   "DOWN"   list scroll down / brightness down
 *
 *   UP + DOWN  held together ≥ 5 s  →  PK_BTN_EVT_COMBO_BLE_PAIR
 *
 * Press semantics
 * ---------------
 *  - **Short press** (released within < 3 s of first contact) fires
 *    PK_BTN_EVT_SHORT_PRESS on release.
 *  - **Long press** (held ≥ 3 s without release) fires
 *    PK_BTN_EVT_LONG_PRESS once at the 3 s threshold. Only TARE and
 *    MODE emit single-button long presses — UP/DOWN deliberately
 *    don't, so their long-press time can be reserved for the combo
 *    (a 5 s "UP+DOWN" hold would otherwise race with two separate
 *    3 s single-key long presses).
 *  - **Very-long press** (held ≥ 10 s without release) fires
 *    PK_BTN_EVT_VERY_LONG_PRESS once at the 10 s threshold. Only TARE
 *    emits this — used for "factory reset" via the TARE button so
 *    MODE's long-press slot can be reserved for power on/off.
 *    Order on a sustained hold: SHORT does NOT fire (only on early
 *    release), LONG fires at 3 s, VERY_LONG fires at 10 s. The
 *    application accepts both LONG and VERY_LONG arriving on the same
 *    hold; the VERY_LONG action should be designed to subsume or
 *    override whatever LONG did.
 *  - **UP + DOWN combo** (both held ≥ 5 s, with the second press
 *    landing within 1 s of the first) fires
 *    PK_BTN_EVT_COMBO_BLE_PAIR on PK_BTN_UP. Suppresses any
 *    short/long press that would otherwise fire on UP or DOWN for
 *    the duration of the hold.
 *
 * Polled at 50 Hz (20 ms tick); 40 ms debounce. Each button has its
 * own small FSM (RELEASED → PRESSING → HELD_SHORT → HELD_LONG); the
 * combo detector is a second pass over the FSM states.
 *
 * Callback delivery
 * -----------------
 * Events are delivered on the button task's context. The handler MUST
 * NOT block (no I²C / SPI transactions, no long sleeps) — those should
 * be routed through a separate task. Routing through the cheap
 * fire-and-forget IMU/SDR APIs (pk_imu_tare_yaw, pk_sdr_request_reinit,
 * pk_ui_*) is fine.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    PK_BTN_TARE = 0,    /* GPIO 26 — IMU Tare / cage */
    PK_BTN_MODE,        /* GPIO 27 — PFD/list mode toggle */
    PK_BTN_UP,          /* GPIO 22 — list scroll up */
    PK_BTN_DOWN,        /* GPIO 23 — list scroll down */
    PK_BTN_COUNT,
} pk_button_id_t;

typedef enum {
    PK_BTN_EVT_SHORT_PRESS = 0,   /* press + release within <3 s */
    PK_BTN_EVT_LONG_PRESS,        /* held ≥3 s (TARE / MODE only) */
    PK_BTN_EVT_VERY_LONG_PRESS,   /* held ≥10 s (TARE only) */
    PK_BTN_EVT_COMBO_BLE_PAIR,    /* UP+DOWN held ≥5 s (fires on PK_BTN_UP) */
} pk_button_event_t;

typedef void (*pk_button_callback_t)(pk_button_id_t id,
                                     pk_button_event_t evt);

/*
 * Configure all four button GPIOs as input + internal pull-up, then
 * spawn the polling task on CPU 0 at priority 3 (well below IMU @ 5
 * and PFD @ 4, so the render path is never preempted).
 *
 * `cb` may be NULL, in which case button events are silently swallowed
 * (useful for the first hardware bring-up to confirm the GPIOs work
 * before wiring application logic).
 */
esp_err_t pk_button_init(pk_button_callback_t cb);
