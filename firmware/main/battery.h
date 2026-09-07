/*
 * battery.h — 旧 battery API 的兼容壳（WP-D Task 2）。
 *
 * 硬件事实与采集实现已迁入 power_eta6098.c/h（GPIO20 BAT_ADC + GPIO21
 * STAT，power_service 的第一个 backend）；本头文件只为 diag_page /
 * pfd_statusbar 的既有调用点保留这份最小契约，不再新增调用方。
 *
 * 分压比可标定：CONFIG_PK_BATT_DIVIDER_X100（见 Kconfig.projbuild），
 * 引脚电压 × (本值/100) = 电池电压，标定方法见 power_eta6098.h。
 * 注意：该等式只在 ETA6098 是快照赢家时成立；v4 上 SY6970 赢家时
 * batt_mv 来自 SY6970 的 BATV，与 raw_mv 无关（见 pk_batt_t 注释）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;        /* ADC 就绪且读数在合理量程内 */
    int  raw_mv;       /* ETA6098 引脚侧电压(mV)：恒为该引脚的 EMA 读数，
                        * 与快照赢家无关，标定分压比时看这个 */
    int  batt_mv;      /* 电池电压(mV)：ETA6098 赢家时 = raw_mv × 分压比；
                        * SY6970 赢家（v4 powered）时来自 SY6970 BATV，
                        * 与 raw_mv 无关——不得假设两者恒成等式 */
    int  pct;          /* 0..100，按锂电放电曲线折算 */
    bool charging;     /* 是否在充电（判据见 power_eta6098.c） */
} pk_batt_t;

/* 已弃用：转发 power_eta6098_init()。新代码直接调它，或经
 * power_service 走公共快照。 */
void pk_batt_init(void);

/* 取一份快照（power_service 聚合结果的薄包装）。返回 false 时全部
 * 字段清零；数据由服务的 1 Hz 轮询任务维护，调用方可以每帧问。 */
bool pk_batt_get(pk_batt_t *out);
