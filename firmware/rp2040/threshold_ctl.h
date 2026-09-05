/* threshold_ctl.h — 比较器门限 PWM（TL_PWM）+ 门限/RSSI 回读。
 * F5：R34=10k × C49=100nF → RC≈1ms（极点≈159Hz）；PWM 20kHz 衰减≈42dB。
 * MVP 固定 50%：闭环留给 bring-up 后（PLAN.md §6.5）。 */
#pragma once
void threshold_ctl_init(void);
void threshold_ctl_set_permille(int pml);   /* 0..1000 */
int  threshold_ctl_read_level_mv(void);     /* ADC0，门限直流回读 */
int  threshold_ctl_read_rssi_raw(void);     /* ADC1，AD8313 原始值（未定标）*/
