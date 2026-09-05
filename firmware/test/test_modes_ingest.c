/*
 * test_modes_ingest.c — raw Mode-S 帧 → 解码前门的 host 单测。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/main \
 *      -o /tmp/test_modes_ingest \
 *      firmware/test/test_modes_ingest.c \
 *      firmware/main/modes_ingest.c \
 *   && /tmp/test_modes_ingest
 *
 * 被测的是 modes_ingest.c；mode_s.c 直接 include 进本 TU（仓库惯例，
 * 这样 static 也可见），modes_ingest.c 单独编译，其 mode_s_* 引用由本
 * TU 提供。parity 用 mode_s_checksum 现算（Table 后 24 项为 0 → 校验和
 * 与尾部 24 位取值无关 → 令 last3 == checksum 即 crcok，见 mode-s.c:60-101）。
 */
#include "mode_s.c"        /* 拉进 mode_s_decode / mode_s_checksum */
#include "modes_ingest.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

static int g_sink_calls;
static struct mode_s_msg g_last_mm;
static modes_ingest_meta_t g_last_meta;
static void sink(const struct mode_s_msg *mm, const modes_ingest_meta_t *meta,
                 void *user)
{
    (void)user; g_sink_calls++; g_last_mm = *mm; g_last_meta = *meta;
}

/* DF17 空中位置帧（与 test_mode_s_surface 的构造法一致）+ 合法 parity。 */
static void build_df17(unsigned char msg[14], uint32_t lat_cpr, uint32_t lon_cpr)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(17 << 3);            /* DF17 CA=0 */
    msg[1] = 0x4C; msg[2] = 0xA1; msg[3] = 0xBD;  /* ICAO 4CA1BD */
    msg[4] = (unsigned char)(11 << 3);            /* ME TC=11 airborne position */
    msg[6] = 0x00;                                 /* F=0 even */
    msg[7]  = (unsigned char)((lat_cpr >> 8) & 0xFF);
    msg[8]  = (unsigned char)(lat_cpr & 0xFF);
    msg[9]  = (unsigned char)((lon_cpr >> 11) & 0xFF);
    msg[10] = (unsigned char)((lon_cpr >> 3) & 0xFF);
    uint32_t c = mode_s_checksum(msg, 112);
    /* parity 字段 = 完整最后 24 位（msg[11] 全 8 位在内，mode_s_decode
     * 的 mm->crc 读的就是它），所以必须整 3 字节 == checksum；只填低
     * 21 位或往 msg[11] 高 3 位塞 payload 都会让 crcok=0。 */
    msg[11] = (unsigned char)((c >> 16) & 0xFF);
    msg[12] = (unsigned char)((c >> 8) & 0xFF);
    msg[13] = (unsigned char)(c & 0xFF);
}

int main(void)
{
    /* 1. 合法 DF17：sink 恰好一次、字段正确、meta 透传。 */
    {
        modes_ingest_init(sink, NULL);
        unsigned char msg[14]; build_df17(msg, 0x12345, 0x0A0B0);
        modes_ingest_meta_t meta = { .rssi = 0x24, .rp_ts_us = 123456 };
        modes_ingest_feed(msg, 112, &meta);
        CHECK(g_sink_calls == 1, "sink=%d\n", g_sink_calls);
        CHECK(g_last_mm.msgtype == 17, "df=%d\n", g_last_mm.msgtype);
        CHECK(g_last_mm.crcok == 1, "crcok=%d\n", g_last_mm.crcok);
        CHECK(g_last_mm.aa1 == 0x4C && g_last_mm.aa2 == 0xA1 &&
              g_last_mm.aa3 == 0xBD, "icao\n");
        CHECK(g_last_meta.rssi == 0x24 && g_last_meta.rp_ts_us == 123456,
              "meta\n");
        uint32_t okk = 0, bad = 0; modes_ingest_get_stats(&okk, &bad);
        CHECK(okk == 1 && bad == 0, "stats ok=%u bad=%u\n", okk, bad);
    }

    /* 2. parity 破坏：不进 sink、bad_crc 计 1。 */
    {
        unsigned char msg[14]; build_df17(msg, 0x54321, 0x0C0D0);
        msg[13] ^= 0x40;
        modes_ingest_feed(msg, 112, NULL);
        CHECK(g_sink_calls == 1, "sink=%d\n", g_sink_calls);
        uint32_t okk = 0, bad = 0; modes_ingest_get_stats(&okk, &bad);
        CHECK(okk == 1 && bad == 1, "stats ok=%u bad=%u\n", okk, bad);
    }

    /* 3. 非法 msgbits（既非 56 也非 112）：忽略、不崩、不计数。 */
    {
        unsigned char msg[14] = {0};
        modes_ingest_feed(msg, 100, NULL);
        CHECK(g_sink_calls == 1, "sink=%d\n", g_sink_calls);
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
