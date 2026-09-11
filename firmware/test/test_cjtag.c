/*
 * test_cjtag.c — 拿一个 CC13x2 cJTAG 器件模型验 RP2040 侧的位时序。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 -DCJTAG_HOST_TEST \
 *      -o /tmp/test_cjtag firmware/test/test_cjtag.c firmware/rp2040/cjtag.c \
 *   && /tmp/test_cjtag
 *
 * 为什么要写这个模型
 * ------------------
 * 旧版 test_cjtag.c 是纯逻辑桩：激活序列在 host 构建里被整个 stub 掉，
 * TDO 直接吐预置的 IDCODE。于是一批致命的位级 bug 在 71/71 全绿下活了很久。
 * 现在 cjtag.c 里只有 pin_* 那几个原语是两边不同的，所有时序都是共享代码，
 * 本文件在这些原语下面接一个按 TRM 写的器件模型。
 *
 * 模型依据：CC13x2/CC26x2 TRM **SWCU185G** 第 6 章
 *   §6.4    ICEMelter：JTAG 电源域默认断电，TCK 上 8 个上升沿 + 8 个下降沿
 *           才唤醒。没唤醒之前整块逻辑不响应任何东西。
 *   §6.2    上电默认 2 线 JScan：TMSC 就是 TMS，没有 TDI/TDO 通路。
 *   §6.2.1  cJTAG 命令由「benign JTAG scan activity」承载：命令值 = 该次 DR
 *           扫描在 Shift-DR 里停留的时钟数；CP0 = opcode，CP1 = operand。
 *   §6.2.2.1 两次 ZBS 把 control level 抬到 2，首次进 Shift-DR 锁定。
 *   Table 6-4 STFMT(opcode 3) + operand 9 = OSCAN1。
 *   Table 6-3 OScan1 包 = nTDI / TMS / TDO 三个 TCKC 周期。
 *   ICEPick irlen=6 / JRC_TAPID —— OpenOCD tcl/target/ti/cc26x0.cfg、cc13x2.cfg
 */
#include "cjtag.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define CHECK_EQ32(got, want) do { \
    uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { \
        fprintf(stderr, "FAIL %s:%d: %s = 0x%08X, 期望 0x%08X\n", \
                __FILE__, __LINE__, #got, g_, w_); \
        g_fail++; \
    } \
} while (0)

/* ══ 器件模型 ═════════════════════════════════════════════════════ */

/* ── 1149.1 TAP（ICEPick JRC 的简化模型）───────────────────────── */

enum {
    S_TLR = 0, S_RTI,
    S_SEL_DR, S_CAP_DR, S_SHIFT_DR, S_EX1_DR, S_PAUSE_DR, S_EX2_DR, S_UPD_DR,
    S_SEL_IR, S_CAP_IR, S_SHIFT_IR, S_EX1_IR, S_PAUSE_IR, S_EX2_IR, S_UPD_IR,
};

static const uint8_t k_next[16][2] = {
    /* TMS=0        TMS=1 */
    { S_RTI,        S_TLR      },   /* TLR       */
    { S_RTI,        S_SEL_DR   },   /* RTI       */
    { S_CAP_DR,     S_SEL_IR   },   /* SELECT_DR */
    { S_SHIFT_DR,   S_EX1_DR   },   /* CAPTURE_DR*/
    { S_SHIFT_DR,   S_EX1_DR   },   /* SHIFT_DR  */
    { S_PAUSE_DR,   S_UPD_DR   },   /* EXIT1_DR  */
    { S_PAUSE_DR,   S_EX2_DR   },   /* PAUSE_DR  */
    { S_SHIFT_DR,   S_UPD_DR   },   /* EXIT2_DR  */
    { S_RTI,        S_SEL_DR   },   /* UPDATE_DR */
    { S_CAP_IR,     S_TLR      },   /* SELECT_IR */
    { S_SHIFT_IR,   S_EX1_IR   },   /* CAPTURE_IR*/
    { S_SHIFT_IR,   S_EX1_IR   },   /* SHIFT_IR  */
    { S_PAUSE_IR,   S_UPD_IR   },   /* EXIT1_IR  */
    { S_PAUSE_IR,   S_EX2_IR   },   /* PAUSE_IR  */
    { S_SHIFT_IR,   S_UPD_IR   },   /* EXIT2_IR  */
    { S_RTI,        S_SEL_DR   },   /* UPDATE_IR */
};

static uint8_t  t_state;
static uint32_t t_ir;          /* 已 UPDATE 生效的指令 */
static uint32_t t_ir_shift;    /* IR 移位寄存器 */
static uint64_t t_dr;          /* DR 移位寄存器 */
static int      t_dr_len;

/* ── cJTAG 命令窗（TRM §6.2.1 / §6.2.2.1）──────────────────────── */

static int cw_level;        /* control level：每次 ZBS +1 */
static int cw_locked;       /* 首次进 Shift-DR 即锁定（锁在当时的 level 上） */
static int cw_cmd_ok;       /* 锁定时 level 是否达到 2 —— 没到 2 就不收命令 */
static int cw_lock_scan;    /* 锁定那次扫描的 Update 要丢弃，不算命令 */
static int cw_shift_clk;    /* 本次 DR 扫描在 Shift-DR 里的时钟数 */
static int cw_cp0;          /* -1 = 还没收到 CP0 */

/* 每次 DR 扫描在 Shift-DR 里待了几个时钟，按 Update 结算依次记下来。
 * 这串数就是 cJTAG 命令的全部内容（TRM §6.2.1：命令的值 = 时钟数），
 * 所以拿它跟参考实现对拍，比对 TMS 原始位更直指要害。 */
static int g_scan_clk[32];
static int g_scan_n;

/* ── 链路层 ─────────────────────────────────────────────────────── */

enum { FMT_JSCAN2 = 0, FMT_OSCAN1 };

static int  m_fmt;
static long m_now_us;       /* 虚拟时间：由 cjtag_model_delay_us 累加 */
static long m_wake_us;      /* ICEMelter 数够 8+8 个沿的时刻，-1 = 还没数够 */
static int  m_tck_rise, m_tck_fall;
static int m_tckc;
static int m_host;          /* 主机驱动值，-1 = 松手 */
static int m_dev;           /* 器件驱动值，-1 = 不驱动 */
static int m_phase;         /* OScan1 包内相位 0=nTDI 1=TMS 2=TDO，-1=待对齐 */
static int m_tdi, m_tms;
static int m_contention;    /* 器件驱动 TMSC 期间主机还在推挽输出的次数 */

/* 线上的实际电平：谁驱动听谁的，都松手就是上拉 1。 */
static int line(void)
{
    if (m_host >= 0) return m_host;
    if (m_dev  >= 0) return m_dev;
    return 1;
}

static void cw_reset(void)
{
    cw_level = 0; cw_locked = 0; cw_cmd_ok = 0; cw_lock_scan = 0;
    cw_shift_clk = 0; cw_cp0 = -1;
}

static void tap_enter_tlr(void)
{
    t_state = S_TLR;
    t_ir    = JTAG_IR_IDCODE;   /* 1149.1：TLR 自动把 IDCODE 装进 IR */
    cw_reset();                 /* TRM §6.2.2.3：进 TLR 会关掉命令窗 */
}

/* 当前应当呈现在 TDO 上的位（移位前的值——4 线 JTAG 里主机就是在同一个
 * 上升沿采 TDO / 送 TDI 的）。 */
static int tap_tdo(void)
{
    if (t_state == S_SHIFT_DR) return (int)(t_dr & 1u);
    if (t_state == S_SHIFT_IR) return (int)(t_ir_shift & 1u);
    return 0;
}

/* 命令窗收齐一对 CP0/CP1 时执行（TRM Table 6-4，本模型只实现 STFMT）。 */
static void cw_execute(int cp0, int cp1)
{
    if (cp0 == CJTAG_CMD_STFMT && cp1 == CJTAG_FMT_CODE_OSCAN1) {
        m_fmt   = FMT_OSCAN1;
        /* -1 = 「还在当前这一位的尾巴上」：格式是在 Update-DR 的上升沿换的，
         * 紧跟的那个下降沿仍属于旧格式的这一位，不能算作 OScan1 的相位。 */
        m_phase = -1;
        m_dev   = -1;
    }
}

static void tap_clock(int tms, int tdi)
{
    /* 先在当前状态做移位，再转移——和真 TAP 一致。
     * 命令窗一旦开到 level≥2，器件 TAP 就被解耦（TRM §6.2.2.1），
     * 这段时间的 DR 扫描只喂 cJTAG 命令模块，不动器件的 DR。 */
    int decoupled = (cw_level >= 2);

    if (t_state == S_SHIFT_DR) {
        cw_shift_clk++;
        if (!decoupled && t_dr_len > 0)
            t_dr = (t_dr >> 1) | ((uint64_t)(tdi & 1) << (t_dr_len - 1));
    } else if (t_state == S_SHIFT_IR) {
        t_ir_shift = (t_ir_shift >> 1) |
                     ((uint32_t)(tdi & 1) << (ICEPICK_IR_BITS - 1));
    }

    uint8_t next = k_next[t_state][tms & 1];

    switch (next) {
    case S_TLR:
        tap_enter_tlr();
        return;
    case S_CAP_IR:
        t_ir_shift = 0x01;                       /* ircapture 0x1 */
        cw_reset();                              /* IR 扫描关命令窗 */
        break;
    case S_CAP_DR:
        cw_shift_clk = 0;                        /* 新的一次 DR 扫描 */
        if (!decoupled) {
            if (t_ir == JTAG_IR_IDCODE) { t_dr = CC13_JRC_IDCODE; t_dr_len = 32; }
            else                        { t_dr = 0;               t_dr_len = 1;  }
        }
        break;
    case S_SHIFT_DR:
        /* TRM §6.2.1：「The control level is locked when the first Shift DR
         * state occurs.」—— 锁在当时到达的 level 上。§6.2.2.1 要求「the
         * control level must be set to 2 and locked」，锁在 1 就不收命令。 */
        if (!cw_locked) {
            cw_locked = 1;
            cw_lock_scan = 1;
            cw_cmd_ok = (cw_level >= 2);
        }
        break;
    case S_UPD_DR:
        if (g_scan_n < (int)(sizeof g_scan_clk / sizeof g_scan_clk[0]))
            g_scan_clk[g_scan_n++] = cw_shift_clk;   /* 对拍用：逐次扫描的计数 */
        if (!cw_locked) {
            if (cw_shift_clk == 0) cw_level++;   /* ZBS */
        } else if (!cw_cmd_ok) {
            /* 锁定时没到 control level 2，命令窗没开，扫描不被当成命令。 */
        } else if (cw_lock_scan) {
            cw_lock_scan = 0;                    /* 锁定扫描本身不是命令 */
        } else if (cw_cp0 < 0) {
            cw_cp0 = cw_shift_clk;
        } else {
            cw_execute(cw_cp0, cw_shift_clk);
            cw_cp0 = -1;
        }
        break;
    case S_UPD_IR:
        t_ir = t_ir_shift & ((1u << ICEPICK_IR_BITS) - 1u);
        break;
    default:
        break;
    }
    t_state = next;
}

/* TRM §6.4：数够 8 个上升沿 + 8 个下降沿只是发出上电请求，之后「the emulator
 * must allow power-up time of at least 200 µs for JTAG power domain before
 * sending remaining commands」。没等够，命令是打在还没起来的逻辑上。 */
static int jtag_ready(void)
{
    return m_wake_us >= 0 && (m_now_us - m_wake_us) >= 200;
}

static void on_tckc_rise(void)
{
    if (!jtag_ready()) return;

    if (m_fmt == FMT_JSCAN2) {
        /* 2 线 JScan：TMSC 就是 TMS，没有 TDI 线，器件按全 1 收
         * （所以 TRM 让你用全 1 的 BYPASS 当惰性指令）。 */
        tap_clock(line(), 1);
        return;
    }
    if (m_phase == 0)      m_tdi = !line();   /* 周期1 送的是 ~TDI */
    else if (m_phase == 1) m_tms = line();
}

static void on_tckc_fall(void)
{
    if (!jtag_ready() || m_fmt != FMT_OSCAN1) return;

    if (m_phase < 0) {          /* 换格式那一位的尾巴，不算 OScan1 相位 */
        m_phase = 0;
        return;
    }
    if (m_phase == 0) {
        m_phase = 1;
    } else if (m_phase == 1) {
        m_phase = 2;
        m_dev = tap_tdo();      /* 从这个下降沿起由器件驱动 TMSC */
        /* 主机必须在这个下降沿之前就松手。没松手 = 两个推挽驱动对顶，
         * 线上电平由驱动能力决定，模型判不出谁赢——直接当违规计数。 */
        if (m_host >= 0) m_contention++;
    } else {
        m_phase = 0;
        m_dev = -1;
        tap_clock(m_tms, m_tdi);
    }
}

/* ── 给 cjtag.c 的引脚原语 ──────────────────────────────────────── */

static void model_power_off(void)
{
    m_wake_us = -1; m_tck_rise = 0; m_tck_fall = 0;
    m_fmt = FMT_JSCAN2; m_dev = -1; m_phase = 0;
    cw_reset();
    t_state = S_TLR; t_ir = JTAG_IR_IDCODE;
}

void cjtag_model_pin_init(void)
{
    m_tckc = 0; m_host = -1; m_contention = 0;
    model_power_off();
}

void cjtag_model_reset(int level)
{
    if (level == 0) model_power_off();
}

void cjtag_model_tckc(int level)
{
    level = level ? 1 : 0;
    if (level == m_tckc) return;
    if (level) {
        m_tckc = 1;
        /* TRM §6.4 ICEMelter：8 个上升沿 + 8 个下降沿唤醒 JTAG 电源域。 */
        if (++m_tck_rise >= 8 && m_tck_fall >= 8 && m_wake_us < 0)
            m_wake_us = m_now_us;
        on_tckc_rise();
    } else {
        on_tckc_fall();
        m_tckc = 0;
        if (++m_tck_fall >= 8 && m_tck_rise >= 8 && m_wake_us < 0)
            m_wake_us = m_now_us;
    }
}

void cjtag_model_tmsc_drive(int level)
{
    level = level ? 1 : 0;
    if (m_dev >= 0) m_contention++;   /* 器件正在驱动 TDO，主机不该推挽 */
    m_host = level;
}

void cjtag_model_delay_us(int us) { m_now_us += us; }

void cjtag_model_tmsc_hiz(void) { m_host = -1; }
int  cjtag_model_tmsc_read(void) { return line(); }

/* ══ 被测代码的测试钩子 ═══════════════════════════════════════════ */

void     cjtag_test_shift_ir(uint32_t instr, int bits);
uint32_t cjtag_test_shift_dr(uint32_t tdi, int bits);
void     cjtag_test_goto_pause_dr(void);
void     cjtag_test_open_and_cmd(uint8_t inert, int cp0, int cp1);
int      cjtag_test_tap_state(void);

/* ══ 用例 ═════════════════════════════════════════════════════════ */

/* ICEMelter 必须被唤醒，否则后面所有协议都是对着断电逻辑说话。 */
static void test_icemelter_wakes_jtag_domain(void)
{
    cjtag_enter();
    CHECK(jtag_ready());
}

/* 命令窗 + STFMT 必须真的把链路切到 OScan1，
 * 而不是「反正读回来是 0xFFFFFFFF 也看不出来」。 */
static void test_reaches_oscan1(void)
{
    cjtag_enter();
    CHECK(m_fmt == FMT_OSCAN1);
    if (m_fmt != FMT_OSCAN1)
        fprintf(stderr, "      链路还在 2 线 JScan（control level=%d locked=%d）\n",
                cw_level, cw_locked);
}

/* 最短存活证明：TLR 自动装 IDCODE，不依赖 IR 宽度。 */
static void test_idcode_after_enter(void)
{
    cjtag_enter();
    CHECK_EQ32(cjtag_read_idcode(), CC13_JRC_IDCODE);
    /* 连读两次必须一致——排除「碰巧移出一串上拉 1」这种假通过。 */
    CHECK_EQ32(cjtag_read_idcode(), CC13_JRC_IDCODE);
}

/* cjtag_read_idcode() 必须自带 TLR，不能依赖「调用时 IR 正好还是 IDCODE」。 */
static void test_read_idcode_is_self_sufficient(void)
{
    cjtag_enter();
    cjtag_test_shift_ir(JTAG_IR_BYPASS, ICEPICK_IR_BITS);
    CHECK_EQ32(cjtag_read_idcode(), CC13_JRC_IDCODE);
}

/* 半双工交接：器件驱动 TDO 的那个周期里，主机必须已经松手。 */
static void test_no_bus_contention(void)
{
    cjtag_enter();
    (void)cjtag_read_idcode();
    CHECK(m_contention == 0);
    if (m_contention)
        fprintf(stderr, "      主机在器件驱动 TMSC 期间仍在推挽 %d 次\n",
                m_contention);
}

/* 走 IR 的路径：IR 宽度错了就选不中 IDCODE。 */
static void test_idcode_via_ir(void)
{
    cjtag_enter();
    cjtag_test_shift_ir(JTAG_IR_IDCODE, ICEPICK_IR_BITS);
    CHECK_EQ32(cjtag_test_shift_dr(0, 32), CC13_JRC_IDCODE);
}

/* IR 确实生效了：选 BYPASS 之后 DR 只有 1 位，移 32 位只能出 0。
 * 若 IR 移位是坏的，TAP 会停在 IDCODE 上，这条就会读回 IDCODE 而变红。 */
static void test_bypass_selects_one_bit_dr(void)
{
    cjtag_enter();
    cjtag_test_shift_ir(JTAG_IR_BYPASS, ICEPICK_IR_BITS);
    CHECK_EQ32(cjtag_test_shift_dr(0, 32), 0u);
    cjtag_test_shift_ir(JTAG_IR_IDCODE, ICEPICK_IR_BITS);
    CHECK_EQ32(cjtag_test_shift_dr(0, 32), CC13_JRC_IDCODE);
}

/* 从 PAUSE_DR 出发也要能走对：命令窗序列全程在 Pause-DR 附近打转，
 * 旧版 tap_goto 在这里会走飞（PAUSE_DR 的 TMS=0 是自环）。 */
static void test_navigation_from_pause_dr(void)
{
    cjtag_enter();
    cjtag_test_goto_pause_dr();
    CHECK(cjtag_test_tap_state() == 6 /* TAP_PAUSE_DR */);
    cjtag_test_shift_ir(JTAG_IR_IDCODE, ICEPICK_IR_BITS);
    CHECK_EQ32(cjtag_test_shift_dr(0, 32), CC13_JRC_IDCODE);
}

/* cjtag_exit() 的 RESET 脉冲要让器件回到断电 + 2 线默认态。 */
static void test_exit_powers_down(void)
{
    cjtag_enter();
    CHECK_EQ32(cjtag_read_idcode(), CC13_JRC_IDCODE);
    cjtag_exit();
    CHECK(!jtag_ready());
    CHECK(m_fmt == FMT_JSCAN2);
}

/*
 * 与 OpenOCD 的 ti_cjtag_to_4pin_jtag 逐次扫描对拍。
 *
 * 那段 TCL 是**实测能用**的参考（社区拿它在真 CC26xx 上把器件切到 4 线
 * JTAG）。把它的 pathmove 展开成 TMS 位再按 Update 结算，得到每次 DR 扫描
 * 在 Shift-DR 里的时钟数是 [0, 0, 1, 2, 9]：两次 ZBS、锁 control level 的
 * 1 位扫描、CP0=2(STC2)、CP1=9(APFC=01)。
 *
 * 这串数就是命令的全部内容（TRM §6.2.1：命令的值 = 时钟数）。本实现发同一
 * 条命令时必须给出同一串数——不然就是我们的开窗/计数理解错了。
 *
 * 钉住它的价值：2 线模式下器件对命令窗不做任何应答，上板时没有任何办法
 * 二分定位。这条用例是唯一能在不上板的情况下判定"序列本身对不对"的判据。
 */
static void test_matches_openocd_reference(void)
{
    cjtag_model_pin_init();
    /* 唤醒 + 等够 200 µs，让命令窗那段跑在已上电的逻辑上 */
    for (int i = 0; i < 16; i++) { cjtag_model_tckc(1); cjtag_model_tckc(0); }
    cjtag_model_delay_us(1000);

    g_scan_n = 0;
    cjtag_test_open_and_cmd(JTAG_IR_BYPASS, 2, 9);   /* = OpenOCD 的 4 线切换 */

    static const int WANT[] = { 0, 0, 1, 2, 9 };
    const int n_want = (int)(sizeof WANT / sizeof WANT[0]);
    CHECK(g_scan_n == n_want);
    if (g_scan_n != n_want) {
        fprintf(stderr, "      扫描次数 %d，期望 %d：", g_scan_n, n_want);
        for (int i = 0; i < g_scan_n; i++) fprintf(stderr, " %d", g_scan_clk[i]);
        fprintf(stderr, "\n");
        return;
    }
    for (int i = 0; i < n_want; i++) {
        CHECK(g_scan_clk[i] == WANT[i]);
        if (g_scan_clk[i] != WANT[i])
            fprintf(stderr, "      第 %d 次扫描 Shift-DR 时钟数 = %d，"
                            "OpenOCD 参考是 %d\n", i, g_scan_clk[i], WANT[i]);
    }
}

/* 同一条路径换成 STFMT：只有 CP0 从 2(STC2) 变成 3(STFMT)，其余一模一样。 */
static void test_stfmt_differs_only_in_opcode(void)
{
    cjtag_model_pin_init();
    for (int i = 0; i < 16; i++) { cjtag_model_tckc(1); cjtag_model_tckc(0); }
    cjtag_model_delay_us(1000);

    g_scan_n = 0;
    cjtag_test_open_and_cmd(JTAG_IR_BYPASS, CJTAG_CMD_STFMT,
                            CJTAG_FMT_CODE_OSCAN1);
    static const int WANT[] = { 0, 0, 1, 3, 9 };
    CHECK(g_scan_n == 5);
    for (int i = 0; i < 5 && i < g_scan_n; i++) {
        CHECK(g_scan_clk[i] == WANT[i]);
        if (g_scan_clk[i] != WANT[i])
            fprintf(stderr, "      第 %d 次扫描 = %d，期望 %d\n",
                    i, g_scan_clk[i], WANT[i]);
    }
}

int main(void)
{
    test_matches_openocd_reference();
    test_stfmt_differs_only_in_opcode();
    test_icemelter_wakes_jtag_domain();
    test_reaches_oscan1();
    test_idcode_after_enter();
    test_read_idcode_is_self_sufficient();
    test_no_bus_contention();
    test_idcode_via_ir();
    test_bypass_selects_one_bit_dr();
    test_navigation_from_pause_dr();
    test_exit_powers_down();

    if (g_fail) { printf("test_cjtag: %d FAIL\n", g_fail); return 1; }
    printf("test_cjtag: all OK\n");
    return 0;
}
