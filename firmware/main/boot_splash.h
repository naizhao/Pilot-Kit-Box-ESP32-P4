/*
 * boot_splash.h — pre-PFD boot logo screen.
 *
 * Renders the Pilot Kit logo (128×128 RGB565, embedded as a
 * .rodata blob via the build system's EMBED_FILES mechanism) into
 * the centre of the framebuffer, with "PILOT KIT BOX" and a short
 * version line below it. Called from main.c once `pk_display_init()`
 * returns, *before* the rest of the firmware finishes coming up —
 * gives the user something to look at during the ~1 second between
 * LCD ready and PFD task starting (USB host, ESP-Hosted, IMU init,
 * BLE all happen in that window).
 *
 * Once the PFD task starts spinning at 30 FPS it will overwrite the
 * splash on the next frame — no explicit dismiss needed.
 */
#pragma once

#include <stdint.h>

/* Renders the boot splash into the 240×320 RGB565 framebuffer. */
void pk_boot_splash_render(uint16_t *fb);
