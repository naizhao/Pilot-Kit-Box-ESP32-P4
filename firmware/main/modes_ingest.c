#include <string.h>
#include "modes_ingest.h"

/* 32,780 B 的 mode_s_t（大头是 ICAO 地址缓存）必须放 PSRAM：dsp_task 时代
 * 同一实例就带 EXT_RAM_BSS_ATTR（机理见 firmware/scripts/check_early_heap.py
 * 头注释——内部 .bss 会把调度器启动前的堆窗口吃穿，2026-08-01 实测 boot
 * loop）。host 单测没有 ESP-IDF，attr 退化为空宏，保持本文件 host 可编。 */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

static EXT_RAM_BSS_ATTR mode_s_t s_dec;
static modes_ingest_sink_fn s_sink;
static void                *s_user;
static uint32_t             s_msgs_total;
static uint32_t             s_frames_bad_crc;

void modes_ingest_init(modes_ingest_sink_fn sink, void *user)
{
    s_sink = sink;
    s_user = user;
    s_msgs_total = 0;
    s_frames_bad_crc = 0;
    mode_s_init(&s_dec);
    /* 与 IQ 时代同一策略：只认 CRC 正确帧；不做单/双比特纠错。 */
    s_dec.check_crc  = 1;
    s_dec.fix_errors = 0;
    s_dec.aggressive = 0;
}

void modes_ingest_feed(const uint8_t *frame, int msgbits,
                       const modes_ingest_meta_t *meta)
{
    if (msgbits != 56 && msgbits != 112) return;

    unsigned char buf[MODE_S_LONG_MSG_BYTES] = {0};
    memcpy(buf, frame, (size_t)(msgbits / 8));   /* 短帧补零到 14B，表读安全 */

    struct mode_s_msg mm;
    mode_s_decode(&s_dec, &mm, buf);
    if (!mm.crcok) {
        s_frames_bad_crc++;
        return;
    }
    s_msgs_total++;
    if (s_sink) {
        modes_ingest_meta_t m = {0};
        if (meta) m = *meta;
        s_sink(&mm, &m, s_user);
    }
}

void modes_ingest_get_stats(uint32_t *msgs_total, uint32_t *frames_bad_crc)
{
    if (msgs_total)    *msgs_total    = s_msgs_total;
    if (frames_bad_crc) *frames_bad_crc = s_frames_bad_crc;
}
