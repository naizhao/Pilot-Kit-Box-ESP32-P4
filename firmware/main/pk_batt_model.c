/*
 * pk_batt_model.c — 见 pk_batt_model.h。
 *
 * 2026-09-13：曲线换成**本机实测标定值**（见头文件"实测标定"一节）。
 * 在此之前是从 power_eta6098.c 逐字搬来的通用锂电曲线，实测下来最大误差
 * 42%、平均 24%——中段能把"还剩一半"报成 18%。
 */
#include "pk_batt_model.h"

int pk_batt_mv_to_pct(int mv)
{
    if (mv >= 4026) return 100;
    if (mv >= 3872) return 90 + (mv - 3872) * 10 / 154;
    if (mv >= 3700) return 80 + (mv - 3700) * 10 / 172;
    if (mv >= 3583) return 60 + (mv - 3583) * 20 / 117;
    if (mv >= 3380) return 40 + (mv - 3380) * 20 / 203;
    if (mv >= 3249) return 15 + (mv - 3249) * 25 / 131;
    if (mv >= 2839) return      (mv - 2839) * 15 / 410;
    return 0;   /* 含负输入：落到这里返回 0，不许算出负数 */
}
