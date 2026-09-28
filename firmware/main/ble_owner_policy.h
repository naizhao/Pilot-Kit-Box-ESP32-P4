#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t type;
    uint8_t value[6];
} pk_ble_peer_id_t;

typedef enum {
    PK_BLE_OWNER_DENY = 0,
    PK_BLE_OWNER_ALLOW,
    PK_BLE_OWNER_CLAIM,
} pk_ble_owner_decision_t;

/* Encryption prevents plaintext writes but does not identify the owner.
 * An already persisted identity is accepted; a different/first peer can claim
 * ownership only during a pairing window explicitly opened on the Box. */
pk_ble_owner_decision_t pk_ble_owner_decide(
    bool owner_set, const pk_ble_peer_id_t *owner,
    const pk_ble_peer_id_t *peer, bool encrypted, bool bonded,
    bool local_window_open);

/* Physical pairing control is a toggle, not a renewable one-shot.  An active
 * deadline is cancelled by the next tap; a closed/expired deadline starts a
 * fresh window.  Kept pure so the exact deadline boundary is host-tested. */
int64_t pk_ble_owner_window_toggle_deadline(int64_t now_us,
                                            int64_t current_until_us,
                                            int64_t duration_us);
