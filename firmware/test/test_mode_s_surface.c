/*
 * test_mode_s_surface.c — DF17 ME type 5-8（地面位置报文）解析的 host 单测。
 *
 * 跑法（与 firmware/test/ 下其它测试同一套路：把被测 .c 直接拉进本 TU）：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/components/esp32-rtl-sdr/main \
 *      -o /tmp/test_mode_s_surface firmware/test/test_mode_s_surface.c -lm
 *   /tmp/test_mode_s_surface
 *
 * 为什么用**构造样本**而不是找一条真实报文的十六进制串：
 * 真实样本只能证明"某一条能解对"，而地面报文最容易错的地方是 movement 的
 * 分段量化表——那张表有 7 个分段、每段步进不同，错一格解出来的滑行速度依然
 * 像模像样，肉眼与单条样本都发现不了。构造样本可以逐位指定 MOV/TRK/CPR，
 * 把每个分段的**边界值**都钉住，这比一条真实报文严格得多。
 *
 * ME 字段布局（DO-260B，偏移从 ME 第一位起 0-indexed）：
 *   TC[0:5) MOV[5:12) S[12] TRK[13:20) T[20] F[21] LAT[22:39) LON[39:56)
 * DF17 报文里 ME 从 msg[4] 起，故 ME bit 0 == msg[4] 的 bit7。
 */
#include "mode-s.c"   /* 连 static 的 decode_movement_field 一起拉进来 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  [FAIL] " __VA_ARGS__);                                 \
            printf("        at %s:%d\n", __FILE__, __LINE__);                \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_EQ_I(got, want, label)                                         \
    CHECK((got) == (want), "%s got=%d want=%d\n", (label), (int)(got), (int)(want))

#define CHECK_NEAR(got, want, tol, label)                                    \
    CHECK(fabs((double)(got) - (double)(want)) <= (tol),                     \
          "%s got=%.4f want=%.4f\n", (label), (double)(got), (double)(want))

/* 按位组装一条 DF17 地面位置报文。parity 不填——本测试直接调
 * decode 的字段提取部分，不走 CRC 校验路径。 */
static void build_surface_msg(unsigned char msg[14], int metype, int mov,
                              int track_valid, int trk, int fflag,
                              uint32_t lat_cpr, uint32_t lon_cpr)
{
    memset(msg, 0, 14);
    msg[0] = (17 << 3);              /* DF17, CA=0 */
    msg[1] = 0x4C; msg[2] = 0xA1; msg[3] = 0xBD;   /* 任意 ICAO */

    /* ME bit 0..4 = TC；5..11 = MOV */
    msg[4] = (unsigned char)((metype << 3) | ((mov >> 4) & 0x07));
    /* ME 8..11 = MOV 低 4 位；12 = S；13..15 = TRK 高 3 位 */
    msg[5] = (unsigned char)(((mov & 0x0F) << 4) |
                             ((track_valid & 1) << 3) |
                             ((trk >> 4) & 0x07));
    /* ME 16..19 = TRK 低 4 位；20 = T(0)；21 = F；22..23 = LAT 高 2 位 */
    msg[6] = (unsigned char)(((trk & 0x0F) << 4) |
                             ((fflag & 1) << 2) |
                             ((lat_cpr >> 15) & 0x03));
    msg[7] = (unsigned char)((lat_cpr >> 7) & 0xFF);
    msg[8] = (unsigned char)(((lat_cpr & 0x7F) << 1) | ((lon_cpr >> 16) & 1));
    msg[9] = (unsigned char)((lon_cpr >> 8) & 0xFF);
    msg[10] = (unsigned char)(lon_cpr & 0xFF);
}

/* 只跑字段提取那一段：mode_s_decode 前半有 CRC/ICAO 缓存逻辑，本测试不关心。
 * 直接复刻 decode 里 metype 5-8 分支的提取式，与被测代码同源比对没有意义，
 * 所以这里改为**调用真正的 mode_s_decode**，并把 CRC 相关字段忽略。 */
static void decode_fields(unsigned char msg[14], struct mode_s_msg *mm)
{
    mode_s_t self;
    memset(&self, 0, sizeof(self));
    memset(mm, 0, sizeof(*mm));
    mode_s_decode(&self, mm, msg);
}

static void test_movement_table_boundaries(void)
{
    printf("-- movement 分段表边界 --\n");
    /* 每一段的首尾都钉住：错一格立刻暴露 */
    CHECK_NEAR(decode_movement_field(0),   -1.0,  1e-9, "0 = 不可用");
    CHECK_NEAR(decode_movement_field(1),    0.0,  1e-9, "1 = 停止");
    CHECK_NEAR(decode_movement_field(2),    0.125, 1e-9, "2 段首");
    CHECK_NEAR(decode_movement_field(8),    0.875, 1e-9, "8 段尾");
    CHECK_NEAR(decode_movement_field(9),    1.0,  1e-9, "9 段首");
    CHECK_NEAR(decode_movement_field(12),   1.75, 1e-9, "12 段尾");
    CHECK_NEAR(decode_movement_field(13),   2.0,  1e-9, "13 段首");
    CHECK_NEAR(decode_movement_field(38),  14.5,  1e-9, "38 段尾");
    CHECK_NEAR(decode_movement_field(39),  15.0,  1e-9, "39 段首");
    CHECK_NEAR(decode_movement_field(93),  69.0,  1e-9, "93 段尾");
    CHECK_NEAR(decode_movement_field(94),  70.0,  1e-9, "94 段首");
    CHECK_NEAR(decode_movement_field(108), 98.0,  1e-9, "108 段尾");
    CHECK_NEAR(decode_movement_field(109), 100.0, 1e-9, "109 段首");
    CHECK_NEAR(decode_movement_field(123), 170.0, 1e-9, "123 段尾");
    CHECK_NEAR(decode_movement_field(124), 175.0, 1e-9, "124 = >=175kt");
    CHECK_NEAR(decode_movement_field(125), -1.0,  1e-9, "125 保留");
    CHECK_NEAR(decode_movement_field(127), -1.0,  1e-9, "127 保留");

    /* 单调性：整张表必须单调不减，否则某段公式接不上 */
    double prev = -2.0;
    for (int i = 1; i <= 124; i++) {
        double v = decode_movement_field(i);
        CHECK(v >= prev, "movement 表在 %d 处不单调 (%.3f < %.3f)\n", i, v, prev);
        prev = v;
    }
}

static void test_bit_extraction(void)
{
    printf("-- 位段提取 --\n");
    unsigned char msg[14];
    struct mode_s_msg mm;

    /* 全 1 的极值：每个字段都应取到各自的最大值，串位会立刻露馅 */
    build_surface_msg(msg, 7, 0x7F, 1, 0x7F, 1, 0x1FFFF, 0x1FFFF);
    decode_fields(msg, &mm);
    CHECK_EQ_I(mm.metype, 7, "metype");
    CHECK_EQ_I(mm.surface_movement_raw, 0x7F, "MOV 全 1");
    CHECK_EQ_I(mm.surface_track_valid, 1, "S=1");
    CHECK_EQ_I(mm.surface_track_raw, 0x7F, "TRK 全 1");
    CHECK(mm.fflag != 0, "fflag=1\n");
    CHECK_EQ_I(mm.raw_latitude, 0x1FFFF, "LAT 全 1");
    CHECK_EQ_I(mm.raw_longitude, 0x1FFFF, "LON 全 1");

    /* 全 0 */
    build_surface_msg(msg, 5, 0, 0, 0, 0, 0, 0);
    decode_fields(msg, &mm);
    CHECK_EQ_I(mm.metype, 5, "metype 下界");
    CHECK_EQ_I(mm.surface_movement_raw, 0, "MOV 全 0");
    CHECK_EQ_I(mm.surface_track_valid, 0, "S=0");
    CHECK_EQ_I(mm.surface_track_raw, 0, "TRK 全 0");
    CHECK_EQ_I(mm.fflag, 0, "fflag=0");
    CHECK_EQ_I(mm.raw_latitude, 0, "LAT 全 0");
    CHECK_EQ_I(mm.raw_longitude, 0, "LON 全 0");

    /* 各字段取互不相同的值：串位（把 TRK 读成 MOV 之类）会被抓出来 */
    build_surface_msg(msg, 6, 0x55, 1, 0x2A, 0, 0x12345, 0x0ABCD);
    decode_fields(msg, &mm);
    CHECK_EQ_I(mm.surface_movement_raw, 0x55, "MOV 交叉值");
    CHECK_EQ_I(mm.surface_track_raw, 0x2A, "TRK 交叉值");
    CHECK_EQ_I(mm.surface_track_valid, 1, "S 交叉值");
    CHECK_EQ_I(mm.fflag, 0, "fflag 交叉值");
    CHECK_EQ_I(mm.raw_latitude, 0x12345, "LAT 交叉值");
    CHECK_EQ_I(mm.raw_longitude, 0x0ABCD, "LON 交叉值");
}

static void test_track_decoding(void)
{
    printf("-- 地面航迹 --\n");
    unsigned char msg[14];
    struct mode_s_msg mm;

    /* 步长 360/128 = 2.8125°；0 → 0°，32 → 90°，64 → 180°，96 → 270° */
    const struct { int raw; double deg; } cases[] = {
        { 0, 0.0 }, { 32, 90.0 }, { 64, 180.0 }, { 96, 270.0 }, { 127, 357.1875 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        build_surface_msg(msg, 7, 40, 1, cases[i].raw, 0, 0, 0);
        decode_fields(msg, &mm);
        CHECK_NEAR(mm.surface_track, cases[i].deg, 1e-6, "TRK 角度");
    }

    /* S=0 时 track 无意义——解出来的值不作数，调用方必须看 valid 位。
     * 这里断言的是"valid 位如实反映 S"，而不是"无效时把值清零"。 */
    build_surface_msg(msg, 7, 40, 0, 64, 0, 0, 0);
    decode_fields(msg, &mm);
    CHECK_EQ_I(mm.surface_track_valid, 0, "S=0 时 valid 位");
}

static void test_no_altitude_on_surface(void)
{
    printf("-- 地面帧不应带高度 --\n");
    /* MOV/TRK 占的正是空中帧放高度的那几位。若哪天有人给 5-8 分支接上
     * decode_ac12_field，就会把 MOV 当高度解出一个荒谬的数——用一个
     * MOV 很大的样本把这条钉住。 */
    unsigned char msg[14];
    struct mode_s_msg mm;
    build_surface_msg(msg, 8, 120, 1, 64, 1, 0x10000, 0x10000);
    decode_fields(msg, &mm);
    CHECK_EQ_I(mm.altitude, 0, "地面帧 altitude 必须保持 0");
}

int main(void)
{
    test_movement_table_boundaries();
    test_bit_extraction();
    test_track_decoding();
    test_no_altitude_on_surface();

    if (g_fail == 0) {
        printf("PASS: all mode-s surface tests passed\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_fail);
    return 1;
}
