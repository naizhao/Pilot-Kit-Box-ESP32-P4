/*
 * pfd_attitude.h — attitude-indicator widget (sky/ground horizon,
 * pitch ladder, bank arc, yellow reticle).
 *
 * Stateless — every call re-draws into the supplied framebuffer using
 * the supplied IMU snapshot. The widget owns the attitude region; the
 * caller is expected to fill the rest of the panel.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Per-frame IMU snapshot. The attitude widget needs roll/pitch only;
 * yaw is consumed by the HSI/statusbar widgets. `valid` reflects
 * whether pk_imu_sample_get() succeeded. */
typedef struct {
    bool  valid;
    float roll_deg;
    float pitch_deg;
} pk_pfd_imu_t;

void pk_pfd_attitude_render(uint16_t *fb, const pk_pfd_imu_t *imu);
