/*
 * imu_task.h — Phase 4b BNO085 sensor-fusion driver.
 *
 * Talks SH-2 / SHTP over the on-board I²C0 bus (shared with the ES8311
 * codec, no address conflict — codec is 0x18, BNO085 default is 0x4A).
 *
 * The driver does the bare-minimum to deliver a single fused attitude
 * report at 100 Hz to the rest of the firmware: it doesn't expose
 * accelerometer, gyro, or magnetometer streams; it doesn't run
 * calibration commands; it doesn't speak the executable channel for
 * firmware updates. Those can be added incrementally once Phase 4
 * verifies the pipeline.
 *
 *   Reset → drain SHTP advertisement → enable "Rotation Vector" report
 *   (Sensor Report ID 0x05) at 10 ms interval → poll input reports on
 *   channel 3 → quaternion → ZYX Tait-Bryan Euler angles → stash latest
 *   under a mutex for the display task to consume.
 *
 * Convention exposed to callers (pk_imu_sample_t below):
 *   roll  = rotation about the sensor's X axis  (right-wing-down +)
 *   pitch = rotation about the sensor's Y axis  (nose-up +)
 *   yaw   = rotation about the sensor's Z axis  (clockwise-from-above +)
 * Mounting offset will be configurable in Phase 4c. For now, the sensor
 * frame is the PFD frame.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int64_t  ts_us;        /* esp_timer_get_time() at sample */
    float    roll_deg;     /* -180 .. +180 */
    float    pitch_deg;    /* -90 .. +90 */
    float    yaw_deg;      /* 0 .. 360 */
    uint8_t  accuracy;     /* 0=unreliable, 3=high */
    bool     valid;
} pk_imu_sample_t;

/*
 * Bring up I²C, reset the BNO085, drain the SHTP advertisement, enable
 * the Rotation Vector report, and spawn the IMU task. Safe to call
 * once on boot. Returns ESP_OK only when the sensor is fully responsive;
 * downstream code (display task, etc.) checks pk_imu_sample_get()'s
 * `valid` flag rather than relying on this return value.
 */
esp_err_t pk_imu_init(void);

/*
 * Atomically copy the latest IMU sample into `*out`. Returns false if
 * the IMU task hasn't produced a sample yet (e.g. sensor not present,
 * or still booting). Safe to call from any task.
 */
bool pk_imu_sample_get(pk_imu_sample_t *out);

/*
 * Zero the yaw axis only — "DG sync" / heading reset. Subsequent
 * Rotation Vector reports will treat the current heading as 0°. Roll
 * and pitch are unaffected (they stay referenced to gravity, which is
 * absolute). Fire-and-forget; the call returns as soon as the SH-2
 * Tare Now command has been pushed onto the bus.
 *
 * Safe to call from any task. Typical caller: button_task on short
 * press of BTN1.
 */
esp_err_t pk_imu_tare_yaw(void);

/*
 * "Erect and cage" — set the current pose as the new (level, heading
 * 0) reference for ALL three axes, then persist the tare and the
 * dynamic calibration data (mag/gyro/accel zero offsets) into the
 * BNO085's internal flash. Survives a power cycle.
 *
 * REFUSES if the current sample's accuracy is < 2 (mag fusion not
 * yet converged), returning ESP_ERR_INVALID_STATE — calling Save DCD
 * with an uncalibrated mag is the original mistake that poisons the
 * BNO085 flash and breaks heading on every subsequent boot. Do a
 * figure-8 motion for ~15 s and watch the `imu: rpy = ... (acc=N)`
 * 1 Hz log line until `acc=2` or `acc=3`, then long-press again.
 *
 * Sends three SH-2 commands in sequence with 50 ms gaps so the chip
 * has time to digest each one:
 *   1. Tare Now    (axes = X | Y | Z, basis = Rotation Vector)
 *   2. Persist Tare
 *   3. Save DCD
 *
 * Typical caller: button_task on long press of BTN1.
 */
esp_err_t pk_imu_full_reorient(void);

/*
 * Undo a previously-persisted Tare by setting the BNO085's
 * reorientation matrix to the identity quaternion and persisting that
 * to flash. Doesn't touch DCD (mag/gyro/accel calibration). Useful as
 * a standalone "cancel a bad Tare" command. Internally a building
 * block of pk_imu_factory_reset().
 */
esp_err_t pk_imu_clear_tare(void);

/*
 * Wipe the persistent Dynamic Calibration Data (DCD) from BNO085
 * flash via SH-2 Command 0x0B. Erases the stored magnetometer,
 * gyroscope and accelerometer zero-offset estimates so the next
 * fusion startup learns them fresh from raw sensor data. Doesn't
 * take effect until the BNO085 is re-initialised; pair with
 * bno_bring_up() (or call pk_imu_factory_reset() which does both).
 */
esp_err_t pk_imu_clear_dcd(void);

/*
 * "Factory reset" — composes pk_imu_clear_tare() + pk_imu_clear_dcd()
 * + a hard reset + replay of the SH-2 init handshake, so the BNO085
 * comes back from a guaranteed-clean state with no leftover Tare and
 * no poisoned calibration data.
 *
 * After this returns, the user should hold the device and do a
 * figure-8 motion for ~15 seconds so the magnetometer fusion can
 * re-learn the local magnetic field. Watch the 1 Hz `imu` log line
 * — `acc` will climb from 0 toward 3 as fusion converges. Once
 * `acc >= 2`, a TARE long-press will succeed and persist a clean
 * calibration for the long term.
 *
 * Typical caller: button_task on long press of BTN2 (MODE).
 */
esp_err_t pk_imu_factory_reset(void);
