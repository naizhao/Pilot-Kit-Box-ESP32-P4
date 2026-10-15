#include "airband_wav_reader.h"
#include "airband_wav.h"

#include <stddef.h>
#include <string.h>

static uint16_t get_u16(const uint8_t *p)
{ return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8); }

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

pk_airband_wav_read_result_t pk_airband_wav_read_header(
    const uint8_t header[44], uint64_t actual_file_size,
    pk_airband_wav_info_t *out)
{
    if (!header || !out) return PK_AIRBAND_WAV_READ_ERR_INVALID;
    if (actual_file_size < PK_AIRBAND_WAV_HEADER_BYTES ||
        actual_file_size > PK_AIRBAND_WAV_MAX_FILE_BYTES)
        return PK_AIRBAND_WAV_READ_ERR_LENGTH;
    if (memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVEfmt ", 8) != 0 ||
        memcmp(header + 36, "data", 4) != 0)
        return PK_AIRBAND_WAV_READ_ERR_SIGNATURE;

    const uint32_t data_bytes = get_u32(header + 40);
    if (get_u32(header + 16) != 16u || get_u16(header + 20) != 1u ||
        get_u16(header + 22) != 1u || get_u32(header + 24) != 16000u ||
        get_u32(header + 28) != 32000u || get_u16(header + 32) != 2u ||
        get_u16(header + 34) != 16u)
        return PK_AIRBAND_WAV_READ_ERR_FORMAT;
    if ((data_bytes & 1u) != 0 ||
        data_bytes > PK_AIRBAND_WAV_MAX_FILE_BYTES -
                         PK_AIRBAND_WAV_HEADER_BYTES ||
        (uint64_t)44u + data_bytes > actual_file_size ||
        get_u32(header + 4) != 36u + data_bytes)
        return PK_AIRBAND_WAV_READ_ERR_LENGTH;

    const uint32_t frames = data_bytes / 2u;
    *out = (pk_airband_wav_info_t){
        .data_offset = 44u,
        .data_bytes = data_bytes,
        .sample_frames = frames,
        .duration_ms = (uint32_t)(((uint64_t)frames * 1000u) / 16000u),
    };
    return PK_AIRBAND_WAV_READ_OK;
}
