/*
 * test_sd_trim_guard.c — SD TRIM no-op 守卫 host 单测。
 *
 * 本测试只验证守卫的 no-op 语义：wrap 返回 ESP_OK、计数递增、__real
 * 从不被转发。当前 SDK 的 FatFS 能力检查会拒绝普通 microSD 的 TRIM，
 * 不能用直接调用守卫的测试证明 microSD 故障由这条路径导致。
 * 实际 --wrap / -u 接线由 test_sd_trim_link.py 的归档链接测试另行验证。
 *
 * host 下没有 --wrap 重定向，__wrap_sdmmc_erase_sectors 只是普通函数；
 * 假 __real 的意义正是「谁把 no-op 改回转发，这里就会被看见」。
 *
 * -D sdmmc_erase_arg_t=int：record_stubs 的 sdmmc_cmd.h 还没有这个 SDK
 * typedef（IDF 6.0.1 在 sd_protocol_types.h:304，取值 0/1），host 直编用
 * 宏代换保持两个编译单元签名一致。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -D sdmmc_erase_arg_t=int \
 *      -I firmware/test/record_stubs -I firmware/test/host_stubs \
 *      -I firmware/main \
 *      -o /tmp/test_sd_trim_guard firmware/test/test_sd_trim_guard.c \
 *      firmware/main/pk_sd_trim_guard.c \
 *      && /tmp/test_sd_trim_guard
 */
#include <assert.h>
#include <stdio.h>

#include "pk_sd_trim_guard.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"

extern esp_err_t __wrap_sdmmc_erase_sectors(sdmmc_card_t *, size_t, size_t,
                                            sdmmc_erase_arg_t);

static int real_calls;
static sdmmc_card_t card;

/* 假 __real：wrap 永远不该转发到这里（no-op 语义）。 */
esp_err_t __real_sdmmc_erase_sectors(sdmmc_card_t *c, size_t start,
                                     size_t count, sdmmc_erase_arg_t arg)
{
    (void)c; (void)start; (void)count; (void)arg;
    real_calls++;
    return ESP_FAIL;
}

int main(void)
{
    assert(pk_sd_trim_guard_count() == 0);

    /* 守卫报告提示已处理，不调用介质擦除。 */
    assert(__wrap_sdmmc_erase_sectors(&card, 17, 3, (sdmmc_erase_arg_t)0)
           == ESP_OK);
    assert(__wrap_sdmmc_erase_sectors(&card, 4096, 128, (sdmmc_erase_arg_t)1)
           == ESP_OK);
    assert(pk_sd_trim_guard_count() == 2);

    /* no-op 守卫不解引用卡对象；空范围/空卡同样处理并计数。 */
    assert(__wrap_sdmmc_erase_sectors(NULL, 0, 0, (sdmmc_erase_arg_t)0)
           == ESP_OK);
    assert(pk_sd_trim_guard_count() == 3);

    assert(real_calls == 0);

    puts("PASS SD trim guard: erase no-op, counted, real never called");
    return 0;
}
