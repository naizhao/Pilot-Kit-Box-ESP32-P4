/*
 * config_qnh.h — QNH(修正海压)可调值 + 来源模式(AUTO/MANUAL),NVS 持久化。
 *
 * 单位: hPa。默认标准大气 1013.25 hPa。
 * 线程安全:baro_task 读/写(auto)、settings 写(manual),volatile 标量在 RISC-V
 * 上读写原子;模式用 portMUX 临界区保护。
 *
 * 模式:
 *   AUTO   — baro_task 每拍用「本站气压 + GPS 正高」反解当地 QNH
 *            (qnh_math.h),慢速滤波后写入,使气压高度贴合 GNSS。
 *   MANUAL — 用户用设置页步进器拨的值,不被 auto 覆盖。
 * 出厂/无 NVS 时默认 AUTO。
 */
#pragma once

typedef enum {
    PK_QNH_MODE_AUTO = 0,
    PK_QNH_MODE_MANUAL = 1,
} pk_qnh_mode_t;

float pk_qnh_get(void);
pk_qnh_mode_t pk_qnh_mode_get(void);

/* 手动设值:钳制到 [950,1050],切到 MANUAL,并立即 NVS 持久化。 */
void  pk_qnh_set(float hpa);

/* 自动设值:仅更新 RAM 值(不写 NVS——auto 每拍都在动,写 flash 会磨损),
 * 并把模式置为 AUTO。调用方负责滤波。 */
void  pk_qnh_set_auto(float hpa);

/* 显式切换模式并持久化。 */
void  pk_qnh_set_mode(pk_qnh_mode_t mode);

/* 开机从 NVS 加载(无值则默认 1013.25 / AUTO)。 */
void  pk_qnh_load(void);
