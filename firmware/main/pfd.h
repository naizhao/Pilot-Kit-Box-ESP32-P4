/*
 * pfd.h — Phase 4c Primary Flight Display rendering task.
 *
 * Reads pk_imu_sample_get() at ~30 Hz, redraws into the framebuffer
 * owned by display.c, and flushes the panel via pk_display_flush_full().
 *
 * The first cut (4c v1, this revision) renders:
 *   - A sky-blue / brown ground split with the horizon rotated by roll
 *     and translated by pitch, just like a mechanical attitude
 *     indicator
 *   - A fixed yellow center reticle so the pilot's eye has an anchor
 *   - A bottom status bar showing the number of currently-tracked ADS-B
 *     aircraft as a column of green squares (1 square per plane,
 *     wrapping at the screen width)
 *
 * Phase 4c v2 will add:
 *   - Pitch ladder marks (numbered −30 .. +30)
 *   - Bank arc + indicator at the top
 *   - Heading tape strip
 *   - 5×7 bitmap font for numeric readouts (roll/pitch/yaw/count)
 *
 * Phase 4d converts the synchronous flush to a double-buffer + GDMA
 * async pipeline so 60 FPS becomes free and the panel can be redrawn
 * in parallel with CPU drawing the next frame.
 */
#pragma once

#include "esp_err.h"

/*
 * Spawn the PFD render task. Must be called after pk_display_init()
 * has succeeded (otherwise we have no framebuffer to draw into).
 * pk_imu_init() failing is fine — the PFD just draws a level horizon
 * until an IMU sample arrives.
 */
esp_err_t pk_pfd_start(void);
