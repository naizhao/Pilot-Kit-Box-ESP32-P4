/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_sd_format_barrier firmware/test/test_sd_format_barrier.c \
 * && /tmp/test_sd_format_barrier
 */
#include <stdio.h>
#include "pk_sdcard.h"
#include "esp_vfs_fat.h"
static int callbacks, open_handles=2, inflight=2, fail;
static void first_consumer(void) { if(pk_sdcard_is_mounted())fail++;callbacks++;open_handles--;inflight--; }
static void second_consumer(void) { if(pk_sdcard_is_mounted())fail++;callbacks++;open_handles--;inflight--; }
esp_err_t esp_vfs_fat_sdcard_format(const char *p,sdmmc_card_t *c)
{ (void)p;(void)c;if(open_handles||inflight||callbacks!=2)fail++;return ESP_OK; }
#include "../main/pk_sdcard.c"
int main(void)
{
    pk_sdcard_init();pk_sdcard_register_pre_unmount_cb(first_consumer);pk_sdcard_register_pre_unmount_cb(second_consumer);
    uint32_t before=pk_sdcard_mount_generation();
    if(pk_sdcard_format()!=ESP_OK)fail++;
    if(callbacks!=2||!pk_sdcard_is_mounted()||pk_sdcard_mount_generation()!=before+1)fail++;
    printf("%s: callbacks=%d handles=%d inflight=%d generation=%u\n",fail?"FAIL":"OK",callbacks,open_handles,inflight,pk_sdcard_mount_generation());
    return fail!=0;
}
