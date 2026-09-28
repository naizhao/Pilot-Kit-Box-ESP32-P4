/* Pure-C AIRBAND BLE audio wire codec. No RTOS/NimBLE dependency. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_BLE_PROTOCOL_VERSION 1u
#define PK_AIRBAND_BLE_SAMPLE_RATE 16000u
#define PK_AIRBAND_BLE_FRAME_SAMPLES 320u
#define PK_AIRBAND_BLE_FRAME_HEADER_BYTES 16u
#define PK_AIRBAND_BLE_FRAME_BYTES 176u
#define PK_AIRBAND_BLE_FRAGMENT_HEADER_BYTES 12u
#define PK_AIRBAND_BLE_MIN_FRAGMENT_DATA 64u

typedef struct { int16_t predictor; uint8_t step_index; } pk_airband_ima_state_t;

/* Header is LE: version, flags, stream_id, frame_seq, sample_count,
 * sample_clock, starting predictor, starting step-index, reserved. ADPCM
 * follows with the earlier sample in each byte's low nibble. */
size_t pk_airband_ble_encode_frame(pk_airband_ima_state_t *state,
                                   uint16_t stream_id, uint16_t frame_seq,
                                   uint32_t sample_clock,
                                   const int16_t pcm[PK_AIRBAND_BLE_FRAME_SAMPLES],
                                   uint8_t *out, size_t capacity);

/* mtu is the negotiated ATT MTU; usable notification payload is mtu-3.
 * A full 176-byte frame is sent without a transport header when it fits.
 * Otherwise each packet begins AF 01 + stream/seq/index/count/offset/total. */
size_t pk_airband_ble_fragment_count(size_t frame_len, uint16_t mtu);
size_t pk_airband_ble_make_fragment(uint16_t stream_id, uint16_t frame_seq,
                                    const uint8_t *frame, size_t frame_len,
                                    uint16_t mtu, size_t fragment_index,
                                    uint8_t *out, size_t capacity);
