/* test_qmc5883p.c — QMC5883P 纯解码的 host 单测（WP-B Task 4，TDD 先红后绿）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -DQMC5883P_HOST_TEST \
 *      -o /tmp/test_qmc5883p firmware/test/test_qmc5883p.c \
 *      firmware/main/qmc5883p.c && /tmp/test_qmc5883p
 *
 * 只测「不碰硬件」的部分：寄存器字节帧 → 值。注入的字节帧按 QMC5883P.pdf
 * 的寄存器布局手工构造（01H~06H 连读 6 字节：X LSB, X MSB, Y LSB, Y MSB,
 * Z LSB, Z MSB —— Table 14/15, p.15/18；16 位二进制补码，§9.2.1）。
 *
 * 目标端专属部分（探测/配置/轮询）不伪造覆盖，靠两板系构建 + 启动日志
 * 验证，同 test_pk_i2c0_bus.c 的口径。
 *
 * 钉死的契约：
 *   1. 小端拼装：低地址字节是 LSB（01H=X_LSB、02H=X_MSB，Table 14）；
 *   2. 二进制补码符号：负值、-32768、全 0xFF 帧（= -1,-1,-1）都安全，
 *      全 1 是「总线读失败常见残留」，必须解码成确定值而不是 UB；
 *   3. 状态解码只看 bit0(DRDY)/bit1(OVFL)（Table 16, p.15/18），其余位是
 *      厂测保留（§9.2.2），必须忽略 —— 0xFC 这类帧不得误报。
 */

#include <stdio.h>
#include <stdint.h>

#include "../main/qmc5883p.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* 1 正常帧：小端拼装（0x1234/0x5678/0x4321）。 */
static void test_decode_raw_positive_frame(void)
{
    /* X=0x1234: LSB=0x34@01H, MSB=0x12@02H —— 字节序颠倒时此断言必红 */
    const uint8_t frame[6] = { 0x34, 0x12, 0x78, 0x56, 0x21, 0x43 };
    int16_t x = 0, y = 0, z = 0;
    qmc5883p_decode_raw(frame, &x, &y, &z);
    CHECK(x == 0x1234);
    CHECK(y == 0x5678);
    CHECK(z == 0x4321);
}

/* 2 负值：二进制补码。-1234=0xFB2E, -2=0xFFFE, -32768=0x8000。 */
static void test_decode_raw_negative_values(void)
{
    const uint8_t frame[6] = {
        0x2E, 0xFB,   /* X = -1234 */
        0xFE, 0xFF,   /* Y = -2 */
        0x00, 0x80,   /* Z = -32768（饱和下限，§9.2.1） */
    };
    int16_t x = 0, y = 0, z = 0;
    qmc5883p_decode_raw(frame, &x, &y, &z);
    CHECK(x == -1234);
    CHECK(y == -2);
    CHECK(z == -32768);
}

/* 3 全 0xFF 帧容错：必须得到 -1,-1,-1（0xFFFF 补码），不得溢出/UB。 */
static void test_decode_raw_all_ones_frame(void)
{
    const uint8_t frame[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int16_t x = 0, y = 0, z = 0;
    qmc5883p_decode_raw(frame, &x, &y, &z);
    CHECK(x == -1);
    CHECK(y == -1);
    CHECK(z == -1);
}

/* 4 全 0 帧。 */
static void test_decode_raw_zero_frame(void)
{
    const uint8_t frame[6] = { 0 };
    int16_t x = 0x7FFF, y = 0x7FFF, z = 0x7FFF;
    qmc5883p_decode_raw(frame, &x, &y, &z);
    CHECK(x == 0);
    CHECK(y == 0);
    CHECK(z == 0);
}

/* 5 状态标志：DRDY=bit0、OVFL=bit1（Table 16, p.15/18）。 */
static void test_decode_status_flags(void)
{
    qmc5883p_status_t st = { false, false };

    qmc5883p_decode_status(0x01, &st);
    CHECK(st.drdy == true);
    CHECK(st.ovfl == false);

    qmc5883p_decode_status(0x02, &st);
    CHECK(st.drdy == false);
    CHECK(st.ovfl == true);

    qmc5883p_decode_status(0x03, &st);
    CHECK(st.drdy == true);
    CHECK(st.ovfl == true);

    qmc5883p_decode_status(0x00, &st);
    CHECK(st.drdy == false);
    CHECK(st.ovfl == false);
}

/* 6 保留位必须忽略：§9.2.2 说 09H 除 DRDY/OVFL 外「reserved for factory
 * use」，总线里出现过 0xFC 这类帧时不得误报成 DRDY/OVFL。 */
static void test_decode_status_ignores_reserved_bits(void)
{
    qmc5883p_status_t st = { true, true };

    qmc5883p_decode_status(0xFC, &st);
    CHECK(st.drdy == false);
    CHECK(st.ovfl == false);
}

int main(void)
{
    test_decode_raw_positive_frame();
    test_decode_raw_negative_values();
    test_decode_raw_all_ones_frame();
    test_decode_raw_zero_frame();
    test_decode_status_flags();
    test_decode_status_ignores_reserved_bits();

    if (g_fail == 0) printf("test_qmc5883p: ALL PASS\n");
    else             printf("test_qmc5883p: %d FAILED\n", g_fail);
    return g_fail != 0;
}
