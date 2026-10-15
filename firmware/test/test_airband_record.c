/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -D_DEFAULT_SOURCE \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_airband_record firmware/test/test_airband_record.c \
 * firmware/main/airband_wav.c firmware/main/airband_audio.c -lm && /tmp/test_airband_record
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <errno.h>
#include "freertos/task.h"
#include "pk_sdcard.h"
#include "airband_audio.h"
#include "airband_wav.h"
static char root[128];
static atomic_bool mounted = true;
static atomic_uint mount_gen = 1;
static void (*unmount_cb)(void);
static bool register_ok=true;
static bool media_error;
static int64_t now_us;
void pk_airband_recording_store_request_refresh(void) {}
bool pk_sdcard_is_mounted(void) { return mounted; }
uint32_t pk_sdcard_mount_generation(void) { return mount_gen; }
const char *pk_sdcard_mount_point(void) { return root; }
bool pk_sdcard_media_error(void) { return media_error; }
bool pk_sdcard_register_pre_unmount_cb(void (*cb)(void)) { if(!register_ok)return false;unmount_cb = cb;return true; }
int64_t esp_timer_get_time(void) { return now_us; }
static pthread_mutex_t write_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t write_cond=PTHREAD_COND_INITIALIZER;
static bool stall_write,write_entered,release_write;
static atomic_int inflight;
static atomic_uint data_write_calls;
static int tracked_fd;
static bool fail_data_write;
static bool preparation_active;
static unsigned preparation_writes, preparation_renames, preparation_truncates;
static int tracked_rename(const char *old, const char *new_path)
{ if(preparation_active) preparation_renames++; return rename(old,new_path); }
static int tracked_ftruncate(int fd, off_t size)
{ if(preparation_active) preparation_truncates++; return ftruncate(fd,size); }
static size_t real_file_write(const void *ptr,size_t size,size_t n,FILE *fp)
{
    if(preparation_active) preparation_writes++;
    if(size*n!=PK_AIRBAND_WAV_HEADER_BYTES)atomic_fetch_add(&data_write_calls,1);
    atomic_fetch_add(&inflight,1);
    pthread_mutex_lock(&write_lock);
    if(stall_write) {
        write_entered=true;tracked_fd=fileno(fp);pthread_cond_broadcast(&write_cond);
        while(!release_write)pthread_cond_wait(&write_cond,&write_lock);
    }
    pthread_mutex_unlock(&write_lock);
    size_t wrote;
    if(fail_data_write && size*n==128) { fail_data_write=false;wrote=fwrite(ptr,1,3,fp); }
    else wrote=fwrite(ptr,size,n,fp);
    atomic_fetch_sub(&inflight,1);return wrote;
}
#define fwrite real_file_write
#define rename tracked_rename
#define ftruncate tracked_ftruncate
#include "../main/airband_record.c"
#undef fwrite
#undef rename
#undef ftruncate
static int fail;
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s (%d)\n",m,__LINE__);fail++; } } while(0)
static pk_airband_record_status_t status(void) { pk_airband_record_status_t s;pk_airband_record_get(&s);return s; }
static void drain(void) { for (unsigned i=0;i<PK_AIRBAND_RECORD_QUEUE_BLOCKS+8u;i++) record_poll(); }
static unsigned u32(const unsigned char *p) { return p[0]|p[1]<<8|p[2]<<16|p[3]<<24; }
static void check_file(const char *name, unsigned bytes, int first)
{
    char path[320];snprintf(path,sizeof(path),"%s/airband/%s",root,name);
    FILE *f=fopen(path,"rb");CHECK(f!=NULL,"final WAV exists");if(!f)return;
    unsigned char h[46]={0};size_t n=fread(h,1,sizeof(h),f);fclose(f);
    CHECK(n>=44 && !memcmp(h,"RIFF",4) && !memcmp(h+8,"WAVEfmt ",8),"WAV recognizable");
    CHECK(u32(h+24)==16000 && u32(h+40)==bytes,"WAV actual byte length/rate");
    if(bytes) CHECK((int16_t)(h[44]|h[45]<<8)==first,"PCM sample preserved");
}
static void *writer_once(void *arg) { (void)arg;record_poll();return NULL; }
static atomic_bool barrier_returned;
static void *remove_card(void *arg) { (void)arg;mounted=false;unmount_cb();barrier_returned=true;return NULL; }
static void test_barrier_and_recovery(int16_t *pcm)
{
    CHECK(pk_airband_record_set_enabled(true),"start barrier session");
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"barrier first block");
    record_poll();
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"inflight block queued");
    pthread_mutex_lock(&write_lock);stall_write=true;pthread_mutex_unlock(&write_lock);
    pthread_t writer,remover;pthread_create(&writer,NULL,writer_once,NULL);
    pthread_mutex_lock(&write_lock);
    while(!write_entered)pthread_cond_wait(&write_cond,&write_lock);
    pthread_mutex_unlock(&write_lock);
    /* Holding a real I/O mutex must not delay any producer/control call. */
    /* A busy shared SD card can hold FAT for hundreds of milliseconds.  The
     * recorder must absorb at least one second of 16 kHz mono audio without
     * dropping the active transmission. */
    for(unsigned i=1;i<250;i++)CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"one-second FAT stall remains lossless");
    for(unsigned i=250;i<PK_AIRBAND_RECORD_QUEUE_BLOCKS;i++)
        CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"copy continues until finite queue capacity");
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),"slow FAT queue has finite capacity");
    CHECK(pk_airband_record_set_enabled(false),"stop does not wait for FAT");
    pthread_create(&remover,NULL,remove_card,NULL);
    while(!status().sd_missing) sched_yield();
    CHECK(!barrier_returned && inflight==1,"pre-unmount waits for inflight write");
    pthread_mutex_lock(&write_lock);release_write=true;pthread_cond_broadcast(&write_cond);pthread_mutex_unlock(&write_lock);
    pthread_join(writer,NULL);pthread_join(remover,NULL);
    CHECK(barrier_returned && inflight==0 && !status().active && status().queued_blocks==0,
          "pre-unmount returns only after all I/O closes");
    CHECK(fcntl(tracked_fd,F_GETFD)==-1 && errno==EBADF,"actual OS file descriptor released");
    stall_write=false;
    char path[320];snprintf(path,sizeof(path),"%s/airband/s0000000999-c121500-f121500000-0000000001.part.wav",root);
    const unsigned char recovered[51]={
        'R','I','F','F',36,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',0,0,0,0,
        0xd2,4,0xd3,4,0xd4,4,0xff};
    FILE *part=fopen(path,"wb");CHECK(part!=NULL,"power loss fixture");
    if(part){CHECK(fwrite(recovered,1,sizeof(recovered),part)==sizeof(recovered),"power loss fixture bytes");fclose(part);}
    const char *parts[] = {
        "s0000000999-c121500-f121500000-0000000001.part.wav",
        "s0000001000-c121500-f121500000-0000000001.part.wav",
        "s0000001001-c121500-f121500000-0000000001.part.wav",
    };
    unsigned char fixtures[3][51];
    memcpy(fixtures[0],recovered,sizeof(recovered));
    memcpy(fixtures[1],recovered,sizeof(recovered));
    fixtures[1][4]=42; fixtures[1][40]=6; /* Valid, complete checkpoint. */
    memcpy(fixtures[2],fixtures[1],sizeof(recovered));
    fixtures[2][4]=99; /* Inconsistent RIFF size must never be published. */
    for(unsigned i=1;i<3;i++) {
        snprintf(path,sizeof(path),"%s/airband/%s",root,parts[i]);
        part=fopen(path,"wb");CHECK(part!=NULL,"checkpoint fixture opened");
        if(part){CHECK(fwrite(fixtures[i],1,50,part)==50,"checkpoint fixture bytes");fclose(part);}
    }
    mounted=true;mount_gen++;
    CHECK(pk_airband_record_set_enabled(true),"explicit enable inspects pending parts");
    preparation_active=true; record_poll(); preparation_active=false;
    CHECK(pk_airband_record_set_enabled(false),"stop after part inspection");
    CHECK(!status().recovered && !status().recovery_errors && status().recovery_skipped==3,
          "all interrupted parts stay pending without claiming recovery");
    CHECK(!preparation_writes && !preparation_renames && !preparation_truncates,
          "automatic preparation never writes, renames or truncates pending parts");
    for(unsigned i=0;i<3;i++) {
        const size_t bytes=i==0?51:50;
        snprintf(path,sizeof(path),"%s/airband/%s",root,parts[i]);
        FILE *kept=fopen(path,"rb");CHECK(kept!=NULL,"pending part name preserved");
        if(kept){unsigned char actual[52];size_t got=fread(actual,1,sizeof(actual),kept);fclose(kept);
            CHECK(got==bytes && !memcmp(actual,fixtures[i],bytes),"pending part bytes preserved exactly");}
        char final[320];snprintf(final,sizeof(final),"%s/airband/%.*s-interrupted.wav",root,
            (int)(strlen(parts[i])-9),parts[i]);
        struct stat st;CHECK(stat(final,&st)!=0 && errno==ENOENT,"pending part has no published twin");
    }
}

static void test_transient_capture_gap_does_not_split_recording(int16_t *pcm)
{
    CHECK(pk_airband_record_set_enabled(true),"start transient-gap session");
    uint32_t segments_before=status().segments;
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"pre-gap block accepted");
    record_poll();
    CHECK(!pk_airband_record_submit(NULL,0,true,true,121500,121500000),"empty capture block is not audio");
    record_poll();
    CHECK(status().active && !status().interrupted,
          "one transient capture gap must not close or mark the recording dropped");
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"post-gap block accepted");
    CHECK(pk_airband_record_set_enabled(false),"stop transient-gap session");
    drain();
    CHECK(status().segments==segments_before+1,
          "transient gap remains in one recording segment");
    check_file(status().file,256,1234);
}

static void test_sustained_capture_gap_interrupts_recording(int16_t *pcm)
{
    CHECK(pk_airband_record_set_enabled(true),"start sustained-gap session");
    const pk_airband_record_status_t before=status();
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),
          "pre-gap block accepted");
    record_poll();

    for(unsigned i=1;i<PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS;i++)
        CHECK(!pk_airband_record_submit(NULL,0,true,true,121500,121500000),
              "short source gap contains no audio");
    CHECK(status().active && !status().interrupted &&
          status().dropped_samples==before.dropped_samples,
          "sub-threshold source gap remains tolerated");

    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),
          "valid PCM resets consecutive source gaps");
    record_poll();
    for(unsigned i=1;i<PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS;i++)
        CHECK(!pk_airband_record_submit(NULL,0,true,true,121500,121500000),
              "second short source gap contains no audio");
    CHECK(status().active && !status().interrupted &&
          status().dropped_samples==before.dropped_samples,
          "recovered PCM starts a fresh source-gap window");

    CHECK(!pk_airband_record_submit(NULL,0,true,true,121500,121500000),
          "threshold source gap contains no audio");
    CHECK(status().interrupted &&
          status().dropped_samples==before.dropped_samples+
              PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS*
              PK_AIRBAND_RECORD_BLOCK_SAMPLES,
          "sustained gap estimates missing samples and marks interruption");
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),
          "interrupted open call remains held off");
    drain();
    CHECK(!status().active && status().segments==before.segments+1 &&
          strstr(status().file,"-interrupted.wav"),
          "sustained gap closes and publishes the drained segment as interrupted");
    pk_airband_record_submit(NULL,0,true,false,121500,121500000);
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),
          "quiet edge rearms recording after sustained gap");
    CHECK(pk_airband_record_set_enabled(false),"stop rearmed sustained-gap session");
    drain();
}
int main(void)
{
    strcpy(root,"/tmp/pk_airband_record_XXXXXX");CHECK(mkdtemp(root)!=NULL,"temporary SD root");
    register_ok=false;CHECK(!pk_airband_record_init() && status().error,"cannot start without registered SD barrier");register_ok=true;
    pk_host_task_create_fail=1;CHECK(!pk_airband_record_init(),"task failure reported");
    pk_host_task_create_fail=0;
    pk_host_task_create_hook=record_poll;
    if(!pk_airband_record_init()){CHECK(false,"record worker initialization");return 1;}
    CHECK(!status().sd_missing,"init does not overwrite an already-running worker's mounted state");
    pk_host_task_create_hook=NULL;
    record_poll();CHECK(unmount_cb!=NULL,"pre-unmount barrier registered");
    mounted=false;unmount_cb();media_error=true;record_poll();
    CHECK(status().error && !strcmp(pk_airband_record_label(&(pk_airband_record_status_t){.error=status().error,.sd_missing=true}),"ERROR"),"FAT corruption is ERROR rather than a missing card");
    media_error=false;mounted=true;mount_gen++;record_poll();
    int16_t pcm[64];for(unsigned i=0;i<64;i++)pcm[i]=(int16_t)(1234+i);
    pk_airband_record_submit(NULL,0,true,false,121500,121500000);
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),"REC off writes nothing");
    CHECK(pk_airband_record_set_enabled(true),"REC enabled");
    record_poll();
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"open squelch copied");
    record_poll();CHECK(status().active && status().bytes==128,"worker wrote PCM");
    CHECK(!pk_airband_record_submit(pcm,64,true,false,121500,121500000),"closed squelch ends segment");
    drain();pk_airband_record_status_t s=status();CHECK(!s.active && s.segments==1,"squelch finalized one file");
    check_file(s.file,128,1234);
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"next opening starts new segment");
    CHECK(pk_airband_record_submit(pcm,64,true,true,123450,123450000),"channel change copied");
    CHECK(pk_airband_record_set_enabled(false),"stop accepted");
    CHECK(!status().enabled && status().stopping,"stop visible before close");
    CHECK(!pk_airband_record_set_enabled(true),"cannot restart while prior stop is still WRITING");
    drain();CHECK(!status().stopping && status().segments==3,"queued stop cannot be lost");
    CHECK(!pk_airband_record_submit(pcm,64,true,true,123450,123450000),
          "prepared card still rejects capture after explicit stop");
    check_file(status().file,128,1234);
    CHECK(pk_airband_record_set_enabled(true),"restart");
    for(unsigned i=0;i<PK_AIRBAND_RECORD_QUEUE_BLOCKS;i++)
        CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"bounded queue accepts capacity");
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),"overflow rejects immediately");
    CHECK(status().dropped_blocks==1 && status().dropped_samples==64,"overflow counted");
    unsigned write_calls_before=atomic_load(&data_write_calls);
    drain();CHECK(!status().active && strstr(status().file,"interrupted"),"overflow closes interrupted segment");
    CHECK(atomic_load(&data_write_calls)-write_calls_before<=PK_AIRBAND_RECORD_QUEUE_BLOCKS/32u+1u,
          "queued PCM is coalesced into sector-sized sequential writes");
    check_file(status().file,PK_AIRBAND_RECORD_QUEUE_BLOCKS*128u,1234);
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),"overflow waits for quiet edge");
    pk_airband_record_submit(NULL,0,false,true,121500,121500000);
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),"capture glitch cannot rearm an overflowed still-open call");
    pk_airband_record_submit(NULL,0,true,false,121500,121500000);
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"new squelch opening recovers");
    record_poll();mounted=false;unmount_cb();CHECK(!status().active && status().queued_blocks==0,"unmount closes all handles and clears queue");
    CHECK(!pk_airband_record_submit(pcm,64,true,true,121500,121500000),"old card PCM rejected");
    mounted=true;mount_gen++;record_poll();
    CHECK(!status().active && status().mount_generation==mount_gen,"remount generation resets old data");
    pk_airband_record_submit(NULL,0,true,false,121500,121500000);
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"new card new opening");
    pk_airband_record_set_enabled(false);drain();check_file(status().file,128,1234);
    test_barrier_and_recovery(pcm);
    test_transient_capture_gap_does_not_split_recording(pcm);
    test_sustained_capture_gap_interrupts_recording(pcm);
    CHECK(pk_airband_record_set_enabled(false) && !status().stopping,"idle stop without card must not remain WRITING");
    CHECK(pk_airband_record_set_enabled(true),"start RF discontinuity session");
    uint32_t segments_before=status().segments;
    pk_airband_record_submit(pcm,64,true,true,121500,121500000);record_poll();
    pk_airband_record_submit(pcm,64,false,true,121500,121500000);drain();
    CHECK(!status().active && status().segments==segments_before+1 &&
          strstr(status().file,"-interrupted.wav"),
          "untrusted RF publishes the drained segment as interrupted");
    pk_airband_record_submit(pcm,64,true,true,121500,121500000);record_poll();
    pk_airband_record_submit(NULL,0,true,true,121500,121500000);record_poll();
    CHECK(status().active && status().segments==segments_before+1,
          "transient capture gap keeps the current segment open");
    pk_airband_record_set_enabled(false);drain();
    CHECK(status().segments==segments_before+2,"stop closes post-gap segment once");
    CHECK(pk_airband_record_set_enabled(true),"record muted program");
    int16_t program[64],speaker[384];
    for(unsigned i=0;i<64;i++)program[i]=10000;
    pk_airband_audio_route_t route={0};pk_airband_audio_route_reset(&route);
    CHECK(pk_airband_audio_route16(&route,program,64,true,true,speaker,192)==192,"muted speaker route");
    for(unsigned i=0;i<384;i++)CHECK(speaker[i]==0,"speaker branch is silent");
    int first=program[0];CHECK(pk_airband_record_submit(program,64,true,true,121500,121500000),"mute does not suppress recorder");
    pk_airband_record_set_enabled(false);drain();check_file(status().file,128,first);
    CHECK(pk_airband_record_set_enabled(true),"start bounded API test");
    int16_t oversized[65]={0};
    segments_before=status().segments;
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),
          "pre-oversized block accepted");
    record_poll();
    CHECK(!pk_airband_record_submit(oversized,65,true,true,121500,121500000),"capture API only accepts a bounded 64-sample block");
    drain();
    CHECK(!status().active && status().segments==segments_before+1 &&
          strstr(status().file,"-interrupted.wav"),
          "oversized input publishes the drained segment as interrupted");
    pk_airband_record_set_enabled(false);drain();
    CHECK(pk_airband_record_set_enabled(true),"start short-write session");
    uint64_t before_bytes=status().bytes;
    fail_data_write=true;pk_airband_record_submit(pcm,64,true,true,121500,121500000);record_poll();
    CHECK(status().error && status().write_errors && !status().active && status().bytes==before_bytes+3,
          "short write reports actual bytes/error and closes descriptor");
    CHECK(strstr(status().file,".part.wav")!=NULL,"failed write is not published as complete");
    pk_airband_record_set_enabled(false);drain();
    mounted=false;unmount_cb();mounted=true;mount_gen++;
    CHECK(pk_airband_record_set_enabled(true),"explicit enable inspects short-write part");record_poll();
    CHECK(pk_airband_record_set_enabled(false),"stop after short-write inspection");
    CHECK(!status().recovered && !status().recovery_errors, "short-write part remains pending on remount");
    mounted=false;unmount_cb();
    char damaged[320];snprintf(damaged,sizeof(damaged),"%s/airband/s0000019999-c121500-f121500000-0000000001.part.wav",root);
    char source[320];snprintf(source,sizeof(source),"%s/airband/s0000000999-c121500-f121500000-0000000001.part.wav",root);
    FILE *src=fopen(source,"rb"),*bad=fopen(damaged,"wb");
    CHECK(src && bad,"damaged part fixture opened");
    if(src && bad) {
        unsigned char h[44];CHECK(fread(h,1,44,src)==44,"damaged part header");
        CHECK(fwrite(h,1,44,bad)==44 && fflush(bad)==0,"damaged part copy");
        CHECK(ftruncate(fileno(bad),PK_AIRBAND_WAV_MAX_FILE_BYTES+1)==0,"oversized part extent");
    }
    if(src)fclose(src);if(bad)fclose(bad);
    char invalid_format[320];snprintf(invalid_format,sizeof(invalid_format),"%s/airband/s0000020000-c121500-f121500000-0000000001.part.wav",root);
    src=fopen(source,"rb");bad=fopen(invalid_format,"wb");
    CHECK(src && bad,"corrupt format fixture opened");
    if(src && bad) {
        unsigned char h[50];CHECK(fread(h,1,50,src)==50,"corrupt format source");h[23]=1;
        CHECK(fwrite(h,1,50,bad)==50,"corrupt high channel byte written");
    }
    if(src)fclose(src);if(bad)fclose(bad);
    /* 只读恢复（调查 §4-4）：尾部与 checkpoint 头不一致的两种 part——奇数
     * 半采样、头声明超出实际文件——必须原样保留且不发布。修复它们需要写卡，
     * 而自动准备路径已不允许写卡；发布一个尾部撒谎的 WAV 比留一个 .part.wav
     * 更糟。两种都计入 recovery_skipped，不计错误、不阻断录音就绪。 */
    char odd_part[320];snprintf(odd_part,sizeof(odd_part),"%s/airband/s0000020001-c121500-f121500000-0000000001.part.wav",root);
    FILE *op=fopen(odd_part,"wb");
    CHECK(op!=NULL,"odd half-sample fixture opened");
    if(op){
        const unsigned char odd[49]={
            'R','I','F','F',41,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
            1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',5,0,0,0,
            0xd2,4,0xd3,4,0xd4};
        CHECK(fwrite(odd,1,sizeof(odd),op)==sizeof(odd),"odd half-sample fixture bytes");
        fclose(op);
    }
    char overlong_part[320];snprintf(overlong_part,sizeof(overlong_part),"%s/airband/s0000020002-c121500-f121500000-0000000001.part.wav",root);
    FILE *lp=fopen(overlong_part,"wb");
    CHECK(lp!=NULL,"header-exceeds-file fixture opened");
    if(lp){
        unsigned char over[100]={
            'R','I','F','F',136,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
            1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',100,0,0,0};
        CHECK(fwrite(over,1,sizeof(over),lp)==sizeof(over),"header-exceeds-file fixture bytes");
        fclose(lp);
    }
    uint32_t skipped_before=status().recovery_skipped;
    mounted=true;mount_gen++;
    CHECK(pk_airband_record_set_enabled(true),"explicit enable permits corrupt-part inspection");record_poll();struct stat damaged_stat;
    CHECK(!status().recovery_errors && !status().error,"pending content never blocks a new recording");
    CHECK(status().recovery_skipped==skipped_before+8,"every existing part remains pending on this mount");
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"pending files do not block normal recording");
    pk_airband_record_set_enabled(false);drain();check_file(status().file,128,1234);
    CHECK(stat(damaged,&damaged_stat)==0 && damaged_stat.st_size==PK_AIRBAND_WAV_MAX_FILE_BYTES+1,
          "failed recovery must not truncate corrupt input");
    CHECK(stat(invalid_format,&damaged_stat)==0 && damaged_stat.st_size==50,"invalid PCM format is preserved for inspection");
    CHECK(stat(odd_part,&damaged_stat)==0 && damaged_stat.st_size==49,"odd half-sample part stays unpublished");
    CHECK(stat(overlong_part,&damaged_stat)==0 && damaged_stat.st_size==100,"header-exceeds-file part stays unpublished");
    char twin[320];struct stat twin_stat;
    snprintf(twin,sizeof(twin),"%s/airband/s0000020001-c121500-f121500000-0000000001-interrupted.wav",root);
    CHECK(stat(twin,&twin_stat)!=0,"skipped parts have no published twin");
    printf("%s: record files retained in %s\n",fail?"FAILED":"OK",root);return fail!=0;
}
