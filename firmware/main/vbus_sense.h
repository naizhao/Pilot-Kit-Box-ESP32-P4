/*
 * vbus_sense.h — 把 RP2040 上报的 USB_VBUS_SENSE 分压中点换算成 VBUS。
 *
 * 分工（PLAN.md F6）
 * ------------------
 * 分压网物理在 **RP2040** 的 ADC2（U8 pad 40，PCB 焊盘已核），P4 没有直读
 * 路径。RP2040 是两块板共用的**单一构建**、没有板型检测，所以它只上报
 * **分压中点的 mV**；换算成 VBUS 的那一步在这里做——P4 有 Kconfig 驱动的
 * pk_board_profile()，是全仓唯一可信的板型来源。
 *
 * 把分压比放到 RP2040 侧就得先发明一套板型检测，而猜错是**静默的 2 倍误差**：
 *   V4 = 30k/10k → 4.0     V3 = 10k/10k → 2.0
 *
 * ⚠ 这个读数**未经标定**
 * ----------------------
 * PLAN.md F6 明确写着「不得把理想倍率冒充校准系数」。ADC 输入阻抗会对分压网
 * 造成负载，实际中点电压低于理想值。以 V4、ADC Zin ≥100k 算：
 *
 *   VBUS    理想中点    实际中点    偏差
 *   5 V     1.250 V     1.163 V     −7%
 *   9 V     2.250 V     2.093 V     −7%
 *   10 V    2.500 V     2.326 V     −7%
 *
 * 所以：
 *   · pk_vbus_mv() 用**理想倍率**换算，结果**系统性偏低约 7%**，只能当粗读，
 *     不能当电压表。函数名里带 _uncal 就是为了让调用点看得见这件事。
 *   · 判 5V/9V 档位**不要**拿换算后的电压去比 7V——那会把偏差带进判据。
 *     pk_vbus_class() 直接在**中点电压**上比，门限取上表两档实测值的中点，
 *     余量近 1 V，偏差 7% 吃不掉它。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pk_board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PK_VBUS_ABSENT = 0,   /* 中点电压太低，判为没插 */
    PK_VBUS_5V,           /* USB 5 V 档 */
    PK_VBUS_9V,           /* USB PD 9 V 档 */
    PK_VBUS_UNKNOWN,      /* 高于 9V 档判据但不成档，或读数无效 */
} pk_vbus_class_t;

/* 分压中点 mV → VBUS mV（理想倍率，**未标定、系统性偏低约 7%**）。
 * node_mv < 0 视为无效，返回 -1。 */
int pk_vbus_mv_uncal(pk_board_profile_t profile, int node_mv);

/* 直接在中点电压上判档位——不经过换算，避免把 7% 偏差带进判据。 */
pk_vbus_class_t pk_vbus_class(pk_board_profile_t profile, int node_mv);

const char *pk_vbus_class_name(pk_vbus_class_t c);

/* 链路任务收到 HEALTH_STATS 时喂进来（协议 v1.3 字段 10）。 */
void pk_vbus_sense_update(int node_mv);

/* 最近一次读数。node_mv / vbus_mv 可传 NULL。
 * 返回 false = 还没收到过（RP2040 未连接，或对端是 v1.0 固件只发 40 字节）。 */
bool pk_vbus_sense_get(int *node_mv, int *vbus_mv_uncal, pk_vbus_class_t *cls);

#ifdef __cplusplus
}
#endif
