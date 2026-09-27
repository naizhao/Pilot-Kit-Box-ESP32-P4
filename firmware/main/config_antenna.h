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
 * GNSS 那一路的 V4.10 当前实现与实板状态以 RP2040 CONFIG_ACK 回读为准；
 * 设置页不再显示早期版本的 ECO 警告，原始/实际来源仍在诊断页保留。
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

typedef struct {
    pk_ant_gnss_t requested;  /* P4/NVS 用户意图 */
    pk_ant_gnss_t actual;     /* actual_known=true 时才有效 */
    bool actual_known;        /* 已有匹配 CONFIG_ACK 确认 */
    bool request_pending;     /* 至少一帧完整 CONFIG_REQ 在等 ACK */
} pk_ant_gnss_status_t;

/* 当前选择。热路径安全（只读一个受临界区保护的字节，不碰 NVS）。 */
pk_ant_1090_t pk_ant_1090_get(void);
pk_ant_gnss_t pk_ant_gnss_get(void);
void pk_ant_gnss_status_get(pk_ant_gnss_status_t *out);

/* 仅供 RP2040 链路层驱动实际状态机。started 必须在完整 CONFIG_REQ 已编码、
 * 已进入发送队列之后调用；confirmed 只能来自匹配的 3/0 CONFIG_ACK。 */
void pk_ant_gnss_apply_started(pk_ant_gnss_t source);
void pk_ant_gnss_apply_confirmed(pk_ant_gnss_t source);
void pk_ant_gnss_apply_failed(pk_ant_gnss_t source);
void pk_ant_config_link_reset(void);

/* 设值并落 NVS，随即把整套配置推给 RP2040。 */
void pk_ant_1090_set(pk_ant_1090_t sel);
void pk_ant_gnss_set(pk_ant_gnss_t sel);

/* 开机从 NVS 读取。须在链路任务启动前调用。 */
void pk_config_antenna_load(void);

/* ── 1090 解调门限 ────────────────────────────────────────────────────
 * 取值 0 = 用 RP2040 的固件默认（见 rp2040/threshold_ctl.h），否则
 * 10..1000 permille。
 *
 * 为什么这个值必须由 P4 持久化：RP2040 一侧不存任何配置，每次上电都回到
 * 固件默认。而门限的最优点**不是常数**——2026-09-13 同一块 V4.4 板、同一
 * 根天线，一天之内就从 600 漂到 700，换天线换位置后又变。存在 P4 并随
 * 握手重下发（和天线选择同一条路径），调好的值才能活过 RP2040 的重启。
 *
 * 精度 10 permille：受协议 cfg val 只有 8 位所限，编码见
 * adsb_link.h 的 adsb_link_threshold_to_val()。扫描步进是 100，手调也只到
 * 10 的量级，够用。 */
int  pk_thresh_1090_get(void);          /* 0，或 10..1000 */
void pk_thresh_1090_set(int permille);  /* <=0 视为"用固件默认" */

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
