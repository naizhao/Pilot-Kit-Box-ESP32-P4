#include "ble_owner_policy.h"

#include <string.h>

pk_ble_owner_decision_t pk_ble_owner_decide(
    bool owner_set, const pk_ble_peer_id_t *owner,
    const pk_ble_peer_id_t *peer, bool encrypted, bool bonded,
    bool local_window_open)
{
    if (peer == NULL || !encrypted || !bonded) return PK_BLE_OWNER_DENY;
    if (owner_set && owner != NULL &&
        owner->type == peer->type &&
        memcmp(owner->value, peer->value, sizeof(owner->value)) == 0)
        return PK_BLE_OWNER_ALLOW;
    return local_window_open ? PK_BLE_OWNER_CLAIM : PK_BLE_OWNER_DENY;
}

int64_t pk_ble_owner_window_toggle_deadline(int64_t now_us,
                                            int64_t current_until_us,
                                            int64_t duration_us)
{
    return current_until_us > now_us ? 0 : now_us + duration_us;
}
