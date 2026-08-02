/* pk_vib.h — IMU vibration-intensity metric (accel-magnitude RMS).
 *
 * Pure standard-C (no IDF headers, no FreeRTOS) so it can be built and
 * tested on the host — see firmware/test/test_pk_vib.c. imu_task.c owns
 * the actual pk_vib_state_t instance and feeds it from
 * parse_linear_acceleration().
 *
 * Algorithm: RMS of the linear-acceleration vector's magnitude over a
 * sliding window, quantized to 0-255. Caller pushes one aircraft
 * body-frame accel sample per Linear Acceleration (SH-2 0x04) report
 * (50 Hz — see imu_task.c's bno_enable_linear_acceleration() comment);
 * pk_vib_level() is cheap (O(1), no window rescan) and can be polled
 * every push.
 *
 * *** vib_level == 0 means "unavailable", NOT "zero vibration" ***
 * It's returned only while the window hasn't filled yet (cold start,
 * driver not ready) or the caller never pushed a sample (have_accel
 * was false). A truly still aircraft on quiet ramp still produces a
 * small nonzero RMS — noise floor, engine idle, wind gust on the
 * airframe — and pk_vib_level() floors its output at 1 once the window
 * is full, specifically so "genuinely calm" (1-2) can never be
 * confused with "no data" (0). pk_flight_phase.c's phase state machine
 * depends on this distinction: it gates on `vib_level != 0` to decide
 * whether vibration is available evidence at all, separately from
 * gating on the value being low (see PK_PHASE_VIB_LOW_MAX in
 * pk_flight_phase.c). Collapsing the two would make "IMU absent"
 * indistinguishable from "engine off, dead calm" and corrupt the
 * ground/taxi/airborne classification.
 *
 * Quantization scale: full scale is RMS_FULL_SCALE_MPS2 = 2.0 m/s²,
 * mapped to 255, saturating above that. 2.0 m/s² RMS is comfortably
 * above rough taxiway / gravel-strip bumps and turbulence-shaken cruise
 * on a light GA airframe, so normal flight uses the low-to-mid part of
 * the range with headroom to spare for hard landings / rough water
 * ditching without instantly pegging the gauge. At the low end, the
 * flight-phase state machine's "quiet" threshold (PK_PHASE_VIB_LOW_MAX
 * = 20/255) corresponds to ~0.157 m/s² RMS — comfortably above the
 * near-zero true-stationary noise floor but well below idle-engine
 * buzz and taxi roll, giving that threshold a real gap to sit in.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical RMS window length, in samples. 50 Hz Linear Acceleration feed
 * (imu_task.c) × 1 s = 50 samples, per spec. */
#define PK_VIB_WINDOW_LEN 50u

/* Physical ring-buffer capacity. Kept above PK_VIB_WINDOW_LEN as
 * headroom so a future window-length tweak (e.g. bumping to 60 samples
 * to tune responsiveness) doesn't also require resizing this array;
 * indices are always taken mod PK_VIB_WINDOW_LEN, so the extra slots
 * sit unused today. */
#define PK_VIB_BUF_CAP 64u

/* Saturation point of the RMS quantization — see file header. */
#define PK_VIB_RMS_FULLSCALE_MPS2 2.0f

typedef struct {
    float    buf[PK_VIB_BUF_CAP];  /* squared magnitude per sample; only
                                       indices [0, PK_VIB_WINDOW_LEN) are
                                       ever touched */
    float    sum_sq;               /* running sum of buf[0..count) —
                                       avoids rescanning the window on
                                       every pk_vib_level() call */
    uint16_t head;                 /* next write index, mod PK_VIB_WINDOW_LEN */
    uint16_t count;                /* samples pushed so far, saturates at
                                       PK_VIB_WINDOW_LEN */
} pk_vib_state_t;

/* Clears the window. After reset, pk_vib_level() returns 0
 * (unavailable) until PK_VIB_WINDOW_LEN samples have been pushed. */
void pk_vib_reset(pk_vib_state_t *st);

/* Pushes one aircraft body-frame linear-acceleration sample (m/s²,
 * gravity already subtracted — see imu_task.h's pk_imu_sample_t).
 * O(1): evicts the oldest sample from the running sum before adding
 * the new one. */
void pk_vib_push(pk_vib_state_t *st, float ax, float ay, float az);

/* Returns the current vibration level, 0-255. 0 = unavailable (window
 * not yet full — see file header for why this is never conflated with
 * "calm"). Otherwise the accel-magnitude RMS over the window,
 * quantized per PK_VIB_RMS_FULLSCALE_MPS2 and floored at 1. */
uint8_t pk_vib_level(const pk_vib_state_t *st);

#ifdef __cplusplus
}
#endif
