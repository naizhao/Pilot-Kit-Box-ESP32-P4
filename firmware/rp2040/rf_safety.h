/* rf_safety.h — F2：bias 关断 + RF 开关合法互补态，上电第一件事。 */
#pragma once
#include <stdbool.h>

typedef struct {
    int  gpio;
    bool level;
    const char *why;
} rf_safety_pin_t;

/* 纯函数（host 可测）。返回上电安全向量，*count 出条数。 */
const rf_safety_pin_t *rf_safety_boot_vector(int *count);

/* 上板应用：逐脚 init + 置位。 */
void rf_safety_apply_boot_state(void);

/* ── 运行期天线选择 ──────────────────────────────────────────────────
 * U16(1090) / U17(GNSS) 都是 SPDT，靠两根选择线的**互补**电平切换；两根
 * 同时为高或同时为低不是合法态。真值表只写在这里一份：CDC 按键和 P4 下发
 * 的 CONFIG_REQ 都走这两个函数，避免两处各抄一份、改一处漏一处。
 *
 * 取值刻意让 0 = 上电默认（与 rf_safety_boot_vector 一致），这样"配置没
 * 送到"和"配置是默认值"是同一个状态，不会出现半套 RF 通路配置。 */
typedef enum {
    RF_ANT_1090_ONBOARD  = 0,   /* J1-J3 板载 IFA（开机默认） */
    RF_ANT_1090_EXTERNAL = 1,   /* J1-J2 外接 J6 */
} rf_ant_1090_t;

typedef enum {
    RF_ANT_GNSS_EXTERNAL = 0,   /* J2 外接（开机默认） */
    RF_ANT_GNSS_ONBOARD  = 1,   /* J8 板载 patch */
} rf_ant_gnss_t;

/* 纯函数（host 可测）：给定选择，出两根选择线的电平，两者必须互补。 */
void rf_safety_ant_1090_levels(rf_ant_1090_t sel, bool *a, bool *b);
void rf_safety_ant_gnss_levels(rf_ant_gnss_t sel, bool *a, bool *b);

/* 上板应用。非法枚举值按默认处理（不留"两线同电平"的非法中间态）。 */
void rf_safety_set_ant_1090(rf_ant_1090_t sel);
void rf_safety_set_ant_gnss(rf_ant_gnss_t sel);

/* **板上实际生效**的选择（不是 P4 下发的值）。初值 = 上电向量。
 *
 * 与 p4_link_ant_1090() 的区别是本次排障踩出来的：CDC 的 'a'/'A' 直接调
 * set_ant_*，不经过 CONFIG_REQ，于是 p4_link 那份缓存不会更新——'S' 打印
 * 出「切到 IFA」但状态行仍报 EXT-J6，恰恰违背了它自己「屏上选的和板上生效
 * 的必须能对上」的初衷。两个值分别来自两条路径，就得分别报。 */
rf_ant_1090_t rf_safety_get_ant_1090(void);
rf_ant_gnss_t rf_safety_get_ant_gnss(void);
