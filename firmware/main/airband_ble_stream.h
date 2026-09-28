/* Pure-C sender-side stream state; integration owns the RTOS raw PCM queue. */
#pragma once
#include "airband_ble_codec.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_BLE_QUEUE_FRAMES 4u
typedef enum { PK_AIRBAND_BLE_ROUTE_DEVICE=0, PK_AIRBAND_BLE_ROUTE_PHONE=1,
               PK_AIRBAND_BLE_ROUTE_MUTE=2 } pk_airband_ble_route_t;
typedef enum { PK_BLE_TX_NONE=0, PK_BLE_TX_AUDIO=1,
               PK_BLE_TX_PRIORITY=2 } pk_ble_tx_choice_t;
typedef struct { bool priority_pending; uint16_t audio_fragments; }
    pk_ble_tx_scheduler_t;
typedef struct {
    uint16_t stream_id,frame_seq;uint32_t sample_clock;
    uint8_t data[PK_AIRBAND_BLE_FRAME_BYTES];
} pk_airband_ble_packet_t;
typedef struct {
    uint32_t encoded,sent,dropped,congested,mtu_paused;
} pk_airband_ble_stats_t;
typedef struct {
    bool connected,subscribed;uint16_t mtu;pk_airband_ble_route_t route;
    uint16_t stream_id,next_seq;uint32_t sample_clock;
    pk_airband_ima_state_t ima;
    int16_t partial[PK_AIRBAND_BLE_FRAME_SAMPLES];size_t partial_samples;
    pk_airband_ble_packet_t queue[PK_AIRBAND_BLE_QUEUE_FRAMES];
    unsigned head,tail,queued;pk_airband_ble_stats_t stats;
} pk_airband_ble_stream_t;

void pk_airband_ble_stream_init(pk_airband_ble_stream_t *s);
void pk_airband_ble_stream_link(pk_airband_ble_stream_t *s,bool connected,
                                bool subscribed,uint16_t mtu);
bool pk_airband_ble_stream_set_route(pk_airband_ble_stream_t *s,
                                     pk_airband_ble_route_t route);
void pk_airband_ble_stream_break(pk_airband_ble_stream_t *s);
bool pk_airband_ble_stream_remote_mute(const pk_airband_ble_stream_t *s);
bool pk_airband_ble_stream_ingest(pk_airband_ble_stream_t *s,
                                  const int16_t *pcm,size_t samples);
bool pk_airband_ble_stream_pop(pk_airband_ble_stream_t *s,
                               pk_airband_ble_packet_t *out);
void pk_ble_tx_scheduler_init(pk_ble_tx_scheduler_t *s);
void pk_ble_tx_scheduler_priority_begin(pk_ble_tx_scheduler_t *s);
void pk_ble_tx_scheduler_priority_end(pk_ble_tx_scheduler_t *s);
void pk_ble_tx_scheduler_audio_begin(pk_ble_tx_scheduler_t *s,uint16_t fragments);
pk_ble_tx_choice_t pk_ble_tx_scheduler_next(const pk_ble_tx_scheduler_t *s);
void pk_ble_tx_scheduler_audio_complete(pk_ble_tx_scheduler_t *s,bool success);
