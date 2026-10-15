/* Driver failure closes admission independently of the SD lifecycle lock.
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_sd_transaction_lifecycle firmware/test/test_sd_transaction_lifecycle.c \
 * firmware/main/pk_sd_transaction_safety.c \
 * && /tmp/test_sd_transaction_lifecycle
 */
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pk_sdcard.h"
#include "pk_sd_transaction_safety.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static int unmounts, callbacks, status_calls, delays;
static bool lease;
static bool expect_unmount_callback, power_config_fails, ldo_fails, init_block_error;
static int mount_calls, power_on_calls, power_off_calls, power_config_calls, ldo_calls;
static esp_err_t status_error;
static esp_err_t mount_error = ESP_ERR_TIMEOUT;
static esp_err_t injected_power_config(const gpio_config_t *cfg)
{ (void)cfg;power_config_calls++;return power_config_fails?ESP_FAIL:ESP_OK; }
static esp_err_t injected_power_level(int pin,int level)
{ assert(pin==45);if(level==0)power_on_calls++;else {assert(level==1);power_off_calls++;}return ESP_OK; }
static esp_err_t injected_ldo(const sd_pwr_ctrl_ldo_config_t *cfg,sd_pwr_ctrl_handle_t *out)
{ assert(cfg->ldo_chan_id==4);ldo_calls++;if(ldo_fails)return ESP_FAIL;*out=(void *)1;return ESP_OK; }
static jmp_buf detection_done;
static esp_err_t injected_unmount(const char *p,sdmmc_card_t *c)
{ (void)p;(void)c;assert(!lease);if(expect_unmount_callback)assert(callbacks==1);unmounts++;return ESP_OK; }
static esp_err_t healthy_status(sdmmc_card_t *c)
{ (void)c;status_calls++;return status_error; }
esp_err_t esp_vfs_fat_sdcard_format(const char *p,sdmmc_card_t *c)
{ (void)p;(void)c;assert(!"faulted card must not format");return ESP_FAIL; }
/* 退避场景需要「先连续失败、后成功」的 mount；父进程场景会锁存只有重启才能
 * 清除的事务故障，所以退避场景在 fork() 子进程里跑，拿到全部模块静态变量的
 * 干净副本（见 main）。 */
static bool mount_fails;
static esp_err_t injected_mount(const char *p,const sdmmc_host_t *h,
    const sdmmc_slot_config_t *s,const esp_vfs_fat_sdmmc_mount_config_t *c,
    sdmmc_card_t **out)
{ (void)p;(void)s;assert(!c->format_if_mount_failed);pk_host_last_sd_mount_host=*h;mount_calls++;
  if(mount_fails){pk_sd_transaction_complete(0,init_block_error,mount_error);return mount_error;}
  static sdmmc_card_t card;*out=&card;return ESP_OK; }
#define esp_vfs_fat_sdcard_unmount injected_unmount
#define sdmmc_get_status healthy_status
#define esp_vfs_fat_sdmmc_mount injected_mount
#define sd_pwr_ctrl_new_on_chip_ldo injected_ldo
#define gpio_config injected_power_config
#define gpio_set_level injected_power_level
#include "../main/pk_sdcard.c"
#undef esp_vfs_fat_sdcard_unmount
#undef sdmmc_get_status
#undef esp_vfs_fat_sdmmc_mount
#undef sd_pwr_ctrl_new_on_chip_ldo
#undef gpio_config
#undef gpio_set_level

static void consumer_close(void)
{ assert(!pk_sdcard_is_mounted());assert(!pk_sdcard_io_acquire());callbacks++; }
static void *publish_fault(void *unused)
{ (void)unused;pk_sd_transaction_complete(0,true,ESP_ERR_TIMEOUT);return NULL; }
static void detector_delay(unsigned ticks)
{
    if (lease && ticks==1) {pk_sdcard_io_release();lease=false;return;}
    if (ticks<=20) return; /* Card-power settling is not a detector period. */
    if (++delays==2) longjmp(detection_done,1);
}
static void detect_once(void)
{
    delays=0;pk_host_delay_hook=detector_delay;
    if(setjmp(detection_done)==0)pk_host_created_task(NULL);
    pk_host_delay_hook=NULL;
}

/* Real detector loops run against observable mount/GPIO boundaries; a missing
 * retry gate, off-by-one limit, or failure to reset on success must fail here. */
static unsigned probe_ticks[64];
static jmp_buf probe_done;
static int probe_wanted, probe_count;
static void probe_delay(unsigned ticks)
{
    if (ticks<=20) return; /* Card-power settling is not a detector period. */
    if (probe_count<probe_wanted) { probe_ticks[probe_count++]=ticks; return; }
    longjmp(probe_done,1); /* entering iteration N+1: N iterations completed */
}
static void probe_run(int iterations)
{
    probe_wanted=iterations;probe_count=0;
    pk_host_delay_hook=probe_delay;
    if (setjmp(probe_done)==0) pk_host_created_task(NULL);
    pk_host_delay_hook=NULL;
}
static void test_mount_limit_child(void)
{
    static const unsigned want[]={3000,3000,3000,4500,6750,10125,12000};
    init_block_error=true; /* Unarmed initialization errors still use budget. */
    mount_fails=true;
    pk_sdcard_init(); /* boot attempt #1 fails */
    assert(!pk_sdcard_is_mounted());
    assert(!pk_sdcard_probe_exhausted());
    probe_run(7); /* failures #2..#8: 3s x3, then x1.5 growth to the 12s cap */
    for (int i=0;i<7;i++) assert(probe_ticks[i]==want[i]);
    assert(mount_calls==8&&power_on_calls==8&&power_off_calls==8);
    assert(pk_sdcard_probe_exhausted());
    assert(pk_sd_transaction_fault()==ESP_OK); /* init probes are not armed */
    probe_run(32); /* A whole night cannot restart the electrical cycle. */
    assert(mount_calls==8&&power_on_calls==8&&power_off_calls==8);
    mount_fails=false;
    probe_run(4); /* No CD pin: inserting/replacing a card cannot reset budget. */
    pk_sdcard_init(); /* init is idempotent, not a recovery/reset API */
    assert(!pk_sdcard_is_mounted());
    assert(pk_sdcard_probe_exhausted());
    assert(!sd_mount_locked());
    assert(mount_calls==8&&power_on_calls==8&&power_off_calls==8);
    assert(power_config_calls==1);
    assert(pk_sdcard_media_error()==(mount_error==ESP_FAIL));
    assert(pk_sdcard_format()==ESP_ERR_INVALID_STATE);
}
static void test_power_failure_limit_child(void)
{
    power_config_fails=true;
    pk_sdcard_init();
    probe_run(7); /* Eight GPIO-power failures exhaust the boot-inclusive budget */
    assert(power_config_calls==8&&mount_calls==0);
    assert(pk_sdcard_probe_exhausted());
    assert(power_on_calls==0&&power_off_calls==0);
    power_config_fails=false;
    probe_run(32);
    assert(!sd_mount_locked());
    assert(power_config_calls==8&&mount_calls==0);
    assert(!pk_sdcard_media_error());
}
static void test_ldo_failure_limit_child(void)
{
    ldo_fails=true;
    pk_sdcard_init();
    probe_run(7);
    assert(ldo_calls==8&&power_config_calls==0&&mount_calls==0);
    assert(pk_sdcard_probe_exhausted());
    assert(power_on_calls==0&&power_off_calls==0);
    ldo_fails=false;
    probe_run(32);
    assert(!sd_mount_locked());
    assert(ldo_calls==8&&power_config_calls==0&&mount_calls==0);
    assert(!pk_sdcard_media_error());
}
static void test_success_resets_limit_child(void)
{
    mount_fails=true;
    pk_sdcard_init();
    probe_run(6); /* Seven failures, then success on the final allowed attempt. */
    mount_fails=false;
    probe_run(1);
    assert(pk_sdcard_is_mounted()&&mount_calls==8&&power_on_calls==8);
    assert(!pk_sdcard_probe_exhausted());
    status_error=ESP_ERR_TIMEOUT;
    probe_run(1); /* Actual healthy-card removal path, not direct state edit. */
    assert(!pk_sdcard_is_mounted()&&unmounts==1&&status_calls==1);
    assert(!pk_sdcard_probe_exhausted());
    assert(pk_sd_transaction_fault()==ESP_OK);
    status_error=ESP_OK;
    mount_fails=true;
    probe_run(7);
    assert(probe_ticks[0]==3000); /* Success cleared both backoff and budget. */
    assert(mount_calls==15&&power_on_calls==15&&power_off_calls==15);
    mount_fails=false;
    probe_run(1); /* Healthy hot insertion on the fresh budget’s final attempt. */
    assert(pk_sdcard_is_mounted());
    assert(mount_calls==16&&power_on_calls==16&&power_off_calls==15);
    assert(!pk_sdcard_probe_exhausted());
    assert(pk_sdcard_mount_generation()==2);
    /* Success must reset the failure *streak*, not just the period: the third
     * failure series has to start over from the 3 s base cadence and re-grow
     * through the x1.5 ladder. A leftover streak would surface here as a 12 s
     * second tick. */
    status_error=ESP_ERR_TIMEOUT;
    probe_run(1); /* remove the card a second time */
    status_error=ESP_OK;
    mount_fails=true;
    probe_run(5);
    assert(probe_ticks[0]==3000&&probe_ticks[1]==3000&&probe_ticks[2]==3000&&
           probe_ticks[3]==3000&&probe_ticks[4]==4500);
}
static void run_child(void (*test)(void))
{
    pid_t child=fork();assert(child>=0);
    if(child==0){test();_exit(0);}
    int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
int main(void)
{
    /* 退避场景先跑、跑在子进程里：它需要失败的 mount 后跟一次成功，而下面的
     * 场景会锁存事务故障（只有重启能清）。子进程断言失败 → 非零退出码。 */
    run_child(test_mount_limit_child);
    mount_error=ESP_FAIL;run_child(test_mount_limit_child); /* FAT failure */
    mount_error=ESP_ERR_INVALID_ARG;run_child(test_mount_limit_child); /* non-media */
    run_child(test_power_failure_limit_child);
    run_child(test_ldo_failure_limit_child);
    mount_error=ESP_ERR_TIMEOUT;run_child(test_success_resets_limit_child);

    /* Initialization data/protocol probes do not latch a mount-time failure. */
    pk_sd_transaction_complete(0,true,ESP_ERR_TIMEOUT);
    assert(pk_sd_transaction_fault()==ESP_OK);
    pk_sdcard_init();
    assert(pk_host_last_sd_mount_host.check_buffer_alignment==sd_dma_buffer_alignment);
    assert(pk_sdcard_is_mounted());
    assert(pk_sdcard_register_pre_unmount_cb(consumer_close));
    expect_unmount_callback=true;
    lease=pk_sdcard_io_acquire();assert(lease);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    pthread_t worker;assert(pthread_create(&worker,NULL,publish_fault,NULL)==0);
    assert(pthread_join(worker,NULL)==0); /* Publish never waits for s_lock. */
    assert(pk_sd_transaction_fault()==ESP_ERR_TIMEOUT);
    assert(!pk_sdcard_is_mounted());
    assert(!pk_sdcard_io_acquire());
    assert(pk_sdcard_media_error());
    assert(!pk_sdcard_probe_exhausted()); /* Block isolation is a separate reason. */
    assert(!pk_sdcard_info(NULL,NULL));
    xSemaphoreGive(s_lock);
    detect_once(); /* CMD13 would be healthy, nevertheless detach and drain. */
    assert(unmounts==1 && callbacks==1 && s_card==NULL);
    assert(status_calls==0);
    uint32_t attempts=s_mount_attempts;
    detect_once();detect_once();
    assert(s_mount_attempts==attempts && unmounts==1);
    assert(!sd_mount_locked());
    assert(pk_sdcard_format()==ESP_ERR_INVALID_STATE);
    pk_sd_transaction_complete(0,true,ESP_FAIL);
    assert(pk_sd_transaction_fault()==ESP_ERR_TIMEOUT); /* Preserve first fault. */
    assert(pk_sd_transaction_allowed(1,true));
    assert(pk_sd_transaction_allowed(0,false));
    assert(!pk_sd_transaction_allowed(0,true));
    puts("PASS SD bounded init/power retries, success/reset/hot insertion, transaction isolation");
    return 0;
}
