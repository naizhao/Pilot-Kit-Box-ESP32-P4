/*
 * battery.h — 锂电池电压 / 电量（4.3" Rev1.2 一体板）。
 *
 * 硬件依据（docs/hardware/ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md）：
 *   - 18. MX1.25 锂电池座 — 3.7 V，支持充放电
 *   - GPIO20 在这块板上是 **BAT_ADC**（旧 2.4" 载板那张表里它是 BNO085 INT，
 *     照那份写会得出"板上没有电量检测硬件"的错误结论——已经犯过一次）
 *
 * 分压比未知：原理图 PDF 里 BAT_ADC 网络关联不出电阻对，只看得到附近有
 * 10K。所以做成**可标定**的——CONFIG_PK_BATT_DIVIDER_X100 默认按 1:1
 * 分压（=200，即读数 ×2.00 得电池电压），拿万用表对一次就能定死，
 * 不必重编固件去试。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;        /* ADC 就绪且读数在合理量程内 */
    int  raw_mv;       /* ADC 引脚上的电压(mV)，标定分压比时看这个 */
    int  batt_mv;      /* 推算的电池电压(mV) = raw_mv × 分压比 */
    int  pct;          /* 0..100，按锂电放电曲线折算 */
    bool charging;     /* 是否在充电（见 .c 里的判据说明） */
} pk_batt_t;

/* 安装 ADC。失败不致命——没有电量显示照样能飞。 */
void pk_batt_init(void);

/* 取一份快照。内部 1 Hz 采样并做滑动平均，调用方可以每帧问。 */
bool pk_batt_get(pk_batt_t *out);
