/*
 * pfd_hsi.h — bottom HSI half-rose with HDG box + aircraft symbol.
 *
 * Layout (spec §3): y ∈ [138, 240), full 320 wide. Half-rose has its
 * virtual center at (160, 240) radius 70 — the bottom half is hidden
 * below the screen, giving the cropped G1000 look. The top of the
 * visible arc shows the current heading; the rose rotates with yaw.
 *
 * The HDG box (74×24 white-bordered scale-3 digits) sits centered
 * above the rose at y ∈ [138, 162).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool  imu_valid;
    float yaw_deg;
} pk_pfd_hsi_t;

void pk_pfd_hsi_render(uint16_t *fb, const pk_pfd_hsi_t *h);
