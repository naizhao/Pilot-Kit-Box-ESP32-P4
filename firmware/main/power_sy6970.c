/*
 * power_sy6970.c — SY6970 寄存器解码纯层（WP-D Task 3）。
 *
 * 结构同 qmc5883p：纯解码函数 host 可测（SY6970_HOST_TEST 隔离），
 * 目标端 I²C 胶水关在 #ifndef 里（Task 4 落地，本阶段留空）。
 *
 * ── 寄存器事实取证（三份本地文档互核，页码 = PDF 物理页）──────────────
 *
 * 文档版本：
 *   [DS]  SY6970_DS.pdf     — "SY6970 Rev.0.9B" © 2020 Silergy，40 页；
 *                             页脚编号 = 物理页（已逐页核对页脚 15~25）。
 *   [ALT] SY6970_DS_alt.pdf — "SY6970 Rev.0.1" © 2019 Silergy，37 页；
 *                             页脚编号 = 物理页；同一张寄存器表整体
 *                             前移 3 页（寄存器章从 p.12 开始）。
 *   [AN]  SY6970.pdf        — "AN_SY6970 Rev.0.9B"，40 页；页脚文本
 *                             大多提取不出（仅 p.34 可见"34"），按
 *                             物理页引用；其寄存器章与 [DS] 逐页同文
 *                             （REG00/03/07/0B/0C/0E~12 逐条比对一致）。
 *   三份在下面用到的每一条事实上一致；个别差异（[ALT] BHOT1 阈值
 *   34.37% vs [DS] 34.35%、SDP 在 OTG 时的行为描述）不涉及本层任何
 *   实现，不影响解码。**SY7069.pdf 是另一颗芯片，未参与取证。**
 *
 * 用到的事实（QMC 教训：每一条都对着寄存器表本体核对，不是应用示例）：
 *   I²C 地址 0x6A ........ [DS] p.15 / [ALT] p.12 / [AN] p.15
 *                          （"I2C Registers — Address: 6AH"）
 *   REG00 POR：EN_HIZ=0(bit7)、EN_ILIM=1(bit6)、IINLIM[5:0]=001000
 *                          [DS] p.15 / [ALT] p.12 / [AN] p.15
 *   REG03 WD_RST=bit6，写 1 复位看门狗定时器、写后自回 0
 *                          [DS] p.17 / [ALT] p.14 / [AN] p.17
 *   REG07 WATCHDOG[5:4]：00=关定时器、01=40s、10=80s、11=160s
 *                          [DS] p.19 / [ALT] p.16 / [AN] p.19
 *   看门狗语义：host 模式下须在超时前写 1 到 WD_RST 喂狗，或把
 *   WATCHDOG 位清 0 关掉；超时后器件整体回到默认模式（寄存器打回
 *   POR）——所以轮询后端上电第一件事是关狗，看门狗故障出现意味着
 *   曾有一段时间没人喂也没关。
 *                          [DS] p.29 / [ALT] p.27 / [AN] p.29
 *   REG0B（只读）：BUS_STAT[7:5]（000 无输入/001 SDP/010 CDP/
 *   011 DCP/100 HVDCP/101 未知/110 非标/111 OTG）；CHRG_STAT[4:3]
 *   （00 未充/01 预充 VBAT<VBATLOWV/10 快充/11 终止完成）；PG_STAT[2]；
 *   SDP_STAT[1]（BUS_STAT≠001 时恒 1）；VSYS_STAT[0]
 *                          [DS] p.22 / [ALT] p.19 / [AN] p.22
 *   REG0C（只读）：WATCHDOG_FAULT[7]；BOOST_FAULT[6]；CHRG_FAULT[5:4]
 *   （00 正常/01 输入故障/10 热关断/11 安全定时器超时）；BAT_FAULT[3]
 *   （1=BATOVP）；NTC_FAULT[2:0]（buck：000 正常/010 Warm/011 Cool/
 *   101 Cold/110 Hot）
 *                          [DS] p.22-23 / [ALT] p.19 / [AN] p.22-23
 *   故障锁存：REG0C 锁存上一次故障直到被读走；要拿当前实况须连读两遍
 *   （Task 4 轮询侧注意，与解码无关，本层不做）。唯一例外是 NTC_FAULT：
 *   它不锁存、始终如实反映 NTC 引脚当前状态（"The only exception is
 *   NTC_FAULT which always reports the actual condition on the NTC pin"）
 *                          [DS] p.29 / [AN] p.29（连读两遍：[DS] p.29 / [AN] p.35）
 *   REG0E（只读）：THERM_STAT[7]；BATV[6:0] = 2.304V + code×20mV，
 *   量程 2.304~4.844V      [DS] p.23 / [ALT] p.20 / [AN] p.23
 *   REG0F（只读）：SYSV[6:0] = 2.304V + code×20mV（窗口内，本层不消费）
 *                          [DS] p.24 / [ALT] p.21 / [AN] p.24
 *   REG10（只读）：NTCPCT[6:0] = 21% + code×0.465%（NTC/REGN），
 *   21%~80.055%
 *                          [DS] p.24 / [ALT] p.21 / [AN] p.24
 *   REG11（只读）：BUS_GD[7]（1=BUS 在位）；BUSV[6:0] = 2.6V +
 *   code×100mV，量程 2.6~15.3V
 *                          [DS] p.24 / [ALT] p.21 / [AN] p.24
 *   REG12（只读）：ICHGR[6:0] = code×50mA，量程 0~6350mA；VBAT<VSHORT
 *   时芯片读回 0000000（0mA 是芯片口径，不是解码层编的）
 *                          [DS] p.24-25 / [ALT] p.21 / [AN] p.24-25
 *   REG02 CONV_START[7]/CONV_RATE[6]：ADC 单次/连续（1s）转换控制——
 *   ADC 不开则 0E~12 全是陈旧值；打开它是 Task 4 轮询侧的职责，
 *   本解码层不管（也不写它：首阶段只读豁免只有看门狗管理）
 *                          [DS] p.16-17 / [ALT] p.13-14 / [AN] p.16-17
 *   窗口 REG0B..REG12 连续无洞：寄存器表本身 00~14 逐址连续列出，
 *   0B/0C/0D/0E/0F/10/11/12 每个地址都在表里
 *                          [DS] p.22-24 / [ALT] p.19-21 / [AN] p.22-24
 *
 * 派生语义（非寄存器原值，算术直译 + 一处口径决策）：
 *   vbus_present = BUS_GD=1 且 BUS_STAT≠111(OTG)。依据：BUS_GD 的
 *   定义是"BUS attached"（[DS] p.24），而 OTG 模式下 BUS 上的电是
 *   本机电池输出的（BUS_STAT=111=OTG，[DS] p.22）——在位≠在供电。
 *   charging = CHRG_STAT∈{01,10}；11（终止完成）不算充电但 vbus 仍
 *   按上式报（充满维持时 UI 需要"插着电"这个事实）。
 *   NTCPCT 用 0.001% 整数单位（×1000）：0.465%/code × 1000 = 465/
 *   code 恰好整除，满码 80055 超出 uint16 才用 uint32。
 */

#include "power_sy6970.h"

#include <stddef.h>

/* ── 寄存器与器件常量（取证页码见文件头表）───────────────────────────── */

#define SY6970_REG00 0x00  /* EN_HIZ/EN_ILIM/IINLIM，POR=0x60（[DS] p.15）*/
#define SY6970_REG03 0x03  /* WD_RST=bit6（[DS] p.17）                    */
#define SY6970_REG07 0x07  /* WATCHDOG[5:4]（[DS] p.19）                  */

/* REG00 的 POR 位值口径（[DS] p.15）：EN_HIZ=0 | EN_ILIM=1 = 0x40。
 * IINLIM 会随适配器检测变，不进校验掩码。 */
#define SY6970_REG00_VERIFY_MASK 0xC0
#define SY6970_REG00_VERIFY_VAL  0x40

/* 看门狗管理的位掩码（[DS] p.19 / p.17）。 */
#define SY6970_WATCHDOG_MASK 0x30  /* REG07 WATCHDOG[5:4] */
#define SY6970_WDRST_MASK    0x40  /* REG03 WD_RST bit6（写 1 自清） */

/* ── F1 初始化序列（数据表，host 可测）────────────────────────────────
 * 顺序：写前先回读校验 REG00（器件在应答且不在 HIZ 的在位证据，读、
 * 不改状态）→ 关狗 → 喂狗 → **写后再回读一次 REG00**：计划约束是
 * 「写入后必须回读 REG00 验证」——只有写后回读能证明 RMW 真的落到了
 * 寄存器里（写前那步只证明器件在场）。关狗用 RMW 清位而不是整字节
 * 覆盖：REG07 其余位（终止使能/安全定时器等）保持芯片当前值，不去赌
 * POR 值没被别人改过。 */
static const sy6970_init_step_t s_init_seq[] = {
    { SY6970_SEQ_VERIFY, SY6970_REG00,
      SY6970_REG00_VERIFY_MASK, SY6970_REG00_VERIFY_VAL,
      "写前在位校验：REG00 POR: EN_HIZ=0|EN_ILIM=1 "
      "(DS p.15 / ALT p.12 / AN p.15)" },
    { SY6970_SEQ_RMW_CLEAR, SY6970_REG07,
      SY6970_WATCHDOG_MASK, 0,
      "WATCHDOG[5:4]=00 关看门狗 (DS p.19 / ALT p.16 / AN p.19；"
      "超时回默认模式见 DS p.29)" },
    { SY6970_SEQ_RMW_SET, SY6970_REG03,
      SY6970_WDRST_MASK, 0,
      "WD_RST=1 喂狗，写 1 自清 (DS p.17 / ALT p.14 / AN p.17)" },
    { SY6970_SEQ_VERIFY, SY6970_REG00,
      SY6970_REG00_VERIFY_MASK, SY6970_REG00_VERIFY_VAL,
      "写后回读验证写入已落定（计划约束：写入后必须回读 REG00；"
      "POR 位值 DS p.15 / ALT p.12 / AN p.15）" },
};

const sy6970_init_step_t *sy6970_init_seq(size_t *n)
{
    if (n != NULL) *n = sizeof(s_init_seq) / sizeof(s_init_seq[0]);
    return s_init_seq;
}

/* ── 纯解码（host 可测）─────────────────────────────────────────────── */

bool sy6970_decode_status(const uint8_t *regs, size_t n, sy6970_status_t *out)
{
    if (out == NULL) return false;
    /* 先给 *out 一个明确的全零态：失败路径（含全 0xFF）返回后调用方
     * 不需要猜 out 里是什么。 */
    *out = (sy6970_status_t){0};
    if (regs == NULL || n < SY6970_WIN_LEN) return false;

    /* 器件缺席/总线被上拉 → 整窗 0xFF。照位定义硬解会得到"处处故障
     * 但数据有效"的垃圾态，必须在此拦下（见 power_sy6970.h 窗口合同）。 */
    bool all_ff = true;
    for (size_t i = 0; i < SY6970_WIN_LEN; i++) {
        if (regs[i] != 0xFF) {
            all_ff = false;
            break;
        }
    }
    if (all_ff) return false;

    /* regs[0]=REG0B ... regs[7]=REG12（窗口合同见 power_sy6970.h）。 */
    const uint8_t r0b = regs[0];
    const uint8_t r0c = regs[1];
    const uint8_t r0e = regs[3];
    const uint8_t r10 = regs[5];
    const uint8_t r11 = regs[6];
    const uint8_t r12 = regs[7];

    /* REG0B（[DS] p.22）：BUS_STAT[7:5] / CHRG_STAT[4:3] / PG_STAT[2]。 */
    out->bus_stat = (uint8_t)((r0b >> 5) & 0x07);
    const uint8_t chrg = (uint8_t)((r0b >> 3) & 0x03);
    out->charging  = (chrg == 1) || (chrg == 2);  /* 预充/快充都在充 */
    out->term_done = (chrg == 3);                 /* 终止完成不算充 */
    out->power_good = (r0b & 0x04) != 0;

    /* REG0C（[DS] p.22-23）：故障位原样拆出，语义不做二次演绎。 */
    out->wd_fault      = (r0c & 0x80) != 0;
    out->boost_fault   = (r0c & 0x40) != 0;
    out->chrg_fault    = (uint8_t)((r0c >> 4) & 0x03);
    out->bat_ovp_fault = (r0c & 0x08) != 0;
    out->ntc_fault     = (uint8_t)(r0c & 0x07);

    /* REG0E（[DS] p.23）：THERM_STAT[7] + BATV[6:0] = 2304mV + code×20。 */
    out->therm_reg = (r0e & 0x80) != 0;
    out->batt_mv   = (uint16_t)(2304 + (r0e & 0x7F) * 20);

    /* REG10（[DS] p.24）：NTCPCT[6:0] = 21% + code×0.465%，×1000 整数化。 */
    out->ntc_pct_x1000 = 21000u + (uint32_t)(r10 & 0x7F) * 465u;

    /* REG11（[DS] p.24）：BUS_GD[7] + BUSV[6:0] = 2600mV + code×100。 */
    out->vbus_mv = (uint16_t)(2600 + (r11 & 0x7F) * 100);
    out->vbus_present = ((r11 & 0x80) != 0) && (out->bus_stat != 7);

    /* REG12（[DS] p.24-25）：ICHGR[6:0] = code×50；VBAT<VSHORT 时芯片
     * 自报 0（充电电流读 0 不代表没在充，Task 4 消费时注意）。 */
    out->ichg_ma = (uint16_t)((r12 & 0x7F) * 50);

    return true;
}

/* ── 目标端（I²C 胶水）：host 单测不编译 ──────────────────────────────
 * Task 4 在这里落地：pk_i2c0_bus_get() 挂 0x6A 器件 → REG02 开 ADC
 * 连续转换 → 按 sy6970_init_seq() 执行 F1 序列 → 1 Hz 连读
 * SY6970_WIN_REG0 起 SY6970_WIN_LEN 字节 → sy6970_decode_status() →
 * power_service_register()。本阶段（Task 3）刻意留空：解码层无 I²C
 * 依赖，host 单测直编，两板系构建验证本文件参与固件链接。 */
#ifndef SY6970_HOST_TEST

#endif /* SY6970_HOST_TEST */
