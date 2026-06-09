/*
 * baro.h — BMP388 气压计驱动接口。
 *
 * 挂载在 I²C0 总线上(与 BNO085 IMU 共享)，地址 0x76。
 * 使用前必须先调用 pk_imu_init() 让总线建立，再调用 pk_baro_start()。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    valid;          /* 末次读取成功且 CHIP_ID 正确 */
    float   pressure_pa;    /* 气压, Pa */
    float   temp_c;         /* 温度, ℃ */
    int     alt_ft;         /* 气压高度, ft(用 QNH 换算) */
    int     vs_fpm;         /* 升降率, ft/min(气压高度微分) */
    int64_t updated_us;     /* esp_timer_get_time() of last good read */
} pk_baro_state_t;

/* 启动 baro_task。必须在 pk_imu_init() 成功后调用(依赖 I²C0 总线已建)。 */
void pk_baro_start(void);

/* 快照当前气压状态。返回 out->valid。线程安全。 */
bool pk_baro_get(pk_baro_state_t *out);
