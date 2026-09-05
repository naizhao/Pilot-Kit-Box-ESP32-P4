/* test_pk_i2c0_bus.c — host proof for pk_i2c0_bus（I²C0 板级总线的纯逻辑部分）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -DPK_I2C0_BUS_HOST_TEST \
 *      -o /tmp/test_i2c0_bus firmware/test/test_pk_i2c0_bus.c \
 *      firmware/main/pk_i2c0_bus.c && /tmp/test_i2c0_bus
 *
 * 同 test_pk_i2c0_policy.c 的口径：只测「不碰硬件」的部分。总线创建
 * （i2c_new_master_bus）在 host 上不存在，目标端靠两板系构建 + app_main
 * 启动日志验证，这里不伪造。
 *
 * 钉死的四条契约：
 *   1. generation 开机初值 0 —— 各器件「该重放 bring-up 了」的唯一信号，
 *      语义与旧 pk_i2c0_recover_generation()（pk_i2c0_gate_t.generation）一致；
 *   2. inc 一次 +1、可累加 —— 只由 pk_i2c0_recover.c 在恢复判据判真处调用；
 *   3. 未 init 时 pk_i2c0_bus_get() 返回 NULL（同旧 imu_task 版本）；
 *   4. init 失败不得留下半初始化状态 —— get() 必须仍是 NULL。
 *
 * 并发语义 host 测不到，靠 pk_i2c0_bus.h 的注释钉死：generation 单写者
 * （pk_i2c0_recover.c，先拿到恢复闸门才 inc），多读者（imu/baro/touch
 * 任务各自轮询）；32 位对齐读写在双核上原子，volatile 防轮询缓存。
 */

#include <stdio.h>

#include "../main/pk_i2c0_bus.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

static void test_generation_starts_at_zero(void)
{
    CHECK(pk_i2c0_bus_generation() == 0);
}

static void test_generation_inc_advances_by_one(void)
{
    pk_i2c0_bus_generation_inc();
    CHECK(pk_i2c0_bus_generation() == 1);

    /* 多轮恢复各 +1，不许重置、不许跳变。 */
    pk_i2c0_bus_generation_inc();
    pk_i2c0_bus_generation_inc();
    CHECK(pk_i2c0_bus_generation() == 3);
}

static void test_bus_get_null_before_init(void)
{
    CHECK(pk_i2c0_bus_get() == NULL);
}

static void test_failed_init_leaves_no_half_state(void)
{
    /* host 上建不出 esp 总线（预期失败）：契约是失败后 get() 仍为 NULL，
     * 不能留下一个"看起来能用"的句柄。 */
    CHECK(pk_i2c0_bus_init() != ESP_OK);
    CHECK(pk_i2c0_bus_get() == NULL);
}

int main(void)
{
    test_generation_starts_at_zero();
    test_generation_inc_advances_by_one();
    test_bus_get_null_before_init();
    test_failed_init_leaves_no_half_state();

    if (g_fail == 0) printf("test_pk_i2c0_bus: ALL PASS\n");
    else             printf("test_pk_i2c0_bus: %d FAILED\n", g_fail);
    return g_fail != 0;
}
