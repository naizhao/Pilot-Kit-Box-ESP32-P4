/* AIRBAND BLE owner authorization policy:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_ble_owner_policy firmware/test/test_ble_owner_policy.c \
 *      firmware/main/ble_owner_policy.c && /tmp/test_ble_owner_policy
 */
#include "ble_owner_policy.h"

#include <stdio.h>

static int fail;
#define CHECK(c, msg) do { if (!(c)) { puts("FAIL: " msg); fail++; } } while (0)

int main(void)
{
    const pk_ble_peer_id_t owner = {.type=1,.value={1,2,3,4,5,6}};
    const pk_ble_peer_id_t stranger = {.type=1,.value={7,8,9,10,11,12}};

    CHECK(pk_ble_owner_decide(false, NULL, &stranger, true, true, false) ==
              PK_BLE_OWNER_DENY,
          "first nearby paired phone was accepted without local window");
    CHECK(pk_ble_owner_decide(false, NULL, &stranger, true, true, true) ==
              PK_BLE_OWNER_CLAIM,
          "local pairing window did not authorize first owner claim");
    CHECK(pk_ble_owner_decide(true, &owner, &owner, true, true, false) ==
              PK_BLE_OWNER_ALLOW,
          "persisted owner was rejected");
    CHECK(pk_ble_owner_decide(true, &owner, &stranger, true, true, false) ==
              PK_BLE_OWNER_DENY,
          "paired stranger was accepted as owner");
    CHECK(pk_ble_owner_decide(true, &owner, &stranger, true, true, true) ==
              PK_BLE_OWNER_CLAIM,
          "physical pairing window could not replace a lost owner phone");
    CHECK(pk_ble_owner_decide(true, &owner, &owner, false, true, true) ==
              PK_BLE_OWNER_DENY,
          "plaintext owner write was accepted");
    CHECK(pk_ble_owner_decide(true, &owner, &owner, true, false, true) ==
              PK_BLE_OWNER_DENY,
          "encrypted but unbonded peer was accepted");

    const int64_t now = 100000000LL;
    const int64_t duration = 60000000LL;
    CHECK(pk_ble_owner_window_toggle_deadline(now, 0, duration) ==
              now + duration,
          "closed enrollment window did not open for one duration");
    CHECK(pk_ble_owner_window_toggle_deadline(now, now + 30000000LL,
                                               duration) == 0,
          "second tap extended an active enrollment window instead of cancelling");
    CHECK(pk_ble_owner_window_toggle_deadline(now, now - 1, duration) ==
              now + duration,
          "expired enrollment window did not reopen");
    CHECK(pk_ble_owner_window_toggle_deadline(now, now, duration) ==
              now + duration,
          "window deadline boundary was treated as still active");
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
