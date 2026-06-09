/*
 * test_imu_tare.c — host-side proof for the "TARE resets the compass" bug.
 *
 * Standalone (plain `cc`, no ESP-IDF). Copies the pure quaternion math
 * out of firmware/main/imu_task.c verbatim — quat_mul / quat_to_euler /
 * quat_conj — and replays the display pipeline:
 *
 *     q_displayed = s_tare · q_aircraft           (left-multiply tare)
 *     (roll, pitch, yaw) = quat_to_euler(q_displayed)
 *
 * The bug: pk_imu_tare_now() sets s_tare = conj(q_aircraft_at_tare),
 * the FULL conjugate including the heading (yaw) component. So at the
 * tare pose the displayed yaw collapses to 0 and the HSI / HDG compass
 * (which reads s.yaw_deg) loses the true heading.
 *
 * The fix keeps roll/pitch caged via the tared quaternion but sources
 * yaw from the RAW (un-tared) aircraft quaternion, so the compass is
 * never touched by a tare.
 *
 * Build & run:
 *     cc -std=c11 -O2 -o /tmp/test_imu_tare firmware/test/test_imu_tare.c -lm
 *     /tmp/test_imu_tare
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- verbatim copies from firmware/main/imu_task.c ----------------- */

static inline void quat_mul(float aw, float ax, float ay, float az,
                            float bw, float bx, float by, float bz,
                            float *ow, float *ox, float *oy, float *oz)
{
    *ow = aw * bw - ax * bx - ay * by - az * bz;
    *ox = aw * bx + ax * bw + ay * bz - az * by;
    *oy = aw * by - ax * bz + ay * bw + az * bx;
    *oz = aw * bz + ax * by - ay * bx + az * bw;
}

static void quat_to_euler(float qi, float qj, float qk, float qw,
                          float *roll_deg, float *pitch_deg, float *yaw_deg)
{
    float sinr_cosp = 2.0f * (qw * qi + qj * qk);
    float cosr_cosp = 1.0f - 2.0f * (qi * qi + qj * qj);
    *roll_deg = atan2f(sinr_cosp, cosr_cosp) * (180.0f / (float)M_PI);

    float sinp = 2.0f * (qw * qj - qk * qi);
    if (sinp >  1.0f) sinp =  1.0f;
    if (sinp < -1.0f) sinp = -1.0f;
    *pitch_deg = asinf(sinp) * (180.0f / (float)M_PI);

    float siny_cosp = 2.0f * (qw * qk + qi * qj);
    float cosy_cosp = 1.0f - 2.0f * (qj * qj + qk * qk);
    float y = atan2f(siny_cosp, cosy_cosp) * (180.0f / (float)M_PI);
    if (y < 0) y += 360.0f;
    *yaw_deg = y;
}

static inline void quat_conj(float w, float x, float y, float z,
                             float *ow, float *ox, float *oy, float *oz)
{
    *ow =  w; *ox = -x; *oy = -y; *oz = -z;
}

/* ---- test helpers --------------------------------------------------- */

/* ZYX Tait-Bryan euler → quaternion (q = Rz(yaw)·Ry(pitch)·Rx(roll)),
 * the inverse of quat_to_euler above. Lets us build a known aircraft
 * pose to tare against. */
static void euler_to_quat(float roll_deg, float pitch_deg, float yaw_deg,
                          float *qw, float *qi, float *qj, float *qk)
{
    float r = roll_deg  * (float)M_PI / 180.0f;
    float p = pitch_deg * (float)M_PI / 180.0f;
    float y = yaw_deg   * (float)M_PI / 180.0f;
    float cr = cosf(r * 0.5f), sr = sinf(r * 0.5f);
    float cp = cosf(p * 0.5f), sp = sinf(p * 0.5f);
    float cy = cosf(y * 0.5f), sy = sinf(y * 0.5f);
    *qw = cr * cp * cy + sr * sp * sy;
    *qi = sr * cp * cy - cr * sp * sy;
    *qj = cr * sp * cy + sr * cp * sy;
    *qk = cr * cp * sy - sr * sp * cy;
}

static int g_fail = 0;
static void check(const char *what, float got, float want, float tol)
{
    bool ok = fabsf(got - want) <= tol;
    printf("  [%s] %-40s got=%8.3f want=%8.3f tol=%.2f\n",
           ok ? "PASS" : "FAIL", what, got, want, tol);
    if (!ok) g_fail++;
}

/* Display pipeline under test. `heading_from_raw` selects the fix:
 *   false → legacy: yaw from the tared quaternion (the bug)
 *   true  → fixed:  yaw from the raw aircraft quaternion (compass intact)
 *
 * roll/pitch always come from the tared quaternion, exactly as the
 * current firmware computes them — the fix does not touch them.
 */
static void compute_display(float taw, float tai, float taj, float tak,  /* s_tare */
                            float aqw, float aqi, float aqj, float aqk,  /* q_aircraft (raw) */
                            bool heading_from_raw,
                            float *roll, float *pitch, float *yaw)
{
    float dqw, dqi, dqj, dqk;
    quat_mul(taw, tai, taj, tak, aqw, aqi, aqj, aqk, &dqw, &dqi, &dqj, &dqk);

    float tr, tp, ty;
    quat_to_euler(dqi, dqj, dqk, dqw, &tr, &tp, &ty);   /* tared: roll/pitch (+ legacy yaw) */
    *roll = tr; *pitch = tp;

    if (heading_from_raw) {
        float rr, rp, ry;
        quat_to_euler(aqi, aqj, aqk, aqw, &rr, &rp, &ry);
        *yaw = ry;                                       /* fixed: true heading */
    } else {
        *yaw = ty;                                       /* legacy: heading caged to 0 */
    }
}

int main(void)
{
    /* Aircraft tared while pointing 120°, slightly tilted (the mount /
     * pose is not perfectly level — exactly when the bug bites). */
    const float HDG0 = 120.0f, PITCH0 = 8.0f, ROLL0 = -5.0f;
    float aqw0, aqi0, aqj0, aqk0;
    euler_to_quat(ROLL0, PITCH0, HDG0, &aqw0, &aqi0, &aqj0, &aqk0);

    /* Sanity: the pose round-trips through quat_to_euler. */
    float vr, vp, vy;
    quat_to_euler(aqi0, aqj0, aqk0, aqw0, &vr, &vp, &vy);
    printf("Pose round-trip:\n");
    check("roll",  vr, ROLL0,  0.01f);
    check("pitch", vp, PITCH0, 0.01f);
    check("yaw",   vy, HDG0,   0.01f);

    /* TARE: s_tare = conj(q_aircraft_at_tare) — unchanged by the fix. */
    float tw, ti, tj, tk;
    quat_conj(aqw0, aqi0, aqj0, aqk0, &tw, &ti, &tj, &tk);

    float r, p, y;

    /* --- LEGACY behaviour at the tare pose: this is the BUG ---------- */
    printf("\nLegacy (buggy) — at tare pose, compass collapses to 0:\n");
    compute_display(tw, ti, tj, tk, aqw0, aqi0, aqj0, aqk0, false, &r, &p, &y);
    check("roll==0",  r, 0.0f,   0.01f);
    check("pitch==0", p, 0.0f,   0.01f);
    printf("  (legacy yaw at tare = %.3f — true heading %.0f is lost)\n", y, HDG0);

    /* --- FIXED behaviour at the tare pose --------------------------- */
    printf("\nFixed — at tare pose, horizon caged but compass intact:\n");
    compute_display(tw, ti, tj, tk, aqw0, aqi0, aqj0, aqk0, true, &r, &p, &y);
    check("roll==0",      r, 0.0f,  0.01f);
    check("pitch==0",     p, 0.0f,  0.01f);
    check("yaw==heading", y, HDG0,  0.01f);

    /* --- FIXED behaviour after a 30° heading change, same tilt ------- *
     * The aircraft yaws to 150° (compass MUST track), tilt unchanged so
     * the caged horizon stays ~level. */
    const float HDG1 = 150.0f;
    float aqw1, aqi1, aqj1, aqk1;
    euler_to_quat(ROLL0, PITCH0, HDG1, &aqw1, &aqi1, &aqj1, &aqk1);

    printf("\nFixed — after heading 120->150 (same tilt):\n");
    compute_display(tw, ti, tj, tk, aqw1, aqi1, aqj1, aqk1, true, &r, &p, &y);
    check("yaw tracks true heading", y, HDG1, 0.01f);
    /* roll/pitch are NOT asserted here. A left-multiply tare expresses the
     * differential in the tare-time body frame, so yawing a tilted tare
     * reference makes the caged horizon drift slightly (here ~%.1f/%.1f).
     * That is pre-existing behaviour of the current firmware, identical
     * with or without this fix, and out of scope for the compass bug. */
    printf("  (info: caged horizon drifts to roll=%.2f pitch=%.2f under "
           "tilted-tare + 30 heading change — pre-existing)\n", r, p);

    /* Contrast: legacy yaw after the same move reads the relative 30°,
     * not 150° — the compass is wrong, which is the user's report. */
    compute_display(tw, ti, tj, tk, aqw1, aqi1, aqj1, aqk1, false, &r, &p, &y);
    printf("  (legacy yaw after move = %.3f — wrong, should be %.0f)\n", y, HDG1);

    printf("\n%s (%d failure%s)\n", g_fail ? "TEST FAILED" : "TEST PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
