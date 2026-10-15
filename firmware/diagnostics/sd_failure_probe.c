/* Diagnostic-only: no output on successful operations or command-only probes. */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#include "driver/sdmmc_host.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

extern esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst,
                                         size_t start_block, size_t block_count);
extern esp_err_t __real_sdmmc_host_do_transaction(int slot,
                                                sdmmc_command_t *cmd);

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst,
                                   size_t start_block, size_t block_count)
{
    const int64_t began = esp_timer_get_time();
    const esp_err_t result = __real_sdmmc_read_sectors(card, dst, start_block,
                                                     block_count);
    const int result_errno = errno;
    if (result != ESP_OK) {
        /* printf bypasses the ESP_LOG hook which can write a log to SD. */
        printf("sd_failure: read slot=%d sector=%zu count=%zu dst=%p "
               "external=%d align64=%u us=%" PRId64 " rc=0x%x task=%s\n",
               card ? card->host.slot : -1, start_block, block_count, dst,
               esp_ptr_external_ram(dst), (unsigned)((uintptr_t)dst % 64),
               esp_timer_get_time() - began, (unsigned)result,
               pcTaskGetName(NULL));
    }
    errno = result_errno;
    return result;
}

esp_err_t __wrap_sdmmc_host_do_transaction(int slot, sdmmc_command_t *cmd)
{
    const int64_t began = esp_timer_get_time();
    const esp_err_t result = __real_sdmmc_host_do_transaction(slot, cmd);
    const int result_errno = errno;
    if (slot == SDMMC_HOST_SLOT_0 && cmd && cmd->data &&
        (result != ESP_OK || cmd->error != ESP_OK)) {
        printf("sd_failure: cmd slot=%d op=%" PRIu32 " arg=0x%" PRIx32
               " data=%p bytes=%zu buflen=%zu external=%d align64=%u "
               "timeout_ms=%" PRIu32 " us=%" PRId64
               " rc=0x%x cmd_err=0x%x rsp0=0x%" PRIx32 " task=%s\n",
               slot, cmd->opcode, cmd->arg, cmd->data, cmd->datalen,
               cmd->buflen, esp_ptr_external_ram(cmd->data),
               (unsigned)((uintptr_t)cmd->data % 64), cmd->timeout_ms,
               esp_timer_get_time() - began, (unsigned)result,
               (unsigned)cmd->error, cmd->response[0], pcTaskGetName(NULL));
    }
    errno = result_errno;
    return result;
}
