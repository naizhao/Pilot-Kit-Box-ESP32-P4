/* driver/i2c_master.h — 模拟器桩。
 *
 * imu_task.h 为了给驱动层暴露总线句柄而包含它。PFD 侧只用到该头文件里的
 * pk_imu_sample_t，句柄类型仅需存在、不需可用，故给成不透明指针。
 */
#pragma once

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;
