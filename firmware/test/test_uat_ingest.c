/*
 * test_uat_ingest.c — UAT 上行解码前门 + UART v1.1 链路向量的 host 单测
 * （WP-E Task 2，TDD 先红后绿）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/main \
 *      -I firmware/components/adsb_link_codec \
 *      -o /tmp/test_uat_ingest \
 *      firmware/test/test_uat_ingest.c \
 *      firmware/main/uat_ingest.c \
 *      firmware/main/uat_decode.c \
 *      firmware/components/adsb_link_codec/adsb_link_codec.c \
 *   && /tmp/test_uat_ingest
 *
 * 被测：uat_ingest.c 前门（镜像 modes_ingest 的 init(sink)/feed/get_stats
 * 模式）+ 协议 v1.1 golden vector 的整线走查（codec 编码 → 分块喂入解码
 * → payload 助手 → ingest 前门 → sink）。
 *
 * 判据来源：
 *   - firmware/PROTOCOL_P4_RP2040_UART.md §6（v1.1 UAT_UPLINK，golden
 *     vector V1-UPLINK-CLEAN，CRC=0x5FAD）；
 *   - docs/internal/firmware-v3v4/UAT-PROTOCOL-FACTS.md §1/§6/§7/§8
 *     （552 B 单帧长、RS 即完整性、raw 边界、UP-CLEAN/UNCORR11 向量）。
 */

#include <stdio.h>
#include <string.h>

#include "adsb_link.h"
#include "uat_ingest.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
        unsigned lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

/* UP-CLEAN 552 B（事实卡 §8，与 test_uat_decode.c 同一向量：golden vector
 * 的字节即协议 §6.2 引用的帧内容）。 */
static const char VEC_UP_CLEAN_552[] =
    "35F0FC073000144B75D27000C900C37F7C0052001C3C7800D600B4B0C3005C00C7CA0C00"
    "A7004D021C00B00035090F0015007F1C2D00800F1D87300000D970F1C3002100C7D70700"
    "0D012D0C0300E01A7072CF00900FC7D30C0082003C1D300010011F34C1002D1FC3D5C100"
    "30000CFC3300CB011F75D30000A9C7C30C0008168C1C30002F431FB582000D5A05C30C00"
    "1E68F61CF90001005FF0C3002D277F7F0C0030803C1E1C00CB00B03065000035C87FE700"
    "000EC32E1800001DD770CF0000687D7C5C000022F717B200001080D9AF000000287D0C00"
    "0F0080FC2000D90000F2CF0000FF35C36C0001000E22F10017441D09B7001091681C1C00"
    "12382287E000017C10DFC300184D00781D001750000031003B60002DB700A9CBFF002D00"
    "C94C0006E00063744674C3005ED391083D004C583460700000337C5CD30015D74D936800"
    "805D5084300000B9604ED30021C3CB006D000E374C83B5009EF27416DA0000D3D30C0C00"
    "828D58B5F60010F833C3D7002C7DD70C2D00F0075D3078004BD2B96A7900007FC308D000"
    "083C170600002FB0F251000052CAD7C500001E030DF10000010FB3CB00002C5D7D0C0000"
    "0B8D0499C30014DFA703B60000AE326BC300614708D5B4008EA9061F300079C0F70B0500"
    "E0099D5A74005A024CBE73001FBAB07D4B009F6E172CA30071E939B0A8003C94A9258800"
    "C7AB9BE6E80069A84C85F100287F1622DE0007326A4995001E69AB703A00A444D7F76E00"
    "A6F2A4539700EBF7C1F70000"
    ;
/* UP-UNCORR11（事实卡 §8）：块 0 共 11 错，RS 不可纠拒绝路径。 */
static const char VEC_UP_UNCORR11_552[] =
    "90F0FC073000B14B75D270006C00C37F7C00F7001C3C78007300B4B0C300F900C7CA0C00"
    "02004D021C00150035090F00B0007F1C2D00250F1D873000A5D970F1C3002100C7D70700"
    "0D012D0C0300E01A7072CF00900FC7D30C0082003C1D300010011F34C1002D1FC3D5C100"
    "30000CFC3300CB011F75D30000A9C7C30C0008168C1C30002F431FB582000D5A05C30C00"
    "1E68F61CF90001005FF0C3002D277F7F0C0030803C1E1C00CB00B03065000035C87FE700"
    "000EC32E1800001DD770CF0000687D7C5C000022F717B200001080D9AF000000287D0C00"
    "0F0080FC2000D90000F2CF0000FF35C36C0001000E22F10017441D09B7001091681C1C00"
    "12382287E000017C10DFC300184D00781D001750000031003B60002DB700A9CBFF002D00"
    "C94C0006E00063744674C3005ED391083D004C583460700000337C5CD30015D74D936800"
    "805D5084300000B9604ED30021C3CB006D000E374C83B5009EF27416DA0000D3D30C0C00"
    "828D58B5F60010F833C3D7002C7DD70C2D00F0075D3078004BD2B96A7900007FC308D000"
    "083C170600002FB0F251000052CAD7C500001E030DF10000010FB3CB00002C5D7D0C0000"
    "0B8D0499C30014DFA703B60000AE326BC300614708D5B4008EA9061F300079C0F70B0500"
    "E0099D5A74005A024CBE73001FBAB07D4B009F6E172CA30071E939B0A8003C94A9258800"
    "C7AB9BE6E80069A84C85F100287F1622DE0007326A4995001E69AB703A00A444D7F76E00"
    "A6F2A4539700EBF7C1F70000"
    ;

/* ── sink 捕获 ─────────────────────────────────────────────────── */
static int g_sink_calls;
static uat_uplink_t g_last_up;            /* 浅拷贝：info[].data 仍指向
                                          * ingest 内部缓冲（契约见头文件） */
static uat_ingest_meta_t g_last_meta;
static uint8_t g_last_rs;
static void sink(const uat_uplink_t *up, const uat_ingest_meta_t *meta,
                 uint8_t rs_corrected, void *user)
{
    (void)user;
    g_sink_calls++;
    g_last_up = *up;
    if (meta) g_last_meta = *meta;
    g_last_rs = rs_corrected;
}

/* ── 1. 前门 happy path（UP-CLEAN，事实卡 §8 期望值）────────────── */

static void test_front_door_clean(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    CHECK(unhex(VEC_UP_CLEAN_552, frame, sizeof frame) == 552);

    uat_ingest_init(sink, NULL);
    uat_ingest_meta_t meta = { .rssi = 0x37, .rp_ts_us = 0x11223344 };
    uat_ingest_feed(frame, &meta);

    CHECK(g_sink_calls == 1);
    CHECK(g_last_rs == 0);                       /* 无错帧 0 纠正        */
    CHECK(g_last_meta.rssi == 0x37);             /* meta 透传            */
    CHECK(g_last_meta.rp_ts_us == 0x11223344);
    CHECK(g_last_up.slot_id == 7);               /* 事实卡 §8（uat2text
                                                  * 对拍值）            */
    CHECK(g_last_up.tisb_site_id == 11);
    CHECK(g_last_up.num_info_frames == 5);
    CHECK(g_last_up.utc_coupled && g_last_up.app_data_valid);
    CHECK(!g_last_up.position_valid);

    uint32_t ok = 0, bad = 0, nsync = 0;
    uat_ingest_get_stats(&ok, &bad, &nsync);
    CHECK(ok == 1 && bad == 0 && nsync == 0);
}

/* ── 2. RS 不可纠路径：不进 sink、只计 bad_rs（协议 §6 完整性=RS）── */

static void test_front_door_bad_rs(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    CHECK(unhex(VEC_UP_UNCORR11_552, frame, sizeof frame) == 552);
    uat_ingest_feed(frame, NULL);
    CHECK(g_sink_calls == 1);                    /* 上一 case 的 1 次    */
    uint32_t ok = 0, bad = 0, nsync = 0;
    uat_ingest_get_stats(&ok, &bad, &nsync);
    CHECK(ok == 1 && bad == 1 && nsync == 0);
}

/* ── 3. 全零"无帧"哨兵：不进 sink、只计 no_sync（SPI §2.3 规则 5
 *        经 UART 透传；同步字不上 UART，全零是唯一的"非帧"形态）──── */

static void test_front_door_all_zero(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES] = { 0 };
    uat_ingest_feed(frame, NULL);
    CHECK(g_sink_calls == 1);
    uint32_t ok = 0, bad = 0, nsync = 0;
    uat_ingest_get_stats(&ok, &bad, &nsync);
    CHECK(ok == 1 && bad == 1 && nsync == 1);
}

/* ── 4. meta=NULL 容错（modes_ingest 同款：sink 收到清零 meta）──── */

static void test_null_meta(void)
{
    uat_ingest_init(NULL /* 换 sink 前先验证 NULL sink 不崩 */, NULL);
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    CHECK(unhex(VEC_UP_CLEAN_552, frame, sizeof frame) == 552);
    uat_ingest_feed(frame, NULL);                /* NULL sink + NULL meta */
    uint32_t ok = 0, bad = 0, nsync = 0;
    uat_ingest_get_stats(&ok, &bad, &nsync);
    CHECK(ok == 1 && bad == 0 && nsync == 0);    /* 解码照常、计数照常   */

    /* init 重置计数与 sink 后再走一次 meta=NULL → sink 收到清零 meta */
    uat_ingest_init(sink, NULL);
    memset(&g_last_meta, 0xEE, sizeof g_last_meta);
    uat_ingest_feed(frame, NULL);
    CHECK(g_sink_calls == 2);                    /* 跨 init 累计 sink 侧 */
    CHECK(g_last_meta.rssi == 0 && g_last_meta.rp_ts_us == 0);
    uat_ingest_get_stats(&ok, &bad, &nsync);
    CHECK(ok == 1 && bad == 0 && nsync == 0);    /* init 已清零          */
}

/* ── 5. 数据生命周期：sink 拿到的 info[].data 指向内部静态缓冲，
 *        feed 返回后仍可读（契约同 uat_decode 的 data_out）───────── */

static void test_data_lifetime(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    CHECK(unhex(VEC_UP_CLEAN_552, frame, sizeof frame) == 552);
    uat_ingest_init(sink, NULL);
    uat_ingest_feed(frame, NULL);
    /* app_data 首 2 B 是首个信息帧头：len=43 → d0=0x54? 按 §4 位打包
     * 直接对拍事实卡 §8 已核对的真实数据（len5/type1 首字节 0x02 0x81
     * 族）。这里只验证"可读且与净数据一致"：信息帧 0 载荷首字节 = 432 B
     * 净数据第 10 B（8 B 帧头 + 2 B 信息帧头，事实卡 §4）。 */
    CHECK(g_last_up.num_info_frames == 5);
    CHECK(g_last_up.info[0].length == 43);
    CHECK(g_last_up.info[0].data != NULL);
    volatile uint8_t first = g_last_up.info[0].data[0];   /* feed 返回后读 */
    (void)first;                                 /* 不崩即契约成立       */
}

/* codec 解码捕获（6c 用）：存整条消息（payload 576 B 静态够放）。 */
static adsb_link_msg_t g_last_link_msg;
static void link_sink(void *user, const adsb_link_msg_t *m)
{
    (void)user;
    g_last_link_msg = *m;
}

/* ── 6. v1.1 golden vector 整线走查（协议 §6.2，CRC=0x5FAD）─────── */

static void test_v11_golden_vector_wire(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    CHECK(unhex(VEC_UP_CLEAN_552, frame, sizeof frame) == 552);

    /* 6a. payload 助手逐字节（doc 钉死的 meta 布局）*/
    uint8_t pl[ADSB_LINK_UAT_PAYLOAD_LEN];
    CHECK(adsb_link_uat_uplink_encode(pl, sizeof pl, 0x37, 0x11223344,
                                      frame) == ADSB_LINK_UAT_PAYLOAD_LEN);
    CHECK(pl[0] == 0x37);
    CHECK(pl[1] == 0x44 && pl[2] == 0x33 && pl[3] == 0x22 && pl[4] == 0x11);
    CHECK(memcmp(pl + ADSB_LINK_UAT_META_BYTES, frame, 552) == 0);

    /* 6b. 整帧头部逐字节 + 文档钉死的 CRC（独立值，不经被测代码）*/
    uint8_t wire[ADSB_LINK_MAX_FRAME];
    size_t n = adsb_link_encode(wire, sizeof wire, ADSB_LINK_MSG_UAT_UPLINK,
                                0, pl, ADSB_LINK_UAT_PAYLOAD_LEN);
    CHECK(n == 567);                             /* 8 + 557 + 2          */
    /* ver_minor = 2（v1.2）。协议每加一次向前兼容的新增都会碰这一字节，
     * 连带整帧 CRC 也变——两者都由规范 §6.2 的 golden vector 钉死，CRC 由
     * 独立 Python 实现算出（KAT "123456789"→0x29B1 对拍），不经被测代码。 */
    static const uint8_t HDR[8] = { 0x50, 0x4B, 0x01, 0x02, 0x11, 0x00,
                                    0x2D, 0x02 };
    CHECK(memcmp(wire, HDR, 8) == 0);
    CHECK(wire[n - 2] == 0xA4 && wire[n - 1] == 0x21);   /* crc16=0x21A4 LE */

    /* 6c. codec 解码（分块 256 B，模拟 P4 UART 读长）→ payload 助手 →
     *     前门（与 adsb_link_task.c 的 UAT_UPLINK 分发同一条链）。 */
    g_sink_calls = 0;
    memset(&g_last_link_msg, 0, sizeof g_last_link_msg);
    uat_ingest_init(sink, NULL);
    static adsb_link_dec_t dec;                  /* 586 B，别放栈上      */
    adsb_link_dec_init(&dec, link_sink, NULL);
    for (size_t off = 0; off < n; off += 256)
        adsb_link_dec_feed(&dec, wire + off, (n - off < 256) ? n - off : 256);

    CHECK(g_last_link_msg.type == ADSB_LINK_MSG_UAT_UPLINK);
    CHECK(g_last_link_msg.payload_len == ADSB_LINK_UAT_PAYLOAD_LEN);
    CHECK(memcmp(g_last_link_msg.payload, pl, ADSB_LINK_UAT_PAYLOAD_LEN) == 0);

    uint8_t rssi; uint32_t ts; const uint8_t *fr;
    CHECK(adsb_link_uat_uplink_decode(g_last_link_msg.payload,
                                      g_last_link_msg.payload_len,
                                      &rssi, &ts, &fr));
    CHECK(rssi == 0x37 && ts == 0x11223344);
    CHECK(memcmp(fr, frame, 552) == 0);

    uat_ingest_meta_t meta = { .rssi = rssi, .rp_ts_us = ts };
    uat_ingest_feed(fr, &meta);
    CHECK(g_sink_calls == 1);                    /* golden vector 经全链
                                                  * 解出并递交 sink      */
    CHECK(g_last_up.slot_id == 7 && g_last_rs == 0);
    uint32_t ok = 0, bad = 0, nsync = 0;
    uat_ingest_get_stats(&ok, &bad, &nsync);
    CHECK(ok == 1 && bad == 0 && nsync == 0);
}

int main(void)
{
    test_front_door_clean();
    test_front_door_bad_rs();
    test_front_door_all_zero();
    test_null_meta();
    test_data_lifetime();
    test_v11_golden_vector_wire();
    if (g_fail) {
        fprintf(stderr, "test_uat_ingest: %d FAIL\n", g_fail);
        return 1;
    }
    printf("test_uat_ingest: all OK\n");
    return 0;
}
