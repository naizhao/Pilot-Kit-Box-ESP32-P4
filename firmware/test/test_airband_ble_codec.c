/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 * -o /tmp/test_airband_ble_codec firmware/test/test_airband_ble_codec.c \
 * firmware/main/airband_ble_codec.c && /tmp/test_airband_ble_codec
 *
 * Catches: incompatible IMA nibble order/state, non-versioned wire header,
 * sample-clock/frame-sequence drift, and fragments which cannot be reassembled.
 */
#include "airband_ble_codec.h"
#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); fail++; } } while (0)

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                                (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void test_zero_vector_and_header(void)
{
    int16_t pcm[PK_AIRBAND_BLE_FRAME_SAMPLES] = {0};
    uint8_t out[PK_AIRBAND_BLE_FRAME_BYTES];
    pk_airband_ima_state_t state = {0};
    size_t n = pk_airband_ble_encode_frame(&state, 0x1234, 0x5678,
                                            0x89abcdefu, pcm, out, sizeof(out));
    CHECK(n == 176, "20ms frame must be exactly 176 bytes");
    CHECK(out[0] == 1 && out[1] == 0, "version/flags changed");
    CHECK(le16(out+2) == 0x1234 && le16(out+4) == 0x5678,
          "stream/frame fields are not little endian");
    CHECK(le16(out+6) == 320 && le32(out+8) == 0x89abcdefu,
          "sample count or sample clock wrong");
    CHECK(le16(out+12) == 0 && out[14] == 0 && out[15] == 0,
          "initial IMA state/reserved wrong");
    for (size_t i=16;i<n;i++) CHECK(out[i] == 0, "zero PCM did not encode to zero nibbles");
}

static void test_fixed_ima_vector(void)
{
    int16_t pcm[PK_AIRBAND_BLE_FRAME_SAMPLES] = {0};
    static const int16_t first[] = {0,1000,-1000,2000,-2000,3000,-3000,4000,
                                    -4000,8000,-8000,12000,-12000,16000,-16000,0};
    memcpy(pcm, first, sizeof(first));
    uint8_t out[PK_AIRBAND_BLE_FRAME_BYTES];
    pk_airband_ima_state_t state = {0};
    (void)pk_airband_ble_encode_frame(&state, 1, 0, 0, pcm, out, sizeof(out));
    static const uint8_t expected_first_16_adpcm[] = {
        0x70,0x7f,0x7f,0x7f,0x7f,0x6e,0x4c,0x2c,
        0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08
    };
    CHECK(memcmp(out+16, expected_first_16_adpcm,
                 sizeof(expected_first_16_adpcm)) == 0,
          "IMA fixed vector/nibble order changed");
}

static void test_fragment_contract(void)
{
    uint8_t frame[PK_AIRBAND_BLE_FRAME_BYTES];
    for (size_t i=0;i<sizeof(frame);i++) frame[i]=(uint8_t)i;
    uint8_t rebuilt[sizeof(frame)]; memset(rebuilt,0,sizeof(rebuilt));
    const uint16_t mtu = 100; /* ATT payload 97; 12-byte fragment header => 85 */
    size_t count = pk_airband_ble_fragment_count(sizeof(frame), mtu);
    CHECK(count == 3, "MTU 100 must produce three fragments");
    for (size_t i=0;i<count;i++) {
        uint8_t packet[100];
        size_t n=pk_airband_ble_make_fragment(0x2244,0x0102,frame,sizeof(frame),
                                              mtu,i,packet,sizeof(packet));
        CHECK(n > PK_AIRBAND_BLE_FRAGMENT_HEADER_BYTES && n <= mtu-3,
              "fragment exceeds ATT payload");
        CHECK(packet[0] == 0xaf && packet[1] == 1 && le16(packet+2)==0x2244 &&
              le16(packet+4)==0x0102 && packet[6]==i && packet[7]==count,
              "fragment identity/header wrong");
        uint16_t off=le16(packet+8), total=le16(packet+10);
        CHECK(total==sizeof(frame) && off+n-12<=sizeof(frame),"fragment bounds wrong");
        memcpy(rebuilt+off,packet+12,n-12);
    }
    CHECK(memcmp(rebuilt,frame,sizeof(frame))==0,"fragments did not reassemble exactly");
    CHECK(pk_airband_ble_fragment_count(sizeof(frame), 50) == 0,
          "unsafe low MTU must fail closed");
}

int main(void)
{
    test_zero_vector_and_header(); test_fixed_ima_vector(); test_fragment_contract();
    puts(fail ? "FAILED" : "OK"); return fail != 0;
}
