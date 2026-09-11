/*
 * config_antenna.h — 1090 / GNSS 天线选择，NVS 持久化。
 *
 * 天线开关（U16/U17）在 **RP2040** 那一侧，P4 只存值并经链路下发
 * （ADSB_LINK_MSG_CONFIG_REQ，协议 v1.2 §7）。RP2040 自己不持久化——它上电
 * 一律回到 rf_safety 的安全默认，等 P4 握手后把用户的选择推下去。所以
 * "谁是真源"很清楚：**NVS 里这一份是真源**，RP2040 上的是它的投影。
 *
 * 取值与 rf_safety.h 的枚举同号，且 0 = 上电默认。这条对齐不是顺手：
 * 它让"P4 还没来得及下发"和"用户选的就是默认值"落到同一个 RF 状态，
 * 否则开机那几百毫秒里屏上显示的和板上实际接的会不一致。
 *
 * ⚠ GNSS 那一路：选择线在 v3/v4 上兼做偏置馈电门控，未打 ECO 的板子上
 * 配对是反的——选中哪一路就给另一路供电，有源天线收不到星
 * （board_pins.h "U17 V1 / Q4 gate（ECO 后配对）"）。固件改不了，设置页
 * 那一行必须带提示，见 settings_draw.c。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PK_ANT_1090_ONBOARD  = 0,   /* 板载 IFA（开机默认） */
    PK_ANT_1090_EXTERNAL = 1,   /* 外接 J6 */
} pk_ant_1090_t;

typedef enum {
    PK_ANT_GNSS_EXTERNAL = 0,   /* 外接 J2（开机默认） */
    PK_ANT_GNSS_ONBOARD  = 1,   /* 板载 patch J8 */
} pk_ant_gnss_t;

/* 当前选择。热路径安全（只读一个受临界区保护的字节，不碰 NVS）。 */
pk_ant_1090_t pk_ant_1090_get(void);
pk_ant_gnss_t pk_ant_gnss_get(void);

/* 设值并落 NVS，随即把整套配置推给 RP2040。 */
void pk_ant_1090_set(pk_ant_1090_t sel);
void pk_ant_gnss_set(pk_ant_gnss_t sel);

/* 开机从 NVS 读取。须在链路任务启动前调用。 */
void pk_config_antenna_load(void);

/*
 * 把当前两路选择打包成 CONFIG_REQ 的 payload，写进 out。
 * 返回 payload 长度，cap 不足返回 0（与 codec 同款约定）。
 *
 * 每次都下发**完整两项**而不是只发改动的那一项：RP2040 重启后是一张白纸，
 * 增量下发会让它停在"一半用户值一半默认值"的状态上。RF 通路上的半套配置
 * 最难查——屏上一切正常，实际接的是另一根天线。
 */
size_t pk_antenna_build_config_payload(uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
