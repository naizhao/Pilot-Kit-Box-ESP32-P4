/*
 * test_power_sy6970.c — SY6970 寄存器解码纯函数的 host 单测
 * （WP-D Task 3，TDD 先红后绿）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -DSY6970_HOST_TEST \
 *      -o /tmp/test_power_sy6970 firmware/test/test_power_sy6970.c \
 *      firmware/main/power_sy6970.c && /tmp/test_power_sy6970
 *
 * 只测纯解码部分：注入 REG0B..REG12 连续 8 字节寄存器窗口帧
 * （regs[0]==REG0B ... regs[7]==REG12，见 power_sy6970.h 的窗口合同），
 * 断言状态位 / 故障位 / ADC 换算。全部期望值来自本地数据表取证
 * （SY6970_DS.pdf Rev.0.9B / SY6970_DS_alt.pdf Rev.0.1 / SY6970.pdf
 * AN_SY6970 Rev.0.9B，三份互核；页码引用见 power_sy6970.c 文件头取证表）。
 * SY6970_HOST_TEST 隔离模式同 test_qmc5883p.c；目标端 I²C 胶水（Task 4）
 * 不伪造覆盖，靠两板系构建验证。
 *
 * 钉死的契约：
 *   1. 窗口：n < SY6970_WIN_LEN / regs=NULL / out=NULL 一律 false；
 *   2. 全 0xFF 帧（器件缺席 / 总线被上拉）：false，且 *out 被清零——
 *      绝不把全 1 解码成"处处故障但数据有效"的垃圾态；
 *   3. 充电语义：CHRG_STAT 01(预充)/10(快充)=charging，11(终止)=不充
 *      但 vbus_present 仍按 BUS_GD 报；
 *   4. vbus_present：BUS_GD=1 且 BUS_STAT≠111(OTG)——OTG 是电池对外
 *      供电，BUS 在位≠BUS 在给我们供电；
 *   5. ADC 换算（整数、无浮点）：BATV/SYSV=2304mV+code×20mV，
 *      BUSV=2600mV+code×100mV，ICHGR=code×50mA，NTCPCT=21%+code×0.465%
 *      （×1000 整数化）；
 *   6. init 序列表：4 步（写前 REG00 校验 → REG07 关看门狗 → REG03
 *      喂狗 → 写后 REG00 回读验证），地址/掩码/期望值与取证表一致，
 *      每步 why 非空。
 */

#include <stdio.h>
#include <string.h>

#include "../main/power_sy6970.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ── 帧构造：REG0B..REG12 连续 8 字节窗口 ──────────────────────────── */
typedef struct {
    uint8_t b, c, d, e, f, g10, g11, g12;  /* 0B 0C 0D 0E 0F 10 11 12 */
} frame_t;

static const uint8_t *frame_bytes(const frame_t *f, size_t *n)
{
    /* 结构体成员顺序即窗口顺序（不靠 padding，逐字节显式拷出）。 */
    static uint8_t buf[SY6970_WIN_LEN];
    buf[0] = f->b;   buf[1] = f->c;   buf[2] = f->d;   buf[3] = f->e;
    buf[4] = f->f;   buf[5] = f->g10; buf[6] = f->g11; buf[7] = f->g12;
    *n = SY6970_WIN_LEN;
    return buf;
}

/* 健康无输入帧：BUS_STAT=000 / CHRG_STAT=00 / PG_STAT=0（BUS_GD=0、
 * 无输入在场时"电源好"无从谈起，PG 置 0 保持叙事自洽）/ SDP_STAT=1
 * （BUS_STAT≠001 时恒 1，取证 DS p.22）/ VSYS_STAT=0（3704 mV 不在
 * SYSMIN 调节）；VINDPM 保持 POR 0x12。 */
static frame_t healthy_no_input(void)
{
    frame_t f = { 0 };
    f.b   = 0x02;   /* 仅 SDP_STAT；BUS_STAT=000 CHRG_STAT=00 PG=0      */
    f.c   = 0x00;   /* 无故障                                           */
    f.d   = 0x12;   /* REG0D POR：VINDPM=4.4V（DS p.23，窗口内未消费）  */
    f.e   = 70;     /* BATV code 70 → 2304+70×20 = 3704 mV              */
    f.f   = 32;     /* SYSV（窗口内未消费）                             */
    f.g10 = 0;      /* NTCPCT code 0 → 21.000%                          */
    f.g11 = 0x00;   /* BUS_GD=0，BUSV code 0 → 2600 mV                  */
    f.g12 = 10;     /* ICHGR code 10 → 500 mA                           */
    return f;
}

/* ── 1 正常未充电（无输入）：解码成功，vbus 不在位 ─────────────────── */
static void test_decode_normal_not_charging(void)
{
    frame_t f = healthy_no_input();
    size_t n = 0;
    const uint8_t *regs = frame_bytes(&f, &n);

    sy6970_status_t out;
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.charging == false);
    CHECK(out.term_done == false);
    CHECK(out.vbus_present == false);
    CHECK(out.power_good == false);     /* PG_STAT=0：无输入在场         */
    CHECK(out.batt_mv == 3704);         /* 2304 + 70×20（DS p.23）      */
    CHECK(out.vbus_mv == 2600);         /* 2600 + 0×100（DS p.24）      */
    CHECK(out.ichg_ma == 500);          /* 10×50（DS p.24-25）          */
    CHECK(out.ntc_pct_x1000 == 21000);  /* 21% + 0×0.465%（DS p.24）    */
    CHECK(out.wd_fault == false);
    CHECK(out.boost_fault == false);
    CHECK(out.chrg_fault == 0);
    CHECK(out.bat_ovp_fault == false);
    CHECK(out.ntc_fault == 0);
    CHECK(out.therm_reg == false);
    CHECK(out.bus_stat == 0);
}

/* ── 2 快充中：charging + vbus_present + 全链 ADC 换算 ─────────────── */
static void test_decode_fast_charging(void)
{
    frame_t f = healthy_no_input();
    f.b   = 0x76;   /* BUS_STAT=011(DCP)=0x60 | CHRG_STAT=10(快充)=0x10
                     * | PG_STAT=0x04 | SDP_STAT=0x02                     */
    f.g11 = 0x80 | 24;  /* BUS_GD=1，BUSV code 24 → 2600+24×100=5000 mV */
    f.g10 = 10;         /* NTCPCT code 10 → 21%+10×0.465% = 25.650%     */

    size_t n = 0;
    const uint8_t *regs = frame_bytes(&f, &n);

    sy6970_status_t out;
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.bus_stat == 3);           /* DCP                          */
    CHECK(out.charging == true);
    CHECK(out.term_done == false);
    CHECK(out.vbus_present == true);    /* BUS_GD=1 且非 OTG            */
    CHECK(out.vbus_mv == 5000);
    CHECK(out.batt_mv == 3704);
    CHECK(out.ichg_ma == 500);
    CHECK(out.ntc_pct_x1000 == 25650);  /* 21000 + 10×465               */
}

/* ── 3 预充也算充电；终止完成不算充电但 vbus 仍在 ──────────────────── */
static void test_decode_precharge_and_term_done(void)
{
    frame_t f = healthy_no_input();
    f.b = 0x07 | (1 << 3);              /* CHRG_STAT=01 预充            */
    size_t n = 0;
    const uint8_t *regs = frame_bytes(&f, &n);
    sy6970_status_t out;
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.charging == true);
    CHECK(out.term_done == false);

    f.b = 0x07 | (3 << 3);              /* CHRG_STAT=11 终止完成        */
    regs = frame_bytes(&f, &n);
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.charging == false);
    CHECK(out.term_done == true);
}

/* ── 4 OTG（BUS_STAT=111）时 BUS_GD=1 也不算 vbus_present ──────────── */
static void test_decode_otg_is_not_vbus_powered(void)
{
    frame_t f = healthy_no_input();
    f.b   = 0x07 | (7 << 5);            /* BUS_STAT=111 OTG             */
    f.g11 = 0x80 | 24;                  /* BUS_GD=1（对外供电时也置位） */
    size_t n = 0;
    const uint8_t *regs = frame_bytes(&f, &n);

    sy6970_status_t out;
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.bus_stat == 7);
    CHECK(out.vbus_present == false);   /* OTG：电池供 BUS，不是 BUS 供电 */
}

/* ── 5 故障位解码：WD/CHRG_FAULT/BATOVP/NTC/热调节 ─────────────────── */
static void test_decode_fault_bits(void)
{
    frame_t f = healthy_no_input();
    f.c = 0xAE;     /* bit7 WD 故障 | CHRG_FAULT=10 热关断 | bit3 BATOVP
                     * | NTC_FAULT=110 过热（DS p.22-23 位定义）          */
    f.e = 0x80 | 70;                    /* THERM_STAT=1（REG0E bit7）   */
    size_t n = 0;
    const uint8_t *regs = frame_bytes(&f, &n);

    sy6970_status_t out;
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.wd_fault == true);
    CHECK(out.boost_fault == false);
    CHECK(out.chrg_fault == 2);         /* 10 = 热关断                  */
    CHECK(out.bat_ovp_fault == true);
    CHECK(out.ntc_fault == 6);          /* 110 = NTC Hot                */
    CHECK(out.therm_reg == true);
}

/* ── 6 全 0xFF：器件缺席 → false 且 *out 清零（绝不输出垃圾有效态）─── */
static void test_decode_all_ff_is_unavailable(void)
{
    uint8_t regs[SY6970_WIN_LEN];
    memset(regs, 0xFF, sizeof(regs));

    sy6970_status_t out;
    memset(&out, 0xA5, sizeof(out));    /* 预填脏数据证明被清零         */
    CHECK(sy6970_decode_status(regs, sizeof(regs), &out) == false);
    static const sy6970_status_t zero;
    CHECK(memcmp(&out, &zero, sizeof(out)) == 0);
}

/* ── 7 窗口防御：短帧 / NULL 一律 false ────────────────────────────── */
static void test_decode_rejects_bad_args(void)
{
    frame_t f = healthy_no_input();
    size_t n = 0;
    const uint8_t *regs = frame_bytes(&f, &n);

    sy6970_status_t out;
    CHECK(sy6970_decode_status(NULL, SY6970_WIN_LEN, &out) == false);
    CHECK(sy6970_decode_status(regs, SY6970_WIN_LEN, NULL) == false);
    CHECK(sy6970_decode_status(regs, SY6970_WIN_LEN - 1, &out) == false);

    /* 比窗口长的帧可以：只消费前 8 字节（任务 4 连读大窗口时无痛）。 */
    uint8_t big[SY6970_WIN_LEN + 4];
    memcpy(big, regs, SY6970_WIN_LEN);
    memset(big + SY6970_WIN_LEN, 0xEE, 4);
    CHECK(sy6970_decode_status(big, sizeof(big), &out) == true);
    CHECK(out.batt_mv == 3704);
}

/* ── 8 ADC 换算边界：每个公式在 code=0 与 code=127（满码）处 ───────── */
static void test_decode_adc_boundaries(void)
{
    frame_t f = healthy_no_input();
    f.e   = 0x00;                       /* BATV code 0                  */
    f.g10 = 0x00;                       /* NTCPCT code 0                */
    f.g11 = 0x00;                       /* BUS_GD=0，BUSV code 0        */
    f.g12 = 0x00;                       /* ICHGR code 0                 */
    size_t n = 0;
    const uint8_t *regs = frame_bytes(&f, &n);
    sy6970_status_t out;
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.batt_mv == 2304);
    CHECK(out.ntc_pct_x1000 == 21000);
    CHECK(out.vbus_mv == 2600);
    CHECK(out.ichg_ma == 0);

    f.e   = 0x7F;                       /* 满码 127                     */
    f.g10 = 0x7F;
    f.g11 = 0x7F;
    f.g12 = 0x7F;
    regs = frame_bytes(&f, &n);
    CHECK(sy6970_decode_status(regs, n, &out) == true);
    CHECK(out.batt_mv == 4844);         /* 2304+127×20（DS p.23 量程）  */
    CHECK(out.ntc_pct_x1000 == 80055);  /* 21%+127×0.465% = 80.055%     */
    CHECK(out.vbus_mv == 15300);        /* 2600+127×100（DS p.24 量程） */
    CHECK(out.ichg_ma == 6350);         /* 127×50（DS p.24-25 量程）    */
}

/* ── 9 init 序列表与取证一致（地址/操作/掩码/期望值 + why 非空）────── */
static void test_init_seq_matches_evidence(void)
{
    size_t n = 0;
    const sy6970_init_step_t *seq = sy6970_init_seq(&n);
    CHECK(seq != NULL);
    CHECK(n == 4);

    /* 步骤 0（写前）：REG00 回读校验——EN_HIZ=0|EN_ILIM=1（POR 位值，
     * DS p.15），在位证据 */
    CHECK(seq[0].op == SY6970_SEQ_VERIFY);
    CHECK(seq[0].reg == 0x00);
    CHECK(seq[0].mask == 0xC0);
    CHECK(seq[0].val == 0x40);

    /* 步骤 1：REG07 清 WATCHDOG[5:4] → 00 关看门狗（DS p.19）*/
    CHECK(seq[1].op == SY6970_SEQ_RMW_CLEAR);
    CHECK(seq[1].reg == 0x07);
    CHECK(seq[1].mask == 0x30);

    /* 步骤 2：REG03 置 WD_RST(bit6) 喂狗，写 1 自清（DS p.17）*/
    CHECK(seq[2].op == SY6970_SEQ_RMW_SET);
    CHECK(seq[2].reg == 0x03);
    CHECK(seq[2].mask == 0x40);

    /* 步骤 3（写后）：再回读 REG00 验证写入已落定——计划约束「写入后
     * 必须回读 REG00 验证」，掩码/期望值与写前一行同源（DS p.15）*/
    CHECK(seq[3].op == SY6970_SEQ_VERIFY);
    CHECK(seq[3].reg == 0x00);
    CHECK(seq[3].mask == 0xC0);
    CHECK(seq[3].val == 0x40);

    for (size_t i = 0; i < n; i++) {
        CHECK(seq[i].why != NULL && seq[i].why[0] != '\0');
    }
}

int main(void)
{
    test_decode_normal_not_charging();
    test_decode_fast_charging();
    test_decode_precharge_and_term_done();
    test_decode_otg_is_not_vbus_powered();
    test_decode_fault_bits();
    test_decode_all_ff_is_unavailable();
    test_decode_rejects_bad_args();
    test_decode_adc_boundaries();
    test_init_seq_matches_evidence();

    if (g_fail == 0) {
        printf("test_power_sy6970: all OK\n");
        return 0;
    }
    printf("test_power_sy6970: %d FAIL\n", g_fail);
    return 1;
}
