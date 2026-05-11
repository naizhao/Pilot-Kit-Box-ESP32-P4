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
