/*
 * power_eta6098.h — ETA6098 载板电源 backend（WP-D Task 2，自 battery.c 迁入）。
 *
 * 硬件与采集事实（原 battery.c / battery.h 的内容整体搬家到这里）：
 *   - GPIO20 是 **BAT_ADC**（4.3" Rev1.2 一体板；旧 2.4" 载板那张表里它是
 *     BNO085 INT，照那份写会得出"板上没有电量检测硬件"的错误结论——已经
 *     犯过一次），GPIO21 是 ETA6098 STAT（TP1 飞线 -> J3 pin 15）。
 *   - 分压比未知：原理图 PDF 里 BAT_ADC 网络关联不出电阻对，只看得到附近
 *     有 10K。所以做成**可标定**的——CONFIG_PK_BATT_DIVIDER_X100 默认按
 *     实测标定值（见 Kconfig.projbuild），拿万用表对一次就能定死。
 *
 * 它是 power_service 的第一个 backend：ETA6098 在两代载板上都必然在位，
 * init 成功后把自己注册进去（注册点在 main.c，当前唯一源=当前最高优先级）；
 * v4 powered 的 SY6970（Task 4）须在 power_eta6098_init() **之前**探测
 * 注册（注册次序=优先级，见 power_service.h:15-20），届时本 backend
 * 自然回落为兜底。
 *
 * raw_mv（ADC 引脚侧电压）没有进公共快照——它只是分压比标定的辅助量，
 * 诊断页需要时用 power_eta6098_raw_mv() 直接向本 backend 要。
 */
#pragma once

#include <stdbool.h>

/*
 * 安装 GPIO20 ADC + GPIO21 STAT 并把 backend 注册进 power_service。
 * 失败不致命——没有电量显示照样能飞（此时服务里没有这个 backend，
 * snapshot 如实报 UNKNOWN）。幂等：重复调用只生效一次。
 * 旧入口 pk_batt_init()（battery.h）现在是它的转发壳。
 */
void power_eta6098_init(void);

/*
 * 取 EMA 平滑后的 **引脚侧** 电压（mV），标定分压比时看这个
 * （电池电压 = 引脚电压 × CONFIG_PK_BATT_DIVIDER_X100/100）。
 * 只要采到过至少一拍就返回 true——即使量程检查判了"无电池"
 * （引脚浮空乱跳）也照报，诊断页在无电池分支上也要渲染它；
 * 从未采到过（init 失败或一拍都没跑）返回 false。
 */
bool power_eta6098_raw_mv(int *out_mv);

/*
 * 锂电放电曲线 → 百分比（0..100）。
 *
 * 电芯模型是**化学属性**，不是某一颗充电芯片的属性——本模块因为是第一个
 * backend 才落了这份标定曲线（分段表 + 推导见 power_eta6098.c），公开出来
 * 给 SY6970 backend 复用（它同样只有电压没有库仑计）。两处各养一张
 * SoC 表必然漂移，禁止在 power_sy6970.c 里再抄一份。
 */
int power_eta6098_mv_to_pct(int batt_mv);
