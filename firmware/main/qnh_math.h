/*
 * qnh_math.h — 气压高度表公式反解 QNH 的纯函数（无 esp/NVS 依赖，host 可测）。
 *
 * baro_task.c 的正向公式（国际民航标准大气）：
 *     alt_m = 44330 * (1 - (P / QNH)^0.190295)
 * 本单元是它的反解：已知本站气压 P 与已知海拔 h（GPS 正高 MSL），求使
 * 高度表读数等于 h 的 QNH。
 *
 * 用途：auto-QNH —— 用 GPS 正高把气压高度表标到当地海压，使 BARO 高度
 * 与 GNSS 一致（消费级做法；航空规范里 QNH 由 ATIS/ATC 人工输入）。
 */
#pragma once

/* 由气压与已知海拔反解 QNH。
 *   pressure_pa : 本站气压（帕）
 *   alt_m       : 已知海拔（米，GNSS 正高 MSL）
 * 返回 hPa，钳制到 [950, 1050]；非法输入（P<=0 或 alt>=44330 导致底数
 * 非正）返回标准海压 1013.25，绝不产生 NaN/Inf。 */
float pk_qnh_from_pressure_alt(float pressure_pa, float alt_m);
