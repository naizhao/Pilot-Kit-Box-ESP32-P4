/* Validation for the fixed 44-byte WAV files emitted by airband_wav.c. */
#pragma once

#include <stdint.h>

typedef enum {
    PK_AIRBAND_WAV_READ_OK = 0,
    PK_AIRBAND_WAV_READ_ERR_INVALID,
    PK_AIRBAND_WAV_READ_ERR_SIGNATURE,
    PK_AIRBAND_WAV_READ_ERR_FORMAT,
    PK_AIRBAND_WAV_READ_ERR_LENGTH,
} pk_airband_wav_read_result_t;

typedef struct {
    uint32_t data_offset;
    uint32_t data_bytes;
    uint32_t sample_frames;
    uint32_t duration_ms;
} pk_airband_wav_info_t;

pk_airband_wav_read_result_t pk_airband_wav_read_header(
    const uint8_t header[44], uint64_t actual_file_size,
    pk_airband_wav_info_t *out);
