/*
 * config_qnh.h — QNH(修正海压)可调值,NVS 持久化。
 *
 * 单位: hPa。默认标准大气 1013.25 hPa。
 * 线程安全:baro_task 读、settings 写,volatile float 在 RISC-V 上读写原子。
 */
#pragma once

float pk_qnh_get(void);
void  pk_qnh_set(float hpa);   /* 钳制到 [950, 1050] 并立即 NVS 持久化 */
void  pk_qnh_load(void);       /* 开机从 NVS 加载(无值则默认 1013.25) */
