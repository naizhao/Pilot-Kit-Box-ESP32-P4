/* Pure monotonic-time AM gain controller; register I/O remains task-owned. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PK_AIRBAND_AM_GAIN_MAX_INDEX 38u
#define PK_AIRBAND_AM_TARGET_RSSI 142u
typedef enum {
    PK_AIRBAND_AM_GAIN_RESET = 0,
    PK_AIRBAND_AM_GAIN_HOLD,
    PK_AIRBAND_AM_GAIN_ATTACK,
    PK_AIRBAND_AM_GAIN_FAST_ATTACK,
    PK_AIRBAND_AM_GAIN_RELEASE,
    PK_AIRBAND_AM_GAIN_INVALID,
} pk_airband_am_gain_reason_t;
typedef struct {
    uint8_t applied_index;
    uint8_t requested_index;
    pk_airband_am_gain_reason_t reason;
    uint64_t last_ms;
    uint64_t hold_until_ms;
    uint64_t low_since_ms;
    uint64_t next_release_ms;
    bool low_tracking;
} pk_airband_am_gain_t;

void pk_airband_am_gain_reset(pk_airband_am_gain_t *s, unsigned applied, uint64_t now_ms);
bool pk_airband_am_gain_update(pk_airband_am_gain_t *s, bool valid, uint16_t raw, uint64_t now_ms);
/* Call only after exact register readback. A failed transaction never calls this. */
void pk_airband_am_gain_commit(pk_airband_am_gain_t *s, unsigned index, uint64_t now_ms);
int pk_airband_am_gain_db(unsigned index);
uint16_t pk_airband_am_gain_reg13(unsigned index);
int16_t pk_airband_am_reference_rssi_x2(uint16_t raw, unsigned applied_index);
const char *pk_airband_am_gain_reason_name(pk_airband_am_gain_reason_t reason);
