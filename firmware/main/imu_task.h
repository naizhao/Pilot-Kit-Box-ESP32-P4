/*
 * imu_task.h — BNO085 sensor-fusion driver.
 *
 * Talks SH-2 / SHTP over the on-board I²C0 bus (shared with the ES8311
 * codec, no address conflict — codec is 0x18, BNO085 default is 0x4A).
 *
 * The driver delivers a fused attitude report at 100 Hz plus a
 * gravity-subtracted linear-acceleration report at 50 Hz to the rest
 * of the firmware. It still doesn't expose raw accelerometer, gyro, or
 * magnetometer streams and doesn't speak the executable channel for
 * firmware updates. Add those only when a concrete firmware feature
 * needs them.
 *
 * The one calibration command it does issue on its own is SH-2 Save
 * DCD (0x06), fired once per boot after the fusion accuracy has been
 * pegged at 3 for a few seconds, so the magnetometer calibration
 * survives a power cycle instead of re-converging from scratch every
 * time. See the DCD block in imu_task.c for the trigger and the
 * flash-wear throttle. Note this is BNO085-internal flash and is a
 * different thing from pk_imu_tare_persist()'s NVS blob.
 *
 *   Reset → drain SHTP advertisement → enable "Rotation Vector" report
 *   (Sensor Report ID 0x05) at 10 ms interval + "Linear Acceleration"
 *   report (Sensor Report ID 0x04) at 20 ms interval → poll input
 *   reports on channel 3 → quaternion → ZYX Tait-Bryan Euler angles
 *   (+ accel vector rotated chip→aircraft) → stash latest under a
 *   mutex for the display task to consume.
 *
 * Linear Acceleration (0x04) was chosen over the plain Accelerometer
 * report (0x01): SH-2 already subtracts gravity for us, so we don't
 * have to do it in firmware (and don't have to trust our own gravity
 * estimate while the aircraft is maneuvering). 50 Hz is plenty for an
 * RMS-window vibration metric downstream and keeps I²C/SHTP bandwidth
 * headroom for the 100 Hz Rotation Vector stream.
 *
 * Convention exposed to callers (pk_imu_sample_t below):
 *   roll  = rotation about the sensor's X axis  (right-wing-down +)
 *   pitch = rotation about the sensor's Y axis  (nose-up +)
 *   yaw   = rotation about the sensor's Z axis  (clockwise-from-above +)
 * The mounting transform is NOT in this file: it depends on which
 * expansion board the image is built for (V3.9 mounts U4 at 0°, V4.3
 * at +90°), so it comes from pk_board.h. There is deliberately no
 * second, hand-tuned correction path here — see the sandwich section
 * below.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

/* --- Mounting + world-frame transformation (sandwich form) ---------- *
 *
 * The BNO085 outputs the Rotation Vector in the standard Android
 * convention: world reference frame = Earth ENU (East-North-Up), and
 * the device frame = chip body axes. To convert that to the aerospace
 * convention quat_to_euler() expects (aircraft body in NED world), we
 * need TWO rotations, not just one — a left-mul to fix the world
 * frame, and a right-mul to fix the body frame:
 *
 *     q_aircraft  =  q_world_fix  ·  q_bno_raw  ·  q_body_fix
 *
 *     ─ q_world_fix = R_ENU→NED — turns the BNO's ENU world reference
 *       into NED. Geometrically a 180° rotation around (1,1,0)/√2:
 *
 *           q_world_fix = (0, √2/2, √2/2, 0)
 *
 *       This is a FIXED constant — independent of how the chip is
 *       physically mounted, and independent of which board it is on.
 *       It belongs to the Euler convention, so it lives here. Don't
 *       touch it unless quat_to_euler() changes.
 *
 *     ─ q_body_fix = R_aircraft→chip — turns aircraft body vectors into
 *       chip body vectors. THIS is the per-board mounting term, and it
 *       is NOT a constant in this header any more: V3.9 mounts U4 at
 *       0° and V4.3 at +90°, so it comes from
 *
 *           pk_board_imu_body_fix_quat(pk_board_profile(), q)
 *
 *       See pk_board.h for the datasheet / footprint / enclosure chain
 *       that produces it. The old hand-tuned PK_IMU_MOUNT_QUAT_* macros
 *       described a 2026-08-03 re-soldered breakout, not either PCB,
 *       and have been removed so nothing can silently keep using them.
 *
 * Regression tests derive the expected q_bno for known poses straight
 * from the physical axis mapping — they do NOT reuse pk_board's
 * matrices, so a wrong constant there turns them red:
 *     firmware/test/test_pk_board_mount.c   (both profiles + wrong-profile)
 *     firmware/test/test_imu_mount.c        (V3.9 deep-dive + accel path)
 */

/* q_world_fix — DO NOT EDIT unless quat_to_euler() changes. */
#define PK_IMU_WORLD_FIX_W             0.0f
#define PK_IMU_WORLD_FIX_X             0.7071068f
#define PK_IMU_WORLD_FIX_Y             0.7071068f
#define PK_IMU_WORLD_FIX_Z             0.0f

typedef struct {
    int64_t  ts_us;        /* esp_timer_get_time() at sample */
    float    roll_deg;     /* -180 .. +180 */
    float    pitch_deg;    /* -90 .. +90 */
    float    yaw_deg;      /* 0 .. 360 */
    uint8_t  accuracy;     /* 0=unreliable, 3=high */
    bool     valid;
    /* Gravity-subtracted linear acceleration, aircraft body frame
     * (+X forward, +Y right, +Z down), m/s². Rotated chip→aircraft
     * with the same board-profile q_body_fix used for attitude — NOT
     * the world-frame fix, since this is a body-frame vector, not a
     * world-referenced orientation. have_accel is false until the
     * first Linear Acceleration (0x04) report parses successfully
     * (e.g. sensor absent, or still booting); consumers must check it
     * before reading accel_x/y/z. */
    float    accel_x_mps2;
    float    accel_y_mps2;
    float    accel_z_mps2;
    bool     have_accel;
    /* Vibration intensity 0-255 from a 1 s RMS window over the
     * accel-magnitude, see pk_vib.h. 0 = unavailable (have_accel is
     * false, or the window hasn't filled since boot/reset yet) — NOT
     * "zero vibration"; a genuinely still aircraft reads as a small
     * nonzero value once the window is full. Never conflate the two:
     * pk_flight_phase.c's phase state machine keys off this exact
     * distinction. */
    uint8_t  vib_level;
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
 * "Tare now" — capture the current attitude (post-sandwich, in
 * aircraft NED frame) and use its conjugate as a software offset
 * applied to every subsequent frame, so the PFD horizon (roll/pitch)
 * reads (0, 0) immediately and tracks differential tilt from this
 * reference onwards.
 *
 * Heading (yaw / HSI / HDG) is deliberately NOT affected: the compass
 * keeps reading the true magnetic heading across a tare. Only the
 * artificial horizon is caged. (parse_rotation_vector() sources yaw
 * from the raw pre-tare attitude — see the note at its euler-extraction
 * call site.)
 *
 * Implemented purely in firmware (no BNO SH-2 Tare commands) so that
 * it composes cleanly with the world/body-frame sandwich applied by
 * parse_rotation_vector(). DOES NOT touch BNO flash and DOES NOT touch
 * the magnetometer Dynamic Calibration Data (DCD) — those survive
 * across this call.
 *
 * No accuracy precondition: works even when the BNO's mag fusion has
 * not converged (acc=0). That makes it the right operation to fire on
 * "cage on power-up before flight" workflows where the operator
 * doesn't want to wait for figure-8 mag calibration.
 *
 * Volatile: the new offset lives only in RAM. It is overwritten on
 * the next tare and wiped on power-cycle. Use pk_imu_tare_persist()
 * if the reference should survive a reboot.
 *
 * Safe to call from any task.
 */
esp_err_t pk_imu_tare_now(void);

/*
 * Same effect on the live attitude as pk_imu_tare_now(), additionally
 * writes the new offset quaternion into NVS so it is restored on the
 * next boot inside pk_imu_init(). Use this when the operator is
 * happy with the current calibration and wants it to persist across
 * power cycles.
 *
 * Returns the NVS write status — ESP_OK on success, the underlying
 * nvs_* error code on failure. The in-RAM tare is updated either way
 * (a flash-write failure does not invalidate the live calibration).
 */
esp_err_t pk_imu_tare_persist(void);

/*
 * "Factory reset" — wipe the persistent NVS tare offset AND the BNO's
 * persistent calibration state, then re-initialise the chip.
 * Specifically:
 *   1. Reset the in-RAM software tare back to identity (so the PFD
 *      reverts to the raw mounting-corrected attitude).
 *   2. Erase the NVS tare key so the next boot starts clean.
 *   3. Tell the BNO to clear its persistent Dynamic Calibration Data
 *      (SH-2 Command 0x0B) — undoes any mag/gyro/accel zero-offset
 *      poisoning that may have been written by older firmware that
 *      used BNO Tare directly.
 *   4. Also clear the BNO's internal reorientation matrix (set to
 *      identity quaternion + persist) — same reason: undo any old
 *      BNO Tare that may still be in chip flash.
 *   5. Hard-reset the chip and replay SH-2 init so the fusion engine
 *      restarts from a guaranteed-clean state.
 *
 * After this returns, do a figure-8 motion for ~15 s so the
 * magnetometer fusion re-converges. Once `acc >= 2` on the 1 Hz log
 * line, a TARE long-press persists a clean calibration for the long
 * term.
 */
esp_err_t pk_imu_factory_reset(void);

/*
 * 暴露 I²C0 主总线 handle,供同总线的其它 device(BMP388)复用。
 * i2c_new_master_bus() 全局只能建一次,此 handle 是唯一入口。
 * baro_task 用它 i2c_master_bus_add_device() 挂自己的 BMP388 device。
 * 返回 NULL 表示 IMU 尚未初始化(总线未建)。
 */
/* 必须在 pk_imu_init() 返回后调用(s_bus 此时已建)。无并发保护,
 * s_bus 由 app_main 单线程在 init 期写入,之后只读。 */
i2c_master_bus_handle_t pk_i2c0_bus_get(void);
