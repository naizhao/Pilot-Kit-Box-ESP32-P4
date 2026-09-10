/* threshold_ctl.h — 比较器门限 PWM（TL_PWM）+ 门限/RSSI 回读。
 * F5：R34=10k × C49=100nF → RC≈1ms（极点≈159Hz）；PWM 20kHz 衰减≈42dB。
 * MVP 固定门限：闭环留给 bring-up 后（PLAN.md §6.5）。
 *
 * 固定默认值 800 permille 来自 2026-09-10 实板扫描（室外 J6、V4.4）：
 * 门限 300→0 帧、500→4、600→189、700→442、**800→808**、900→751、
 * 1000→361（每点 10s）。旧的 50% 使比较器淹没在噪声里、解不出帧。
 * 详见 docs/internal/firmware-v3v4/2026-09-10-rp2040-1090-bringup-findings.md。 */
#pragma once

#define THRESHOLD_CTL_DEFAULT_PERMILLE 800

void threshold_ctl_init(void);
void threshold_ctl_set_permille(int pml);   /* 0..1000 */
int  threshold_ctl_read_level_mv(void);     /* ADC0，门限直流回读 */
int  threshold_ctl_read_rssi_raw(void);     /* ADC1，AD8313 原始值（未定标）*/
