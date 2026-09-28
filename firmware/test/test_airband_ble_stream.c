/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 * -o /tmp/test_airband_ble_stream firmware/test/test_airband_ble_stream.c \
 * firmware/main/airband_ble_stream.c firmware/main/airband_ble_codec.c && \
 * /tmp/test_airband_ble_stream
 *
 * Catches: encoding while disconnected/unsubscribed, stale audio surviving a
 * route/capture break, incorrect route overlay, and audio winning over GDL90.
 */
#include "airband_ble_stream.h"
#include <stdio.h>
#include <string.h>
static int fail;
#define CHECK(c,m) do {if(!(c)){fprintf(stderr,"FAIL: %s\n",m);fail++;}}while(0)

static void fill(int16_t *p,size_t n,int16_t v){for(size_t i=0;i<n;i++)p[i]=v;}

static void test_route_and_early_gate(void)
{
    pk_airband_ble_stream_t s; pk_airband_ble_stream_init(&s);
    int16_t pcm[64]; fill(pcm,64,1000);
    CHECK(!pk_airband_ble_stream_ingest(&s,pcm,64),"idle route encoded audio");
    pk_airband_ble_stream_link(&s,true,true,247);
    CHECK(!pk_airband_ble_stream_ingest(&s,pcm,64),"DEVICE route encoded audio");
    CHECK(pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_PHONE),"valid PHONE route rejected");
    CHECK(pk_airband_ble_stream_remote_mute(&s),"PHONE must overlay-mute device speaker");
    for(int i=0;i<5;i++) (void)pk_airband_ble_stream_ingest(&s,pcm,64);
    CHECK(s.stats.encoded==1 && s.queued==1,"five 64-sample chunks did not make one frame");
    CHECK(pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_DEVICE),"DEVICE rejected");
    CHECK(!pk_airband_ble_stream_remote_mute(&s) && s.queued==0 && s.partial_samples==0,
          "DEVICE route did not stop/flush phone stream");
    CHECK(pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_MUTE),"MUTE rejected");
    CHECK(pk_airband_ble_stream_remote_mute(&s),"MUTE must overlay-mute speaker");
    pk_airband_ble_stream_link(&s,false,false,23);
    CHECK(!pk_airband_ble_stream_remote_mute(&s),"disconnect left remote mute latched");
}

static void test_phone_route_fails_safe_when_transport_disappears(void)
{
    pk_airband_ble_stream_t s; pk_airband_ble_stream_init(&s);
    pk_airband_ble_stream_link(&s,true,true,50);
    CHECK(!pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_PHONE) &&
          s.route==PK_AIRBAND_BLE_ROUTE_DEVICE && !pk_airband_ble_stream_remote_mute(&s),
          "unsafe MTU accepted PHONE and muted the only audible route");
    pk_airband_ble_stream_link(&s,true,true,247);
    CHECK(pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_PHONE),"usable PHONE rejected");
    pk_airband_ble_stream_link(&s,true,false,247);
    CHECK(s.route==PK_AIRBAND_BLE_ROUTE_DEVICE && !pk_airband_ble_stream_remote_mute(&s),
          "CCCD unsubscribe left PHONE overlay mute latched");
}

static void test_break_changes_stream_and_clock(void)
{
    pk_airband_ble_stream_t s; pk_airband_ble_stream_init(&s);
    pk_airband_ble_stream_link(&s,true,true,247);
    CHECK(pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_PHONE),"PHONE rejected");
    int16_t pcm[320]; fill(pcm,320,123);
    CHECK(pk_airband_ble_stream_ingest(&s,pcm,320),"first frame not encoded");
    pk_airband_ble_packet_t a,b;
    CHECK(pk_airband_ble_stream_pop(&s,&a),"first frame missing");
    CHECK(a.frame_seq==0 && a.sample_clock==0,"first frame clock not zero based");
    uint16_t old=a.stream_id;
    pk_airband_ble_stream_break(&s);
    CHECK(pk_airband_ble_stream_ingest(&s,pcm,320),"post-break frame not encoded");
    CHECK(pk_airband_ble_stream_pop(&s,&b),"post-break frame missing");
    CHECK(b.stream_id!=old && b.frame_seq==0 && b.sample_clock==0,
          "break did not create a fresh stream timeline");
}

static void test_queue_drop_restarts_instead_of_playing_old_audio(void)
{
    pk_airband_ble_stream_t s; pk_airband_ble_stream_init(&s);
    pk_airband_ble_stream_link(&s,true,true,247);
    CHECK(pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_PHONE),"PHONE rejected");
    int16_t pcm[320]; fill(pcm,320,99);
    for(unsigned i=0;i<PK_AIRBAND_BLE_QUEUE_FRAMES;i++)
        CHECK(pk_airband_ble_stream_ingest(&s,pcm,320),"queue filled too early");
    uint16_t old=s.stream_id;
    CHECK(!pk_airband_ble_stream_ingest(&s,pcm,320),"overflow was not reported");
    CHECK(s.stats.dropped==PK_AIRBAND_BLE_QUEUE_FRAMES+1 && s.queued==0 &&
          s.stream_id!=old,"overflow retained stale queued frames");
}

static void test_sequence_and_sample_clock_wrap_are_wire_visible(void)
{
    pk_airband_ble_stream_t s; pk_airband_ble_stream_init(&s);
    pk_airband_ble_stream_link(&s,true,true,247);
    CHECK(pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_PHONE),"PHONE rejected");
    s.next_seq=UINT16_MAX;s.sample_clock=UINT32_MAX-319u;
    int16_t pcm[320]={0};pk_airband_ble_packet_t a,b;
    CHECK(pk_airband_ble_stream_ingest(&s,pcm,320)&&pk_airband_ble_stream_pop(&s,&a),
          "pre-wrap frame missing");
    CHECK(pk_airband_ble_stream_ingest(&s,pcm,320)&&pk_airband_ble_stream_pop(&s,&b),
          "post-wrap frame missing");
    CHECK(a.frame_seq==UINT16_MAX&&a.sample_clock==UINT32_MAX-319u&&
          b.frame_seq==0&&b.sample_clock==0,
          "sequence/sample clock wrap was not modulo wire width");
}

static void test_mtu_and_priority(void)
{
    pk_airband_ble_stream_t s; pk_airband_ble_stream_init(&s);
    pk_airband_ble_stream_link(&s,true,true,50);
    CHECK(!pk_airband_ble_stream_set_route(&s,PK_AIRBAND_BLE_ROUTE_PHONE),
          "low MTU PHONE route was accepted");
    int16_t pcm[320]={0};
    CHECK(!pk_airband_ble_stream_ingest(&s,pcm,320) && s.stats.mtu_paused==1,
          "low MTU did not pause before encoding");
    pk_ble_tx_scheduler_t tx; pk_ble_tx_scheduler_init(&tx);
    pk_ble_tx_scheduler_audio_begin(&tx,50); /* deep fragmented-audio backlog */
    pk_ble_tx_scheduler_priority_begin(&tx);
    CHECK(pk_ble_tx_scheduler_next(&tx)==PK_BLE_TX_PRIORITY,
          "audio backlog preempted heartbeat/traffic");
    pk_ble_tx_scheduler_priority_end(&tx);
    CHECK(pk_ble_tx_scheduler_next(&tx)==PK_BLE_TX_AUDIO,"audio did not resume");
    pk_ble_tx_scheduler_audio_complete(&tx,false); /* ENOMEM/congestion */
    CHECK(pk_ble_tx_scheduler_next(&tx)==PK_BLE_TX_NONE,
          "failed audio retained stale fragments");
    pk_ble_tx_scheduler_priority_begin(&tx);
    CHECK(pk_ble_tx_scheduler_next(&tx)==PK_BLE_TX_PRIORITY,
          "audio failure blocked next heartbeat/traffic deadline");
}

int main(void)
{
 test_route_and_early_gate(); test_phone_route_fails_safe_when_transport_disappears();
 test_break_changes_stream_and_clock();
 test_queue_drop_restarts_instead_of_playing_old_audio();
 test_sequence_and_sample_clock_wrap_are_wire_visible(); test_mtu_and_priority();
 puts(fail?"FAILED":"OK"); return fail!=0;
}
