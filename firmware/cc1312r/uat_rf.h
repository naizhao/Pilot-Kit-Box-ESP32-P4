#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "uat_phy.h"

typedef void (*uat_rf_frame_cb_t)(const uat_phy_packet_t *packet, void *user);

/* Starts a real, receive-only TI RF Driver command chain. */
bool uat_rf_start(uat_rf_frame_cb_t callback, void *user);

/* Non-blocking health/recovery service.  Call from the main loop. */
/* Returns a newly observed raw RF Core command status, or 0 when healthy/no
 * new fault.  A terminal fault is held for diagnosis instead of immediately
 * entering RF_close/open, so the SPI control plane remains serviceable long
 * enough to report the exact status to the RP2040. */
uint16_t uat_rf_poll(void);

/* Recovery exhausted its bounded RF Driver cancel/termination window.  The
 * main loop must reset the CC1312R instead of continuing to feed the watchdog
 * with an RX chain that can no longer make progress. */
bool uat_rf_reset_required(void);

const uat_phy_health_t *uat_rf_health(void);

/* Periodic idle-channel probe snapshot.  generation changes only when a new
 * probe result or recovery transition is available for CC→RP publication. */
void uat_rf_health_status(rp_cc13xx_rf_health_t *out, uint32_t *generation);
