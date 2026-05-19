/*
 * pfd_legacy.h — transitional home for the old heading-tape and
 * text-panel widgets carried over from the portrait PFD. Phase D
 * deletes this file once the G1000-style statusbar + HSI replace
 * the widgets it contains.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "imu_task.h"

void pk_pfd_legacy_render(uint16_t *fb,
                          const pk_imu_sample_t *s, bool imu_valid,
                          size_t aircraft_count);
