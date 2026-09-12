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
#define SY6970_REG02 0x02  /* CONV_START/CONV_RATE/AUTO_DPDM_EN（p.16-17）*/
#define SY6970_REG03 0x03  /* WD_RST=bit6（[DS] p.17）                    */
#define SY6970_REG07 0x07  /* WATCHDOG[5:4]（[DS] p.19）                  */
#define SY6970_REG09 0x09  /* BATFET_DIS=bit5（[DS] p.20）                */
#define SY6970_REG0C 0x0C  /* 故障寄存器，连读两遍取实况（[DS] p.29）     */

/* 关机：REG09 BATFET_DIS bit5=1 → Force BATFET Off（[DS] p.20）。 */
#define SY6970_BATFET_DIS_MASK 0x20

/* JEITA 高温段充电电压：REG09[4] JEITA_VSET（[DS] p.20）。
 * 0（POR）= Warm(T3~T4) 段把 VREG 降 150mV；1 = 维持 VREG 不降。
 * 取值理由见 power_sy6970.h 的 JEITA 注释。 */
#define SY6970_JEITA_VSET_MASK 0x10

/* REG00 的 POR 位值口径（[DS] p.15）：EN_HIZ=0 | EN_ILIM=1 = 0x40。
 * IINLIM 会随适配器检测变，不进校验掩码。 */
#define SY6970_REG00_VERIFY_MASK 0xC0
#define SY6970_REG00_VERIFY_VAL  0x40

/* 看门狗管理的位掩码（[DS] p.19 / p.17）。 */
#define SY6970_WATCHDOG_MASK 0x30  /* REG07 WATCHDOG[5:4] */
#define SY6970_WDRST_MASK    0x40  /* REG03 WD_RST bit6（写 1 自清） */

/* IINLIM 与自动 DP/DM 检测（取值依据见 power_sy6970.h 的 IINLIM 注释）。
 * AUTO_DPDM_EN 必须在写 IINLIM 之前清掉：DP/DM 检测跑完会按适配器类型
 * 改写 IINLIM（[DS] p.15），而板上 DP/DM 悬空只判得出 SDP=500mA——不关
 * 它，写进去的码每次插拔都被芯片自己覆盖回去。 */
#define SY6970_IINLIM_MASK       0x3F  /* REG00 IINLIM[5:0]（[DS] p.15）*/
#define SY6970_AUTO_DPDM_MASK    0x01  /* REG02 AUTO_DPDM_EN bit0（p.17）*/

/* ── F1 初始化序列（数据表，host 可测）────────────────────────────────
 * 顺序：写前先回读校验 REG00（器件在应答且不在 HIZ 的在位证据，读、
 * 不改状态）→ 关狗 → 喂狗 → **写后再回读 REG07 与 REG00**：审计 F2
 * 裁定只回读 REG00 只能证明器件在场，证明不了配置真的落定——REG07
 * 回读钉死「WATCHDOG[5:4]=00 关狗已落定」（DS p.19），REG00 回读保持
 * 计划约束「写入后必须回读 REG00 验证」+ RMW 落定证据。关狗用 RMW
 * 清位而不是整字节覆盖：REG07 其余位（终止使能/安全定时器等）保持
 * 芯片当前值，不去赌 POR 值没被别人改过。REG02（开 ADC）的写后回读
 * 在 bring_up() 里做（CONV_START 写 1 自清，进不了这张表）。 */
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
    { SY6970_SEQ_RMW_CLEAR, SY6970_REG02,
      SY6970_AUTO_DPDM_MASK, 0,
      "AUTO_DPDM_EN=0 关自动 DP/DM 检测 (DS p.17)：板上 DP/DM 悬空，"
      "BC1.2 只判得出 SDP，会把下一步的 IINLIM 覆盖回 500mA (DS p.15)" },
    { SY6970_SEQ_RMW_FIELD, SY6970_REG00,
      SY6970_IINLIM_MASK, SY6970_IINLIM_CODE,
      "IINLIM[5:0]=100110 → 100mA+50mA×38=2000mA (DS p.15)：此前从未配过，"
      "实测停在 POR 的 500mA(REG00=0x48)；硬件 ILIM 脚(R38=180R)≈2.08A "
      "与 AICL/VINDPM 仍在外侧兜底 (DS p.10 / p.28)" },
    { SY6970_SEQ_RMW_SET, SY6970_REG09,
      SY6970_JEITA_VSET_MASK, 0,
      "JEITA_VSET=1：Warm(T3~T4) 段不降充电电压 (DS p.20)。POR=0 会降 "
      "150mV → 4.058V，实测停充在 4024mV、电池永远差最后一成；而本板 NTC "
      "贴着电池测的是环境温度，广东室温即达 40°C+ (用户 2026-09-12)" },
    { SY6970_SEQ_VERIFY, SY6970_REG07,
      SY6970_WATCHDOG_MASK, 0,
      "写后回读：REG07 WATCHDOG[5:4]==00 证明关狗已落定 "
      "(DS p.19 / ALT p.16 / AN p.19)" },
    { SY6970_SEQ_VERIFY, SY6970_REG02,
      SY6970_AUTO_DPDM_MASK, 0,
      "写后回读：REG02 AUTO_DPDM_EN==0 证明自动检测已关 (DS p.17)" },
    { SY6970_SEQ_VERIFY, SY6970_REG00,
      SY6970_REG00_VERIFY_MASK, SY6970_REG00_VERIFY_VAL,
      "写后回读验证写入已落定（计划约束：写入后必须回读 REG00；"
      "POR 位值 DS p.15 / ALT p.12 / AN p.15）" },
    { SY6970_SEQ_VERIFY, SY6970_REG00,
      SY6970_IINLIM_MASK, SY6970_IINLIM_CODE,
      "写后回读：IINLIM 已落定。与上一行拆开，是为了让失败日志能分清"
      "「器件配置位」还是「限流码」没写进去——两者排查方向不同 (DS p.15)" },
    { SY6970_SEQ_VERIFY, SY6970_REG09,
      SY6970_JEITA_VSET_MASK, SY6970_JEITA_VSET_MASK,
      "写后回读：JEITA_VSET 已落定。这条落不定的后果是「电池永远差最后"
      "一成」——不报错、不掉线，只会让人觉得电池不行 (DS p.20)" },
};

const sy6970_init_step_t *sy6970_init_seq(size_t *n)
{
    if (n != NULL) *n = sizeof(s_init_seq) / sizeof(s_init_seq[0]);
    return s_init_seq;
}

/* ── 关机序列（语义、板级理由与调用方约束见 power_sy6970.h）──────────
 * 只有一步，且没有写后回读——写下去芯片就断电了，读不回来。
 * 用 RMW_SET 而不是整字节覆盖：REG09 其余位（JEITA_VSET bit4、
 * BATFET_RST_EN bit2、TMR2X_EN bit6）保持芯片当前值，关机不该顺手改
 * 掉别的配置。BATFET_DLY(bit3) POR=0 = 立即关断，正是我们要的，不动。 */
static const sy6970_init_step_t s_shutdown_seq[] = {
    { SY6970_SEQ_RMW_SET, SY6970_REG09,
      SY6970_BATFET_DIS_MASK, 0,
      "BATFET_DIS=1 关断 Q4 进 shipping mode (DS p.20 REG09[5] / p.30)："
      "/QON 悬空 + J1 无电源控制线，这是本板唯一的软件断电途径；"
      "唤醒只能靠插 USB (DS p.30)" },
};

const sy6970_init_step_t *sy6970_shutdown_seq(size_t *n)
{
    if (n != NULL) *n = sizeof(s_shutdown_seq) / sizeof(s_shutdown_seq[0]);
    return s_shutdown_seq;
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
 * 职责（Task 4）：探测 0x6A → ACK 才 add_device + bring-up → 注册成
 * power_service 的第一个（权威）backend；1 Hz 由服务的 poll 任务驱动，
 * 连读 SY6970_WIN_REG0 起 SY6970_WIN_LEN 字节（连读两遍取 REG0C 实况）
 * → sy6970_decode_status() → 摊成公共快照。F1 序列按 sy6970_init_seq()
 * 逐字执行（写前后 REG00 回读值都进日志）；总线代数比对 + 失败连击
 * 自愈重放，结构照 qmc5883p 的器件侧契约（pk_i2c0_bus.h）。
 */
#ifndef SY6970_HOST_TEST

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "pk_i2c0_bus.h"
#include "pk_batt_model.h"
#include "power_service.h"

static const char *TAG = "sy6970";

/* I²C 地址 0x6A [DS] p.15 / [ALT] p.12 / [AN] p.15（"Address: 6AH"）。 */
#define SY6970_I2C_ADDR       0x6A
#define SY6970_I2C_TIMEOUT_MS 100  /* 与 qmc/baro 的单笔超时口径一致   */

/* REG02 CONV_START[7] / CONV_RATE[6] [DS] p.16 / [ALT] p.13 / [AN] p.16：
 * CONV_RATE=1 → "Start 1s continuous Conversion"，且转换自动开始；
 * CONV_RATE=1 时 CONV_START 变只读（转换期间保持 1）。ADC 不开则
 * REG0E~12 全是陈旧值——轮询侧的职责（Task 3 取证表事实 14）。
 * 用 RMW 置位而不是整字节覆盖：REG02 其余位（AICL_EN/HVDCP_EN 等
 * POR=1）保持芯片当前值，与 F1 对 REG07 的 RMW 同一哲学。 */
#define SY6970_CONV_RATE_MASK 0x40  /* CONV_RATE bit6：写后回读判据      */
#define SY6970_CONV_START_MASK 0x80 /* CONV_START bit7：写 1 自清，不验  */
#define SY6970_CONV_MASK      (SY6970_CONV_START_MASK | SY6970_CONV_RATE_MASK)

/* 自愈节拍：连续 10 拍（≈10 s）才动一次 bring-up 级重试，与 qmc5883p
 * 的失明自愈一致——单拍抖动不值得动配置写。!s_ready 补试与代数失配
 * 重放同用这个节拍做失败日志节流：首拍立试立报，之后每 10 拍一次，
 * 失败 WARN 不以 1 Hz 刷日志。 */
#define SY6970_FAIL_STREAK_MAX 10

/* 开机 60 s 后复检充电电流（F1 配套）：CH224K 诱骗 + SY6970 配置都该
 * 已稳定；若期间看门狗把寄存器打回过默认模式，这一拍的数据能看出来。 */
#define SY6970_ICHG_RECHECK_US (60LL * 1000 * 1000)

/* 周期详情日志的节拍（成功拍计数，1 Hz 轮询 → 30 s 一行）。
 *
 * 为什么要单独一条而不是并进 power_service 那行：公共快照里只有
 * batt_mv/pct/charging，**没有** ICHG、VBUS、NTCPCT 和寄存器原值——
 * 而这几个恰恰是充电排障的全部证据。2026-09-12 查 "NTC 2" 时，屏上
 * 只有原码、串口上一个字都没有，只能靠复位重抓开机日志才看到
 * REG00=0x48（IINLIM 停在 POR 500mA）；那次的代价就是这条日志的理由。
 * 30 s 而不是 10 s：这些也是慢变量，不跟 1090 帧流抢串口。 */
#define SY6970_DETAIL_LOG_TICKS 30

/* 5V/9V 判档中点：CH224K 诱骗档位只到 9V，7V 中点区分两档。仅作
 * 诊断参考口径——权威 VBUS 值来自 BUSV ADC，见 sy6970_poll 的 F6 注。 */
#define SY6970_VBUS_9V_MIDPOINT_MV 7000

static i2c_master_dev_handle_t s_dev;

/* ── 单写者状态（写者 = power_service 的 1 Hz poll 任务）───────────────
 * 诊断态（st/regs/reg00/ready/updated_us）捆在 s_diag 一份里，poll 末尾
 * 在 s_diag_mux 临界区内整体提交、sy6970_diag_get()（UI 上下文）锁内
 * 整体读——2026-09 审计二轮：首轮的 C11 原子 seqlock 复审被否，教训同
 * gps_task.c:48-53（RVWMO 弱序下计数与负载无真全序；C11 口径下裸
 * 负载/存储的竞争是 UB）。portMUX 自旋锁是 ESP-IDF 标准，跨核正确性
 * 由构造保证；临界区只有几条拷贝，纳秒级（合同见 power_sy6970.h）。 */
static bool            s_ready;      /* bring-up 成功（在读数）；仅 poll
                                      * 上下文读写，诊断页经 s_diag.ready
                                      * 取副本                             */
static uint8_t         s_reg00;      /* 最近一次 REG00 回读（F1 证据），
                                      * 仅 poll 上下文，随每拍提交进 s_diag */
static power_snapshot_t s_last_good; /* 最近一份好快照：读失败时原样上报，
                                      * 让服务端按 updated_us 判 stale 回落 */
static uint32_t        s_bus_gen;    /* 已认账的总线代数                   */
static int             s_fail_streak;
static int             s_boot_streak;   /* !s_ready 补试连击（日志节流）   */
static int             s_gen_streak;    /* 代数失配重放连击（日志节流）    */
static int             s_wd_streak;     /* WATCHDOG_FAULT 重放连击（节流） */
static int64_t         s_up_us;      /* 首拍成功时刻：60 s 复检的锚        */
static bool            s_ichg_recheck_done;
static uint32_t        s_detail_tick;   /* 周期详情日志的成功拍计数         */

/* 诊断单快照（s_diag_mux 保护，见上）。 */
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    sy6970_status_t  st;           /* 最近一次成功解码                     */
    uint8_t          regs[SY6970_WIN_LEN]; /* 提交当拍的原始窗口字节      */
    uint8_t          reg00;        /* 提交当拍的 REG00 回读（F1 证据）     */
    bool             ready;
    int64_t          updated_us;   /* 最近成功采集时刻（0=从未报数哨兵）   */
} s_diag;

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return pk_i2c0_bus_transmit_receive(s_dev, &reg, 1, buf, n,
                                        SY6970_I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return pk_i2c0_bus_transmit(s_dev, b, 2, SY6970_I2C_TIMEOUT_MS);
}

/*
 * 逐字执行一张步骤表（bring-up 的 F1 与关机序列共用）。
 *
 * label 只进日志，用来分清是哪张表在跑——两张表的失败含义完全不同：
 * F1 失败要走自愈重放，关机失败是"没关掉"，得让用户知道。
 * 任一步失败即中止并返回 false，失败行带上该步的 why（页码引用）。
 */
static bool run_seq(const sy6970_init_step_t *seq, size_t n, const char *label)
{
    for (size_t i = 0; i < n; i++) {
        const sy6970_init_step_t *st = &seq[i];
        switch (st->op) {
        case SY6970_SEQ_VERIFY: {
            uint8_t val = 0;
            const esp_err_t rd = reg_read(st->reg, &val, 1);
            if (rd != ESP_OK) {
                ESP_LOGW(TAG, "%s 第 %u 步校验失败：REG%02X 读失败（%s）— %s",
                         label, (unsigned)i, st->reg, esp_err_to_name(rd),
                         st->why);
                return false;
            }
            if ((val & st->mask) != st->val) {
                ESP_LOGW(TAG, "%s 第 %u 步校验失败：REG%02X=0x%02X（掩码 0x%02X "
                              "期望 0x%02X）— %s",
                         label, (unsigned)i, st->reg, val, st->mask, st->val,
                         st->why);
                return false;
            }
            /* s_reg00 存**完整**回读字节（不是 masked 值）：详情日志要从
             * 它的低 6 位还原 IINLIM，掩码过的值还原不出来。 */
            if (st->reg == SY6970_REG00) s_reg00 = val;
            ESP_LOGI(TAG, "%s %s：REG%02X=0x%02X（掩码 0x%02X==0x%02X）",
                     label, i == 0 ? "写前在位校验" : "写后回读验证",
                     st->reg, val, st->mask, st->val);
            break;
        }
        case SY6970_SEQ_RMW_CLEAR:
        case SY6970_SEQ_RMW_SET:
        case SY6970_SEQ_RMW_FIELD: {
            uint8_t old = 0;
            if (reg_read(st->reg, &old, 1) != ESP_OK) {
                ESP_LOGW(TAG, "%s 第 %u 步 RMW 回读失败：REG%02X — %s",
                         label, (unsigned)i, st->reg, st->why);
                return false;
            }
            /* FIELD 是「清掩码位再填 val」——置位/清位都拼不出确定的
             * 位域值（IINLIM 这种码必须整体写定，见 h 里的取值注释）。 */
            uint8_t newv;
            if (st->op == SY6970_SEQ_RMW_SET) {
                newv = (uint8_t)(old | st->mask);
            } else if (st->op == SY6970_SEQ_RMW_CLEAR) {
                newv = (uint8_t)(old & ~st->mask);
            } else {
                newv = (uint8_t)((old & ~st->mask) | (st->val & st->mask));
            }
            if (reg_write(st->reg, newv) != ESP_OK) {
                ESP_LOGW(TAG, "%s 第 %u 步 RMW 写入失败：REG%02X — %s",
                         label, (unsigned)i, st->reg, st->why);
                return false;
            }
            ESP_LOGI(TAG, "%s RMW：REG%02X 0x%02X→0x%02X",
                     label, st->reg, old, newv);
            break;
        }
        }
    }
    return true;
}

/*
 * F1 看门狗管理 + ADC 连续转换开启（bring-up）。
 *
 * 序列按 sy6970_init_seq() **逐字执行**（顺序不得重排）：写前 REG00
 * 在位校验 → REG07 关狗 → REG03 喂狗 → 写后 REG07 回读（关狗落定）
 * → 写后 REG00 回读验证。开 ADC 的 REG02 写后也回读验证 CONV_RATE
 * （审计 F2：不回读就证明不了配置真的落进寄存器，REG00 回读只能
 * 证明器件在场）。各回读值都以 INFO 进日志（write-intent +
 * write-result 证据，计划约束）；任一步失败 WARN 带 why 并返回
 * false。本文件唯一的寄存器写豁免是看门狗管理（F1，计划全局约束）
 * + ADC 转换开启（Task 3 取证表事实 14 点名 Task 4 职责）；充电参数
 * （IINLIM/ICHG/VREG 等）一概不碰。
 */
static bool bring_up(void)
{
    /* 先开 ADC 连续转换，再走 F1：F1 的最后一步回读 REG00 时，REG0E~12
     * 已在按 1 s 刷新，第一拍轮询拿到的就是新鲜值。 */
    uint8_t r02 = 0;
    if (reg_read(SY6970_REG02, &r02, 1) != ESP_OK) {
        ESP_LOGW(TAG, "REG02 回读失败，无法开 ADC");
        return false;
    }
    const uint8_t r02_new = r02 | SY6970_CONV_MASK;
    ESP_LOGI(TAG, "开 ADC 连续转换：REG02 0x%02X→0x%02X（CONV_RATE=1，1s 连续）",
             r02, r02_new);
    if (reg_write(SY6970_REG02, r02_new) != ESP_OK) {
        ESP_LOGW(TAG, "REG02 写入失败");
        return false;
    }
    /* 审计 F2 写后回读：只验 CONV_RATE(bit6)=1——CONV_START(bit7) 写 1
     * 自清（转换期只读保持 1），不要求它回读为 1（[DS] p.16）。
     * 回读成功值照 F1 的 write-intent/result 证据链以 INFO 记账
     * （审计修复 P3：只写在失败分支的话，"各回读值都以 INFO 进日志"
     * 就是空话，台架核对清单也少一环）。 */
    uint8_t r02_rb = 0;
    const esp_err_t rb = reg_read(SY6970_REG02, &r02_rb, 1);
    if (rb != ESP_OK) {
        ESP_LOGW(TAG, "REG02 写后回读失败（%s）", esp_err_to_name(rb));
        return false;
    }
    if ((r02_rb & SY6970_CONV_RATE_MASK) == 0) {
        ESP_LOGW(TAG, "REG02 写后回读 0x%02X：CONV_RATE 未落定（期望 bit6=1）",
                 r02_rb);
        return false;
    }
    ESP_LOGI(TAG, "REG02 写后回读：0x%02X（CONV_RATE=1 已落定，[DS] p.16）",
             r02_rb);

    size_t n = 0;
    const sy6970_init_step_t *seq = sy6970_init_seq(&n);
    return run_seq(seq, n, "F1");
}

/*
 * 按需关机：执行 sy6970_shutdown_seq()（BATFET_DIS=1 → shipping mode）。
 * 语义、板级理由与调用方约束全部写在 power_sy6970.h 的序列注释里。
 *
 * VBUS 在位时照写不误但打 WARN：BATFET 只是电池↔SYS 的开关，插着 USB
 * 时 SYS 由 VBUS 供电，写下去不会真断电（[DS] p.30）。要不要先拦住
 * 用户、要不要提示"请先拔 USB"，是 UI 层的决定，不是本层替它拿主意；
 * 本层的职责是把这个事实以日志说清楚，别让调用方以为关机成功了。
 *
 * 未探测到器件（v3 / 未上电的 v4）直接返回 false——那种板子上根本没有
 * 这颗芯片，谈不上关机。
 *
 * ── 为什么必须等 s_ready 而不是只看 s_dev ───────────────────────────
 * s_dev != NULL 只说明 0x6A 探测 ACK、handle 建起来了，**不说明 bring_up
 * 成功**——而「关看门狗」正是 bring_up 里的一步（REG07 WATCHDOG[5:4]=00）。
 * 看门狗没关成时它还是 POR 的 40 s，而 ship mode 一进 MCU 就掉电、再没人
 * 喂狗：超时后器件整体打回 POR 默认值（[DS] p.29），BATFET_DIS 跟着被清，
 * 设备在关机约 40 s 后**自己活过来**。用户看到的是"点了关机，黑屏一会儿
 * 又开机了"，而串口那时已经断了，极难查。
 * 所以 bring_up 没落定就不许关机：宁可关不掉（显性失败、日志说得清），
 * 也不要关一半又自己复活。
 */
bool power_sy6970_shutdown(void)
{
    if (s_dev == NULL) {
        ESP_LOGW(TAG, "关机请求被忽略：未探测到 SY6970（v3 / 未上电 v4）");
        return false;
    }
    if (!s_ready) {
        ESP_LOGW(TAG, "关机请求被拒：bring-up 未落定，看门狗可能仍在跑"
                      "（POR 40 s）——ship mode 后无人喂狗，超时会把 "
                      "BATFET_DIS 一起打回默认值，设备约 40 s 后自行复活"
                      "（[DS] p.29）。先让 bring-up 成功再关机。");
        return false;
    }
    if (s_last_good.vbus_present) {
        ESP_LOGW(TAG, "关机时 VBUS 仍在位：BATFET 只断电池，SYS 仍由 VBUS "
                      "供电，系统不会真的断电——要整板断电须先拔 USB "
                      "（[DS] p.30）");
    }
    size_t n = 0;
    const sy6970_init_step_t *seq = sy6970_shutdown_seq(&n);
    ESP_LOGW(TAG, "执行关机：置 BATFET_DIS，唤醒只能靠插 USB（/QON 悬空）");
    if (!run_seq(seq, n, "关机")) return false;

    /*
     * 走到这里说明**系统还活着**——电池供电下 BATFET 一断 MCU 就该掉电，
     * 能执行到下一条指令本身就是"没真关掉"的证据。最常见的原因是 VBUS 在位
     * （SYS 由 VBUS 供电，[DS] p.30），但也可能是写根本没落到寄存器里。
     * 这两种情况的排查方向完全不同，回读 REG09 把它们分开：
     *   bit5=1 → 写成功了，是外部供电撑着，拔 USB 即可；
     *   bit5=0 → 写没落定（总线问题 / 被看门狗打回），是固件侧的事。
     * 关机序列表里刻意没有 VERIFY 步（"写完就断电，读不回来"），那条在
     * 电池供电路径上成立；这里是它不成立的那条路径，所以补在执行器里。
     */
    uint8_t r09 = 0;
    if (reg_read(SY6970_REG09, &r09, 1) != ESP_OK) {
        ESP_LOGW(TAG, "关机后仍在运行，且 REG09 回读失败——无法判断 "
                      "BATFET_DIS 是否落定");
        return false;
    }
    ESP_LOGW(TAG, "关机后仍在运行：REG09=0x%02X，BATFET_DIS=%d。%s",
             r09, (r09 & SY6970_BATFET_DIS_MASK) ? 1 : 0,
             (r09 & SY6970_BATFET_DIS_MASK)
                 ? "位已置上 → 是 VBUS 在供电撑着，拔掉 USB 才会真断电"
                 : "位没置上 → 写未落定，查 I²C 或看门狗");
    return false;   /* 没真关掉，就不能报成功 */
}

/* 快照组装（成功拍）。pct：SY6970 没有库仑计，只有电压，走公共电芯模型
 * pk_batt_mv_to_pct()（合同见 pk_batt_model.h）。2026-09-12 之前这里调的是
 * power_eta6098_mv_to_pct——曲线长在一颗**本板上根本不存在**的充电芯片的
 * 文件里（微雪载板的 ETA6098 自 2026-09-10 起已不再注册，见 main.c），
 * 现已搬到独立模块，两边都调它。
 *
 * 插电维持电压的虚高偏差在这里**仍未补偿**——ETA 的 CC/HOLD 压降补偿是
 * 2026-08-04 按那颗芯片实测标定的，直接套用到 SY6970 属于编造，标定
 * 数据到手前如实带着偏差。实测量级：9V PD 充电中 BATT=4084mV 报 94%，
 * 若套 ETA 的 CC 常数（150mV）应在 82% 附近，系统性虚高约 12 个百分点。
 * SY6970 比 ETA 多一个条件——REG12 ICHGR 给出实测充电电流，可以做
 * 压降=ICHG×R_internal 的连续补偿，而不是 ETA 那样分档硬切；R 需要一轮
 * CC 段（约 40~70% 电量、电流顶在上限）的拔插实测才能定。
 * 量程闸与 ETA6098 backend 同口径。 */
/* 电量用的电压 EMA（定点 ×256）。只有 poll 任务写，单写者。
 *
 * 为什么必须平滑：BATV 的 LSB 是 20 mV，而 SoC 曲线在 3850~4150 段是
 * 25 点 / 300 mV——**一个 ADC 码的抖动就换算成 1.67 个百分点**，整数截断
 * 之后表现为 87 ↔ 89 来回跳 2 点（2026-09-12 实测 4004/4024 mV 相邻码）。
 * 用户看到的是电量自己在跳，而电池根本没动。
 *
 * α=1/8，1 Hz 轮询 → 时间常数约 8 s。电池是慢变量，这个档位既压得住单码
 * 抖动，也不会让拔电后的真实下降迟迟不显示。定点是为了避开整数除法的
 * 静差：直接用 int 做 (x-ema)/8，差值小于 8 时增量恒为 0，EMA 会卡住。
 *
 * 只平滑 **pct**，不动快照里的 batt_mv：后者是寄存器直读值，诊断页与详情
 * 日志都按"原始读数"在用它，平滑过的电压会让人对不上寄存器。这处不对称
 * 是刻意的。 */
static int32_t s_pct_ema_mv_x256;

static power_snapshot_t build_snapshot(const sy6970_status_t *st,
                                       int64_t now_us)
{
    power_snapshot_t out;
    out.source           = st->vbus_present ? POWER_SRC_SY6970_VBUS
                                            : POWER_SRC_BATTERY;
    out.backend          = POWER_BACKEND_SY6970;
    out.batt_mv          = st->batt_mv;
    {
        const int32_t mv_x256 = (int32_t)st->batt_mv * 256;
        if (s_pct_ema_mv_x256 == 0) s_pct_ema_mv_x256 = mv_x256;  /* 首拍 */
        else s_pct_ema_mv_x256 += (mv_x256 - s_pct_ema_mv_x256) / 8;
        out.pct_est = (uint8_t)pk_batt_mv_to_pct(
            (int)(s_pct_ema_mv_x256 / 256));
    }
    out.pct_valid        = (out.batt_mv > 2500 && out.batt_mv < 4500);
    out.charging         = st->charging;
    /* F6 范围裁定（controller 2026-09-07）：计划里的 VBUS 分压网络
     * （v4=30k/10k、v3=10k/10k）物理上接在 **RP2040 的 ADC**（U8 pin 40，
     * 网络 USB_VBUS_SENSE，取证 hardware/test_component_contract.py:33-50
     * 与 :568-570），不在 ESP32-P4 上；P4 要读它得扩展 RP2040 UART 协议
     * （v1.0 已冻结），超出本任务范围、明确不做。因此 vbus_present 与
     * VBUS 电压取自 SY6970 自己的 BUSV ADC（[DS] p.24：2.6V+code×100mV，
     * 2.6~15.3V，已在 st->vbus_mv），覆盖 5V/9V 档判别；7V 中点
     * 阈值只在 60 s 复检日志里作诊断参考。 */
    out.vbus_present     = st->vbus_present;
    /* 没有库仑计，剩余时间不可估，如实标注（同 ETA6098 backend）。 */
    out.time_degraded_na = true;
    out.updated_us       = now_us;
    return out;
}

static power_snapshot_t poll_fail(int64_t now_us);

/*
 * 诊断态整体提交（s_diag_mux 临界区，见 statics 处注释）：锁内整体
 * 拷贝，无重试路径。只在"读 ok + 解码 ok"的完整成功拍调用（审计 F4：
 * 候选帧半路失败不许污染已提交证据）——含 wd_fault 帧（审计修复 P2）：
 * 诊断证据与服务新鲜度分账，故障帧照常上屏；服务快照（s_last_good）
 * 是否刷新由调用方决定。
 */
static void diag_commit(const sy6970_status_t *st, const uint8_t *win,
                        int64_t now_us)
{
    portENTER_CRITICAL(&s_diag_mux);
    s_diag.st         = *st;
    memcpy(s_diag.regs, win, SY6970_WIN_LEN);
    s_diag.reg00      = s_reg00;
    s_diag.ready      = s_ready;
    s_diag.updated_us = now_us;
    portEXIT_CRITICAL(&s_diag_mux);
}

/*
 * 服务的 1 Hz 轮询入口（单写者）。连读两遍窗口取 REG0C 实况，第二遍帧
 * 解码进状态；读失败时原样上报最近一份好快照——updated_us 停在最后好拍，
 * 服务端 5 s 后判 stale 自动回落 ETA6098（power_service.h:15-20 的选择
 * 语义），这里绝不编造新鲜数据。
 */
static power_snapshot_t sy6970_poll(int64_t now_us)
{
    /* 防御闸（同 eta6098_poll 的口径，服务层不假设 backend 内部状态）：
     * 正常路径注册前 bring-up 已成功；唯一能走到 !s_ready 的场景是
     * init 期 bring-up 失败但 ACK 已注册——按连击节流补试：首拍立试
     * 立报，之后每 SY6970_FAIL_STREAK_MAX 拍一试，失败 WARN 不以 1 Hz
     * 刷日志；成功只报一条恢复行。 */
    if (!s_ready) {
        s_boot_streak++;
        if (s_boot_streak != 1 &&
            s_boot_streak % SY6970_FAIL_STREAK_MAX != 0) {
            return s_last_good;               /* 初值=全零快照，恒 stale */
        }
        if (!bring_up()) return s_last_good;
        s_ready       = true;
        s_boot_streak = 0;
        s_bus_gen     = pk_i2c0_bus_generation();
        ESP_LOGI(TAG, "SY6970 bring-up 成功");
    }

    /* 总线被救回来 → 重放配置 + F1。器件 handle 不重建（总线复位不清
     * add_device，qmc5883p 同款契约）；重放失败不提交代数，按连击节流
     * 重试：首拍立试立报，之后每 SY6970_FAIL_STREAK_MAX 拍一试（总线
     * 一直挂着时每拍两条 WARN 会刷日志），成功只报一条恢复行。 */
    const uint32_t gen = pk_i2c0_bus_generation();
    if (gen != s_bus_gen) {
        s_gen_streak++;
        if (s_gen_streak != 1 &&
            s_gen_streak % SY6970_FAIL_STREAK_MAX != 0) {
            return s_last_good;               /* 节流拍：静默回落旧快照 */
        }
        ESP_LOGW(TAG, "I²C0 总线已复位（第 %lu 轮）— 重放 SY6970 bring-up",
                 (unsigned long)gen);
        if (!bring_up()) {
            ESP_LOGW(TAG, "重放失败，再等 %d 拍后重试",
                     SY6970_FAIL_STREAK_MAX);
            return s_last_good;
        }
        s_bus_gen    = gen;
        s_gen_streak = 0;
        ESP_LOGI(TAG, "I²C0 总线恢复：SY6970 配置重放成功（第 %lu 轮）",
                 (unsigned long)gen);
    }

    /* REG0C 故障锁存到被读走，取实况须连读两遍（[DS] p.29 / [AN] p.29，
     * 连读两遍的示例见 [AN] p.35）；唯一例外 NTC_FAULT 不锁存、恒如实
     * （[DS] p.29）。第一遍把上次的锁存冲掉，第二遍帧整体解码——其余
     * 寄存器是 ADC 快照，用同一帧保持整帧一致。
     * 审计 F4：第二遍读进**本地候选帧**，不直接写诊断态——读失败或
     * 解码失败（全 0xFF）时已提交的证据原封不动，不再出现"旧解码状态
     * 配 0xFF 原始字节"的矛盾快照。 */
    uint8_t flush[SY6970_WIN_LEN];
    uint8_t win[SY6970_WIN_LEN];
    if (reg_read(SY6970_WIN_REG0, flush, SY6970_WIN_LEN) != ESP_OK ||
        reg_read(SY6970_WIN_REG0, win, SY6970_WIN_LEN) != ESP_OK) {
        return poll_fail(now_us);
    }

    sy6970_status_t st;
    if (!sy6970_decode_status(win, sizeof(win), &st)) {
        /* 整窗全 0xFF = 器件掉电/离线（窗口合同，power_sy6970.h）。 */
        return poll_fail(now_us);
    }

    /* 看门狗自愈（审计 F2）：WATCHDOG_FAULT（REG0C[7]，[DS] p.29"超时
     * → 回默认模式"）意味着关狗写已被默认模式吃掉——与总线代数失配
     * 同一个「配置丢了」条件，走同款连击节流重放 bring-up。重放成功
     * 前不提交**服务快照**（不刷 s_last_good/updated_us）：本帧窗口读
     * 于默认模式（ADC 可能已停，值是陈货），提交它就是"新鲜时间戳盖
     * 冻结读数"；原样回旧快照，让服务端看到的是 staleness。
     * 诊断证据是另一回事（审计修复 P2）：本帧读+解码都成功，照常
     * diag_commit——不然 WDFAULT 的屏上证据（REG0C 原始字节 + 译码）
     * 永远不可达，只剩一条控制台 WARN。 */
    if (st.wd_fault) {
        diag_commit(&st, win, now_us);
        s_wd_streak++;
        if (s_wd_streak != 1 &&
            s_wd_streak % SY6970_FAIL_STREAK_MAX != 0) {
            return s_last_good;               /* 节流拍：静默守旧 */
        }
        ESP_LOGW(TAG, "WATCHDOG_FAULT（REG0C[7]）— 寄存器被打回默认模式，"
                      "重放 SY6970 bring-up");
        if (!bring_up()) {
            ESP_LOGW(TAG, "看门狗自愈重放失败，再等 %d 拍后重试",
                     SY6970_FAIL_STREAK_MAX);
            return s_last_good;
        }
        s_wd_streak = 0;
        ESP_LOGI(TAG, "看门狗自愈：SY6970 配置重放成功（本拍弃用，下拍取新）");
        return s_last_good;                   /* 本帧来自默认模式，弃用 */
    }
    s_wd_streak = 0;

    s_fail_streak = 0;
    if (s_up_us == 0) s_up_us = now_us;    /* 60 s 复检的锚：首拍成功时刻 */
    s_last_good   = build_snapshot(&st, now_us);
    diag_commit(&st, win, now_us);

    /* 周期详情日志（理由见 SY6970_DETAIL_LOG_TICKS 处注释）。首拍就打
     * 一行——排障时最想要的是"现在什么样"，不是等 30 s。
     * NTCPCT 是 0.001% 整数单位，拆成 xx.x% 无浮点。
     *
     * REG00 **当场重读**而不是用 s_reg00：后者只在 bring-up 时更新，
     * 拿它算出来的 IINLIM 永远等于"我们写进去的值"，证明不了芯片有没有
     * 在运行期把它改回去——而 IINLIM 恰恰是会被 DP/DM 检测改写的寄存器
     * （[DS] p.15），关掉 AUTO_DPDM_EN 到底有没有用，只有实时值能回答。
     * 读失败不算故障（主窗口那两遍才是数据来源），退回 bring-up 值并
     * 在行里标 stale，绝不把陈值冒充实时。 */
    if ((s_detail_tick++ % SY6970_DETAIL_LOG_TICKS) == 0) {
        uint8_t r00_now = 0;
        const bool r00_live = (reg_read(SY6970_REG00, &r00_now, 1) == ESP_OK);
        const uint8_t r00 = r00_live ? r00_now : s_reg00;
        const unsigned iinlim_ma = 100u + 50u * (unsigned)(r00 & 0x3F);
        /* ICHGR 在**不充电时不刷新**，保持上一次的有效值（2026-09-12
         * 实测：拔掉输入后 ICHG 仍恒报 250 mA）。照原样打出来会让人以为
         * 还在充——不充电时显式标成 stale，不拿陈值冒充读数，也不编造
         * 一个 0（那同样是我们没有的信息）。 */
        char ichg_s[16];
        if (st.charging)
            snprintf(ichg_s, sizeof ichg_s, "%umA", (unsigned)st.ichg_ma);
        else
            snprintf(ichg_s, sizeof ichg_s, "%umA(stale)",
                     (unsigned)st.ichg_ma);
        ESP_LOGI(TAG,
                 "ICHG=%s VBUS=%umV BATT=%umV NTC=%u(%lu.%lu%%) "
                 "IINLIM=%umA REG00=0x%02X%s REG0C=0x%02X chg=%d therm=%d",
                 ichg_s, (unsigned)st.vbus_mv,
                 (unsigned)st.batt_mv, (unsigned)st.ntc_fault,
                 (unsigned long)(st.ntc_pct_x1000 / 1000u),
                 (unsigned long)(st.ntc_pct_x1000 % 1000u / 100u),
                 iinlim_ma, r00, r00_live ? "" : "(stale)", win[1],
                 (int)st.charging, (int)st.therm_reg);
        /* 只在真有故障位时才连读取证，平时不占日志。
         *
         * 2026-09-12 查过一轮 CHRG_FAULT=01("输入故障")：它在 9V 正常供电、
         * 900mA 正常充电时也一直报，一度以为是伪报。实测结论是**真实瞬态被
         * 锁存**——拔电源线那一刻 VBUS 跌破 3.8V，正好命中 [DS] p.22 的
         * 01 判据，此后锁存不放；插回并复位若干次后自行清除（复现：REG0C
         * 由 0x12 变回 0x02）。
         * 所以看到 CHRG_FAULT 非 0 时，先问"刚才插拔过电源吗"，别急着查
         * 供电质量。连读四次是为了分辨"锁存清不掉"与"每拍都有新故障"：
         * 四个值一样 = 当前状态就是它；逐次变化 = 锁存正在被读掉。
         * NTC_FAULT 不锁存（[DS] p.29），不受这段影响。 */
        if (st.chrg_fault != 0 || st.wd_fault || st.boost_fault ||
            st.bat_ovp_fault) {
            uint8_t p0c[4] = { 0 };
            for (int i = 0; i < 4; ++i)
                (void)reg_read(SY6970_REG0C, &p0c[i], 1);
            ESP_LOGW(TAG, "REG0C 故障连读：%02X %02X %02X %02X"
                          "（主路径 win[1]=%02X；四值相同=当前态，"
                          "递变=锁存正被读掉）",
                     p0c[0], p0c[1], p0c[2], p0c[3], win[1]);
        }
    }

    /* 开机 60 s 复检充电电流（一次性）：CH224K 诱骗与配置此时都该稳定。
     * ICHGR 在窗口帧的 REG12（win[7]；注意 REG11 是 BUSV 不是 ICHG）。
     * 若期间看门狗曾把寄存器打回默认模式，这拍数据就是证据。 */
    if (!s_ichg_recheck_done &&
        now_us - s_up_us >= SY6970_ICHG_RECHECK_US) {
        s_ichg_recheck_done = true;
        /* 档位标签只在 VBUS 真在位时给：没插电时 BUSV 寄存器是残值
         * （2600 mV 下限），照常判档会把无输入报成"5V档"。 */
        ESP_LOGI(TAG, "60s 复检：ICHG=%umA VBUS=%umV chg=%d —— %s"
                      "（7V 中点判档，诊断参考；VBAT<VSHORT 时芯片自报 0mA）",
                 (unsigned)st.ichg_ma, (unsigned)st.vbus_mv,
                 (int)st.charging,
                 !st.vbus_present          ? "无 VBUS"
                     : st.vbus_mv >= SY6970_VBUS_9V_MIDPOINT_MV
                                           ? "9V档" : "5V档");
    }

    return s_last_good;
}

/* 读失败/解码失败共用路径：连击计数 + 周期性自愈重放，原样返回旧快照。 */
static power_snapshot_t poll_fail(int64_t now_us)
{
    (void)now_us;
    s_fail_streak++;
    if (s_fail_streak >= SY6970_FAIL_STREAK_MAX) {
        ESP_LOGW(TAG, "连续 %d 拍无有效数据 — 重放 SY6970 bring-up",
                 s_fail_streak);
        if (!bring_up()) {
            ESP_LOGW(TAG, "自愈重放失败，再等 %d 拍后重试",
                     SY6970_FAIL_STREAK_MAX);
        }
        s_fail_streak = 0;
    }
    return s_last_good;
}

/* F7 诊断快照：锁内整体拷贝（s_diag_mux，见 statics 处注释），无重试
 * 路径。从未拿到过数据（updated_us==0，从未报数哨兵）返回 false——
 * 这是哨兵语义，不是锁失败路径。 */
bool sy6970_diag_get(sy6970_diag_t *out)
{
    if (out == NULL) return false;

    sy6970_diag_t c;
    portENTER_CRITICAL(&s_diag_mux);
    c.st         = s_diag.st;
    c.reg00      = s_diag.reg00;
    memcpy(c.regs, s_diag.regs, sizeof(c.regs));
    c.ready      = s_diag.ready;
    c.updated_us = s_diag.updated_us;
    portEXIT_CRITICAL(&s_diag_mux);

    if (c.updated_us == 0) return false;   /* 从未拿到过数据 */
    *out = c;
    return true;
}

/* backend 登记项：name 仅用于日志；id 由服务盖进聚合快照（同源守卫）。 */
static const power_backend_t s_backend = {
    .name = "sy6970",
    .id   = POWER_BACKEND_SY6970,
    .poll = sy6970_poll,
};

void power_sy6970_init(void)
{
    static bool s_brought_up;
    if (s_brought_up) return;                      /* 幂等：只探一次 */
    s_brought_up = true;

    i2c_master_bus_handle_t bus = pk_i2c0_bus_get();
    if (bus == NULL) {
        ESP_LOGW(TAG, "I2C0 总线不可用——SY6970 探测跳过，电源回落 ETA6098");
        return;
    }

    /* 地址探活。
     *
     * 这条日志原写"NACK 是预期路径（v3 载板 / 未上电的 v4），电源回落
     * ETA6098"——两处都已不成立：v3 支持于 2026-09-12 取消，ETA6098
     * backend 连同文件一起退役，服务里再没有兜底源。现在 NACK 意味着
     * **这块板上本该在的充电芯片没应答**，是异常不是常态，所以升到 WARN
     * 并说清后果。
     *
     * 已知会误伤的瞬态：2026-09-12 实测过一次开机恰逢插拔电源线导致
     * NACK，下一次复位即 ACK。而本函数只在 app_main 跑一次、NACK 之后
     * 不再重试，于是整个开机周期都没有电量显示。重试机制待定（poll 侧
     * 已有每 10 拍补试 bring-up 的自愈逻辑，缺的是"未注册也要轮询"这一
     * 层），在那之前这条 WARN 至少让人知道该复位一次。 */
    if (pk_i2c0_bus_probe(SY6970_I2C_ADDR, SY6970_I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X 探测 NACK——充电芯片无应答，本次开机将没有任何"
                      "电源数据（v3 与 ETA6098 兜底均已退役）。复位可重试。",
                 SY6970_I2C_ADDR);
        return;
    }

    /* add_device 只许 init 期单线程调用（pk_i2c0_bus.h 合同）——此刻在
     * app_main，先于一切轮询/恢复任务，无并发窗口。 */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SY6970_I2C_ADDR,
        .scl_speed_hz    = 400000,   /* 与同总线的 BNO085/BMP388/GT911 同速 */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add_device 失败（%s）——不注册", esp_err_to_name(err));
        return;
    }

    /* ACK 即注册（bring-up 失败由 poll 的 1 Hz 自愈重试兜住，见下）。
     * 注册次序=优先级，SY6970 占第一槽 = 权威源。
     * 注：原注释说"先于 power_eta6098_init() 调用、ETA6098 随后注册为
     * 兜底"，这在 2026-09-10 之后已不成立——电池挂在扩展板，微雪载板的
     * ETA6098 不再读也**不再注册**（见 main.c 电源链注释），本 backend
     * 是电源服务里唯一的注册者。 */
    power_service_register(&s_backend);
    ESP_LOGI(TAG, "0x%02X ACK——SY6970 注册为电源权威源", SY6970_I2C_ADDR);
}

#endif /* SY6970_HOST_TEST */
