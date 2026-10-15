/* Diagnostic-only link wrappers: preserve native results and errno. */
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

/* Bypass the ESP_LOG hook: this evidence must not be copied back to SD. */
#define PROBE_LOG(format, ...) printf("sd_probe: " format "\n", __VA_ARGS__)
static atomic_uint s_reads;
static atomic_uint s_data_commands;

extern FILE *__real_fopen(const char *path, const char *mode);
extern DIR *__real_opendir(const char *path);
extern esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst,
                                         size_t start_block, size_t block_count);
extern esp_err_t __real_sdmmc_host_do_transaction(int slot,
                                                sdmmc_command_t *cmd);

FILE *__wrap_fopen(const char *path, const char *mode)
{
    const bool watched = path && strstr(path, "/sdcard/airband/") == path;
    const int original_errno = errno;
    if (watched) PROBE_LOG("open begin path=%s mode=%s task=%s", path,
                          mode, pcTaskGetName(NULL));
    errno = original_errno;
    const int64_t began = esp_timer_get_time();
    FILE *fp = __real_fopen(path, mode);
    const int result_errno = errno;
    if (watched) PROBE_LOG("open end path=%s ok=%d errno=%d us=%" PRId64,
                          path, fp != NULL, result_errno,
                          esp_timer_get_time() - began);
    errno = result_errno;
    return fp;
}

DIR *__wrap_opendir(const char *path)
{
    const int64_t began = esp_timer_get_time();
    DIR *dir = __real_opendir(path);
    const int result_errno = errno;
    if (path && strcmp(path, "/sdcard/airband") == 0) {
        PROBE_LOG("directory path=%s ok=%d errno=%d us=%" PRId64 " task=%s",
                  path, dir != NULL, result_errno, esp_timer_get_time() - began,
                  pcTaskGetName(NULL));
    }
    errno = result_errno;
    return dir;
}

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst,
                                   size_t start_block, size_t block_count)
{
    const int64_t began = esp_timer_get_time();
    const esp_err_t result = __real_sdmmc_read_sectors(card, dst, start_block,
                                                     block_count);
    const int result_errno = errno;
    const int64_t elapsed = esp_timer_get_time() - began;
    const unsigned sequence = atomic_fetch_add(&s_reads, 1);
    if (result != ESP_OK || elapsed >= 200000 || sequence < 5) {
        PROBE_LOG("read slot=%d sector=%zu count=%zu dst=%p external=%d "
                 "align64=%u us=%" PRId64 " rc=0x%x task=%s core=%d",
                 card->host.slot, start_block, block_count, dst,
                 esp_ptr_external_ram(dst), (unsigned)((uintptr_t)dst % 64),
                 elapsed, (unsigned)result, pcTaskGetName(NULL),
                 xPortGetCoreID());
    }
    if (result == ESP_OK && block_count && sequence < 5 &&
        card->csd.sector_size >= 512) {
        const uint8_t *bytes = dst;
        uint32_t hash = UINT32_C(2166136261);
        for (size_t i = 0; i < 512; ++i) hash = (hash ^ bytes[i]) * UINT32_C(16777619);
        PROBE_LOG("sector-content sector=%zu fnv512=%08" PRIx32
                  " first=%02x%02x%02x%02x sig=%02x%02x",
                  start_block, hash, bytes[0], bytes[1], bytes[2], bytes[3],
                  bytes[510], bytes[511]);
    }
    errno = result_errno;
    return result;
}

esp_err_t __wrap_sdmmc_host_do_transaction(int slot, sdmmc_command_t *cmd)
{
    const int64_t began = esp_timer_get_time();
    const esp_err_t result = __real_sdmmc_host_do_transaction(slot, cmd);
    const int result_errno = errno;
    const int64_t elapsed = esp_timer_get_time() - began;
    const bool first_data = slot == SDMMC_HOST_SLOT_0 && cmd->data &&
                           atomic_fetch_add(&s_data_commands, 1) < 5;
    if (slot == SDMMC_HOST_SLOT_0 &&
        (result != ESP_OK || cmd->error != ESP_OK || elapsed >= 200000 || first_data)) {
        PROBE_LOG("cmd slot=%d op=%" PRIu32 " arg=0x%" PRIx32
                 " flags=0x%" PRIx32 " data=%p bytes=%zu buflen=%zu "
                 "external=%d align64=%u timeout_ms=%" PRIu32
                 " us=%" PRId64 " rc=0x%x cmd_err=0x%x rsp0=0x%" PRIx32
                 " task=%s core=%d",
                 slot, cmd->opcode, cmd->arg, (uint32_t)cmd->flags, cmd->data,
                 cmd->datalen, cmd->buflen,
                 cmd->data && esp_ptr_external_ram(cmd->data),
                 (unsigned)((uintptr_t)cmd->data % 64), cmd->timeout_ms,
                 elapsed, (unsigned)result, (unsigned)cmd->error, cmd->response[0],
                 pcTaskGetName(NULL), xPortGetCoreID());
    }
    errno = result_errno;
    return result;
}
