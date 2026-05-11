/*
 * record_sink_uart.c — synchronous UART/stdout sink.
 *
 * Emits one Pilot-Kit ts-format line per CRC-valid Mode-S frame:
 *
 *     1715432198765 *8D4840D6202CC371C32CE0576098;
 *
 * This is the exact wire format consumed by Pilot-Kit's
 * scripts/adsb_to_track.py, so capturing the firmware's UART output
 * with any serial logger (idf.py monitor / minicom / pyserial) and
 * piping into a file produces a directly-importable dump.
 *
 * The line goes out through ESP-IDF's logging vprintf hook so it
 * shares ordering and locking with ESP_LOG* — no risk of interleaving
 * inside a single line. We deliberately avoid ESP_LOGI / ESP_LOGW
 * because their format prepends the I/W tag + timestamp + module, which
 * would corrupt the AVR parser; printf-bare keeps the raw line intact.
 */

#include "record_sink.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"

static bool uart_write(record_sink_t *self, const record_t *rec)
{
    (void)self;
    /* printf is thread-safe in ESP-IDF (stdout is a line-buffered
     * VFS sink with an internal mutex). No further locking needed. */
    printf("%" PRId64 " *%s;\n", rec->ts_ms, rec->hex);
    return true;
}

static record_sink_t s_uart_sink = {
    .name  = "uart",
    .write = uart_write,
    .priv  = NULL,
};

record_sink_t *record_sink_uart_create(void)
{
    return &s_uart_sink;
}
