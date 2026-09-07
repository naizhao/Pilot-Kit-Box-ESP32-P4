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
#include "pk_i2c0_bus.h"
#include "power_eta6098.h"
#include "power_service.h"

static const char *TAG = "sy6970";

/* I²C 地址 0x6A [DS] p.15 / [ALT] p.12 / [AN] p.15（"Address: 6AH"）。 */
#define SY6970_I2C_ADDR       0x6A
#define SY6970_I2C_TIMEOUT_MS 100  /* 与 qmc/baro 的单笔超时口径一致   */

#define SY6970_REG02 0x02

/* REG02 CONV_START[7] / CONV_RATE[6] [DS] p.16 / [ALT] p.13 / [AN] p.16：
 * CONV_RATE=1 → "Start 1s continuous Conversion"，且转换自动开始；
 * CONV_RATE=1 时 CONV_START 变只读（转换期间保持 1）。ADC 不开则
 * REG0E~12 全是陈旧值——轮询侧的职责（Task 3 取证表事实 14）。
 * 用 RMW 置位而不是整字节覆盖：REG02 其余位（AICL_EN/HVDCP_EN 等
 * POR=1）保持芯片当前值，与 F1 对 REG07 的 RMW 同一哲学。 */
#define SY6970_CONV_MASK      0xC0  /* CONV_START | CONV_RATE          */

/* 自愈节拍：连续 10 拍（≈10 s）才动一次 bring-up 级重试，与 qmc5883p
 * 的失明自愈一致——单拍抖动不值得动配置写。!s_ready 补试与代数失配
 * 重放同用这个节拍做失败日志节流：首拍立试立报，之后每 10 拍一次，
 * 失败 WARN 不以 1 Hz 刷日志。 */
#define SY6970_FAIL_STREAK_MAX 10

/* 开机 60 s 后复检充电电流（F1 配套）：CH224K 诱骗 + SY6970 配置都该
 * 已稳定；若期间看门狗把寄存器打回过默认模式，这一拍的数据能看出来。 */
#define SY6970_ICHG_RECHECK_US (60LL * 1000 * 1000)

/* 5V/9V 判档中点：CH224K 诱骗档位只到 9V，7V 中点区分两档。仅作
 * 诊断参考口径——权威 VBUS 值来自 BUSV ADC，见 sy6970_poll 的 F6 注。 */
#define SY6970_VBUS_9V_MIDPOINT_MV 7000

static i2c_master_dev_handle_t s_dev;

/* ── 单写者状态（写者 = power_service 的 1 Hz poll 任务；读者自由拷贝，
 * 撕裂容忍口径同 power_service.h:27-32）────────────────────────────── */
static bool            s_ready;      /* bring-up 成功（在读数）            */
static sy6970_status_t s_status;     /* 最近一次成功解码                   */
static uint8_t         s_regs[SY6970_WIN_LEN];  /* 原始窗口字节（F7）     */
static uint8_t         s_reg00;      /* 最近一次 REG00 回读（F1 证据）     */
static int64_t         s_updated_us; /* 最近成功采集时刻（0=从未）         */
static power_snapshot_t s_last_good; /* 最近一份好快照：读失败时原样上报，
                                     * 让服务端按 updated_us 判 stale 回落 */
static uint32_t        s_bus_gen;    /* 已认账的总线代数                   */
static int             s_fail_streak;
static int             s_boot_streak;   /* !s_ready 补试连击（日志节流）   */
static int             s_gen_streak;    /* 代数失配重放连击（日志节流）    */
static int64_t         s_up_us;      /* 首拍成功时刻：60 s 复检的锚        */
static bool            s_ichg_recheck_done;

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
 * F1 看门狗管理 + ADC 连续转换开启（bring-up）。
 *
 * 序列按 sy6970_init_seq() **逐字执行**（顺序不得重排）：写前 REG00
 * 在位校验 → REG07 关狗 → REG03 喂狗 → 写后 REG00 回读验证。两次 REG00
 * 回读值都以 INFO 进日志（write-intent + write-result 证据，计划约束）；
 * 任一步失败 WARN 带 why 并返回 false。本文件唯一的寄存器写豁免是
 * 看门狗管理（F1，计划全局约束）+ ADC 转换开启（Task 3 取证表事实 14
 * 点名 Task 4 职责）；充电参数（IINLIM/ICHG/VREG 等）一概不碰。
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

    size_t n = 0;
    const sy6970_init_step_t *seq = sy6970_init_seq(&n);
    for (size_t i = 0; i < n; i++) {
        const sy6970_init_step_t *st = &seq[i];
        switch (st->op) {
        case SY6970_SEQ_VERIFY: {
            uint8_t val = 0;
            const esp_err_t rd = reg_read(st->reg, &val, 1);
            if (rd != ESP_OK) {
                ESP_LOGW(TAG, "F1 第 %u 步校验失败：REG%02X 读失败（%s）— %s",
                         (unsigned)i, st->reg, esp_err_to_name(rd), st->why);
                return false;
            }
            if ((val & st->mask) != st->val) {
                ESP_LOGW(TAG, "F1 第 %u 步校验失败：REG%02X=0x%02X（掩码 0x%02X "
                              "期望 0x%02X）— %s",
                         (unsigned)i, st->reg, val, st->mask, st->val, st->why);
                return false;
            }
            if (st->reg == SY6970_REG00) s_reg00 = val;
            ESP_LOGI(TAG, "F1 %s：REG00=0x%02X（掩码 0x%02X==0x%02X）",
                     i == 0 ? "写前在位校验" : "写后回读验证",
                     val, st->mask, st->val);
            break;
        }
        case SY6970_SEQ_RMW_CLEAR:
        case SY6970_SEQ_RMW_SET: {
            uint8_t old = 0;
            if (reg_read(st->reg, &old, 1) != ESP_OK) {
                ESP_LOGW(TAG, "F1 第 %u 步 RMW 回读失败：REG%02X — %s",
                         (unsigned)i, st->reg, st->why);
                return false;
            }
            const uint8_t newv = (st->op == SY6970_SEQ_RMW_SET)
                                     ? (uint8_t)(old | st->mask)
                                     : (uint8_t)(old & ~st->mask);
            if (reg_write(st->reg, newv) != ESP_OK) {
                ESP_LOGW(TAG, "F1 第 %u 步 RMW 写入失败：REG%02X — %s",
                         (unsigned)i, st->reg, st->why);
                return false;
            }
            ESP_LOGI(TAG, "F1 RMW：REG%02X 0x%02X→0x%02X", st->reg, old, newv);
            break;
        }
        }
    }
    return true;
}

/* 快照组装（成功拍）。pct：SY6970 与 ETA6098 一样只有电压没有库仑计，
 * 复用同一张电芯放电曲线（power_eta6098_mv_to_pct，合同见其头文件）；
 * 插电维持电压的虚高偏差在这里**未补偿**——ETA 的 CC/HOLD 压降补偿是
 * 2026-08-04 按那颗芯片实测标定的，直接套用到 SY6970 属于编造，标定
 * 数据到手前如实带着偏差（ichg_ma/charging 已在诊断快照里，标定有据
 * 可依）。量程闸与 ETA6098 backend 同口径。 */
static power_snapshot_t build_snapshot(int64_t now_us)
{
    power_snapshot_t out;
    out.batt_mv          = s_status.batt_mv;
    out.pct_est          = (uint8_t)power_eta6098_mv_to_pct(s_status.batt_mv);
    out.pct_valid        = (out.batt_mv > 2500 && out.batt_mv < 4500);
    out.charging         = s_status.charging;
    /* F6 范围裁定（controller 2026-09-07）：计划里的 VBUS 分压网络
     * （v4=30k/10k、v3=10k/10k）物理上接在 **RP2040 的 ADC**（U8 pin 40，
     * 网络 USB_VBUS_SENSE，取证 hardware/test_component_contract.py:33-50
     * 与 :568-570），不在 ESP32-P4 上；P4 要读它得扩展 RP2040 UART 协议
     * （v1.0 已冻结），超出本任务范围、明确不做。因此 vbus_present 与
     * VBUS 电压取自 SY6970 自己的 BUSV ADC（[DS] p.24：2.6V+code×100mV，
     * 2.6~15.3V，已在 s_status.vbus_mv），覆盖 5V/9V 档判别；7V 中点
     * 阈值只在 60 s 复检日志里作诊断参考。 */
    out.vbus_present     = s_status.vbus_present;
    out.source           = s_status.vbus_present ? POWER_SRC_SY6970_VBUS
                                                 : POWER_SRC_BATTERY;
    /* 没有库仑计，剩余时间不可估，如实标注（同 ETA6098 backend）。 */
    out.time_degraded_na = true;
    out.updated_us       = now_us;
    return out;
}

static power_snapshot_t poll_fail(int64_t now_us);

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
     * 寄存器是 ADC 快照，用同一帧保持整帧一致。顺序读窗口也清 REG0C
     * 的锁存：锁存语义挂在"寄存器被读"这个事件上，与单字节/顺序读无关。 */
    uint8_t flush[SY6970_WIN_LEN];
    if (reg_read(SY6970_WIN_REG0, flush, SY6970_WIN_LEN) != ESP_OK ||
        reg_read(SY6970_WIN_REG0, s_regs, SY6970_WIN_LEN) != ESP_OK) {
        return poll_fail(now_us);
    }

    sy6970_status_t st;
    if (!sy6970_decode_status(s_regs, sizeof(s_regs), &st)) {
        /* 整窗全 0xFF = 器件掉电/离线（窗口合同，power_sy6970.h）。 */
        return poll_fail(now_us);
    }

    s_status      = st;
    s_updated_us  = now_us;
    s_fail_streak = 0;
    if (s_up_us == 0) s_up_us = now_us;    /* 60 s 复检的锚：首拍成功时刻 */
    s_last_good   = build_snapshot(now_us);

    /* 开机 60 s 复检充电电流（一次性）：CH224K 诱骗与配置此时都该稳定。
     * ICHGR 在窗口帧的 REG12（regs[7]；注意 REG11 是 BUSV 不是 ICHG）。
     * 若期间看门狗曾把寄存器打回默认模式，这拍数据就是证据。 */
    if (!s_ichg_recheck_done &&
        now_us - s_up_us >= SY6970_ICHG_RECHECK_US) {
        s_ichg_recheck_done = true;
        /* 档位标签只在 VBUS 真在位时给：没插电时 BUSV 寄存器是残值
         * （2600 mV 下限），照常判档会把无输入报成"5V档"。 */
        ESP_LOGI(TAG, "60s 复检：ICHG=%umA VBUS=%umV chg=%d —— %s"
                      "（7V 中点判档，诊断参考；VBAT<VSHORT 时芯片自报 0mA）",
                 (unsigned)s_status.ichg_ma, (unsigned)s_status.vbus_mv,
                 (int)s_status.charging,
                 !s_status.vbus_present    ? "无 VBUS"
                     : s_status.vbus_mv >= SY6970_VBUS_9V_MIDPOINT_MV
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

/* F7 诊断快照：单写者/无锁读者合同（见 sy6970_diag_t 处注释）。 */
bool sy6970_diag_get(sy6970_diag_t *out)
{
    if (out == NULL) return false;
    *out = (sy6970_diag_t){0};
    if (s_updated_us == 0) return false;   /* 从未拿到过数据 */

    out->st         = s_status;
    out->reg00      = s_reg00;
    memcpy(out->regs, s_regs, sizeof(out->regs));
    out->ready      = s_ready;
    out->updated_us = s_updated_us;
    return true;
}

/* backend 登记项：name 仅用于日志。 */
static const power_backend_t s_backend = {
    .name = "sy6970",
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

    /* 地址探活：NACK 是预期路径（v3 载板 / 未上电的 v4），INFO 不是 WARN
     * ——它表达的是 powered variant，不是故障（pk_board.h:30-32）。 */
    if (pk_i2c0_bus_probe(SY6970_I2C_ADDR, SY6970_I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGI(TAG, "0x%02X 探测 NACK——无 SY6970（v3 / 未上电 v4 预期），"
                      "不注册，电源回落 ETA6098", SY6970_I2C_ADDR);
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
     * 注册次序=优先级：本函数在 main.c 里先于 power_eta6098_init() 调用，
     * SY6970 占第一槽 = 权威源；ETA6098 随后注册自然回落为兜底。 */
    power_service_register(&s_backend);
    ESP_LOGI(TAG, "0x%02X ACK——SY6970 注册为电源权威源", SY6970_I2C_ADDR);
}

#endif /* SY6970_HOST_TEST */
