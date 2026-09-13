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
    if (mv >= 4164) return 100;
    if (mv >= 3844) return 90 + (mv - 3844) * 10 / 320;
    if (mv >= 3704) return 80 + (mv - 3704) * 10 / 140;
    if (mv >= 3544) return 60 + (mv - 3544) * 20 / 160;
    if (mv >= 3384) return 40 + (mv - 3384) * 20 / 160;
    if (mv >= 3244) return 20 + (mv - 3244) * 20 / 140;
    if (mv >= 3224) return  8 + (mv - 3224) * 12 /  20;
    if (mv >= 2724) return      (mv - 2724) *  8 / 500;
    return 0;   /* 含负输入：落到这里返回 0，不许算出负数 */
}
