/*
 * pfd_statusbar.h — top strip of the G1000-style PFD.
 *
 * Layout (spec §3): y ∈ [0, 18), full 320 wide. Renders the current
 * heading on the left and the ADS-B aircraft count on the right.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool    imu_valid;
    float   yaw_deg;
    size_t  aircraft_count;
    bool    gps_have_fix;
    uint8_t gps_sats;
} pk_pfd_status_t;

void pk_pfd_statusbar_render(uint16_t *fb, const pk_pfd_status_t *s);
