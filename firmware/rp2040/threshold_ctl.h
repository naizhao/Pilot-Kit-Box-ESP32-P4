/* threshold_ctl.h — 比较器门限 PWM（TL_PWM）+ 门限/RSSI 回读。
 * F5：R34=10k × C49=100nF → RC≈1ms（极点≈159Hz）；PWM 20kHz 衰减≈42dB。
 * MVP 固定门限：闭环留给 bring-up 后（PLAN.md §6.5）。
 *
 * 固定默认值 800 permille 来自 2026-09-10 实板扫描（室外 J6、V4.4）：
 * 门限 300→0 帧、500→4、600→189、700→442、**800→808**、900→751、
 * 1000→361（每点 10s）。旧的 50% 使比较器淹没在噪声里、解不出帧。
 * 详见 docs/internal/firmware-v3v4/2026-09-10-rp2040-1090-bringup-findings.md。
 *
 * ── rssi_raw 不可信（2026-09-11 实测）────────────────────────────────
 * `H` 报的 rssi_raw **跟着门限 PWM 走**，不是 RF 场强：
 *     P200 → tl_level=790mV  rssi 连读稳定在 ~1065
 *     P600 → tl_level=1448mV rssi ~1765
 *     P1000→ tl_level=2089mV rssi ~2547
 * 两个通道在 2.5:1 的范围里始终相等（差 2–3%），且 P1000 时连读 8 次纹丝不动
 * （2544~2549），所以不是采样电容没充到位、也不是换通道读错——切通道丢弃一次
 * 转换之后实测毫无变化。
 *
 * 机理在网表上：LEVEL_BIAS ─R33(100k)─ SLICER_LEVEL ─R32(10k)─ RF_DET_OUT
 * ─R35(10k)─ RSSI 是一条纯电阻直流通路（sheet_rf1090.py）。只要 RF_DET_OUT
 * 这一端没有被检波器按住，链上没有直流电流，压降就全为 0，RSSI 节点就等于
 * LEVEL_BIAS——正是实测到的样子。为什么没被按住属于硬件侧（U14 供电/贴装/
 * 输出状态），固件量不到，**不在这里下结论**。
 *
 * 固件能做的只有一件事：不要让这个数字看起来像个有效读数。 */
#pragma once

#define THRESHOLD_CTL_DEFAULT_PERMILLE 800

void threshold_ctl_init(void);
void threshold_ctl_set_permille(int pml);   /* 0..1000 */
int  threshold_ctl_read_level_mv(void);     /* ADC0，门限直流回读 */
/* ⚠ ADC1。**当前实测不可信**：读回来的是门限那一路的电压，不是 AD8313 的
 * 检波输出。见文件头「rssi_raw 不可信」。任何基于它的判断都不成立。 */
int  threshold_ctl_read_rssi_raw(void);

/* 诊断：连读同一通道 n 次。被驱动的节点应当立刻稳定，悬空的高阻节点会被
 * 采样电容反复充电而漂——用来区分"这一路没人驱动"和"读错了通道"。 */
void threshold_ctl_adc_burst(unsigned int gpio, uint16_t *out, int n);

/* USB_VBUS_SENSE 分压**中点**的电压（mV），不是 VBUS 本身。
 *
 * 换算成 VBUS 要乘板型分压比（V4=4.0 / V3=2.0），而 RP2040 是两块板共用的
 * 单一构建、没有板型检测，所以这一步交给 P4（见 board_pins.h:PIN_ADC_VBUS）。
 *
 * 本函数放在 threshold_ctl 而不是新开一个模块：ADC 的初始化和"换通道要丢弃
 * 一次转换"的纪律都在这里，另起炉灶会重复 adc_init、并且很容易漏掉那条纪律。 */
int  threshold_ctl_read_vbus_node_mv(void);
