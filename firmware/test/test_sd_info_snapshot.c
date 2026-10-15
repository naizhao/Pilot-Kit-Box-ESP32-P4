/* Capacity publication must expose one complete observation to other tasks.
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_sd_info_snapshot firmware/test/test_sd_info_snapshot.c \
 * firmware/main/pk_sd_transaction_safety.c \
 * && /tmp/test_sd_info_snapshot
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <assert.h>
#include "esp_vfs_fat.h"
static unsigned next_pair;
static atomic_bool finished;
static atomic_uint mixed;
static esp_err_t injected_info(const char *p,uint64_t *total,uint64_t *free_b)
{ (void)p;*total=next_pair?UINT64_C(0x200000002):UINT64_C(0x100000001);
  *free_b=next_pair?UINT64_C(0x200000001):UINT64_C(0x100000000);return ESP_OK; }
esp_err_t esp_vfs_fat_sdcard_format(const char *p,sdmmc_card_t *c)
{ (void)p;(void)c;return ESP_OK; }
#define esp_vfs_fat_info injected_info
#include "../main/pk_sdcard.c"
#undef esp_vfs_fat_info
static void *writer(void *unused)
{ (void)unused;for(unsigned i=0;i<1000000;i++){next_pair=i&1;sd_refresh_info_locked();}
  atomic_store(&finished,true);return NULL; }
int main(void)
{
    pk_sdcard_init();pthread_t worker;assert(pthread_create(&worker,NULL,writer,NULL)==0);
    do {uint64_t total=0,free_b=0;assert(pk_sdcard_info(&total,&free_b));
        if(!((total==UINT64_C(0x100000001)&&free_b==UINT64_C(0x100000000))||
             (total==UINT64_C(0x200000002)&&free_b==UINT64_C(0x200000001))))atomic_fetch_add(&mixed,1);
    }while(!atomic_load(&finished));
    assert(pthread_join(worker,NULL)==0);
    printf("%s SD capacity snapshot: mixed=%u\n",atomic_load(&mixed)?"FAIL":"PASS",atomic_load(&mixed));
    return atomic_load(&mixed)!=0;
}
