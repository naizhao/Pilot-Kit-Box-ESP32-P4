/*
 * pfd_tape.h — right-side altitude tape (spec §3).
 *
 * Layout: x ∈ [248, 320), y ∈ [18, 138). Center digit box bleeds 2 px
 * left into x = 246..320 for the G1000 look. 1 px = 5 ft. Major ticks
 * every 100 ft, minor every 20 ft, labels every 200 ft.
 *
 * When `valid` is false, the tape frame and ticks still draw but the
 * center box shows "----" in grey and labels are suppressed — the
 * widget's silhouette persists so the pilot's eye knows where ALT
 * lives even with no own-ship data.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool valid;
    int  altitude_ft;
} pk_pfd_alt_tape_t;

void pk_pfd_alt_tape_render(uint16_t *fb, const pk_pfd_alt_tape_t *a);
