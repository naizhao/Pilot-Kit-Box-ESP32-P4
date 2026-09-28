#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t version;
    uint32_t channel_khz;
    uint32_t carrier_hz;
    int16_t squelch_dbm_x2;
    uint16_t hysteresis_db_x2;
    uint8_t volume, muted, squelch_enabled, reserved;
} pk_airband_config_t;

/* Only durable settings; scans and test tones are deliberately absent. */
void pk_airband_config_load(void);
pk_airband_config_t pk_airband_config_get(void);
void pk_airband_config_tuned(uint32_t channel_khz, uint32_t carrier_hz);
void pk_airband_config_volume(uint8_t volume);
void pk_airband_config_muted(bool muted);
void pk_airband_config_squelch(bool enabled, int16_t threshold, uint16_t hysteresis);
/* Called by the low priority persistence task; never from the audio path. */
void pk_airband_config_poll(uint32_t now_ms);
bool pk_airband_config_start(void);
