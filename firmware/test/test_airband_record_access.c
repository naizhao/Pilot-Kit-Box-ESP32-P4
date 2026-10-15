/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -D_DEFAULT_SOURCE \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_airband_record_access firmware/test/test_airband_record_access.c \
 * firmware/main/airband_wav.c && /tmp/test_airband_record_access
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "pk_sdcard.h"
static char root[]="/tmp/pk_record_access_XXXXXX";
static uint32_t generation=1;
static int64_t now;
static unsigned mkdir_calls,opendir_calls,recovery_opens;
static unsigned refresh_requests;
static bool fail_mkdir,fail_opendir,fail_recovery;
static bool fail_read,fail_close;
static size_t tracked_fread(void *ptr,size_t size,size_t count,FILE *fp)
{ if(fail_read){errno=EIO;return 0;}return fread(ptr,size,count,fp); }
static int tracked_ferror(FILE *fp)
{ return fail_read ? 1 : ferror(fp); }
static int tracked_fclose(FILE *fp)
{ int rc=fclose(fp);if(fail_close){errno=EIO;return EOF;}return rc; }
void pk_airband_recording_store_request_refresh(void) { refresh_requests++; }
bool pk_sdcard_is_mounted(void) { return true; }
uint32_t pk_sdcard_mount_generation(void) { return generation; }
const char *pk_sdcard_mount_point(void) { return root; }
bool pk_sdcard_media_error(void) { return false; }
bool pk_sdcard_register_pre_unmount_cb(void (*cb)(void)) { return cb!=NULL; }
int64_t esp_timer_get_time(void) { return now; }
static int tracked_mkdir(const char *path,mode_t mode)
{ mkdir_calls++;if(fail_mkdir){errno=EIO;return -1;}return mkdir(path,mode); }
static DIR *tracked_opendir(const char *path)
{ opendir_calls++;if(fail_opendir){errno=EIO;return NULL;}return opendir(path); }
static FILE *tracked_fopen(const char *path,const char *mode)
{
    /* Pending-file inspection is read-only; open failures remain errors. */
    if(!strcmp(mode,"rb")) {
        recovery_opens++;
        if(fail_recovery){errno=EIO;return NULL;}
    }
    return fopen(path,mode);
}
#define mkdir tracked_mkdir
#define opendir tracked_opendir
#define fopen tracked_fopen
#define fread tracked_fread
#define ferror tracked_ferror
#define fclose tracked_fclose
#include "../main/airband_record.c"
#undef mkdir
#undef opendir
#undef fopen
#undef fread
#undef ferror
#undef fclose
static int failures;
#define CHECK(c,m) do { if(!(c)){fprintf(stderr,"FAIL: %s (%d)\n",m,__LINE__);failures++;} }while(0)
static pk_airband_record_status_t status(void)
{ pk_airband_record_status_t out;pk_airband_record_get(&out);return out; }
static void poll_many(void) { for(unsigned i=0;i<100;i++)record_poll(); }
static void fixture(void)
{
    char dir[256],path[320];snprintf(dir,sizeof(dir),"%s/airband",root);
    CHECK(mkdir(dir,0775)==0,"fixture directory");
    snprintf(path,sizeof(path),"%s/s0000000001-c121500-f121500000-0000000001.part.wav",dir);
    FILE *fp=fopen(path,"wb");CHECK(fp!=NULL,"fixture opened");
    if(!fp)return;
    const unsigned char data[46]={
        'R','I','F','F',36,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',0,0,0,0,1,0};
    CHECK(fwrite(data,1,sizeof(data),fp)==sizeof(data),"fixture written");fclose(fp);
}
int main(void)
{
    CHECK(mkdtemp(root)!=NULL,"host root");fixture();
    CHECK(pk_airband_record_init(),"worker started");poll_many();
    CHECK(!mkdir_calls && !opendir_calls && !recovery_opens,
          "disabled idle never creates directories, scans or repairs partial files");
    CHECK(!status().sd_missing,"mounted card is present before recording preparation");
    CHECK(pk_airband_record_set_enabled(true),"first enable requests asynchronous preparation");
    record_poll();
    CHECK(status().enabled && !status().error && !status().recovered && status().recovery_skipped==1,
          "explicit enable inspects and retains the pending part once");
    CHECK(recovery_opens==1,"partial recovery only occurs after permission");
    CHECK(!refresh_requests,"pending inspection publishes no catalog refresh event");
    int16_t pcm[64]={1234};
    CHECK(pk_airband_record_submit(pcm,64,true,true,121500,121500000),"prepared capture accepts audio");
    CHECK(pk_airband_record_set_enabled(false),"stop with queued audio");record_poll();
    CHECK(status().segments==1 && !status().stopping && !status().active,
          "disabled stop still drains and publishes accepted audio");
    CHECK(refresh_requests==1,"successful stop publishes a catalog refresh event");
    unsigned before=mkdir_calls;generation++;poll_many();
    CHECK(mkdir_calls==before,"disabled remount does not prepare card");
    fail_mkdir=true;
    CHECK(pk_airband_record_set_enabled(true),"enable accepts preparation retry request");record_poll();
    CHECK(mkdir_calls==before+1 && status().error && !status().sd_missing,
          "directory failure reports storage error rather than absent card");
    poll_many();CHECK(mkdir_calls==before+1,"mkdir failure does not spin at writer poll rate");
    CHECK(pk_airband_record_set_enabled(false) && pk_airband_record_set_enabled(true),"retry control remains usable");
    poll_many();CHECK(mkdir_calls==before+1 && status().error,"off-on does not bypass backoff or hide failure");
    now+=999999;record_poll();CHECK(mkdir_calls==before+1,"backoff lasts one second");
    now++;record_poll();CHECK(mkdir_calls==before+2,"retry occurs at bounded interval");
    fail_mkdir=false;fail_opendir=true;now+=1000000;record_poll();
    unsigned scans=opendir_calls;poll_many();
    CHECK(opendir_calls==scans,"opendir failure also uses backoff");
    fail_opendir=false;now+=1000000;record_poll();
    CHECK(!status().error && pk_airband_record_submit(pcm,64,true,true,121500,121500000),
          "successful bounded retry restores recording");
    pk_airband_record_set_enabled(false);record_poll();
    /* A short/empty file is ordinary pending content. Real read and close
     * failures must stay distinct; closing is attempted even after a read error. */
    char inspected[320];snprintf(inspected,sizeof(inspected),"%s/airband/s0000000001-c121500-f121500000-0000000001.part.wav",root);
    fail_read=true;
    CHECK(!inspect_part(inspected),"read I/O failure is not a normal pending file");
    fail_read=false;fail_close=true;
    CHECK(!inspect_part(inspected),"close I/O failure is not a normal pending file");
    fail_close=false;
    CHECK(inspect_part(inspected),"readable partial content stays pending");
    char tiny[320];snprintf(tiny,sizeof(tiny),"%s/empty.part.wav",root);
    FILE *tiny_fp=fopen(tiny,"wb");CHECK(tiny_fp!=NULL,"empty pending fixture");if(tiny_fp)fclose(tiny_fp);
    CHECK(inspect_part(tiny),"empty part stays pending without an I/O error");
    CHECK(inspect_part(root),"non-file part entry stays pending without open");
    generation++;fail_mkdir=true;record_poll();
    CHECK(pk_airband_record_set_enabled(true),"enabled after another mount");record_poll();
    before=mkdir_calls;generation++;record_poll();
    CHECK(mkdir_calls==before+1,"new mount resets old mount retry deadline");
    fail_mkdir=false;fail_recovery=true;
    char path[320];snprintf(path,sizeof(path),"%s/airband/s0000000099-c121500-f121500000-0000000001.part.wav",root);
    FILE *fp=fopen(path,"wb");CHECK(fp!=NULL,"failed recovery fixture");
    if(fp){unsigned char bytes[44]={0};fwrite(bytes,1,sizeof(bytes),fp);fclose(fp);}
    generation++;record_poll();before=recovery_opens;
    CHECK(status().error && status().recovery_errors==2,"failed recovery is visible");
    now+=10000000;poll_many();
    pk_airband_record_set_enabled(false);pk_airband_record_set_enabled(true);poll_many();
    CHECK(recovery_opens==before && status().error,
          "failed part recovery cannot repeatedly reopen or mutate same mount");
    printf("%s: recorder access evidence retained in %s\n",failures?"FAILED":"OK",root);
    return failures!=0;
}
