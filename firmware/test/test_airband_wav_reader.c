/*
 * cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
 *   -o /tmp/test_airband_wav_reader \
 *   firmware/test/test_airband_wav_reader.c \
 *   firmware/main/airband_wav_reader.c && \
 *   /tmp/test_airband_wav_reader
 */
#include "airband_wav_reader.h"
#include "airband_wav.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void valid_header(uint8_t h[44], uint32_t data_bytes)
{
    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36u + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16u);
    put16(h + 20, 1u);
    put16(h + 22, 1u);
    put32(h + 24, 16000u);
    put32(h + 28, 32000u);
    put16(h + 32, 2u);
    put16(h + 34, 16u);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

static void test_valid_project_wav_reports_literal_duration(void)
{
    uint8_t h[44];
    valid_header(h, 32000u);
    pk_airband_wav_info_t got = {0};
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_OK,
          "one second project WAV must validate");
    CHECK(got.data_offset == 44u, "project payload must begin at byte 44");
    CHECK(got.data_bytes == 32000u, "payload length must come from header");
    CHECK(got.sample_frames == 16000u, "S16 mono byte count must become frames");
    CHECK(got.duration_ms == 1000u, "32000 bytes at 16 kHz mono S16 is 1000 ms");

    valid_header(h, 16000u);
    CHECK(pk_airband_wav_read_header(h, 16052u, &got) ==
              PK_AIRBAND_WAV_READ_OK,
          "trailing bytes after declared project payload are tolerated");
    CHECK(got.duration_ms == 500u, "duration must use declared audio only");
}

static void test_wrong_audio_contract_is_rejected(void)
{
    uint8_t h[44];
    pk_airband_wav_info_t got;

    valid_header(h, 32000u); h[0] = 'R'; h[1] = 'O';
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_SIGNATURE, "wrong RIFF must fail");

    valid_header(h, 32000u); put16(h + 20, 3u);
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_FORMAT, "non-PCM must fail");
    valid_header(h, 32000u); put16(h + 22, 2u);
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_FORMAT, "stereo must fail");
    valid_header(h, 32000u); put32(h + 24, 48000u);
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_FORMAT, "48 kHz must fail");
    valid_header(h, 32000u); put16(h + 34, 24u);
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_FORMAT, "24-bit must fail");
    valid_header(h, 32000u); put32(h + 28, 16000u);
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_FORMAT, "wrong byte rate must fail");
    valid_header(h, 32000u); put16(h + 32, 1u);
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_FORMAT, "wrong block align must fail");
}

static void test_impossible_lengths_are_rejected(void)
{
    uint8_t h[44];
    pk_airband_wav_info_t got;
    valid_header(h, 32001u);
    CHECK(pk_airband_wav_read_header(h, 32045u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_LENGTH,
          "odd S16 payload length must fail");
    valid_header(h, 32000u);
    CHECK(pk_airband_wav_read_header(h, 32043u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_LENGTH,
          "declared payload extending beyond EOF must fail");
    valid_header(h, 32000u); put32(h + 4, 35u + 32000u);
    CHECK(pk_airband_wav_read_header(h, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_LENGTH,
          "RIFF size inconsistent with data length must fail");
    CHECK(pk_airband_wav_read_header(h, 43u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_LENGTH,
          "file shorter than fixed header must fail");
    CHECK(pk_airband_wav_read_header(NULL, 32044u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_INVALID,
          "NULL header must fail without dereference");
    CHECK(pk_airband_wav_read_header(h, 32044u, NULL) ==
              PK_AIRBAND_WAV_READ_ERR_INVALID,
          "NULL output must fail without dereference");
}

static void test_files_larger_than_writer_contract_are_rejected(void)
{
    uint8_t h[44];
    pk_airband_wav_info_t got;
    const uint32_t too_large =
        PK_AIRBAND_WAV_MAX_FILE_BYTES - PK_AIRBAND_WAV_HEADER_BYTES + 2u;
    valid_header(h, too_large);
    CHECK(pk_airband_wav_read_header(
              h, (uint64_t)PK_AIRBAND_WAV_HEADER_BYTES + too_large, &got) ==
              PK_AIRBAND_WAV_READ_ERR_LENGTH,
          "reader must reject files the project writer can never publish");
    valid_header(h, 32000u);
    CHECK(pk_airband_wav_read_header(
              h, (uint64_t)PK_AIRBAND_WAV_MAX_FILE_BYTES + 1u, &got) ==
              PK_AIRBAND_WAV_READ_ERR_LENGTH,
          "actual file length above the writer limit must fail even with a small header");
}

int main(void)
{
    test_valid_project_wav_reports_literal_duration();
    test_wrong_audio_contract_is_rejected();
    test_impossible_lengths_are_rejected();
    test_files_larger_than_writer_contract_are_rejected();
    if (failures) return 1;
    puts("airband WAV reader tests PASS");
    return 0;
}
