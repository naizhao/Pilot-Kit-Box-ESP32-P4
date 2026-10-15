/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -D_DEFAULT_SOURCE \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_airband_record_stop firmware/test/test_airband_record_stop.c \
 * firmware/main/airband_wav.c && /tmp/test_airband_record_stop
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pk_sdcard.h"

static char root[] = "/tmp/pk_airband_record_stop_XXXXXX";
void pk_airband_recording_store_request_refresh(void) {}
bool pk_sdcard_is_mounted(void) { return true; }
uint32_t pk_sdcard_mount_generation(void) { return 1u; }
const char *pk_sdcard_mount_point(void) { return root; }
bool pk_sdcard_media_error(void) { return false; }
bool pk_sdcard_register_pre_unmount_cb(void (*cb)(void)) { return cb != NULL; }
int64_t esp_timer_get_time(void) { return 0; }

/* Real recorder and real filesystem; only the platform scheduler is stubbed.
 * Poll exactly the writer body used by the registered task, without a thread. */
#include "../main/airband_record.c"
static int fail;
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m);fail++; } } while (0)
static pk_airband_record_status_t status(void)
{
    pk_airband_record_status_t value;
    pk_airband_record_get(&value);
    return value;
}
static void drain(void)
{
    for (unsigned i=0;i<PK_AIRBAND_RECORD_QUEUE_BLOCKS+8u;i++) record_poll();
}
static void overflow(const int16_t *pcm)
{
    for (unsigned i=0;i<PK_AIRBAND_RECORD_QUEUE_BLOCKS;i++)
        CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),
              "queue accepts PCM up to capacity");
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),
          "overflow interrupts recording");
}
static void check_stopped(unsigned losses)
{
    const pk_airband_record_status_t stopped=status();
    CHECK(!stopped.enabled && !stopped.active && !stopped.stopping &&
              !strcmp(pk_airband_record_label(&stopped),"IDLE"),
          "stopped interrupted recording reports IDLE instead of GAP");
    CHECK(stopped.interrupted && stopped.dropped_blocks==losses &&
              stopped.dropped_samples==losses*64u &&
              strstr(stopped.file,"-interrupted.wav"),
          "stop preserves loss counters and interrupted filename");
    char path[320];
    snprintf(path,sizeof(path),"%s/airband/%s",root,stopped.file);
    struct stat st;
    CHECK(stat(path,&st)==0 && st.st_size==44+512*128,
          "interrupted WAV is published with all accepted PCM bytes");
}
int main(void)
{
    if (!mkdtemp(root) || !pk_airband_record_init()) return 1;
    record_poll();
    int16_t pcm[64]={1234};
    CHECK(pk_airband_record_set_enabled(true),"enable recording");
    record_poll();
    overflow(pcm);
    CHECK(pk_airband_record_set_enabled(false),"stop with queued interrupted PCM");
    const pk_airband_record_status_t stopping=status();
    CHECK(stopping.stopping && !strcmp(pk_airband_record_label(&stopping),"WRITING"),
          "pending interrupted PCM reports WRITING until saved");
    drain();
    check_stopped(1);
    CHECK(pk_airband_record_set_enabled(true),"restart recording");
    const pk_airband_record_status_t armed=status();
    CHECK(armed.enabled && !armed.interrupted && armed.dropped_blocks==1 &&
              !strcmp(pk_airband_record_label(&armed),"ARMED"),
          "restart clears current warning but retains historical counters");
    overflow(pcm);
    drain();
    const pk_airband_record_status_t gap=status();
    CHECK(!gap.active && gap.enabled && gap.interrupted &&
              !strcmp(pk_airband_record_label(&gap),"GAP"),
          "armed interrupted recording must continue to warn");
    CHECK(pk_airband_record_set_enabled(false),"stop already drained GAP");
    check_stopped(2);
    printf("%s: evidence retained in %s\n",fail?"FAILED":"OK",root);
    return fail!=0;
}
