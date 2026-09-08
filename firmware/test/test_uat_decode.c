/*
 * test_uat_decode.c — UAT 上/下行解码纯单元的 host 单测
 * （WP-E Task 1，TDD 先红后绿）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -o /tmp/test_uat_decode firmware/test/test_uat_decode.c \
 *      firmware/main/uat_decode.c && /tmp/test_uat_decode
 *
 * 每个期望值的出处都在断言旁以「事实卡 §N」引用
 * docs/internal/firmware-v3v4/UAT-PROTOCOL-FACTS.md；向量溯源总表
 * 见事实卡 §8。解码器是本仓独立实现（上游 dump978 为 GPL-2，事实卡
 * §0 许可审计——只取事实与标准数据，不拷代码）。
 *
 * 钉死的契约：
 *   1. 上行：552 B 交织帧 → 解交织 + RS(92,72)×6 纠错 + 消息层；
 *      单块纠正 >10 即整帧拒绝（dump978 fec.c:60-64 的闸门，事实卡 §2）；
 *   2. 下行：48 B → 长帧先试（纠 ≤7 且 mdb_type≠0）失败再试短帧
 *      （纠 ≤6 且 mdb_type==0），失败拒绝；长帧尝试不得污染短帧重试
 *      （fec.c:34-35 注释契约）；
 *   3. 全零输入（552/48 B）一律拒绝：SPI §2.3 规则 5 全零=合法无帧，
 *      而 RS 数学上全零是合法码字——闸门必须在 RS 之前（事实卡 §8）；
 *   4. 失败路径不改写输出缓冲；
 *   5. UAT 无 payload CRC：三方上游均不校验，完整性=RS（事实卡 §6 裁决）。
 */

#include <stdio.h>
#include <string.h>

#include "../main/uat_decode.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* hex 字符串 → 字节（向量存字符串便于对读事实卡 §8 的溯源串） */
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


/* ── 向量（溯源见事实卡 §8）───────────────────────────────────── */
/* VEC_UP_CLEAN_552 — 552 B，事实卡 §8 UP-CLEAN：sample-data 首条捕获重构 */
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
/* VEC_UP_CORR10_552 — 552 B，事实卡 §8 UP-CORR10：块 0 共 10 字节错 */
static const char VEC_UP_CORR10_552[] =
    "90F0FC073000B14B75D270006C00C37F7C00F7001C3C78007300B4B0C300F900C7CA0C00"
    "02004D021C00150035090F00B0007F1C2D00250F1D87300000D970F1C3002100C7D70700"
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
/* VEC_UP_UNCORR11_552 — 552 B，事实卡 §8 UP-UNCORR11：块 0 共 11 字节错 */
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
/* VEC_UP_DATA_432 — 432 B，UP-CLEAN/CORR10 的期望净数据（即原始捕获） */
static const char VEC_UP_DATA_432[] =
    "3514C952D65CA7B0158000210DE09082102D30CB00082F0D1E012D30CB00000000000000"
    "0FD900011710120118173BA9C9635E4C00158000210E9E0082102CF04B00082F521E012C"
    "F04B000000000000000FD900011A0F00011F0001A916435A6800278000350E1D68221000"
    "0000FF004491387C4D5060CB4C74D35833D75DB9C337F2D38DF87D07D27F3CB0CA030F5D"
    "FC75C31CB4C74D357F1D70C72D70C73C1FC30C1FC78C1F05F65F7F3CB0C8C3D77DF78028"
    "8000350E1D682210000000FF004691347C4D5060CB4C74D35833D75DB9C317F2D70DB37D"
    "07D27F3CB0CA02091C87F1D70C72D31D34D5FC75C31CB5C31CF07F1E307F2E707C17D97D"
    "FCF2C322091C87DF78002D00067408605C93844E0083160CB5C30C306A080651C5F1CB0C"
    "30707C78C30C1C0F2D30C30703CF0C30C1C133D30C30820CF9C30C1C65E718CF5CB2AF0C"
    "20CF6CF1B71CE0C31D31B72DE0C33D70D36830D36DB5DA0CF6D72D7879D0000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    ;

/* UP-TERM（事实卡 §8）：钉死 [MUT] 的终止符判据 "len==0 && type==0"
 * ——len==0 而 type≠0 的零长帧不终止游走（Stratux 相反，见事实卡 §4
 * 两源差异；本仓跟随 [MUT]）。头 8 B 复用真实捕获，app_data 手工构造：
 * [len=5,type=1 + 5B] [len=0,type=5] [len=3,type=0 + 3B] [00 00 终止]。 */
static const char VEC_UP_TERM_552[] =
    "350000000000140000000000C90000000000520000000000D600000000005C0000000000"
    "A70000000000B00000000000020000000000810000000000AA0000000000BB0000000000"
    "CC0000000000DD0000000000EE0000000000000000000000050000000000010000000000"
    "800000000000110000000000220000000000330000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "480000000000700000000000230000000000CD00000000000200000000004B0000000000"
    "3E0000000000280000000000C10000000000B70000000000E600000000009A0000000000"
    "E40000000000BE00000000002F0000000000E000000000006E0000000000380000000000"
    "890000000000430000000000";

/* DO-282B Table 2-104 符合性向量（经 dump978 fec_tests.c:28-30 转录，
 * 事实卡 §0/§8）。全部为 48 B 长帧缓冲——短帧向量只消费前 30 B。 */
static const char VEC_DL_SHORT_IN[] =       /* 2-104 #1：短帧带错 */
    "FF8196782DD44238C1453855F89980C7524F5970940ED83AD89CE7A9BEF8761B"
    "BCD9FCC817D82E2D1ACF90CA78DA3C49";
static const char VEC_DL_SHORT_EXP[] =      /* 纠回的 18 B 净数据 */
    "007E6987D2D74238C1453855F89980C7524F";
static const char VEC_DL_LONG_IN[] =        /* 2-104 #6：长帧带错 */
    "5A8CAA4ABC7AEE2AD0929EB80DA044D556B452A7A73A5716CD1DB40964F5BA105D9D"
    "AAD75342196DEBB63CC972994DEA";
static const char VEC_DL_LONG_EXP[] =       /* 纠回的 34 B 净数据 */
    "A57355B54385ED2AD0929EB80DA044D556B452A7A73A5716CD1DB40964F5BA105D9D";
static const char VEC_DL_FAIL_IN[] =        /* 2-104 #2：不可纠 */
    "007E6987D328BDC73EBA3955F89980C7524F5970940ED83AD89CE7A9BEF8BB7B190B"
    "2EA0EACC7237B7B01B036E07EE04";
static const char VEC_DL_MISMATCH_IN[] =    /* 2-104 #28：basic/long 判别失败 */
    "287F318C2A9FFF7F0784CA4036E252DEB0226A9F9E183CBB933FA68DBF5E1F018F635"
    "195DE87894F16BDA55E42A64137";

/* ── 上行 happy path（UP-CLEAN，事实卡 §8）──────────────────────── */

static void test_uplink_clean(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    uint8_t data[UAT_UPLINK_DATA_BYTES];
    uint8_t exp[UAT_UPLINK_DATA_BYTES];
    uat_uplink_t up;

    CHECK(unhex(VEC_UP_CLEAN_552, frame, sizeof frame) == 552);
    CHECK(unhex(VEC_UP_DATA_432, exp, sizeof exp) == 432);

    memset(data, 0xEE, sizeof data);
    uint8_t corr = 255;
    CHECK(uat_uplink_decode(frame, data, &up, &corr));
    CHECK(corr == 0);                       /* 无错帧 0 纠正            */
    CHECK(memcmp(data, exp, UAT_UPLINK_DATA_BYTES) == 0);

    /* 消息层期望值经上游 uat2text 对同条捕获逐项核对（事实卡 §8）。 */
    CHECK(up.raw_lat == 1739364);           /* → +37.3227°             */
    CHECK(up.raw_lon == 11103022);          /* → −121.7550°            */
    /* deg_e6 = floor(raw·360e6/2²⁴)，经度 >180° 再减 360e6（.c 注释） */
    CHECK(up.lat_deg_e6 == 37322702);
    CHECK(up.lon_deg_e6 == -121754995);
    CHECK(up.position_valid == false);
    CHECK(up.utc_coupled == true);
    CHECK(up.app_data_valid == true);
    CHECK(up.slot_id == 7);
    CHECK(up.tisb_site_id == 11);

    CHECK(up.num_info_frames == 5);         /* 43/43/79/81/90 B + 终止符 */
    static const uint16_t lens[5] = { 43, 43, 79, 81, 90 };
    static const uint16_t pids[5] = { 8, 8, 13, 13, 413 };
    for (int i = 0; i < 5; ++i) {
        CHECK(up.info[i].length == lens[i]);
        CHECK(up.info[i].type == 0);        /* 全部 FIS-B APDU          */
        CHECK(up.info[i].is_fisb);
        CHECK(up.info[i].fisb.product_id == pids[i]);
    }
    /* 首帧 FIS-B 头：t_opt=2（月日时分，事实卡 §4），1/23 16:18，
     * 与 uat2text 输出 "Product time: 1/23 16:18" 一致。 */
    CHECK(up.info[0].fisb.monthday_valid);
    CHECK(!up.info[0].fisb.seconds_valid);
    CHECK(up.info[0].fisb.month == 1 && up.info[0].fisb.day == 23);
    CHECK(up.info[0].fisb.hours == 16 && up.info[0].fisb.minutes == 18);
    /* 末帧 t_opt=0（仅时分 02:06），product_id=413（DLAC 文本）。 */
    CHECK(!up.info[4].fisb.monthday_valid);
    CHECK(!up.info[4].fisb.seconds_valid);
    CHECK(up.info[4].fisb.hours == 2 && up.info[4].fisb.minutes == 6);
    CHECK(up.info[4].fisb.payload_len == 90 - 4);
    /* 信息帧 data 指向 data_out 内部且偏移正确：app_data 起 8 B 帧头，
     * 首帧 2 B 信息帧头 → 载荷 data_out+10；t_opt=2 的 FIS-B 头再 5 B
     * → APDU 载荷 data_out+15（事实卡 §4）。 */
    CHECK(up.info[0].data == data + 10);
    CHECK(up.info[0].fisb.payload == data + 15);
}

/* ── 上行 RS 可纠边界：块 0 恰 10 错（t=10 纠满即收）────────────── */

static void test_uplink_correctable_boundary(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    uint8_t data[UAT_UPLINK_DATA_BYTES];
    uint8_t exp[UAT_UPLINK_DATA_BYTES];
    uat_uplink_t up;

    CHECK(unhex(VEC_UP_CORR10_552, frame, sizeof frame) == 552);
    CHECK(unhex(VEC_UP_DATA_432, exp, sizeof exp) == 432);

    uint8_t corr = 0;
    CHECK(uat_uplink_decode(frame, data, &up, &corr));
    CHECK(corr == 10);                      /* 事实卡 §2：≤10 收       */
    CHECK(memcmp(data, exp, UAT_UPLINK_DATA_BYTES) == 0);
}

/* ── 上行 RS 不可纠边界：块 0 共 11 错（> t=10）────────────────── */

static void test_uplink_uncorrectable(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    uint8_t data[UAT_UPLINK_DATA_BYTES];
    uat_uplink_t up;

    CHECK(unhex(VEC_UP_UNCORR11_552, frame, sizeof frame) == 552);
    memset(data, 0xEE, sizeof data);
    uint8_t corr = 0;
    CHECK(!uat_uplink_decode(frame, data, &up, &corr));
    /* 契约 4：失败不改写输出 */
    for (size_t i = 0; i < sizeof data; ++i)
        CHECK(data[i] == 0xEE);
}

/* ── 上行全零容错（契约 3）────────────────────────────────────── */

static void test_uplink_all_zero_rejected(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES] = { 0 };
    uint8_t data[UAT_UPLINK_DATA_BYTES];
    uat_uplink_t up;
    memset(data, 0xEE, sizeof data);
    CHECK(!uat_uplink_decode(frame, data, &up, NULL));
    for (size_t i = 0; i < sizeof data; ++i)
        CHECK(data[i] == 0xEE);
}


/* ── 终止符约定（UP-TERM，[MUT] uat_decode.c:1081 vs Stratux）──── */

static void test_uplink_terminator_convention(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    uint8_t data[UAT_UPLINK_DATA_BYTES];
    uat_uplink_t up;

    CHECK(unhex(VEC_UP_TERM_552, frame, sizeof frame) == 552);
    CHECK(uat_uplink_decode(frame, data, &up, NULL));
    /* app_data 前 16 B 逐字节（头 8 B + 手工序列）核对净数据 */
    static const uint8_t want[16] = { 0x02, 0x81, 0xAA, 0xBB, 0xCC, 0xDD,
                                      0xEE, 0x00, 0x05, 0x01, 0x80, 0x11,
                                      0x22, 0x33, 0x00, 0x00 };
    CHECK(memcmp(data + 8, want, sizeof want) == 0);
    /* len=0/type=5 的零长帧被计为第 2 个信息帧而不是终止符（[MUT] 判据） */
    CHECK(up.num_info_frames == 3);
    CHECK(up.info[0].length == 5 && up.info[0].type == 1);
    CHECK(!up.info[0].is_fisb);
    CHECK(up.info[1].length == 0 && up.info[1].type == 5);
    CHECK(up.info[2].length == 3 && up.info[2].type == 0);
    CHECK(!up.info[2].is_fisb); /* length<4 不构成 FIS-B 头（事实卡 §4） */
}

/* ── 下行短帧 happy path（DO-282B 2-104 #1）────────────────────── */

static void test_downlink_short(void)
{
    uint8_t frame[UAT_DL_LONG_FRAME_BYTES];
    uint8_t data[UAT_DL_LONG_DATA_BYTES];
    uint8_t exp[UAT_DL_SHORT_DATA_BYTES];
    uat_dl_hdr_t hdr;

    CHECK(unhex(VEC_DL_SHORT_IN, frame, sizeof frame) == 48);
    CHECK(unhex(VEC_DL_SHORT_EXP, exp, sizeof exp) == 18);

    uint8_t corr = 255;
    int r = uat_downlink_decode(frame, data, &hdr, &corr);
    CHECK(r == 1);                          /* 短帧                     */
    CHECK(corr <= 6);                       /* 事实卡 §2：短帧 t=6      */
    CHECK(memcmp(data, exp, UAT_DL_SHORT_DATA_BYTES) == 0);
    /* 头部（事实卡 §5）：纠错后的 d0=0x00 → mdb_type 0 / qualifier 0 */
    CHECK(hdr.mdb_type == 0);
    CHECK(hdr.addr_qualifier == 0);
    CHECK(hdr.address == 0x7E6987);
}

/* ── 下行长帧 happy path（DO-282B 2-104 #6）────────────────────── */

static void test_downlink_long(void)
{
    uint8_t frame[UAT_DL_LONG_FRAME_BYTES];
    uint8_t data[UAT_DL_LONG_DATA_BYTES];
    uint8_t exp[UAT_DL_LONG_DATA_BYTES];
    uat_dl_hdr_t hdr;

    CHECK(unhex(VEC_DL_LONG_IN, frame, sizeof frame) == 48);
    CHECK(unhex(VEC_DL_LONG_EXP, exp, sizeof exp) == 34);

    uint8_t corr = 255;
    int r = uat_downlink_decode(frame, data, &hdr, &corr);
    CHECK(r == 2);                          /* 长帧                     */
    CHECK(corr <= 7);                       /* 事实卡 §2：长帧 t=7      */
    CHECK(memcmp(data, exp, UAT_DL_LONG_DATA_BYTES) == 0);
    /* 纠错后 d0=0xA5：mdb_type=0x14=20，qualifier=5，地址 7355B5 */
    CHECK(hdr.mdb_type == 20);
    CHECK(hdr.addr_qualifier == 5);
    CHECK(hdr.address == 0x7355B5);
}

/* ── 下行不可纠（DO-282B 2-104 #2）与 basic/long 判别失败（#28）── */

static void test_downlink_failures(void)
{
    uint8_t frame[UAT_DL_LONG_FRAME_BYTES];
    uint8_t data[UAT_DL_LONG_DATA_BYTES];

    CHECK(unhex(VEC_DL_FAIL_IN, frame, sizeof frame) == 48);
    memset(data, 0xEE, sizeof data);
    CHECK(uat_downlink_decode(frame, data, NULL, NULL) == 0);
    for (size_t i = 0; i < sizeof data; ++i)
        CHECK(data[i] == 0xEE);             /* 契约 4：失败不写输出     */

    /* #28：RS 可纠但纠出的码字 d0>>3 与长/短判别都矛盾
     * （fec_tests.c:67 注释 "basic/long mismatch"）→ 拒绝。 */
    CHECK(unhex(VEC_DL_MISMATCH_IN, frame, sizeof frame) == 48);
    CHECK(uat_downlink_decode(frame, data, NULL, NULL) == 0);
}

/* ── 下行全零容错（契约 3；上游会误收为 mdb_type=0 短帧，本仓拒绝，
 *      事实卡 §8 全零行）───────────────────────────────────────── */

static void test_downlink_all_zero_rejected(void)
{
    uint8_t frame[UAT_DL_LONG_FRAME_BYTES] = { 0 };
    uint8_t data[UAT_DL_LONG_DATA_BYTES];
    memset(data, 0xEE, sizeof data);
    CHECK(uat_downlink_decode(frame, data, NULL, NULL) == 0);
    for (size_t i = 0; i < sizeof data; ++i)
        CHECK(data[i] == 0xEE);
}

/* ── NULL/越界合同（契约 4）───────────────────────────────────── */

static void test_null_contracts(void)
{
    uint8_t frame[UAT_UPLINK_FRAME_BYTES] = { 1 };
    uint8_t data[UAT_UPLINK_DATA_BYTES];
    uint8_t dl[UAT_DL_LONG_FRAME_BYTES] = { 1 };
    uint8_t dd[UAT_DL_LONG_DATA_BYTES];

    CHECK(!uat_uplink_decode(NULL, data, NULL, NULL));
    CHECK(!uat_uplink_decode(frame, NULL, NULL, NULL));

    /* 下行短帧陷阱：只填前 30 B、其余全零——全零闸会先拒绝（契约 3），
     * 这里只验证 NULL 指针合同。 */
    CHECK(uat_downlink_decode(NULL, dd, NULL, NULL) == 0);
    CHECK(uat_downlink_decode(dl, NULL, NULL, NULL) == 0);
}

/* ── 回环自证：Python 独立编码器与本 C 解码器互为对拍（事实卡 §8
 *      UP-CLEAN 即其产物），再叠加真实下行向量共 40+ 条已在本文件
 *      抽样覆盖（全量对拍见任务报告）。─────────────────────────── */

int main(void)
{
    uat_fec_init();
    test_uplink_clean();
    test_uplink_correctable_boundary();
    test_uplink_uncorrectable();
    test_uplink_all_zero_rejected();
    test_uplink_terminator_convention();
    test_downlink_short();
    test_downlink_long();
    test_downlink_failures();
    test_downlink_all_zero_rejected();
    test_null_contracts();
    if (g_fail) {
        fprintf(stderr, "test_uat_decode: %d FAIL\n", g_fail);
        return 1;
    }
    printf("test_uat_decode: all OK\n");
    return 0;
}
