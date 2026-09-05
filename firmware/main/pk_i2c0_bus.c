/* pk_i2c0_bus.c — 见 pk_i2c0_bus.h 的设计说明。
 *
 * 目标端胶水（esp_driver_i2c）用 PK_I2C0_BUS_HOST_TEST 隔离，让 host 单测
 * （firmware/test/test_pk_i2c0_bus.c）可以零 ESP-IDF 直接编本文件——
 * 同 rp2040/rf_safety.c 的 RF_SAFETY_HOST_TEST 模式。 */

#include "pk_i2c0_bus.h"

#ifndef PK_I2C0_BUS_HOST_TEST
#include "esp_log.h"
#endif

/* s_bus：app_main 单线程写一次（init 成功时发布），之后全系统只读。
 * s_generation：单写者 = pk_i2c0_recover.c（恢复闸门临界区内），
 * 多读者 = 各器件任务轮询。并发论证见 pk_i2c0_bus.h。 */
static i2c_master_bus_handle_t s_bus;
static volatile uint32_t       s_generation;

uint32_t pk_i2c0_bus_generation(void)
{
    return s_generation;
}

void pk_i2c0_bus_generation_inc(void)
{
    s_generation++;
}

i2c_master_bus_handle_t pk_i2c0_bus_get(void)
{
    return s_bus;
}

#ifndef PK_I2C0_BUS_HOST_TEST

static const char *TAG = "i2c0";

esp_err_t pk_i2c0_bus_init(void)
{
    /* 幂等：重复 init 是无害 no-op。IDF 不允许 i2c_new_master_bus 对同一
     * port 建第二次，所以「已建好」必须在这里短路而不是往下撞。 */
    if (s_bus != NULL) return ESP_OK;

    /* 端口配置照抄 imu_task 旧日的 i2c_bring_up()，一个值都不改：
     * 内部上拉是板上的临时手段，等整板波形实测后再动（PLAN.md §6.1）。 */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 7,    /* docs/hardware/board_pinout: I2C0_SDA */
        .scl_io_num = 8,    /* docs/hardware/board_pinout: I2C0_SCL */
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    /* 先落局部变量、成功才发布到 s_bus：失败路径（引脚被占/内存不足）下
     * 必须维持「get() 返回 NULL」的契约，不能留下半初始化状态。 */
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;

    s_bus = bus;
    ESP_LOGI(TAG, "master bus up (SDA GPIO7 / SCL GPIO8 @ 400 kHz)");
    return ESP_OK;
}

#else

esp_err_t pk_i2c0_bus_init(void)
{
    /* host 上没有 esp_driver_i2c，总线建不出来是**预期**。返回失败而不是
     * 假装成功，测试据此钉住「init 失败 ⇒ get() 仍为 NULL」的契约。 */
    return ESP_ERR_INVALID_STATE;
}

#endif /* PK_I2C0_BUS_HOST_TEST */
