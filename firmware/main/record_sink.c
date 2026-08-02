/*
 * record_sink.c — registry + dispatcher.
 *
 * Holds a tiny static array of sink pointers. Registration is one-way
 * (no deregistration), which keeps dispatch lock-free: writers never
 * mutate the array, so readers don't need a mutex.
 */

#include "record_sink.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"

#define RECORD_SINK_MAX  4   /* uart, ble, file (LittleFS or SD), rec_store —
                               * all 4 slots used, no spare left (2026-08-02
                               * ADS-B persistence design's session-scoped
                               * sink took the last one). */

static const char *TAG = "record_sink";

static record_sink_t *s_sinks[RECORD_SINK_MAX];
static size_t         s_sink_count;

esp_err_t record_sink_register(record_sink_t *sink)
{
    if (sink == NULL || sink->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sink_count >= RECORD_SINK_MAX) {
        ESP_LOGE(TAG, "registry full (capacity=%d)", RECORD_SINK_MAX);
        return ESP_ERR_NO_MEM;
    }
    s_sinks[s_sink_count++] = sink;
    ESP_LOGI(TAG, "registered sink '%s' (%u total)",
             sink->name, (unsigned)s_sink_count);
    return ESP_OK;
}

void record_dispatch(const record_t *rec)
{
    for (size_t i = 0; i < s_sink_count; ++i) {
        s_sinks[i]->write(s_sinks[i], rec);
    }
}

const char *record_sinks_install_defaults(void)
{
    /* UART sink first so its log output appears even if the file sink
     * fails to mount the LittleFS partition. */
    record_sink_t *uart = record_sink_uart_create();
    if (uart != NULL) {
        record_sink_register(uart);
    }

    record_sink_t *ble = record_sink_ble_create();
    if (ble != NULL) {
        record_sink_register(ble);
    }

    record_sink_t *file = record_sink_file_create();
    const char *file_mount = NULL;
    if (file != NULL) {
        record_sink_register(file);
        file_mount = (const char *)file->priv;  /* mount point or NULL */
    }

    /* 4th (last) slot: session-scoped falls under /sdcard/rec/<seq>/ via
     * pk_rec_store — see record_sink_rec_store.c. Registered last so the
     * uart/ble/file sinks above keep working even if this one fails to
     * spin up its queue+task (SD not ready yet, alloc failure, ...). */
    record_sink_t *rec_store = record_sink_rec_store_create();
    if (rec_store != NULL) {
        record_sink_register(rec_store);
    }

    return file_mount;
}
