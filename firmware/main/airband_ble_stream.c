#include "airband_ble_stream.h"
#include <string.h>

static bool active(const pk_airband_ble_stream_t *s)
{
 return s->connected && s->subscribed &&
        s->route == PK_AIRBAND_BLE_ROUTE_PHONE;
}
static bool phone_usable(const pk_airband_ble_stream_t *s)
{
 return s->connected && s->subscribed &&
        pk_airband_ble_fragment_count(PK_AIRBAND_BLE_FRAME_BYTES,s->mtu) != 0;
}

void pk_airband_ble_stream_init(pk_airband_ble_stream_t *s)
{
 if (!s) return;
 memset(s, 0, sizeof(*s));
 s->mtu = 23;
 s->stream_id = 1;
}

void pk_airband_ble_stream_break(pk_airband_ble_stream_t *s)
{
 if (!s) return;
 s->stream_id++;
 if (!s->stream_id) s->stream_id = 1;
 s->next_seq = 0;
 s->sample_clock = 0;
 s->ima = (pk_airband_ima_state_t){0};
 s->partial_samples = 0;
 s->head = s->tail = s->queued = 0;
}

void pk_airband_ble_stream_link(pk_airband_ble_stream_t *s,bool connected,bool sub,uint16_t mtu)
{
 if (!s) return;
 bool changed = s->connected != connected || s->subscribed != sub || s->mtu != mtu;
 s->connected = connected;
 s->subscribed = sub;
 s->mtu = mtu;
 if (changed) pk_airband_ble_stream_break(s);
 if (!connected || (s->route == PK_AIRBAND_BLE_ROUTE_PHONE && !phone_usable(s)))
   s->route = PK_AIRBAND_BLE_ROUTE_DEVICE;
}

bool pk_airband_ble_stream_set_route(pk_airband_ble_stream_t *s,pk_airband_ble_route_t route)
{
 if (!s || route > PK_AIRBAND_BLE_ROUTE_MUTE) return false;
 if (route == PK_AIRBAND_BLE_ROUTE_PHONE && !phone_usable(s)) {
   if (s->connected && s->subscribed) s->stats.mtu_paused++;
   return false;
 }
 if (s->route != route) {
   s->route = route;
   pk_airband_ble_stream_break(s);
 }
 return true;
}

bool pk_airband_ble_stream_remote_mute(const pk_airband_ble_stream_t *s)
{
 return s && s->connected && (s->route == PK_AIRBAND_BLE_ROUTE_PHONE ||
                              s->route == PK_AIRBAND_BLE_ROUTE_MUTE);
}

static bool emit(pk_airband_ble_stream_t *s)
{
 if (s->queued == PK_AIRBAND_BLE_QUEUE_FRAMES) {
   s->stats.dropped += s->queued + 1u;
   pk_airband_ble_stream_break(s);
   return false;
 }
 pk_airband_ble_packet_t *p = &s->queue[s->head];
 p->stream_id = s->stream_id;
 p->frame_seq = s->next_seq;
 p->sample_clock = s->sample_clock;
 if (!pk_airband_ble_encode_frame(&s->ima,p->stream_id,p->frame_seq,p->sample_clock,
                                  s->partial,p->data,sizeof(p->data))) return false;
 s->head = (s->head + 1u) % PK_AIRBAND_BLE_QUEUE_FRAMES;
 s->queued++;
 s->next_seq++;
 s->sample_clock += PK_AIRBAND_BLE_FRAME_SAMPLES;
 s->stats.encoded++;
 return true;
}

bool pk_airband_ble_stream_ingest(pk_airband_ble_stream_t *s,const int16_t *pcm,size_t n)
{
 if (!s || (!pcm && n) || !active(s)) return false;
 if (pk_airband_ble_fragment_count(PK_AIRBAND_BLE_FRAME_BYTES,s->mtu) == 0) {
   s->stats.mtu_paused++;
   return false;
 }
 bool produced = false;
 while (n) {
   size_t take = PK_AIRBAND_BLE_FRAME_SAMPLES - s->partial_samples;
   if (take > n) take = n;
   memcpy(s->partial + s->partial_samples,pcm,take * sizeof(*pcm));
   s->partial_samples += take;
   pcm += take;
   n -= take;
   if (s->partial_samples == PK_AIRBAND_BLE_FRAME_SAMPLES) {
     bool ok = emit(s);
     s->partial_samples = 0;
     produced |= ok;
     if (!ok) break;
   }
 }
 return produced;
}

bool pk_airband_ble_stream_pop(pk_airband_ble_stream_t *s,pk_airband_ble_packet_t *out)
{
 if (!s || !out || !s->queued) return false;
 *out = s->queue[s->tail];
 s->tail = (s->tail + 1u) % PK_AIRBAND_BLE_QUEUE_FRAMES;
 s->queued--;
 return true;
}

void pk_ble_tx_scheduler_init(pk_ble_tx_scheduler_t *s)
{ if (s) *s = (pk_ble_tx_scheduler_t){0}; }
void pk_ble_tx_scheduler_priority_begin(pk_ble_tx_scheduler_t *s)
{ if (s) s->priority_pending = true; }
void pk_ble_tx_scheduler_priority_end(pk_ble_tx_scheduler_t *s)
{ if (s) s->priority_pending = false; }
void pk_ble_tx_scheduler_audio_begin(pk_ble_tx_scheduler_t *s,uint16_t fragments)
{ if (s) s->audio_fragments = fragments; }
pk_ble_tx_choice_t pk_ble_tx_scheduler_next(const pk_ble_tx_scheduler_t *s)
{
 if (!s) return PK_BLE_TX_NONE;
 if (s->priority_pending) return PK_BLE_TX_PRIORITY;
 return s->audio_fragments ? PK_BLE_TX_AUDIO : PK_BLE_TX_NONE;
}
void pk_ble_tx_scheduler_audio_complete(pk_ble_tx_scheduler_t *s,bool success)
{
 if (!s) return;
 if (!success) s->audio_fragments = 0;
 else if (s->audio_fragments) s->audio_fragments--;
}
