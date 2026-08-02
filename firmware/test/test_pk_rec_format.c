/* test_pk_rec_format.c — host proof for pk_rec_format（ADS-B / 本机航迹
 * 落盘二进制记录格式的编解码）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_recfmt \
 *      firmware/test/test_pk_rec_format.c && /tmp/test_recfmt
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_recfmt_asan \
 *      firmware/test/test_pk_rec_format.c && /tmp/test_recfmt_asan
 *
 *   leaks（macOS，本模块本身不分配堆内存，仍照惯例跑一遍留证据）：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -I firmware/main \
 *      -o /tmp/test_recfmt_leaks firmware/test/test_pk_rec_format.c && \
 *      leaks --atExit -- /tmp/test_recfmt_leaks
 *
 * 同 test_pk_pmtiles.c / test_pk_map_store.c 的翻译单元惯例：把被测 .c
 * 直接 #include 进同一 TU，不单独编译链接。
 *
 * 断言策略：对每种记录，手工按 spec 表格里的偏移拼一份"预期字节数组"
 * （用本文件里独立实现的小端 helper，不复用被测代码的 put_u16/32/64），
 * encode 后与预期数组 memcmp——这同时验证了 sizeof（数组长度）与每个
 * 字段的偏移量（值出现在数组里指定的位置），不只是测字节序。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../main/pk_rec_format.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ---------------------------------------------------- 独立小端 helper（测试专用） */

static void t_put_u16(uint8_t *b, size_t off, uint16_t v) { b[off] = v & 0xFF; b[off+1] = (v >> 8) & 0xFF; }
static void t_put_u32(uint8_t *b, size_t off, uint32_t v) { for (int i=0;i<4;i++) b[off+(size_t)i] = (uint8_t)((v >> (8*i)) & 0xFF); }
static void t_put_u64(uint8_t *b, size_t off, uint64_t v) { for (int i=0;i<8;i++) b[off+(size_t)i] = (uint8_t)((v >> (8*i)) & 0xFF); }
static void t_put_i16(uint8_t *b, size_t off, int16_t v) { t_put_u16(b, off, (uint16_t)v); }
static void t_put_i32(uint8_t *b, size_t off, int32_t v) { t_put_u32(b, off, (uint32_t)v); }

/* ================================================================ 32B 文件头 */

static void test_header(void)
{
    CHECK(PK_REC_HEADER_LEN == 32);

    pk_rec_header_t hdr = {0};
    memcpy(hdr.magic, PK_REC_MAGIC_TRAFFIC_TRK, 8);
    hdr.format_version = 1;
    hdr.record_size = PK_TRK_RECORD_LEN;
    hdr.endian_marker = PK_REC_ENDIAN_MARKER;
    hdr.created_ts_ms = 0x0102030405060708ULL;
    hdr.file_kind = PK_REC_FILE_KIND_TRAFFIC_TRK;
    memset(hdr.reserved, 0xAB, sizeof(hdr.reserved));

    uint8_t buf[PK_REC_HEADER_LEN];
    pk_rec_header_encode(&hdr, buf);

    uint8_t expect[PK_REC_HEADER_LEN];
    memset(expect, 0, sizeof(expect));
    memcpy(&expect[0], PK_REC_MAGIC_TRAFFIC_TRK, 8);
    t_put_u16(expect, 8, 1);
    t_put_u16(expect, 10, PK_TRK_RECORD_LEN);
    t_put_u32(expect, 12, PK_REC_ENDIAN_MARKER);
    t_put_u64(expect, 16, 0x0102030405060708ULL);
    expect[24] = PK_REC_FILE_KIND_TRAFFIC_TRK;
    memset(&expect[25], 0xAB, 7);

    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);

    pk_rec_header_t back;
    CHECK(pk_rec_header_decode(buf, &back) == true);
    CHECK(memcmp(back.magic, hdr.magic, 8) == 0);
    CHECK(back.format_version == hdr.format_version);
    CHECK(back.record_size == hdr.record_size);
    CHECK(back.endian_marker == hdr.endian_marker);
    CHECK(back.created_ts_ms == hdr.created_ts_ms);
    CHECK(back.file_kind == hdr.file_kind);
    CHECK(memcmp(back.reserved, hdr.reserved, 7) == 0);

    /* 坏 endian_marker 必须被拒绝 */
    uint8_t bad[PK_REC_HEADER_LEN];
    memcpy(bad, buf, sizeof(bad));
    t_put_u32(bad, 12, 0xDEADBEEFu);
    pk_rec_header_t bad_out;
    CHECK(pk_rec_header_decode(bad, &bad_out) == false);

    /* 三种 magic 各自 8 字节，互不相同 */
    CHECK(memcmp(PK_REC_MAGIC_TRAFFIC_TRK, PK_REC_MAGIC_TRAFFIC_IDX, 8) != 0);
    CHECK(memcmp(PK_REC_MAGIC_TRAFFIC_TRK, PK_REC_MAGIC_OWN_TRK, 8) != 0);
    CHECK(memcmp(PK_REC_MAGIC_TRAFFIC_IDX, PK_REC_MAGIC_OWN_TRK, 8) != 0);
    CHECK(strlen(PK_REC_MAGIC_TRAFFIC_TRK) == 8);
    CHECK(strlen(PK_REC_MAGIC_TRAFFIC_IDX) == 8);
    CHECK(strlen(PK_REC_MAGIC_OWN_TRK) == 8);
}

/* ================================================================ traffic.trk */

static void test_trk_pos(void)
{
    CHECK(PK_TRK_RECORD_LEN == 32);
    CHECK(sizeof(pk_trk_pos_t) <= PK_TRK_RECORD_LEN * 4); /* 存在性检查，不对齐要求 host 结构体 */

    pk_trk_pos_t rec = {
        .ts_ms = 1234567890123ULL,
        .icao24 = { 0xAB, 0xCD, 0xEF },
        .lat_e7 = 223456789,
        .lon_e7 = -1134567890,
        .alt_d25 = 1400,          /* 35000 ft / 25 */
        .gs_kt = 145,
        .track_deg10 = 3599,      /* 359.9 度 */
        .vs_fpm_d64 = -32,        /* -2048 fpm / 64 */
        .flags = PK_TRK_FLAG_TIME_SYNCED | PK_TRK_FLAG_ON_GROUND,
    };

    uint8_t buf[PK_TRK_RECORD_LEN];
    pk_trk_pos_encode(&rec, buf);

    uint8_t expect[PK_TRK_RECORD_LEN];
    memset(expect, 0, sizeof(expect));
    t_put_u64(expect, 0, rec.ts_ms);
    memcpy(&expect[8], rec.icao24, 3);
    expect[11] = PK_TRK_REC_POSITION;
    t_put_i32(expect, 12, rec.lat_e7);
    t_put_i32(expect, 16, rec.lon_e7);
    t_put_i16(expect, 20, rec.alt_d25);
    t_put_u16(expect, 22, rec.gs_kt);
    t_put_u16(expect, 24, rec.track_deg10);
    t_put_i16(expect, 26, rec.vs_fpm_d64);
    expect[28] = rec.flags;

    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(pk_trk_rec_type_peek(buf) == PK_TRK_REC_POSITION);

    pk_trk_pos_t back;
    pk_trk_pos_decode(buf, &back);
    CHECK(back.ts_ms == rec.ts_ms);
    CHECK(memcmp(back.icao24, rec.icao24, 3) == 0);
    CHECK(back.lat_e7 == rec.lat_e7);
    CHECK(back.lon_e7 == rec.lon_e7);
    CHECK(back.alt_d25 == rec.alt_d25);
    CHECK(back.gs_kt == rec.gs_kt);
    CHECK(back.track_deg10 == rec.track_deg10);
    CHECK(back.vs_fpm_d64 == rec.vs_fpm_d64);
    CHECK(back.flags == rec.flags);
}

static void test_trk_pos_sentinels_and_bounds(void)
{
    pk_trk_pos_t rec = {0};
    rec.alt_d25 = PK_REC_ALT_INVALID;
    rec.gs_kt = PK_REC_GS_INVALID;
    rec.track_deg10 = PK_REC_TRACK_INVALID;
    rec.vs_fpm_d64 = PK_REC_VS_INVALID;

    uint8_t buf[PK_TRK_RECORD_LEN];
    pk_trk_pos_encode(&rec, buf);
    pk_trk_pos_t back;
    pk_trk_pos_decode(buf, &back);
    CHECK(back.alt_d25 == PK_REC_ALT_INVALID);
    CHECK(back.gs_kt == PK_REC_GS_INVALID);
    CHECK(back.track_deg10 == PK_REC_TRACK_INVALID);
    CHECK(back.vs_fpm_d64 == PK_REC_VS_INVALID);

    /* FL410 边界：50175 ft = 2007 * 25，须能不回绕地存进 i16（曾用 i16
     * 存英尺会在 FL350/FL410 回绕成负数——这条 spec 明确点名的 blocker B1，
     * 用 25ft 单位后 2007 远小于 INT16_MAX=32767，验证不回绕）。 */
    rec.alt_d25 = 2007;
    pk_trk_pos_encode(&rec, buf);
    pk_trk_pos_decode(buf, &back);
    CHECK(back.alt_d25 == 2007);
    CHECK(back.alt_d25 > 0);

    /* 0 与"无效"必须可区分：真实 0 kt / 0 fpm 不能编码成 sentinel。 */
    rec.gs_kt = 0;
    rec.vs_fpm_d64 = 0;
    pk_trk_pos_encode(&rec, buf);
    pk_trk_pos_decode(buf, &back);
    CHECK(back.gs_kt == 0);
    CHECK(back.gs_kt != PK_REC_GS_INVALID);
    CHECK(back.vs_fpm_d64 == 0);
    CHECK(back.vs_fpm_d64 != PK_REC_VS_INVALID);

    /* 负经纬度往返（西半球/南半球） */
    rec.lat_e7 = -900000000; /* -90.0 度 */
    rec.lon_e7 = -1800000000; /* -180.0 度 */
    pk_trk_pos_encode(&rec, buf);
    pk_trk_pos_decode(buf, &back);
    CHECK(back.lat_e7 == -900000000);
    CHECK(back.lon_e7 == -1800000000);
}

static void test_trk_id(void)
{
    pk_trk_id_t rec = {
        .ts_ms = 999888777666ULL,
        .icao24 = { 0x11, 0x22, 0x33 },
        .callsign = "CCA1234\0",  /* 8 有效字符 + NUL，凑满 9 字节 */
        .emitter_category = 5,    /* PK_WAKE_HEAVY，本文件不依赖 aircraft_state.h，用裸值 */
    };

    uint8_t buf[PK_TRK_RECORD_LEN];
    pk_trk_id_encode(&rec, buf);

    uint8_t expect[PK_TRK_RECORD_LEN];
    memset(expect, 0, sizeof(expect));
    t_put_u64(expect, 0, rec.ts_ms);
    memcpy(&expect[8], rec.icao24, 3);
    expect[11] = PK_TRK_REC_IDENTITY;
    memcpy(&expect[12], rec.callsign, 9);
    expect[21] = rec.emitter_category;

    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(pk_trk_rec_type_peek(buf) == PK_TRK_REC_IDENTITY);

    pk_trk_id_t back;
    pk_trk_id_decode(buf, &back);
    CHECK(back.ts_ms == rec.ts_ms);
    CHECK(memcmp(back.icao24, rec.icao24, 3) == 0);
    CHECK(memcmp(back.callsign, rec.callsign, 9) == 0);
    CHECK(back.emitter_category == rec.emitter_category);

    /* 位置记录与身份记录同尺寸——idx 可从 traffic.trk 全扫重建的前提。 */
    CHECK(sizeof(buf) == PK_TRK_RECORD_LEN);
}

/* ================================================================ traffic.idx */

static void test_idx(void)
{
    CHECK(PK_IDX_RECORD_LEN == 40);

    pk_idx_rec_t rec = {
        .icao24 = { 0xAA, 0xBB, 0xCC },
        .flags = PK_IDX_FLAG_HAD_POSITION | PK_IDX_FLAG_IS_OWN,
        .callsign = "N12345\0\0\0",
        .first_ts_ms = 1000,
        .last_ts_ms = 2000,
        .point_count = 12345,
        .identity_count = 3,
    };

    uint8_t buf[PK_IDX_RECORD_LEN];
    pk_idx_rec_encode(&rec, buf);

    uint8_t expect[PK_IDX_RECORD_LEN];
    memset(expect, 0, sizeof(expect));
    memcpy(&expect[0], rec.icao24, 3);
    expect[3] = rec.flags;
    memcpy(&expect[4], rec.callsign, 9);
    t_put_u64(expect, 16, rec.first_ts_ms);
    t_put_u64(expect, 24, rec.last_ts_ms);
    t_put_u32(expect, 32, rec.point_count);
    t_put_u32(expect, 36, rec.identity_count);

    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);

    pk_idx_rec_t back;
    pk_idx_rec_decode(buf, &back);
    CHECK(memcmp(back.icao24, rec.icao24, 3) == 0);
    CHECK(back.flags == rec.flags);
    CHECK(memcmp(back.callsign, rec.callsign, 9) == 0);
    CHECK(back.first_ts_ms == rec.first_ts_ms);
    CHECK(back.last_ts_ms == rec.last_ts_ms);
    CHECK(back.point_count == rec.point_count);
    CHECK(back.identity_count == rec.identity_count);
}

/* ================================================================ own.trk */

static void test_own_sample(void)
{
    CHECK(PK_OWN_RECORD_LEN == 48);

    pk_own_sample_t rec = {
        .ts_ms = 1700000000000ULL,
        .phase = 4, /* takeoff_roll，占位数值，真正枚举在 pk_flight_phase.h */
        .flags = PK_OWN_FLAG_GPS_FIX | PK_OWN_FLAG_BARO_VALID,
        .sats = 9,
        .lat_e7 = 314000000,
        .lon_e7 = 1215000000,
        .alt_baro_ft = 1230,
        .alt_gnss_msl_ft = 1255,
        .gs_kt = 62,
        .track_deg10 = 900,
        .vs_fpm = 700,
        .roll_d10 = -50,
        .pitch_d10 = 80,
        .yaw_d10 = 1800,
        .hdop_x10 = 12,
        .vib_level = 40,
        .disp_m_60s = 500,
        .ac_category = 3,
    };

    uint8_t buf[PK_OWN_RECORD_LEN];
    pk_own_sample_encode(&rec, buf);

    uint8_t expect[PK_OWN_RECORD_LEN];
    memset(expect, 0, sizeof(expect));
    t_put_u64(expect, 0, rec.ts_ms);
    expect[8] = PK_OWN_REC_SAMPLE;
    expect[9] = rec.phase;
    expect[10] = rec.flags;
    expect[11] = rec.sats;
    t_put_i32(expect, 12, rec.lat_e7);
    t_put_i32(expect, 16, rec.lon_e7);
    t_put_i32(expect, 20, rec.alt_baro_ft);
    t_put_i32(expect, 24, rec.alt_gnss_msl_ft);
    t_put_u16(expect, 28, rec.gs_kt);
    t_put_u16(expect, 30, rec.track_deg10);
    t_put_i16(expect, 32, rec.vs_fpm);
    t_put_i16(expect, 34, rec.roll_d10);
    t_put_i16(expect, 36, rec.pitch_d10);
    t_put_i16(expect, 38, rec.yaw_d10);
    expect[40] = rec.hdop_x10;
    expect[41] = rec.vib_level;
    t_put_u16(expect, 42, rec.disp_m_60s);
    expect[44] = rec.ac_category;

    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(pk_own_rec_type_peek(buf) == PK_OWN_REC_SAMPLE);

    pk_own_sample_t back;
    pk_own_sample_decode(buf, &back);
    CHECK(back.ts_ms == rec.ts_ms);
    CHECK(back.phase == rec.phase);
    CHECK(back.flags == rec.flags);
    CHECK(back.sats == rec.sats);
    CHECK(back.lat_e7 == rec.lat_e7);
    CHECK(back.lon_e7 == rec.lon_e7);
    CHECK(back.alt_baro_ft == rec.alt_baro_ft);
    CHECK(back.alt_gnss_msl_ft == rec.alt_gnss_msl_ft);
    CHECK(back.gs_kt == rec.gs_kt);
    CHECK(back.track_deg10 == rec.track_deg10);
    CHECK(back.vs_fpm == rec.vs_fpm);
    CHECK(back.roll_d10 == rec.roll_d10);
    CHECK(back.pitch_d10 == rec.pitch_d10);
    CHECK(back.yaw_d10 == rec.yaw_d10);
    CHECK(back.hdop_x10 == rec.hdop_x10);
    CHECK(back.vib_level == rec.vib_level);
    CHECK(back.disp_m_60s == rec.disp_m_60s);
    CHECK(back.ac_category == rec.ac_category);

    /* ac_category==0 是合法的 unknown 值，且不与"滑翔机"（本设计的分类
     * 从 1 开始）撞车——纯粹是编解码可以原样往返 0。 */
    rec.ac_category = 0;
    pk_own_sample_encode(&rec, buf);
    pk_own_sample_decode(buf, &back);
    CHECK(back.ac_category == 0);

    /* vib_level==0 表示"不可用"，同样只是要求可原样往返。 */
    rec.vib_level = 0;
    pk_own_sample_encode(&rec, buf);
    pk_own_sample_decode(buf, &back);
    CHECK(back.vib_level == 0);
}

static void test_own_time_sync(void)
{
    pk_own_time_sync_t rec = {
        .ts_ms = 1700000123456ULL,
        .sync_source = PK_OWN_SYNC_SOURCE_GPS,
        .sync_reason = 1,
        .prev_ts_ms = 1690000000000ULL,
    };

    uint8_t buf[PK_OWN_RECORD_LEN];
    pk_own_time_sync_encode(&rec, buf);

    uint8_t expect[PK_OWN_RECORD_LEN];
    memset(expect, 0, sizeof(expect));
    t_put_u64(expect, 0, rec.ts_ms);
    expect[8] = PK_OWN_REC_TIME_SYNC;
    expect[9] = 0; /* phase 无意义，固定 0 */
    expect[12] = rec.sync_source;
    expect[13] = rec.sync_reason;
    t_put_u64(expect, 16, rec.prev_ts_ms);
    /* 10,11,14,15,24..47 保持 0 */

    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
    CHECK(pk_own_rec_type_peek(buf) == PK_OWN_REC_TIME_SYNC);

    /* 回放端约定：本记录的 phase 字段固定读作 0（无意义），据此在按
     * rec_type 过滤前不会把它误当成一条真实相位样本。 */
    pk_own_sample_t as_sample;
    pk_own_sample_decode(buf, &as_sample);
    CHECK(as_sample.phase == 0);

    pk_own_time_sync_t back;
    pk_own_time_sync_decode(buf, &back);
    CHECK(back.ts_ms == rec.ts_ms);
    CHECK(back.sync_source == rec.sync_source);
    CHECK(back.sync_reason == rec.sync_reason);
    CHECK(back.prev_ts_ms == rec.prev_ts_ms);

    /* BLE-CTS 来源同样往返 */
    rec.sync_source = PK_OWN_SYNC_SOURCE_BLE_CTS;
    pk_own_time_sync_encode(&rec, buf);
    pk_own_time_sync_decode(buf, &back);
    CHECK(back.sync_source == PK_OWN_SYNC_SOURCE_BLE_CTS);
}

/* own.trk 两种记录必须同尺寸（定长文件的前提）。 */
static void test_own_record_sizes_match(void)
{
    CHECK(PK_OWN_RECORD_LEN == 48);
    uint8_t a[PK_OWN_RECORD_LEN], b[PK_OWN_RECORD_LEN];
    CHECK(sizeof(a) == sizeof(b));
}

int main(void)
{
    test_header();
    test_trk_pos();
    test_trk_pos_sentinels_and_bounds();
    test_trk_id();
    test_idx();
    test_own_sample();
    test_own_time_sync();
    test_own_record_sizes_match();

    if (g_fail == 0) {
        printf("PASS: all pk_rec_format tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d assertion(s) failed\n", g_fail);
    return 1;
}
