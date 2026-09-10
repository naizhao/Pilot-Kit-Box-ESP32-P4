#pragma once
/*
 * host_stubs/driver/i2c_master.h — I²C 主机驱动的类型影子。
 *
 * 器件驱动（imu_task / baro_task / qmc5883p）自己不直接调 IDF 的事务 API：
 * 所有读写都过 pk_i2c0_bus_* 包装（见 pk_i2c0_bus.h 的板级互斥契约），
 * 所以 host 上只需要影子化**类型**和 add/rm_device 两个装配 API。
 *
 * add/rm_device 在这里**只声明不定义**：谁挂了几次器件、init 失败时有没有
 * 摘下来，正是被测的资源生命周期，实现由测试自己给。
 */
#include <stdint.h>

#include "esp_err.h"

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7  = 0,
    I2C_ADDR_BIT_LEN_10 = 1,
} i2c_addr_bit_len_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t           device_address;
    uint32_t           scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out_handle);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
