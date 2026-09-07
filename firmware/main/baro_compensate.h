/*
 * baro_compensate.h — BMP388 原始读数 → 补偿温度/气压（纯算术，无 ESP 依赖）。
 *
 * Bosch BMP3_FLOAT 补偿公式（照 BMP388 数据手册 BST-BMP388-DS001-07 §9
 * 附录参考实现一字不改）+ 21 字节 NVM 校准系数的量化展开。从 baro_task.c
 * 提取（WP-B Task 5）：把纯数学与 I²C/FreeRTOS 层拆开，host 单测
 * test_baro_compensate.c 直接编本单元做双实现互证。
 *
 * 调用顺序契约（手册 §9.2/9.3 的 t_lin 传递）：
 *   baro_calib_parse() → 每帧先 baro_compensate_temperature()（把 t_lin
 *   存进 calib），再 baro_compensate_pressure()（读同一 t_lin）。
 */
#pragma once

#include <stdint.h>

/* 量化后的校准系数(手册 §9.1 的 PAR_* float 值) + t_lin 中间结果 */
typedef struct {
    float t1, t2, t3;
    float p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
    float t_lin;   /* compensate_temperature 的中间结果,供 compensate_pressure 复用 */
} baro_calib_t;

/* 21 字节 NVM 校准系数(寄存器 0x31 起) → 量化 float 系数；out->t_lin 清零。 */
void baro_calib_parse(const uint8_t c[21], baro_calib_t *out);

/* 补偿温度 °C。必须先于气压调用——t_lin 存进 calib 供气压复用(手册 §9.2)。 */
float baro_compensate_temperature(baro_calib_t *calib, uint32_t uncomp_temp);

/* 补偿气压 Pa。依赖最近一次 baro_compensate_temperature 留下的 t_lin(手册 §9.3)。 */
float baro_compensate_pressure(const baro_calib_t *calib, uint32_t uncomp_press);
