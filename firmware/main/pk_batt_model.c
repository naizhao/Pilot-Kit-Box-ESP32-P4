/*
 * pk_batt_model.c — 见 pk_batt_model.h。
 *
 * 实现自 power_eta6098.c **逐字搬迁**（2026-09-12），分段与系数一个数都
 * 没动：这是一次纯搬家，行为等价由 test_pk_batt_model.c 钉住——那份表征
 * 测试是**先于搬迁**写的，正是为了让"没改行为"这句话有凭据。
 * 曲线为什么长这样、为什么不用线性映射，见头文件。
 */
#include "pk_batt_model.h"

int pk_batt_mv_to_pct(int mv)
{
    if (mv >= 4150) return 100;
    if (mv >= 3850) return 75 + (mv - 3850) * 25 / 300;
    if (mv >= 3700) return 50 + (mv - 3700) * 25 / 150;
    if (mv >= 3500) return 20 + (mv - 3500) * 30 / 200;
    if (mv >= 3300) return      (mv - 3300) * 20 / 200;
    return 0;   /* 含负输入：落到这里返回 0，不许算出负数 */
}
