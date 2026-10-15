/* Real production writers/readers, real POSIX handles, paused concurrently with
 * the production SD format/unmount entry. Only platform calls are substituted.
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -D_DEFAULT_SOURCE \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -I firmware/components/adsb_link_codec -I firmware/rp2040 \
 * -o /tmp/test_sd_consumers firmware/test/test_sd_consumers.c \
 * firmware/main/pk_sd_transaction_safety.c -lm \
 * && /tmp/test_sd_consumers
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/task.h"
#include "pk_sdcard.h"
#include "esp_vfs_fat.h"
static char root[128];
static atomic_int handles,format_entered,unmount_entered,unsafe_format,failures;
static bool format_fail,direct_unmount;
static _Thread_local int role,delays;
enum { AT_OPEN=1,AT_SYNC,AT_SCAN,AT_READ,AT_STAT,AT_UNLINK };
static int pause_at;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static bool paused,resume_io;
static void pause_io(int point)
{
    if(role!=1 || point!=pause_at)return;
    pthread_mutex_lock(&gate);paused=true;pthread_cond_broadcast(&gate_cond);
    while(!resume_io)pthread_cond_wait(&gate_cond,&gate);
    pthread_mutex_unlock(&gate);
}
static const char *redirect(const char *path,char out[320])
{
    if(strncmp(path,"/sdcard",7))return path;
    snprintf(out,320,"%s%s",root,path+7);return out;
}
static FILE *observed_open(const char *p,const char *mode)
{ pause_io(AT_OPEN);char path[320];FILE *f=fopen(redirect(p,path),mode);if(f)handles++;return f; }
static int observed_close(FILE *f) { int r=fclose(f);handles--;return r; }
static int observed_sync(int fd) { pause_io(AT_SYNC);return fsync(fd); }
static DIR *observed_opendir(const char *p)
{ char path[320];DIR *d=opendir(redirect(p,path));if(d)handles++;return d; }
static struct dirent *observed_readdir(DIR *d) { pause_io(AT_SCAN);return readdir(d); }
static int observed_closedir(DIR *d) { int r=closedir(d);handles--;return r; }
static size_t observed_read(void *p,size_t size,size_t n,FILE *f) { pause_io(AT_READ);return fread(p,size,n,f); }
static int observed_stat(const char *p,struct stat *st) { pause_io(AT_STAT);char path[320];return stat(redirect(p,path),st); }
static int observed_unlink(const char *p) { pause_io(AT_UNLINK);char path[320];return unlink(redirect(p,path)); }
static int observed_rename(const char *p,const char *q)
{ char a[320],b[320];return rename(redirect(p,a),redirect(q,b)); }
esp_err_t esp_vfs_fat_sdcard_format(const char *p,sdmmc_card_t *c)
{ (void)p;(void)c;format_entered++;if(handles)unsafe_format++;return format_fail ? ESP_FAIL : ESP_OK; }
static esp_err_t observed_unmount(const char *p,sdmmc_card_t *c)
{ (void)p;(void)c;unmount_entered++;if(handles)unsafe_format++;return ESP_FAIL; }
#define esp_vfs_fat_sdcard_unmount observed_unmount
#define TAG sd_tag
#include "../main/pk_sdcard.c"
#undef TAG
#undef esp_vfs_fat_sdcard_unmount
#define fopen observed_open
#define fclose observed_close
#define fsync observed_sync
#define opendir observed_opendir
#define readdir observed_readdir
#define closedir observed_closedir
#define fread observed_read
#define stat(...) observed_stat(__VA_ARGS__)
#define unlink observed_unlink
#define rename observed_rename
#define TAG syslog_tag
#include "../main/pk_syslog.c"
#undef TAG
#define TAG power_tag
#include "../main/power_log.c"
#undef TAG
#define TAG rf_tag
#define open_log rf_open_log
#define write_row rf_write_row
#include "../main/rf1090_log.c"
#undef open_log
#undef write_row
#undef TAG
#define TAG demo_tag
#include "../main/demo_track_sd.c"
#include "../main/demo_gpx.c"
#undef TAG
#define TAG diag_tag
#include "../main/config_diaglog.c"
#undef TAG
#undef fopen
#undef fclose
#undef fsync
#undef opendir
#undef readdir
#undef closedir
#undef fread
#undef stat
#undef unlink
#undef rename
bool pk_clock_is_synced(void) { return false; }
int64_t esp_timer_get_time(void) { return 10000000; }
power_snapshot_t power_service_snapshot(void) { return (power_snapshot_t){0}; }
bool sy6970_diag_get(sy6970_diag_t *out) { memset(out,0,sizeof(*out));return false; }
bool pk_gps_get(pk_gps_state_t *out) { memset(out,0,sizeof(*out));return false; }
int pk_adsb_rp_health(uint32_t *out) { memset(out,0,ADSB_LINK_HEALTH_FIELDS*sizeof(*out));return ADSB_LINK_HEALTH_FIELDS; }
int pk_thresh_1090_get(void) { return 100; }
pk_ant_1090_t pk_ant_1090_get(void) { return PK_ANT_1090_ONBOARD; }
void pk_demo_track_use(const pk_demo_track_src_t *src) { (void)src; }
static int delay_limit;
static void task_delay(unsigned ticks)
{ if(role==3){usleep(ticks*1000);return;}if(role==1 && ++delays>=delay_limit)pthread_exit(NULL);sched_yield(); }
static void task_delete(void *p) { (void)p;if(role==1)pthread_exit(NULL); }
static void *consumer(void *arg) { role=1;delays=0;((void (*)(void *))arg)(NULL);return NULL; }
static void *formatter(void *arg)
{
    (void)arg;role=2;
    if(direct_unmount) { xSemaphoreTake(s_lock,portMAX_DELAY);sd_unmount_locked();xSemaphoreGive(s_lock); }
    else (void)pk_sdcard_format();
    return NULL;
}
static void dump_task(void *arg) { (void)arg;rf1090_log_dump(); }
static void size_task(void *arg) { (void)arg;(void)pk_diaglog_size_bytes(); }
static void clear_task(void *arg) { (void)arg;pk_diaglog_clear_files(); }
#define CHECK(c,m) do { if(!(c)){fprintf(stderr,"FAIL: %s\n",m);failures++;} }while(0)
static atomic_int flush_returned;
static bool flush_result;
static void *flush_waiter(void *arg)
{
    (void)arg;role=3;flush_result=pk_syslog_flush(1000);flush_returned=1;return NULL;
}
static void check_syslog_shutdown_flush(void)
{
    static const char marker[]="shutdown-evidence-sync\n";
    xStreamBufferSend(s_ring,marker,sizeof(marker)-1,0);
    pause_at=AT_SYNC;paused=false;resume_io=false;flush_returned=0;
    delay_limit=INT_MAX;
    pthread_t worker;pthread_create(&worker,NULL,consumer,(void *)syslog_task);
    pthread_mutex_lock(&gate);while(!paused)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
    pthread_t waiter;pthread_create(&waiter,NULL,flush_waiter,NULL);
    struct timespec grace={0,10000000};nanosleep(&grace,NULL);
    CHECK(!flush_returned,"shutdown flush cannot return before fsync completes");
    pthread_mutex_lock(&gate);resume_io=true;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);
    pthread_join(waiter,NULL);
    CHECK(flush_result,"shutdown evidence waits through fsync/fclose");
    delay_limit=0;pthread_join(worker,NULL);

    char path[320],buf[4096]={0};
    FILE *f=fopen(redirect("/sdcard/p4.log",path),"r");
    CHECK(f!=NULL,"syslog evidence file exists after synchronous flush");
    if(f){size_t n=fread(buf,1,sizeof(buf)-1,f);buf[n]='\0';fclose(f);}
    CHECK(strstr(buf,marker)!=NULL,"synchronous flush persisted shutdown marker");
}
static void run_case(const char *name,void (*fn)(void *),int point,int limit,bool failed)
{
    if(!pk_sdcard_is_mounted()) { xSemaphoreTake(s_lock,portMAX_DELAY);sd_mount_locked();xSemaphoreGive(s_lock); }
    pause_at=point;delay_limit=limit;paused=false;resume_io=false;
    format_entered=0;unmount_entered=0;format_fail=failed;
    if(fn==syslog_task) xStreamBufferSend(s_ring,"lease-test\n",11,0);
    pthread_t worker,fmt;pthread_create(&worker,NULL,consumer,(void *)fn);
    pthread_mutex_lock(&gate);while(!paused)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
    pthread_create(&fmt,NULL,formatter,NULL);
    while(pk_sdcard_is_mounted() && !format_entered)sched_yield();
    /* Let the lifecycle thread proceed past its state transition. A missing
     * wait must not pass just because the observer won the next CPU slice. */
    struct timespec grace={0,10000000};nanosleep(&grace,NULL);
    CHECK(!format_entered && !unmount_entered,"format/unmount entered while a real consumer was paused");
    bool acquired=pk_sdcard_io_acquire();
    CHECK(!acquired,"closed admission rejected a new I/O session");
    if(acquired)pk_sdcard_io_release();
    pthread_mutex_lock(&gate);resume_io=true;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);
    pthread_join(worker,NULL);pthread_join(fmt,NULL);
    CHECK(handles==0,"all real FILE/DIR handles closed before barrier returns");
    CHECK(format_entered==(direct_unmount?0:1),"format actually ran after consumer completed");
    CHECK(unmount_entered==((failed||direct_unmount)?1:0),"failure/removal actually ran unmount");
    CHECK(pk_sdcard_is_mounted()!= (failed||direct_unmount),"format failure/removal unmounted safely");
    printf("checked %s point=%d failure=%d\n",name,point,failed);
}
int main(void)
{
    strcpy(root,"/tmp/pk_sd_consumers_XXXXXX");CHECK(mkdtemp(root)!=NULL,"temp root");
    char path[320];snprintf(path,sizeof(path),"%s/demo",root);mkdir(path,0700);
    snprintf(path,sizeof(path),"%s/demo/flight.gpx",root);FILE *f=fopen(path,"w");
    fputs("<gpx><trk><trkseg><trkpt lat=\"1\" lon=\"1\"><ele>1</ele><time>2026-01-01T00:00:00Z</time></trkpt><trkpt lat=\"1.01\" lon=\"1.01\"><ele>2</ele><time>2026-01-01T00:00:10Z</time></trkpt></trkseg></trk></gpx>",f);fclose(f);
    pk_host_delay_hook=task_delay;pk_host_delete_hook=task_delete;
    pk_sdcard_init();s_ring=xStreamBufferCreate(8192,1);s_enabled=true;
    check_syslog_shutdown_flush();
    run_case("syslog",syslog_task,AT_OPEN,2,false);
    run_case("syslog",syslog_task,AT_SYNC,2,true);
    run_case("power",power_log_task,AT_OPEN,1,false);
    run_case("power",power_log_task,AT_SYNC,1,true);
    run_case("rf1090",rf1090_log_task,AT_OPEN,2,false);
    run_case("rf1090",rf1090_log_task,AT_SYNC,2,true);
    run_case("demo",demo_sd_task,AT_SCAN,1000,false);
    run_case("demo",demo_sd_task,AT_READ,1000,true);
    run_case("rf dump",dump_task,AT_OPEN,1000,false);
    run_case("diag size",size_task,AT_STAT,1000,false);
    run_case("diag clear",clear_task,AT_UNLINK,1000,true);
    direct_unmount=true;
    run_case("removed syslog",syslog_task,AT_SYNC,2,false);
    run_case("removed power",power_log_task,AT_SYNC,1,false);
    run_case("removed rf",rf1090_log_task,AT_SYNC,2,false);
    run_case("removed demo",demo_sd_task,AT_SCAN,1000,false);
    CHECK(!unsafe_format,"no real file/dir remained open at FAT format entry");
    printf("%s: production consumer tests; %s\n",failures?"FAILED":"OK",root);return failures!=0;
}
