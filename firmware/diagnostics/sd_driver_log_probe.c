/* Diagnostic-only: keep native SD/FATFS calls and expose SDK error status. */
#include <errno.h>
#include <string.h>
#include "esp_log.h"

extern void __real_esp_log_level_set(const char *tag, esp_log_level_t level);

void __wrap_esp_log_level_set(const char *tag, esp_log_level_t level)
{
    __real_esp_log_level_set(tag, level);
    const int result_errno = errno;
    if (tag && strcmp(tag, "sdmmc_req") == 0 && level == ESP_LOG_DEBUG) {
        /* SDK 6 uses SD_TRANS, not the old SD_HOST tag for data/command errors. */
        __real_esp_log_level_set("SD_TRANS", ESP_LOG_DEBUG);
        __real_esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);
    }
    errno = result_errno;
}
