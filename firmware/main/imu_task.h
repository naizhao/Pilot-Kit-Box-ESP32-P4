/*
 * imu_task.h — BNO085 sensor-fusion driver.
 *
 * Talks SH-2 / SHTP over the on-board I²C0 bus (shared with the ES8311
 * codec, no address conflict — codec is 0x18, BNO085 default is 0x4A).
 *
 * The driver does the bare-minimum to deliver a single fused attitude
 * report at 100 Hz to the rest of the firmware: it doesn't expose
 * accelerometer, gyro, or magnetometer streams; it doesn't run
 * calibration commands; it doesn't speak the executable channel for
 * firmware updates. Add those only when a concrete firmware feature
 * needs them.
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
 * The fixed mounting quaternion below must match the physical IMU
 * orientation in the current enclosure or breadboard build.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* --- Mounting orientation corrections (breadboard / debug only) ----- *
 *
 * Applied to the Euler output AFTER BNO085's quaternion → euler
 * conversion. Use these while the chip is glued onto the board in
 * an orientation that doesn't match the "chip +X forward, +Y right,
 * +Z down" aerospace convention.
 *
 * Once the final PCB physically orients the BNO085 correctly, set
 * all four back to 0 / false and this section becomes dead code.
 *
 * Diagnostic recipe — set them all to 0 / 0 first, then adjust:
 *
 *   Step 1.  Place device LEVEL on a desk, "front" edge pointing
 *            away from you, "right" edge to your right.
 *   Step 2.  Check PFD HDG. If it reads ~180° when you face north
 *            (or any consistent 180° offset from the expected value),
 *            set MOUNT_YAW_OFFSET_DEG = 180. Other values (90 / 270)
 *            work too for different mounting rotations.
 *   Step 3.  Tilt the FRONT edge of the device downward (nose-down).
 *            Pitch should read NEGATIVE. If it reads positive,
 *            set MOUNT_INVERT_PITCH = 1.
 *   Step 4.  Tilt the RIGHT edge of the device downward (right-wing-
 *            down). Roll should read POSITIVE. If negative,
 *            set MOUNT_INVERT_ROLL = 1.
 *   Step 5.  Rotate device clockwise viewed from above. HDG should
 *            INCREASE 0→90→180→270. If it decreases, set
 *            MOUNT_INVERT_YAW = 1.
 *
 * INVERT flags are applied first; the offset is applied last
 * (after invert) and wraps yaw into [0, 360).
 *
 * Defaults are all 0 / off — the chip is assumed to be mounted in
 * the canonical "chip +X forward, +Y right, +Z down" aerospace
 * orientation. The temporary INVERT_PITCH=1 default that earlier
 * commits shipped was for an incorrectly-mounted breadboard build
 * that the user is now re-soldering. Once the new mount is in place,
 * walk through the diagnostic recipe above (steps 1-5) and only set
 * a knob if its corresponding test actually fails. */
#define PK_IMU_MOUNT_INVERT_ROLL       0
#define PK_IMU_MOUNT_INVERT_PITCH      0
#define PK_IMU_MOUNT_INVERT_YAW        0
#define PK_IMU_MOUNT_YAW_OFFSET_DEG    0

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
 *       physically mounted. Don't touch it unless we change which
 *       Euler convention quat_to_euler() implements.
 *
 *     ─ q_body_fix  = R_aircraft→chip — turns aircraft body vectors
 *       into chip body vectors. Numerically equal to the inverse of
 *       R_chip→aircraft. For a 180° rotation the matrix is its own
 *       inverse, so we can just store R_chip→aircraft directly here.
 *       THIS is the per-mount knob.
 *
 * Identity body fix (chip body == aircraft body):
 *     W=1, X=0, Y=0, Z=0
 *
 * Current build — chip face toward pilot, header on the pilot's left,
 * VCC pin at the top edge, board vertical. Mapping:
 *     chip +X  →  aircraft up    (= -aircraft +Z)
 *     chip +Y  →  aircraft left  (= -aircraft +Y)
 *     chip +Z  →  aircraft back  (= -aircraft +X)
 * Rotation: 180° around (1, 0, -1)/√2  →  q = (0, √2/2, 0, -√2/2).
 *
 * Math sanity check: for "level facing N" pose in the above mounting,
 * BNO outputs q_bno = (0.5, 0.5, -0.5, 0.5); applying the sandwich
 * with the q's below yields q_aircraft = (-1, 0, 0, 0), which is
 * identity as a rotation, so quat_to_euler() returns (0, 0, 0). ✓ */

/* q_world_fix — DO NOT EDIT unless quat_to_euler() changes. */
#define PK_IMU_WORLD_FIX_W             0.0f
#define PK_IMU_WORLD_FIX_X             0.7071068f
#define PK_IMU_WORLD_FIX_Y             0.7071068f
#define PK_IMU_WORLD_FIX_Z             0.0f

/* q_body_fix — set this to match the physical chip mounting. */
#define PK_IMU_MOUNT_QUAT_W            0.0f
#define PK_IMU_MOUNT_QUAT_X            0.7071068f
#define PK_IMU_MOUNT_QUAT_Y            0.0f
#define PK_IMU_MOUNT_QUAT_Z           -0.7071068f

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
 * Safe to call from any task. Typical caller: button_task on TARE
 * short-press.
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
 *
 * Typical caller: button_task on TARE long-press (≥3 s).
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
 *
 * Typical caller: button_task on TARE very-long-press (≥10 s).
 */
esp_err_t pk_imu_factory_reset(void);
