/* ES8311 codec control interface over the project-owned I2C0 lock. */
#pragma once

#include <stdint.h>

#include "audio_codec_ctrl_if.h"

/* Adds the codec device once during init. addr7 is the real 7-bit address. */
const audio_codec_ctrl_if_t *pk_codec_ctrl_init(uint8_t addr7);
