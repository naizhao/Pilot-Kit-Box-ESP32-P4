/* test_gps_antenna_diag.c — GNSS 天线原始状态与可操作结论必须分开。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_gps_antenna_diag firmware/test/test_gps_antenna_diag.c \
 *      && /tmp/test_gps_antenna_diag
 *
 * ATGM336H-6N 手册 §2.8 明确说无源天线恒报 ANTENNA OPEN。因此 OPEN 只能
 * 作为模块的原始电流判据，不能直接升级成“天线硬件故障”。反过来 SHORT 是
 * 保护状态，即使此前见过卫星也不能被接收证据掩盖。
 */
#include <stdio.h>
#include <string.h>

#include "gps_antenna_diag.h"

static int g_fail;

#define CHECK(x) do {                                                        \
    if (!(x)) {                                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
        g_fail++;                                                            \
    }                                                                        \
} while (0)

int main(void)
{
    CHECK(pk_gps_header_state(true, PK_GPS_ANT_RESULT_SHORT) == PK_GPS_HEADER_SHORT);
    CHECK(pk_gps_header_state(false, PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE) == PK_GPS_HEADER_ANT_UNKNOWN);
    CHECK(pk_gps_header_state(true, PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE) == PK_GPS_HEADER_SATS);
    CHECK(pk_gps_header_state(false, PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK) == PK_GPS_HEADER_SATS);
    CHECK(pk_gps_header_state(false, PK_GPS_ANT_RESULT_RF_RECEIVING) == PK_GPS_HEADER_SATS);
    CHECK(pk_gps_header_state(false, PK_GPS_ANT_RESULT_UNKNOWN) == PK_GPS_HEADER_NO_FIX);
    CHECK(pk_gps_ant_assess(PK_GPS_ANT_UNKNOWN, false) ==
          PK_GPS_ANT_RESULT_UNKNOWN);
    CHECK(pk_gps_ant_assess(PK_GPS_ANT_OK, false) ==
          PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK);

    /* 核心防误报合同：OPEN 也可能只是无源天线，不能判成硬故障。 */
    CHECK(pk_gps_ant_assess(PK_GPS_ANT_OPEN, false) ==
          PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE);
    CHECK(pk_gps_ant_assess(PK_GPS_ANT_OPEN, true) ==
          PK_GPS_ANT_RESULT_RF_RECEIVING);

    /* 接收证据能证明 RF 通路有用，但不能掩盖模块的短路保护。 */
    CHECK(pk_gps_ant_assess(PK_GPS_ANT_UNKNOWN, true) ==
          PK_GPS_ANT_RESULT_RF_RECEIVING);
    CHECK(pk_gps_ant_assess(PK_GPS_ANT_SHORT, true) ==
          PK_GPS_ANT_RESULT_SHORT);

    CHECK(strcmp(pk_gps_ant_raw_name(PK_GPS_ANT_UNKNOWN), "UNKNOWN") == 0);
    CHECK(strcmp(pk_gps_ant_raw_name(PK_GPS_ANT_OK), "OK") == 0);
    CHECK(strcmp(pk_gps_ant_raw_name(PK_GPS_ANT_OPEN), "OPEN") == 0);
    CHECK(strcmp(pk_gps_ant_raw_name(PK_GPS_ANT_SHORT), "SHORT") == 0);
    CHECK(strcmp(pk_gps_ant_raw_name((pk_gps_ant_t)99), "UNKNOWN") == 0);

    CHECK(strcmp(pk_gps_ant_result_name(PK_GPS_ANT_RESULT_UNKNOWN),
                 "UNKNOWN") == 0);
    CHECK(strcmp(pk_gps_ant_result_name(PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK),
                 "ACTIVE-LOAD-OK") == 0);
    CHECK(strcmp(pk_gps_ant_result_name(PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE),
                 "INDETERMINATE-OPEN-OR-PASSIVE") == 0);
    CHECK(strcmp(pk_gps_ant_result_name(PK_GPS_ANT_RESULT_RF_RECEIVING),
                 "RF-RECEIVING") == 0);
    CHECK(strcmp(pk_gps_ant_result_name(PK_GPS_ANT_RESULT_SHORT),
                 "SHORT") == 0);

    if (g_fail == 0) {
        puts("test_gps_antenna_diag: all OK");
        return 0;
    }
    fprintf(stderr, "test_gps_antenna_diag: %d check(s) FAILED\n", g_fail);
    return 1;
}
