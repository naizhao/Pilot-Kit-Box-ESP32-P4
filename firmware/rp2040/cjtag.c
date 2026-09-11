/*
 * cjtag.c — RP2040 位脉冲驱动 CC1312R 的 2 线 cJTAG。
 *
 * 分层：
 *   引脚原语（pin_*）      —— 目标端 = GPIO；host 测试 = 器件模型（唯一的两边差异）
 *   位/包时序（hw_*）      —— JScan 2 线 / OScan1 三周期包（共享）
 *   TAP 状态机 + 命令序列  —— 共享
 *
 * ⚠ 分层的关键约束：**除了 pin_* 那几个函数，所有时序都必须是共享代码**。
 * 旧版把激活序列整个 stub 掉，于是致命的位级 bug 在 71/71 全绿的 host 测试
 * 里完全看不见。器件模型见 firmware/test/test_cjtag.c。
 *
 * ════════════════════════════════════════════════════════════════════
 * 协议依据：CC13x2/CC26x2 TRM **SWCU185G** 第 6 章（本地 /tmp/swcu185.pdf）
 * ════════════════════════════════════════════════════════════════════
 *
 * §6.2  「The 2-pin JTAG mode using only TCK and TMS is the default
 *        configuration after power up.」
 *        上电即 2 线：TMSC 就是 TMS，**没有 TDI/TDO 通路**。想读回任何东西，
 *        必须先把扫描格式切到 OScan1。
 *
 * §6.2.1「cJTAG commands are conveyed through benign JTAG scan activity.」
 *        TI 的 cJTAG 不用 IEEE 1149.7 的 TMSC escape 序列——**整本 TRM 里
 *        "escape" 这个词一次都没出现**。命令是用「在 Shift-DR 里停留了几个
 *        时钟」来编码的：CP0 = opcode，CP1 = operand，各 0~31。
 *
 * §6.2.2.1 开命令窗：IR 扫惰性指令(BYPASS) 停在 Pause-DR → 两次 ZBS
 *        （经 Capture-DR 与 Update-DR 但一次都不进 Shift-DR，每次 control
 *        level +1）→ 1 位 DR 扫描把 control level 锁在 2。
 *
 * §6.2.1 Table 6-4：STFMT（Store Scan Format）opcode = 0b00011 = 3，
 *        operand 9 = OSCAN1。→ 发 CP0=3、CP1=9 即切到 OScan1。
 *        （OpenOCD tcl/target/ti/cjtag.cfg 那段众所周知的"魔法序列"其实就是
 *          CP0=2/CP1=9 = STC2/APFC，作用是切到 **4 线** JTAG。本板 TDI/TDO
 *          没接到 RP2040，跑那段等于把唯一的链路切没——旧版照抄了它。）
 *
 * §6.2.2.3 IR 扫描 / 进 TLR / ECL 都会关掉命令窗，之后扫描重新回到器件 TAP。
 *
 * §6.4  ICEMelter：**JTAG 电源域默认是断电的**，ICEPick 和 cJTAG 模块都在
 *        里面。要先在 TCK 上打够 8 个上升沿 + 8 个下降沿把它唤醒，且第 3 到
 *        第 8 个上升沿之间不得超过 4 ms，之后还要**至少等 200 µs**让电源域
 *        起来才能发命令。不做这一步，前面所有协议都是对着一块断电的逻辑说话。
 *        副作用：唤醒会置 Halt-In-Boot 标志，只能靠 pin reset / POR / JTAG
 *        清掉——cjtag_exit() 的 RESET_N 脉冲就是干这个的。
 *
 * §6.3  ICEPick 是片上唯一的一级 TAP，上电后二级 TAP（Cortex-M DAP）都不在
 *        扫描链上，要用 CONNECT + ROUTER 写才能挂上来。
 *
 * 其他常量出处：ICEPick irlen=6 / JRC_TAPID=0x0BB4102F —— OpenOCD
 * tcl/target/ti/cc26x0.cfg 与 cc13x2.cfg。
 */
#include "cjtag.h"

#include <string.h>
#include <stdio.h>

/* MAYBE_UNUSED：host 测试构建时某些静态函数未被调用（不产出 -Wunused 警告）*/
#ifdef CJTAG_HOST_TEST
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

#ifndef CJTAG_HOST_TEST
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#endif

/* ── 引脚原语 ────────────────────────────────────────────────────── */
/* host 测试下由 test_cjtag.c 的器件模型实现这些符号。 */

#ifdef CJTAG_HOST_TEST
void cjtag_model_pin_init(void);
void cjtag_model_reset(int level);
void cjtag_model_tckc(int level);
void cjtag_model_tmsc_drive(int level);
void cjtag_model_tmsc_hiz(void);
int  cjtag_model_tmsc_read(void);

static void pin_init(void)             { cjtag_model_pin_init(); }
static void pin_reset(int level)       { cjtag_model_reset(level); }
static void pin_tckc(int level)        { cjtag_model_tckc(level); }
static void pin_tmsc_drive(int level)  { cjtag_model_tmsc_drive(level); }
static void pin_tmsc_hiz(void)         { cjtag_model_tmsc_hiz(); }
static int  pin_tmsc_read(void)        { return cjtag_model_tmsc_read(); }
static void pin_tmsc_pull(int mode)    { (void)mode; }
/* 把等待也告诉模型：ICEMelter 的「8 个沿之后还要 ≥200 µs」是个时间条件，
 * 模型看不到时间就验不了这一条。 */
void cjtag_model_delay_us(int us);
static unsigned s_tck_half_us = (CJTAG_TCK_DELAY_NS + 999) / 1000;
static void pin_delay_half(void)       { cjtag_model_delay_us((int)s_tck_half_us); }
static void pin_delay_us(int us)       { cjtag_model_delay_us(us); }
static void pin_sleep_ms(int ms)       { cjtag_model_delay_us(ms * 1000); }
#else
/* 哪个 GPIO 是数据、哪个是时钟，运行期可换。
 * 依据只到「原理图按引脚名连线」这一层（sheet_mcu.py / sheet_subghz.py 都是
 * name-based），从没在实板上验证过——而本项目栽过一次封装引脚号写错、只有
 * 下钻 PCB 焊盘归属才看得出来的账。诊断里把两种接法都跑一遍，比读图可靠。 */
static unsigned s_pin_tmsc = CJTAG_PIN_TMSC;
static unsigned s_pin_tckc = CJTAG_PIN_TCKC;

static void pin_init(void)
{
    gpio_init(s_pin_tmsc);
    gpio_init(s_pin_tckc);
    gpio_init(CJTAG_PIN_RESET);
    gpio_set_dir(s_pin_tckc, GPIO_OUT);
    gpio_set_dir(CJTAG_PIN_RESET, GPIO_OUT);
    gpio_put(s_pin_tckc, 0);             /* TCKC 空闲低 */
    gpio_put(CJTAG_PIN_RESET, 1);        /* RESET 空闲高（低有效） */
    gpio_pull_up(s_pin_tmsc);            /* TMSC 松手时靠上拉（cJTAG 规范） */
    gpio_set_dir(s_pin_tmsc, GPIO_IN);
}

static void pin_reset(int level)  { gpio_put(CJTAG_PIN_RESET, level); }
static void pin_tckc(int level)   { gpio_put(s_pin_tckc, level); }

/* 先写数据再切输出方向：反过来会先把上一次的残留值推到线上。 */
static void pin_tmsc_drive(int level)
{
    gpio_put(s_pin_tmsc, level);
    gpio_set_dir(s_pin_tmsc, GPIO_OUT);
}

static void pin_tmsc_hiz(void)    { gpio_set_dir(s_pin_tmsc, GPIO_IN); }
static int  pin_tmsc_read(void)   { return (int)gpio_get(s_pin_tmsc); }

/* mode: +1 上拉 / -1 下拉 / 0 不拉。诊断用——用来判「线上读到 0」到底是
 * 目标在驱动，还是没人管、只是残留电荷。 */
static void pin_tmsc_pull(int mode)
{
    if (mode > 0)      gpio_pull_up(s_pin_tmsc);
    else if (mode < 0) gpio_pull_down(s_pin_tmsc);
    else               gpio_disable_pulls(s_pin_tmsc);
}

/* TCK 半周期（µs）。默认 2 → ~250 kHz；诊断里可以放慢，用来排除"沿太快
 * 导致对端没采到"这一类假设。 */
static unsigned s_tck_half_us = (CJTAG_TCK_DELAY_NS + 999) / 1000;

static void pin_delay_half(void) { sleep_us(s_tck_half_us); }

static void pin_delay_us(int us)  { sleep_us((uint32_t)us); }
static void pin_sleep_ms(int ms)  { sleep_ms((uint32_t)ms); }
#endif

static inline void tck_high(void) { pin_tckc(1); pin_delay_half(); }
static inline void tck_low(void)  { pin_tckc(0); pin_delay_half(); }

/* ── 扫描格式 ────────────────────────────────────────────────────── */

typedef enum {
    FMT_JSCAN2 = 0,   /* 上电默认：2 线，TMSC=TMS，1 个 TCKC 一位，无 TDI/TDO */
    FMT_OSCAN1,       /* STFMT 切过去之后：1 个 JTAG 位 = 3 个 TCKC 周期 */
} cjtag_fmt_t;

static cjtag_fmt_t s_fmt = FMT_JSCAN2;

/* 诊断用：采 TDO 前额外等待的微秒数。 */
static int s_tdo_settle_us = 0;

/* 总线已交给外部仿真器：spim 必须一直停手，否则会复位掉 CC1312R。 */
static bool s_bus_released = false;

/*
 * 发一个 JTAG 位。
 *
 * FMT_JSCAN2（TRM §6.2 上电默认）：TMSC 只承载 TMS，目标在 TCKC↑ 采样
 *   （§6.1「TMS is sampled at the rising edge of TCK」）。**没有 TDI 线**——
 *   移进 IR 的数据由器件内部当成全 1，所以 TRM 让你用 BYPASS（全 1）当惰性
 *   指令。返回值无意义。
 *
 * FMT_OSCAN1（TRM Table 6-3）：nTDI / TMS / TDO 各占一个 TCKC 周期。
 *   主机必须在周期2 的 TCKC↓ 之前松开 TMSC，目标从那个下降沿起驱动 TDO
 *   （§6.1「TDO is valid on the falling edge of TCK」）。
 */
static uint8_t hw_scan_bit(uint8_t tms, uint8_t tdi)
{
    if (s_fmt == FMT_JSCAN2) {
        (void)tdi;
        pin_tmsc_drive((int)tms);
        tck_high();
        tck_low();
        return 1;                        /* 2 线模式没有 TDO 通路 */
    }

    pin_tmsc_drive((int)(tdi ^ 1));      /* 周期1：nTDI */
    tck_high();
    tck_low();

    pin_tmsc_drive((int)tms);            /* 周期2：TMS */
    tck_high();
    pin_tmsc_hiz();                      /* 交还总线，必须在 TCKC↓ 之前 */
    tck_low();

    tck_high();                          /* 周期3：目标驱动 TDO */
    if (s_tdo_settle_us) pin_delay_us(s_tdo_settle_us);
    uint8_t tdo = (uint8_t)pin_tmsc_read();
    tck_low();
    return tdo;
}

/* ── 复位 / ICEMelter 唤醒 ───────────────────────────────────────── */

static void hw_reset_pulse(void)
{
    pin_tmsc_hiz();
    pin_tckc(0);
    pin_reset(0);
    pin_sleep_ms(2);                      /* ≥1 ms */
    pin_reset(1);
    pin_sleep_ms(10);                     /* 启动等待 */
}

/*
 * TRM §6.4：JTAG 电源域（含 ICEPick 与 cJTAG 模块）默认断电，靠 ICEMelter
 * 监测 TCK 上的活动来唤醒——需要 8 个上升沿 + 8 个下降沿，且第 3 到第 8 个
 * 上升沿之间不能超过 4 ms（我们 ~250 kHz，16 个周期约 64 µs，远够快），
 * 之后还要给电源域**至少 200 µs**才能发命令。
 *
 * 少了这一步，后面所有协议都是对着一块没上电的逻辑在说话。
 * 期间 TMSC 保持 1（=TMS 高），顺带把 TAP 打到 Test-Logic-Reset。
 */
static void hw_wake_icemelter(void)
{
    pin_tmsc_drive(1);
    for (int i = 0; i < 16; i++) {        /* 8 个够，打 16 个留余量 */
        tck_high();
        tck_low();
    }
    pin_sleep_ms(1);                      /* ≥200 µs */
}

/* ── SEGGER 公布的 cJTAG 连接序列 ───────────────────────────────────
 *
 * 出处：kb.segger.com/J-Link_cJTAG_specifics 的 "Standard connect sequence"。
 * J-Link 是实机能连 CC1312R 的（ADSBee 就用它：-if cJTAG），所以这段是目前
 * 能拿到的、唯一一份**经实机验证的完整 TI cJTAG 上电流程**。
 *
 * ⚠ 更正我此前的一个错误推论：我曾以「整本 TRM 里 escape 出现 0 次」断定
 * TI 不用 escape 序列。这个推断不成立——escape 属于 IEEE 1149.7 链路层，
 * TI 没义务在自己的手册里重复它。SEGGER 的序列里 escape 和 TI TRM 的
 * command window 是**同时存在**的：先用 escape 把 TAP.7 唤醒进 JScan0，
 * 再用 TRM §6.2 那套 ZBS + CP0/CP1 发命令。
 *
 * 还有一条我漏掉的：STFMT 之前要先发 **STC1 operand=1**，即
 * cbbbv = 0_000_1 → bbb=000 选中 SEDGE、c=0、v=1 → SEDGE=1
 * =「用 TCKC 上升沿采样 TMSC」（TRM Table 6-4）。SEGGER 显式设它，说明
 * 复位默认很可能是下降沿采样——那样我的 OScan1 每一位都会采在错的沿上。
 */

/* 原始 TMS 位：1 个 TCKC 一位，TMSC 就是 TMS。激活期间格式尚未确定，
 * 不能走 hw_scan_bit（它会按 s_fmt 决定要不要发 3 周期包）。 */
static void raw_tms(uint32_t bits, int n)
{
    for (int i = 0; i < n; i++) {
        pin_tmsc_drive((int)((bits >> i) & 1));
        tck_high();
        tck_low();
    }
}

/* escape：TCKC 拉高期间把 TMSC 翻转 n 次，再拉低结束。 */
static void raw_escape(int toggles)
{
    pin_tmsc_drive(0);
    pin_delay_half();
    pin_tckc(1);
    pin_delay_half();
    int v = 0;
    for (int i = 0; i < toggles; i++) {
        v ^= 1;
        pin_tmsc_drive(v);
        pin_delay_half();
    }
    pin_tmsc_drive(0);
    pin_delay_half();
    pin_tckc(0);
    pin_delay_half();
}

/* SEGGER "Standard connect sequence" 的前半段：把 TAP.7 唤醒到 JScan0。 */
static void cjtag_segger_wake(void)
{
    raw_escape(10);                /* Reset escape：≥8 次翻转 → 回 JScan0 */
    raw_tms(0xFFFFFFFFu, 24);      /* ≥22 拍 TMS=1 → Test-Logic-Reset */
    raw_tms(0x00u, 1);             /* → Run-Test/Idle */
    raw_escape(7);                 /* Selection escape */
    raw_tms(0x00u, 4);             /* OAC = 0000（长式：唤醒全部 technologies）*/
    raw_tms(0x00u, 4);             /* EC  = 0000（长式，后随 24 位全局寄存器）*/
    raw_tms(0x00000000u, 24);      /* SCNFMT/DLYC/RDYC/TPST/TPPREV/TP_DELN */
    raw_tms(0x00u, 4);             /* Check packet */
    /* 调用方负责把 TAP 状态标记成 Run-Test/Idle（上面最后停在那里）——
     * 这个函数在 TAP 状态机定义之前，够不着 s_tap。 */
}

/* ── TAP 状态机 ──────────────────────────────────────────────────── */

typedef enum {
    TAP_TLR = 0, TAP_RTI,
    TAP_SELECT_DR, TAP_CAPTURE_DR, TAP_SHIFT_DR, TAP_EXIT1_DR,
    TAP_PAUSE_DR, TAP_EXIT2_DR, TAP_UPDATE_DR,
    TAP_SELECT_IR, TAP_CAPTURE_IR, TAP_SHIFT_IR, TAP_EXIT1_IR,
    TAP_PAUSE_IR, TAP_EXIT2_IR, TAP_UPDATE_IR,
} tap_state_t;

static const tap_state_t tap_next[16][2] = {
    /* [当前态][TMS=0]              [当前态][TMS=1]           */
    /* TAP_TLR        */ { TAP_RTI,                          TAP_TLR },
    /* TAP_RTI        */ { TAP_RTI,                          TAP_SELECT_DR },
    /* TAP_SELECT_DR  */ { TAP_CAPTURE_DR,                   TAP_SELECT_IR },
    /* TAP_CAPTURE_DR */ { TAP_SHIFT_DR,                     TAP_EXIT1_DR },
    /* TAP_SHIFT_DR   */ { TAP_SHIFT_DR,                     TAP_EXIT1_DR },
    /* TAP_EXIT1_DR   */ { TAP_PAUSE_DR,                     TAP_UPDATE_DR },
    /* TAP_PAUSE_DR   */ { TAP_PAUSE_DR,                     TAP_EXIT2_DR },
    /* TAP_EXIT2_DR   */ { TAP_SHIFT_DR,                     TAP_UPDATE_DR },
    /* TAP_UPDATE_DR  */ { TAP_RTI,                          TAP_SELECT_DR },
    /* TAP_SELECT_IR  */ { TAP_CAPTURE_IR,                   TAP_TLR },
    /* TAP_CAPTURE_IR */ { TAP_SHIFT_IR,                     TAP_EXIT1_IR },
    /* TAP_SHIFT_IR   */ { TAP_SHIFT_IR,                     TAP_EXIT1_IR },
    /* TAP_EXIT1_IR   */ { TAP_PAUSE_IR,                     TAP_UPDATE_IR },
    /* TAP_PAUSE_IR   */ { TAP_PAUSE_IR,                     TAP_EXIT2_IR },
    /* TAP_EXIT2_IR   */ { TAP_SHIFT_IR,                     TAP_UPDATE_IR },
    /* TAP_UPDATE_IR  */ { TAP_RTI,                          TAP_SELECT_DR },
};

static tap_state_t s_tap = TAP_TLR;

static void tap_clock_tms(uint8_t tms)
{
    (void)hw_scan_bit(tms, 0);
    s_tap = tap_next[s_tap][tms];
}

/* 5 拍 TMS=1 回 Test-Logic-Reset（任何状态出发都成立）。
 * 顺带关掉 cJTAG 命令窗（TRM §6.2.2.3）。 */
static void tap_reset(void)
{
    for (int i = 0; i < 5; i++) tap_clock_tms(1);
    s_tap = TAP_TLR;
}

/*
 * 走到目标状态：在 16 态图上现算最短 TMS 序列。
 *
 * 旧版是手写分支，从 PAUSE_DR 出发会走飞：tap_goto(TAP_RTI) 只发一拍
 * TMS=0，而 PAUSE_DR 的 TMS=0 是自环——软件以为回了 RTI，物理态还在
 * PAUSE_DR，之后每一次 IR/DR 移位都打在错误的状态上。命令窗序列全程在
 * Pause-DR 附近打转，这个坑必踩。
 */
static void tap_goto(tap_state_t target)
{
    if (s_tap == target) return;

    int8_t  from[16];
    uint8_t bit[16];
    uint8_t queue[16];
    int qh = 0, qt = 0;

    for (int i = 0; i < 16; i++) from[i] = -1;
    queue[qt++] = (uint8_t)s_tap;
    from[s_tap] = (int8_t)s_tap;

    while (qh < qt && from[target] < 0) {
        uint8_t cur = queue[qh++];
        for (uint8_t t = 0; t < 2; t++) {
            tap_state_t nxt = tap_next[cur][t];
            if (from[nxt] >= 0) continue;
            from[nxt]  = (int8_t)cur;
            bit[nxt]   = t;
            queue[qt++] = (uint8_t)nxt;
        }
    }
    if (from[target] < 0) return;         /* 图强连通，不会发生 */

    uint8_t path[16];
    int n = 0;
    for (tap_state_t s = target; s != s_tap; s = (tap_state_t)from[s])
        path[n++] = bit[s];
    while (n > 0) tap_clock_tms(path[--n]);
}

/* 移位 IR（LSB 先出）。末位与 TMS=1 同拍退出 Shift。
 * ⚠ FMT_JSCAN2 下没有 TDI 线，instr 的值发不出去，器件按全 1 收——这正是
 * TRM 让用 BYPASS(全 1) 当惰性指令的原因，别指望在 2 线模式选别的指令。 */
static void jtag_shift_ir(uint32_t instr, int bits)
{
    tap_goto(TAP_SHIFT_IR);
    for (int i = 0; i < bits; i++) {
        uint8_t tms = (i == bits - 1) ? 1 : 0;
        (void)hw_scan_bit(tms, (uint8_t)((instr >> i) & 1));
    }
    s_tap = TAP_EXIT1_IR;
    tap_goto(TAP_RTI);
}

/* 移位 DR（LSB 先出，返回读到的值）。 */
static uint32_t jtag_shift_dr(uint32_t tdi, int bits)
{
    uint32_t tdo = 0;
    tap_goto(TAP_SHIFT_DR);
    for (int i = 0; i < bits; i++) {
        uint8_t tms = (i == bits - 1) ? 1 : 0;
        uint8_t rbit = hw_scan_bit(tms, (uint8_t)((tdi >> i) & 1));
        tdo |= (uint32_t)rbit << i;
    }
    s_tap = TAP_EXIT1_DR;
    tap_goto(TAP_RTI);
    return tdo;
}

/* ── cJTAG 命令层（TRM §6.2.1 / §6.2.2）─────────────────────────── */

/*
 * 一次 DR 扫描，起止都在 Pause-DR，在 Shift-DR 里恰好停 n 个时钟。
 *
 * TRM §6.2.1：「The number of clocks spent in the Shift DR state is counted
 * for each scan (from 0 to 31 clocks).」—— 命令的值就是这个计数，**跟移进去
 * 的数据无关**（2 线模式下本来也没有 TDI）。计数在 Update-DR 处结算。
 *
 * n == 0 即 ZBS（zero bit scan）：经 Capture-DR 和 Update-DR，但一次都不进
 * Shift-DR。它同时承担两个角色——开窗阶段每次 ZBS 让 control level +1；
 * 开窗之后它就是「Goto Scan (Through Update DR to Pause DR)」，用来结算
 * 上一个命令部分。
 */
/*
 * Update-DR 之后是走 Update→RTI→Select（经 Run-Test/Idle），还是走
 * Update→Select 的「短路径」。
 *
 * OpenOCD 那段实测能用的 ti_cjtag_to_4pin_jtag **每一次 Update 之后都回
 * RUN/IDLE**；而 IEEE 1149.7 第 9 章的说法是 RTI 标记命令边界、不经 RTI
 * 的短路径留在同一条命令里。两种读法冲突，而我们只有一个可信参照——
 * 默认照抄 OpenOCD，另一种留给 cjtag_diag() 一起试。
 */
static bool s_zbs_via_rti = true;

static void cjtag_dr_count(int n)
{
    if (n <= 0) {
        tap_clock_tms(1);   /* Pause-DR  → Exit2-DR  */
        tap_clock_tms(1);   /* Exit2-DR  → Update-DR （结算上一次扫描）*/
        if (s_zbs_via_rti) {
            tap_clock_tms(0);   /* Update-DR → Run-Test/Idle */
            tap_clock_tms(1);   /* RTI       → Select-DR     */
        } else {
            tap_clock_tms(1);   /* Update-DR → Select-DR（短路径）*/
        }
        tap_clock_tms(0);   /* Select-DR → Capture-DR（开始下一次扫描）*/
        tap_clock_tms(1);   /* Capture-DR→ Exit1-DR  （全程没进 Shift-DR）*/
        tap_clock_tms(0);   /* Exit1-DR  → Pause-DR  */
        return;
    }
    tap_clock_tms(1);       /* Pause-DR  → Exit2-DR  */
    tap_clock_tms(0);       /* Exit2-DR  → Shift-DR  */
    for (int i = 1; i < n; i++)
        tap_clock_tms(0);   /* 留在 Shift-DR：第 1..n-1 个时钟 */
    tap_clock_tms(1);       /* Shift-DR  → Exit1-DR ：第 n 个时钟 */
    tap_clock_tms(0);       /* Exit1-DR  → Pause-DR （还没结算）*/
}

/*
 * 开命令窗（TRM §6.2.2.1）：control level 设到 2 并锁定。
 * 「Opening the command window decouples the device TAP; the decoupling
 *   occurs when the second ZBS occurs.」
 */
static void cjtag_open_command_window(uint8_t inert_ir)
{
    /* 1. IR 扫惰性指令，停在 Pause-DR。TRM §6.2.1：BYPASS 或 IDCODE 都行，
     * "Normally bypass is used, because its value (all ones) is dictated by
     * the IEEE 1149.1 specification"——2 线模式下没有 TDI 线，移进去的必然
     * 是全 1，所以这里传什么其实只在 OScan1 之后才有意义。 */
    jtag_shift_ir(inert_ir, ICEPICK_IR_BITS);
    tap_goto(TAP_PAUSE_DR);      /* 经 Select-DR → Capture-DR → Exit1-DR */

    cjtag_dr_count(0);           /* 2. 第一次 ZBS → control level 1 */
    cjtag_dr_count(0);           /* 3. 第二次 ZBS → control level 2，器件 TAP 解耦 */
    cjtag_dr_count(1);           /* 4. 1 位 DR 扫描 → 锁定在 2 */
    cjtag_dr_count(0);           /*    过 Update 结算这次扫描 */
}

/*
 * 发一条 cJTAG 命令（TRM §6.2.1）：CP0 = opcode，CP1 = operand。
 * 返回时停在 Update-DR —— **命令正是在这一拍生效的**。STFMT 会在这里把整条
 * 链路的扫描格式换掉，所以最后半个扫描不能连着发，调用方要先改 s_fmt。
 */
/* CP1 是一段一段凑出来的（OpenOCD 的写法：2+2+2+2+1=9，中间只经 Pause-DR
 * 不经 Update，计数照样累加）还是一口气移完。默认一口气——两种都试给 diag。*/
static bool s_chunk_cp1 = false;

static void cjtag_command(int cp0, int cp1)
{
    cjtag_dr_count(cp0);
    cjtag_dr_count(0);      /* 过 Update 结算 CP0 */
    if (s_chunk_cp1) {
        int left = cp1;
        while (left >= 2) { cjtag_dr_count(2); left -= 2; }
        if (left) cjtag_dr_count(left);
    } else {
        cjtag_dr_count(cp1);
    }
    tap_clock_tms(1);       /* Pause-DR → Exit2-DR */
    tap_clock_tms(1);       /* Exit2-DR → Update-DR：结算 CP1，命令生效 */
}

/*
 * 关命令窗的三种办法（TRM §6.2.2.3：IR 扫描 / 进 TLR / ECL 命令）。
 *
 * 为什么要当成变量试：STFMT 之后链路已经是 OScan1，而我们必须先关掉命令窗
 * 才能对器件 TAP 做扫描（开着的时候器件 TAP 是**解耦**的）。如果"进 TLR"
 * 顺带把扫描格式也复位回 2 线，那 OScan1 在我们读之前就被撤销了，现象与
 * "命令根本没生效"一模一样，光看读数分不开。
 */
typedef enum {
    CLOSE_TLR = 0,    /* 5 拍 TMS=1 进 Test-Logic-Reset */
    CLOSE_IR,         /* 直接扫一条 IR（顺带把 IDCODE 装进 IR），不碰 TLR */
    CLOSE_ECL,        /* STMC 的 ECL 子命令（CP0=0, CP1=1），再进 TLR */
} cjtag_close_t;

typedef struct {
    uint8_t       inert_ir;    /* 开窗第 1 步的惰性指令：TRM 说 BYPASS 或 IDCODE 都行 */
    cjtag_close_t close;
    uint8_t       fmt_delay;   /* 切格式前多走几个旧格式的位（0..2） */
    bool          segger_wake; /* 先跑 SEGGER 的 escape + 长式激活 */
    bool          set_sedge;   /* STFMT 之前先发 STC1 operand=1（SEDGE=1） */
} cjtag_open_opt_t;

/* 切到 OScan1：TRM Table 6-4，STFMT(opcode 3) + operand 9。 */
static void cjtag_select_oscan1(const cjtag_open_opt_t *o)
{
    if (o->segger_wake) {
        cjtag_segger_wake();
        s_fmt = FMT_JSCAN2;
        s_tap = TAP_RTI;           /* SEGGER 序列最后停在 Run-Test/Idle */
    }
    cjtag_open_command_window(o->inert_ir);
    if (o->set_sedge) {
        /* STC1 operand 1 = SEDGE=1（上升沿采样 TMSC）。SEGGER 在 STFMT 之前
         * 显式设它——默认很可能是下降沿，那样 OScan1 每一位都采在错的沿上。 */
        cjtag_command(CJTAG_CMD_STC1, 1);
        s_tap = TAP_UPDATE_DR;
        tap_goto(TAP_PAUSE_DR);
    }
    cjtag_command(CJTAG_CMD_STFMT, CJTAG_FMT_CODE_OSCAN1);
    s_tap = TAP_UPDATE_DR;

    /* 命令在 CP1 的 Update 那一拍生效。器件到底是在那个上升沿换格式，还是
     * 要再走一拍，TRM 没写——多发几个旧格式的位当变体试。 */
    for (uint8_t i = 0; i < o->fmt_delay; i++) tap_clock_tms(0);

    s_fmt = FMT_OSCAN1;

    switch (o->close) {
    case CLOSE_IR:
        /* OScan1 下已经有 TDI 通路了，这条 IR 扫描既关窗又把 IDCODE 装进 IR，
         * 全程不碰 TLR——如果 TLR 会撤销格式，这条路就绕开了它。 */
        jtag_shift_ir(JTAG_IR_IDCODE, ICEPICK_IR_BITS);
        break;
    case CLOSE_ECL:
        /* §6.2.2.3：ECL = STMC(opcode 0) 的子命令，CP0=0 / CP1=1，起点 Pause-DR。 */
        tap_goto(TAP_PAUSE_DR);
        cjtag_command(CJTAG_CMD_STMC, 1);
        s_tap = TAP_UPDATE_DR;
        tap_reset();
        break;
    default:
        tap_reset();
        break;
    }
}

/* ── 公共 API ────────────────────────────────────────────────────── */

void cjtag_enter(void)
{
    pin_init();
    hw_reset_pulse();

    s_fmt = FMT_JSCAN2;     /* TRM §6.2：上电默认就是 2 线 JScan */
    s_tap = TAP_TLR;

    hw_wake_icemelter();    /* TRM §6.4：先把 JTAG 电源域唤醒 */
    tap_reset();
    const cjtag_open_opt_t def = { JTAG_IR_BYPASS, CLOSE_TLR, 0, true, true };
    cjtag_select_oscan1(&def);
}

void cjtag_exit(void)
{
    tap_reset();
    s_fmt = FMT_JSCAN2;
    /* RESET_N 脉冲既让 CC1312R 重启，也清掉 ICEMelter 置上的 Halt-In-Boot
     * 标志（TRM §6.4：只有 pin reset / POR / JTAG 能清）。 */
    hw_reset_pulse();
}

uint32_t cjtag_read_idcode(void)
{
    /* Test-Logic-Reset 会把 IDCODE 自动装进 IR —— 读 IDCODE 不需要知道
     * IR 宽度、也不需要知道 IDCODE 的指令码，是最短的存活证明。 */
    tap_reset();
    return jtag_shift_dr(0, 32);
}

/* ── 诊断 ────────────────────────────────────────────────────────── */

/*
 * 线探针：只读回 RP2040 自己这一侧的 TMSC，用来区分三种线状态——
 *   驱低→松手后爬回 1        = 线上有上拉
 *   上拉=1 而下拉=1          = 有人在驱动高（比 50k 内部拉强）
 *   上拉=1 下拉=0            = 没有别的驱动源，我们说了算
 *   两个方向松手都保持原值    = 纯浮空，读数只是残留电荷（此时「读到一串 0」
 *                              看起来很像目标在回 TDO，其实不携带任何信息）
 */
static void diag_probe_line(const char *when)
{
    pin_tmsc_hiz();
    pin_sleep_ms(1);
    int idle = pin_tmsc_read();

    pin_tmsc_drive(0); pin_delay_us(10); pin_tmsc_hiz();
    pin_delay_us(2);   int lo_us = pin_tmsc_read();
    pin_sleep_ms(1);   int lo_ms = pin_tmsc_read();

    pin_tmsc_pull(+1); pin_sleep_ms(1); int up = pin_tmsc_read();
    pin_tmsc_pull(-1); pin_sleep_ms(1); int dn = pin_tmsc_read();
    pin_tmsc_pull(+1);

    printf("  线探针 %-12s idle=%d 驱低→松:%d/%d 上拉=%d 下拉=%d\n",
           when, idle, lo_us, lo_ms, up, dn);
}

/* 全程松开 TMSC 只发 TCKC：目标若真的在驱动这根线，就会出现 0。 */
static void diag_listen(void)
{
    uint32_t hi = 0, lo = 0;
    pin_tmsc_hiz();
    for (int i = 0; i < 32; i++) {
        pin_tckc(1); pin_delay_half();
        if (pin_tmsc_read()) hi |= 1u << i;
        pin_tckc(0); pin_delay_half();
        if (pin_tmsc_read()) lo |= 1u << i;
    }
    printf("  只听不说   : TCKC高相=0x%08lX 低相=0x%08lX (全 1 = 没人驱动)\n",
           (unsigned long)hi, (unsigned long)lo);
}

/*
 * 拿交替的 TMS 跑 16 个 OScan1 包，比对「周期3 采到的」和「周期2 我们自己
 * 驱动的 TMS」。两者相等 = 目标压根没接管总线，读到的只是残留电荷。
 * 这一条是为了防止把「浮空线记住了上一次驱动值」误判成收到了 TDO。
 */
static void diag_tms_echo(void)
{
    uint32_t sent = 0, got = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t tms = (uint8_t)(i & 1);
        if (tms) sent |= 1u << i;
        if (hw_scan_bit(tms, 0)) got |= 1u << i;
    }
    printf("  TMS回声    : 驱动=0x%04X 采回=0x%04X%s\n",
           (unsigned)sent, (unsigned)got,
           (sent == got) ? "  ← 相等：读到的是自己的残留电荷" : "");
}

#ifndef CJTAG_HOST_TEST
/*
 * 连通性探针：拿 RP2040 自己的上/下拉当欧姆表，量每根线上「有没有别人」。
 *
 * 判据来自 TRM 自己的说法——CC13x2 在 TCK 上有内部上拉（§6.4：「The TCK pin
 * has an internal pullup designed to avoid unintentional traffic due to
 * noise」），RESET_N 与 TMSC 同理。所以：
 *   我们下拉时读到 1  = 线上有个强过 RP2040 内部 50k 的上拉 → 接到了东西
 *   我们下拉时读到 0  = 线上只有我们自己 → 这根线上没有别的上拉源
 * 这是量我们这一侧读到了什么，不是对硬件下结论。
 */
static void diag_probe_pin(const char *name, unsigned gpio)
{
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);   sleep_ms(1); int up = (int)gpio_get(gpio);
    gpio_pull_down(gpio); sleep_ms(1); int dn = (int)gpio_get(gpio);
    gpio_disable_pulls(gpio); sleep_ms(1); int no = (int)gpio_get(gpio);
    gpio_pull_up(gpio);
    printf("  引脚 %-10s (GPIO%-2u) 上拉=%d 下拉=%d 无拉=%d  %s\n",
           name, gpio, up, dn, no,
           (up && dn) ? "← 线上有外部上拉（接到了东西）"
                      : (!up && !dn) ? "← 线被按在低电平"
                                     : "← 只有我们自己在拉，线上没有别的上拉源");
}

/*
 * 推挽回读：把脚设成输出、推到某个电平，再从**同一个焊盘**读回实际线电平
 * （RP2040 的输入缓冲在输出模式下照样使能，gpio_get 读的是线上真实电平，
 * 不是输出寄存器）。
 *
 * 这是唯一一个不接仪器、也能回答「我们的翻转到底有没有出现在线上」的测法：
 *   推 0 读回 0、推 1 读回 1 → 这根线归我们控制，波形确实在动
 *   推 0 却读回 1           → 有比 RP2040 驱动更强的东西把线摁在高电平，
 *                             时钟根本没翻转，后面所有协议都无从谈起
 * TCKC 这一条尤其关键：整个 cJTAG 只有它是纯输出，此前无从证伪。
 */
static void diag_drive_readback(const char *name, unsigned gpio)
{
    gpio_disable_pulls(gpio);
    gpio_put(gpio, 0); gpio_set_dir(gpio, GPIO_OUT);
    sleep_us(50); int lo = (int)gpio_get(gpio);
    gpio_put(gpio, 1);
    sleep_us(50); int hi = (int)gpio_get(gpio);

    /* 再快速翻 100 次，统计回读是否每次都跟得上 —— 排除「静态推得动、
     * 250 kHz 下跟不上」这种容性/驱动能力问题。 */
    int mismatch = 0;
    for (int i = 0; i < 100; i++) {
        int want = i & 1;
        gpio_put(gpio, want);
        sleep_us(2);
        if ((int)gpio_get(gpio) != want) mismatch++;
    }
    gpio_put(gpio, (gpio == CJTAG_PIN_RESET) ? 1 : 0);

    printf("  推挽回读 %-8s (GPIO%-2u) 推0读回=%d 推1读回=%d 翻转100次失配=%d  %s\n",
           name, gpio, lo, hi, mismatch,
           (lo == 0 && hi == 1 && mismatch == 0)
               ? "← 线归我们控制，波形确实在动"
               : (lo != 0) ? "← 推不下去：线被更强的源摁在高电平"
                           : "← 跟不上翻转");
}

/*
 * 交叉驱动：把两根线推成**相反**电平，再各自回读。
 *
 * 为什么非做不可：CC1312R 的 JTAG_TMSC 是 pin 24、JTAG_TCKC 是 pin 25，
 * 在 0.5 mm 间距的 QFN-48 上**紧挨着**。这两脚之间一个锡桥，会造成：
 *   · 两根网络上都量得到片内上拉（连通性测试照常通过）
 *   · 复位时上拉一起消失（芯片"响应复位"的判据照常通过）
 *   · 推挽回读单根测也照常通过（一根一根测时另一根跟着走，看不出来）
 *   · 而器件看到的 TMS/TCK 是同一个信号 → TAP 永远走不对，什么都不应答
 * 也就是说我之前所有的"线是好的"判据，**对这一种故障全部免疫**。
 * 只有同时把两根推成相反电平才暴露得出来。
 */
static void diag_cross_drive(void)
{
    const unsigned a = CJTAG_PIN_TMSC, b = CJTAG_PIN_TCKC;
    int bad = 0;

    for (int phase = 0; phase < 2; phase++) {
        const int va = phase ? 0 : 1, vb = phase ? 1 : 0;
        gpio_disable_pulls(a); gpio_disable_pulls(b);
        gpio_put(a, va); gpio_set_dir(a, GPIO_OUT);
        gpio_put(b, vb); gpio_set_dir(b, GPIO_OUT);
        sleep_us(200);
        const int ra = (int)gpio_get(a), rb = (int)gpio_get(b);
        printf("  交叉驱动: TMSC推%d读%d  TCKC推%d读%d%s\n",
               va, ra, vb, rb,
               (ra != va || rb != vb) ? "   ← 推不动，两脚疑似短接" : "");
        if (ra != va || rb != vb) bad++;
    }

    /* 再来一次更狠的：只推一根，另一根松手接收。没短接的话，松手那根
     * 应当停在自己的上拉（1），而不是跟着被推的那根走。 */
    for (int phase = 0; phase < 2; phase++) {
        const int drv = phase ? 1 : 0;
        gpio_put(a, drv); gpio_set_dir(a, GPIO_OUT); gpio_disable_pulls(a);
        gpio_set_dir(b, GPIO_IN); gpio_pull_up(b);
        sleep_us(500);
        const int rb = (int)gpio_get(b);
        printf("  只推 TMSC=%d，TCKC 松手上拉 → 读到 %d%s\n", drv, rb,
               (drv == 0 && rb == 0) ? "   ← 被 TMSC 拽下去了，两脚短接" : "");
        if (drv == 0 && rb == 0) bad++;
    }

    gpio_set_dir(a, GPIO_IN);  gpio_pull_up(a);
    gpio_set_dir(b, GPIO_OUT); gpio_put(b, 0);
    printf("  交叉驱动结论: %s\n",
           bad ? "两脚之间存在低阻通路（锡桥/短路）"
               : "两脚彼此独立，可以各推各的");
}

static void diag_probe_all_pins(const char *when)
{
    printf("  [%s]\n", when);
    diag_probe_pin("TMSC", CJTAG_PIN_TMSC);
    diag_probe_pin("TCKC", CJTAG_PIN_TCKC);
    diag_probe_pin("RESET_N", CJTAG_PIN_RESET);
    diag_drive_readback("TMSC", CJTAG_PIN_TMSC);
    diag_drive_readback("TCKC", CJTAG_PIN_TCKC);
    diag_cross_drive();
    /* 量完把方向恢复成本模块的常态 */
    gpio_set_dir(CJTAG_PIN_TCKC, GPIO_OUT);
    gpio_put(CJTAG_PIN_TCKC, 0);
    gpio_set_dir(CJTAG_PIN_RESET, GPIO_OUT);
    gpio_put(CJTAG_PIN_RESET, 1);
}
#else
static void diag_probe_all_pins(const char *when) { (void)when; }
#endif

/*
 * 把 TMSC/TCKC/RESET_N 全部置高阻，让外部仿真器接管这三根网络。
 *
 * 本板没有给 SUBG_TMSC/TCKC 留测试点（V4 把 TP1–7 都删了），但这三根网络是
 * **和 RP2040 的 GPIO16/17/18 共用**的。所以外接 J-Link/XDS110 不必另找落点，
 * 焊到 RP2040 那一侧的脚（或网络上的过孔）即可——前提是 RP2040 得先松手，
 * 否则它的推挽输出会和仿真器对顶。
 *
 * RESET_N 也要松：板上 R47 10k 上拉会把它保持在高，仿真器需要时自己拉低。
 * 同时必须暂停 SPI master——它在 RECOVERY 里会周期性拉低 GPIO18 复位 CC1312R，
 * 正好会打断仿真器的会话。
 */
void cjtag_release_bus(void)
{
    pin_init();
    pin_tmsc_hiz();
#ifndef CJTAG_HOST_TEST
    gpio_set_dir(CJTAG_PIN_TCKC, GPIO_IN);
    gpio_disable_pulls(CJTAG_PIN_TCKC);
    gpio_set_dir(CJTAG_PIN_RESET, GPIO_IN);
    gpio_disable_pulls(CJTAG_PIN_RESET);
    gpio_disable_pulls(CJTAG_PIN_TMSC);
#endif
    s_bus_released = true;
    printf("cJTAG 总线已释放：GPIO16/17/18 全部高阻，SPI master 已暂停。\n"
           "外部仿真器可以接管 SUBG_TMSC/TCKC/RESET_N 了（焊 RP2040 侧引脚即可）。\n"
           "恢复请复位 RP2040。\n");
}

void cjtag_diag(void)
{
    pin_init();
    s_tdo_settle_us = 0;

    printf("cjtag-diag: TRM SWCU185G §6.2/§6.4 路径\n");
    diag_probe_all_pins("三根线的连通性");
    diag_probe_line("接管GPIO后");

    /* 按住 RESET_N 时目标的引脚行为应当变化——这是「目标确实在听这根线」
     * 的一个可观察量。 */
    pin_reset(0); pin_sleep_ms(5);
    diag_probe_line("RESET拉低中");
    pin_reset(1); pin_sleep_ms(20);

    hw_reset_pulse();
    s_fmt = FMT_JSCAN2;
    s_tap = TAP_TLR;
    diag_probe_line("复位后");

    hw_wake_icemelter();
    diag_probe_line("ICEMelter唤醒后");

    tap_reset();
    const cjtag_open_opt_t probe_opt = { JTAG_IR_BYPASS, CLOSE_TLR, 0, true, true };
    cjtag_select_oscan1(&probe_opt);
    diag_probe_line("切OScan1后");

    diag_listen();
    tap_reset();
    diag_tms_echo();

    /*
     * 变体矩阵。每一维都对应一条"TRM 没写死、只能试"的分歧，不是瞎撞：
     *   close  —— §6.2.2.3 给了三种关命令窗的办法。STFMT 之后必须先关窗才能
     *             扫器件 TAP；要是"进 TLR"顺带把扫描格式也复位回 2 线，
     *             OScan1 就在我们读之前被撤销了，现象和"命令没生效"一样。
     *   delay  —— 命令在 CP1 的 Update 那一拍生效，器件是当拍换格式还是再走
     *             一拍，手册没写。
     *   inert  —— §6.2.1 说惰性指令 BYPASS 或 IDCODE 都行。
     *   slow   —— 把 TCK 从 250 kHz 放慢到 25 kHz，排除沿太快没采到。
     */
    static const char *k_close_name[] = { "TLR", "IR", "ECL" };
    for (int v = 0; v < 24; v++) {
        cjtag_open_opt_t o;
        o.segger_wake = (v & 1) != 0;      /* SEGGER escape + 长式激活 */
        o.set_sedge   = (v & 2) != 0;      /* STC1 SEDGE=1 */
        o.close       = (cjtag_close_t)((v / 4) % 3);
        o.fmt_delay   = 0;
        o.inert_ir    = JTAG_IR_BYPASS;
        const int slow = (v / 12) % 2;
        /* SEGGER 说 cJTAG 低于 500 kHz 会被它强制提到 500 kHz（KEEPER 逻辑
         * 缺陷的绕法依赖高速）。所以"更慢更保险"在 cJTAG 上不成立。 */
        s_tck_half_us = slow ? 5u : 1u;    /* ~100 kHz / ~500 kHz */

        hw_reset_pulse();
        s_fmt = FMT_JSCAN2;
        s_tap = TAP_TLR;
        hw_wake_icemelter();
        tap_reset();
        cjtag_select_oscan1(&o);
        /* CLOSE_IR 那条已经把 IDCODE 装进 IR 了，再做 TLR 反而多此一举；
         * 其余两条靠 cjtag_read_idcode() 自带的 TLR。 */
        uint32_t id = (o.close == CLOSE_IR) ? jtag_shift_dr(0, 32)
                                            : cjtag_read_idcode();
        printf("  wake=%d sedge=%d close=%-3s %-7s → IDCODE=0x%08lX %s\n",
               o.segger_wake, o.set_sedge, k_close_name[o.close],
               slow ? "100kHz" : "500kHz", (unsigned long)id,
               ((id & CC13_IDCODE_MASK) == (CC13_JRC_IDCODE & CC13_IDCODE_MASK))
                   ? "← 对上 ICEPick JRC" : "");
    }
    s_tck_half_us = 2u;

    hw_reset_pulse();
    s_fmt = FMT_JSCAN2;
}

#ifdef CJTAG_HOST_TEST
/* 测试钩子（只在 host 构建存在）：让 test_cjtag.c 能直接驱动 IR/DR 移位和
 * TAP 导航，从而对「IR 宽度」「从 PAUSE_DR 出发能不能走对」单独下断言。*/
void     cjtag_test_shift_ir(uint32_t instr, int bits) { jtag_shift_ir(instr, bits); }
uint32_t cjtag_test_shift_dr(uint32_t tdi, int bits)   { return jtag_shift_dr(tdi, bits); }
void     cjtag_test_goto_pause_dr(void)                { tap_goto(TAP_PAUSE_DR); }
/* 只开窗 + 发一条命令，不切格式。用来把本实现发出的 TMS 序列与 OpenOCD
 * 那段实测可用的 ti_cjtag_to_4pin_jtag 逐拍对拍（见 test_cjtag.c）。 */
void     cjtag_test_open_and_cmd(uint8_t inert, int cp0, int cp1)
{
    s_fmt = FMT_JSCAN2;      /* 从上电默认态出发，与器件复位后一致 */
    s_tap = TAP_TLR;
    tap_reset();
    cjtag_open_command_window(inert);
    cjtag_command(cp0, cp1);
}
int      cjtag_test_tap_state(void)                    { return (int)s_tap; }
#endif

/* ── ADIv5 AP/DP 操作 ────────────────────────────────────────────── */

/* ADIv5 DPACC/APACC 的 35-bit DR：[34:3]=数据，[2:1]=寄存器地址 A[3:2]，
 * [0]=RnW。A[3:2] 取**字节地址**的 bit[3:2]（OpenOCD adi_v5_jtag.c：
 * ((reg_addr>>1)&0x6)|rnw）。 */
static void jtag_dp_write(uint8_t reg, uint32_t data)
{
    uint64_t dr = ((uint64_t)data << 3) | (uint64_t)(((reg >> 1) & 0x6) | 0);
    jtag_shift_ir(JTAG_IR_DPACC, DAP_IR_BITS);
    jtag_shift_dr((uint32_t)dr, 35);  /* 低 35 位 */
}

static MAYBE_UNUSED uint32_t jtag_dp_read(uint8_t reg)
{
    uint64_t dr = ((uint64_t)0 << 3) | (uint64_t)(((reg >> 1) & 0x6) | 1);
    jtag_shift_ir(JTAG_IR_DPACC, DAP_IR_BITS);
    jtag_shift_dr((uint32_t)dr, 35);
    /* 读结果要读 DP_RDBUFF（RnW=1），不是 DR=0 */
    jtag_shift_ir(JTAG_IR_DPACC, DAP_IR_BITS);
    uint32_t result = jtag_shift_dr((uint32_t)(((DP_RDBUFF >> 1) & 0x6) | 1), 35);
    return result;  /* 高 32 位是数据 */
}

static void jtag_ap_write(uint8_t ap, uint8_t reg, uint32_t data)
{
    /* 先选 AP */
    jtag_dp_write(DP_SELECT, (uint32_t)(ap << 24) | (reg & 0xF0));
    /* 再写 AP 寄存器 */
    uint64_t dr = ((uint64_t)data << 3) | (uint64_t)(((reg >> 1) & 0x6) | 0);
    jtag_shift_ir(JTAG_IR_APACC, DAP_IR_BITS);
    jtag_shift_dr((uint32_t)dr, 35);
}

static uint32_t jtag_ap_read(uint8_t ap, uint8_t reg)
{
    jtag_dp_write(DP_SELECT, (uint32_t)(ap << 24) | (reg & 0xF0));
    uint64_t dr = ((uint64_t)0 << 3) | (uint64_t)(((reg >> 1) & 0x6) | 1);
    jtag_shift_ir(JTAG_IR_APACC, DAP_IR_BITS);
    jtag_shift_dr((uint32_t)dr, 35);
    /* 结果要读 DP_RDBUFF */
    jtag_shift_ir(JTAG_IR_DPACC, DAP_IR_BITS);
    uint32_t result = jtag_shift_dr((uint32_t)(((DP_RDBUFF >> 1) & 0x6) | 1), 35);
    return result;
}

/* AHB-AP（MEM-AP）读写 */
uint32_t cjtag_ahb_read32(uint32_t addr)
{
    /* 设置 TAR */
    jtag_ap_write(0, AP_TAR, addr);
    /* 读 DRW */
    return jtag_ap_read(0, AP_DRW);
}

void cjtag_ahb_write32(uint32_t addr, uint32_t val)
{
    jtag_ap_write(0, AP_TAR, addr);
    jtag_ap_write(0, AP_DRW, val);
}

/* ── Flash 操作 ─────────────────────────────────────────────────── */

/* ⚠ 未决（读通 IDCODE 之后的下一个阻塞点，不要当成已验证的路径）：
 * 1) 上面的 DPACC/APACC 直接发 4 位 DAP IR，但 CC13x2 的 Cortex-M DAP TAP
 *    **默认不在扫描链上**（TRM §6.3「None of the secondary TAPs are selected
 *    or visible in the master scan path」；OpenOCD cc26x0.cfg 也把 cpu TAP
 *    标成 -disable）。必须先经 ICEPick 的 CONNECT(IR=0x07, DR8=0x89) +
 *    ROUTER(IR=0x02, DR32) 把它挂上来，见 tcl/target/ti/icepick.cfg。
 * 2) 下面这套 FLASH_FMC/FADDR/FSTAT 寄存器直写是 Stellaris/CC2538 的 flash
 *    控制器模型。OpenOCD 给 CC13x2 用的是 `flash bank ... cc26xx`，走的是
 *    装进 SRAM 的 flash loader（contrib/loaders/flash/cc26xx），不是寄存器
 *    直写。两条都要在通 IDCODE 之后重做，现状仅保留骨架。 */

/* 等待 flash controller 空闲。位脉冲下每次 AHB 读要几十个 TCKC，用「读次数」
 * 当时间上界（≈ timeout_ms）。BUSY 恒不清（目标被复位/链路坏）时返回 false，
 * 不让 core0 永久自旋——项目未启用看门狗，死循环只能断电恢复。 */
static bool flash_wait_ready(int timeout_ms)
{
    if (timeout_ms < 1) timeout_ms = 1;
    int budget = timeout_ms * 8 + 16;
    uint32_t stat;
    do {
        stat = cjtag_ahb_read32(FLASH_FSTAT);
        if (!(stat & FSTAT_BUSY)) return true;
    } while (--budget > 0);
    return false;
}

bool cjtag_flash_erase_sector(uint32_t addr)
{
    cjtag_ahb_write32(FLASH_FADDR, addr);
    cjtag_ahb_write32(FLASH_FMC, FMC_ERASE_SECTOR);
    return flash_wait_ready(2000);
}

bool cjtag_flash_write_word(uint32_t addr, uint32_t val)
{
    cjtag_ahb_write32(FLASH_FADDR, addr);
    cjtag_ahb_write32(FLASH_FDATA0, val);
    cjtag_ahb_write32(FLASH_FMC, FMC_WRD);
    return flash_wait_ready(100);
}

bool cjtag_flash_verify(uint32_t addr, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t expect = 0xFFFFFFFFu;   /* 未写的高字节 = flash 擦除态 */
        memcpy(&expect, data + i, (len - i >= 4) ? 4 : (len - i));
        uint32_t got = cjtag_ahb_read32(addr + i);
        if (got != expect) return false;
    }
    return true;
}

bool cjtag_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    /* 先解锁 flash（CC13x2 默认锁定） */
    /* 写 FCFG1 的 FLASH_UNLOCK（地址 0x5000130C 写 0xC35A01E2） */
    cjtag_ahb_write32(0x5000130C, 0xC35A01E2);

    /* 使能 DAP 电源 */
    jtag_dp_write(DP_CTRLSTAT, DP_CTRL_CSYSPWRUP | DP_CTRL_CDBGPWRUP);

    /* 擦除涉及的 sectors */
    uint32_t end = addr + len;
    for (uint32_t s = addr & ~(uint32_t)(CC13_SECTOR_SIZE - 1);
         s < end; s += CC13_SECTOR_SIZE) {
        if (!cjtag_flash_erase_sector(s)) return false;
    }

    /* 逐 word 写入 */
    for (size_t i = 0; i < len; i += 4) {
        uint32_t word;
        memcpy(&word, data + i, (len - i >= 4) ? 4 : (len - i));
        if (!cjtag_flash_write_word(addr + i, word)) return false;
    }

    /* 校验 */
    if (!cjtag_flash_verify(addr, data, len)) return false;

    /* 锁回 flash */
    cjtag_ahb_write32(0x5000130C, 0xC35A01E3);

    return true;
}

/* ── CDC 接口 ────────────────────────────────────────────────────── */

static bool     s_active = false;
static uint8_t  s_buf[CC13_SECTOR_SIZE];
static size_t   s_buf_len = 0;
static uint32_t s_flash_addr = 0;   /* 当前 sector 的 flash 地址（镜像从 0 起） */
static uint32_t s_total = 0;        /* 镜像总长（4 字节小端头） */
static uint32_t s_received = 0;     /* 已接收的数据字节数 */
static uint8_t  s_hdr[4];
static uint8_t  s_hdr_len = 0;
static bool     s_hdr_done = false;
static bool     s_failed = false;   /* 某 sector 失败后进入「吞字节」态 */
static uint32_t s_idcode = 0;       /* cjtag_cdc_enter 读到的 IDCODE（供打印） */

/* 解锁 flash + 使能 DAP 电源（整个烧录期间保持；见 cjtag_flash_program）。 */
static void flash_unlock(void)
{
    /* 顺序：先给调试域上电，再配 MEM-AP CSW（Size=32bit, AddrInc=single），
     * 最后才发 AHB 事务——反了首次访问会 fault/尺寸错。 */
    jtag_dp_write(DP_CTRLSTAT, DP_CTRL_CSYSPWRUP | DP_CTRL_CDBGPWRUP);
    jtag_ap_write(0, AP_CSW, 0x23000052u);   /* Size=0b010, AddrInc=0b01 */
    cjtag_ahb_write32(0x5000130C, 0xC35A01E2);
}
static void flash_lock(void)
{
    cjtag_ahb_write32(0x5000130C, 0xC35A01E3);
}

uint32_t cjtag_cdc_idcode(void)
{
    return s_idcode;
}

bool cjtag_cdc_enter(void)
{
    if (s_active) return true;
    cjtag_enter();

    /* Proof of life：读 IDCODE。只认版本号以外的 28 位（不同批次版本号会变，
     * OpenOCD 对这个 TAP 也是 -ignore-version）。 */
    s_idcode = cjtag_read_idcode();
    if ((s_idcode & CC13_IDCODE_MASK) != (CC13_JRC_IDCODE & CC13_IDCODE_MASK)) {
        cjtag_exit();
        return false;
    }

    flash_unlock();
    s_active = true;
    s_buf_len = 0; s_flash_addr = 0; s_total = 0; s_received = 0;
    s_hdr_len = 0; s_hdr_done = false; s_failed = false;
    return true;
}

/* 把当前 4KB 缓冲擦→写→校验进 s_flash_addr，然后前进一个 sector。 */
static bool flash_sector_flush(void)
{
    if (s_buf_len == 0) return true;
    if (!cjtag_flash_erase_sector(s_flash_addr)) return false;
    for (size_t i = 0; i < s_buf_len; i += 4) {
        uint32_t word = 0xFFFFFFFFu;
        size_t n = (s_buf_len - i >= 4) ? 4 : (s_buf_len - i);
        memcpy(&word, s_buf + i, n);
        if (!cjtag_flash_write_word(s_flash_addr + i, word)) return false;
    }
    if (!cjtag_flash_verify(s_flash_addr, s_buf, s_buf_len)) return false;
    s_flash_addr += CC13_SECTOR_SIZE;
    s_buf_len = 0;
    return true;
}

/* 中止：锁 flash、复位 CC1312、退出。 */
void cjtag_cdc_quit(void)
{
    if (!s_active) return;
    flash_lock();
    cjtag_exit();
    s_active = false;
    printf("FLASH-ABORT\n");
}

/*
 * 方案 A 流式接收：先 4 字节小端长度（镜像字节数），再是镜像本体。
 * 边收边按 4KB 擦写校验；收满 s_total 即完成（无需终止符，避免镜像里
 * 的 'Q'(0x51) 被当结束符截断）。任一 sector 失败即中止。
 */
bool cjtag_cdc_data(uint8_t byte)
{
    if (!s_active) return false;

    if (!s_hdr_done) {
        s_hdr[s_hdr_len++] = byte;
        if (s_hdr_len < 4) return true;
        s_total = (uint32_t)s_hdr[0] | ((uint32_t)s_hdr[1] << 8) |
                  ((uint32_t)s_hdr[2] << 16) | ((uint32_t)s_hdr[3] << 24);
        s_hdr_done = true;
        if (s_total == 0 || s_total > CC13_FLASH_SIZE) {
            printf("FLASH-FAIL: bad length %lu (max %u)\n",
                   (unsigned long)s_total, (unsigned)CC13_FLASH_SIZE);
            cjtag_cdc_quit();
            return false;
        }
        printf("FLASH-RECV %lu bytes...\n", (unsigned long)s_total);
        return true;
    }

    s_received++;
    if (s_failed) {
        /* 已失败：继续吞掉剩余字节直到收满长度，避免它们被调用方当命令
         * 解析（镜像含 'B' 会让 RP2040 进 BOOTSEL）。 */
        if (s_received >= s_total) cjtag_cdc_quit();
        return false;
    }

    s_buf[s_buf_len++] = byte;

    if (s_buf_len == sizeof(s_buf)) {
        if (!flash_sector_flush()) {
            printf("FLASH-FAIL: sector @0x%05lX\n", (unsigned long)s_flash_addr);
            s_failed = true;
            s_buf_len = 0;                 /* 丢弃，继续吞剩余字节 */
        }
    }

    if (s_received >= s_total) {
        if (!s_failed && !flash_sector_flush()) {
            printf("FLASH-FAIL: final sector @0x%05lX\n",
                   (unsigned long)s_flash_addr);
            s_failed = true;
        }
        if (s_failed) {
            cjtag_cdc_quit();
            return false;
        }
        flash_lock();
        cjtag_exit();                 /* 复位 CC1312，跑新镜像 */
        s_active = false;
        printf("FLASH-DONE %lu bytes\n", (unsigned long)s_total);
    }
    return true;
}

bool cjtag_cdc_active(void)
{
    /* 总线释放后也算"占用中"，让 core0 一直跳过 spim_poll。 */
    return s_active || s_bus_released;
}
