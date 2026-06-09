/*
 * pfd_speed_tape.h — left-side ground-speed tape (mirror of ALT tape).
 *
 * Layout: x ∈ [0, 64), y ∈ [18, 208). 1 px = 2 kt.
 * Minor ticks every 5 kt, major every 25 kt, labels every 50 kt.
 * Right-edge 1 px cyan divider mirrors the ALT tape's left-edge divider.
 *
 * When `valid` is false the tape frame + ticks still draw (silhouette
 * preserved) but the centre box shows "---" in grey and labels are
 * suppressed — consistent with pfd_tape.c stale behaviour.
 *
 * Below the tape band (y ∈ [170, 208]) a small metric conversion pad
 * shows the current GS in km/h and mph.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool valid;
    int  ground_speed_kt;
} pk_pfd_speed_tape_t;

void pk_pfd_speed_tape_render(uint16_t *fb, const pk_pfd_speed_tape_t *s);
