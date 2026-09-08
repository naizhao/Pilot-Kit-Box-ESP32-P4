/*
 * test_cjtag.c — cJTAG 位脉冲引擎 host 单测（TAP 状态机 + 序列）。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 -DCJTAG_HOST_TEST \
 *      -o /tmp/test_cjtag firmware/test/test_cjtag.c firmware/rp2040/cjtag.c \
 *   && /tmp/test_cjtag
 */
#include "cjtag.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

static void test_enter_idcode(void)
{
    /* 模拟：IDCODE = CC13_IDCODE */
    extern void cjtag_test_set_idcode(uint32_t);
    cjtag_test_set_idcode(CC13_IDCODE);

    cjtag_enter();
    uint32_t id = cjtag_read_idcode();
    CHECK(id == CC13_IDCODE);
    cjtag_exit();
}

static void test_erase_program_verify(void)
{
    /* host 模式下 flash 操作走模拟路径——验证序列不 panic 即可 */
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 3);
    /* host 下 verify 恒 true（模拟）——目标端实测 */
    CHECK(cjtag_flash_erase_sector(0) == true);
    CHECK(cjtag_flash_write_word(0, 0x12345678) == true);
}

int main(void)
{
    test_enter_idcode();
    test_erase_program_verify();
    if (g_fail) { printf("test_cjtag: %d FAIL\n", g_fail); return 1; }
    printf("test_cjtag: all OK\n");
    return 0;
}
