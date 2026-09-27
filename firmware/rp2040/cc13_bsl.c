/* cc13_bsl.c — 见 cc13_bsl.h。 */
#include "cc13_bsl.h"
#include "cc13_bsl_proto.h"

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "board_pins.h"
#include "cc13_bsl_spi.pio.h"
#include "spi_master.h"

/* ROM bootloader 的固定角色 ↔ 本板网络（推导见头文件）。
 * 注意 TO_DEV / FROM_DEV 是**按 ROM 的角色**命名的，与板上 MOSI/MISO 的名字
 * 正好相反——这正是本模块存在的理由，不要"顺手改回来"。 */
#define BSL_CLK       PIN_SUBG_SCK    /* GPIO10 → DIO10 = SSI0_CLK  */
#define BSL_FSS       PIN_SUBG_CSN    /* GPIO13 → DIO11 = SSI0_FSS  */
#define BSL_TO_DEV    PIN_SUBG_MISO   /* GPIO12 → DIO9  = SSI0_RX   */
#define BSL_FROM_DEV  PIN_SUBG_MOSI   /* GPIO11 → DIO8  = SSI0_TX   */

static unsigned s_half_us = 5;        /* ~100 kHz，远低于 4 MHz 上限 */

/* TRM §10.2.2.2 指定 SPO=1/SPH=1（mode 3）。诊断保留 mode 0 作为反证
 * 变体，但生产路径必须从 mode 3 开始。外部 ADSBee 项目的当前源码同时存在
 * mode-3 配置常量、mode-0 初始化和互相矛盾的旧注释，不能作为 mode 0 已被
 * 实机证明的证据。 */
static bool s_mode3 = true;

/* PIO 后端只用于无破坏诊断：本板 ROM 的数据方向与 RP2040 SPI1 固定功能
 * 相反，硬件 SPI 无法直连；PIO 可在任意 GPIO 上产生确定的 mode-3 边沿。 */
static PIO s_bsl_pio = pio0;
static int s_bsl_pio_sm = -1;
static uint s_bsl_pio_offset;
static bool s_use_pio;
static unsigned s_first_gap_us = 500u;

static bool bsl_pio_start(uint input_pin, unsigned hz)
{
    if (s_bsl_pio_sm >= 0) return true;
    if (!pio_can_add_program(s_bsl_pio, &cc13_bsl_spi_cpha1_program))
        return false;

    int sm = pio_claim_unused_sm(s_bsl_pio, false);
    if (sm < 0) return false;
    s_bsl_pio_sm = sm;
    s_bsl_pio_offset = pio_add_program(s_bsl_pio,
                                       &cc13_bsl_spi_cpha1_program);

    pio_sm_config c = cc13_bsl_spi_cpha1_program_get_default_config(
        s_bsl_pio_offset);
    sm_config_set_out_pins(&c, BSL_TO_DEV, 1);
    sm_config_set_in_pins(&c, input_pin);
    sm_config_set_sideset_pins(&c, BSL_CLK);
    sm_config_set_out_shift(&c, false, true, 8); /* MSB first */
    sm_config_set_in_shift(&c, false, true, 8);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (4.0f * hz));

    pio_gpio_init(s_bsl_pio, BSL_CLK);
    pio_gpio_init(s_bsl_pio, BSL_TO_DEV);
    pio_gpio_init(s_bsl_pio, input_pin);
    pio_sm_set_pins_with_mask(s_bsl_pio, (uint)sm, 0,
                              (1u << BSL_CLK) | (1u << BSL_TO_DEV));
    pio_sm_set_pindirs_with_mask(s_bsl_pio, (uint)sm,
                                 (1u << BSL_CLK) | (1u << BSL_TO_DEV),
                                 (1u << BSL_CLK) | (1u << BSL_TO_DEV) |
                                 (1u << input_pin));
    /* PIO 程序的 side=0 是逻辑空闲；反相后引脚空闲高，即 CPOL=1。 */
    gpio_set_outover(BSL_CLK, GPIO_OVERRIDE_INVERT);
    pio_sm_init(s_bsl_pio, (uint)sm, s_bsl_pio_offset, &c);
    pio_sm_set_enabled(s_bsl_pio, (uint)sm, true);
    return true;
}

static void bsl_pio_stop(void)
{
    if (s_bsl_pio_sm < 0) return;
    pio_sm_set_enabled(s_bsl_pio, (uint)s_bsl_pio_sm, false);
    pio_sm_clear_fifos(s_bsl_pio, (uint)s_bsl_pio_sm);
    pio_sm_unclaim(s_bsl_pio, (uint)s_bsl_pio_sm);
    pio_remove_program(s_bsl_pio, &cc13_bsl_spi_cpha1_program,
                       s_bsl_pio_offset);
    s_bsl_pio_sm = -1;
    s_use_pio = false;
    gpio_set_outover(BSL_CLK, GPIO_OVERRIDE_NORMAL);
}

static uint8_t bsl_pio_xfer(uint8_t out)
{
    io_rw_8 *tx = (io_rw_8 *)&s_bsl_pio->txf[s_bsl_pio_sm];
    io_rw_8 *rx = (io_rw_8 *)&s_bsl_pio->rxf[s_bsl_pio_sm];
    while (pio_sm_is_tx_fifo_full(s_bsl_pio, (uint)s_bsl_pio_sm))
        tight_loop_contents();
    *tx = out; /* 8-bit FIFO 写会复制字节，配合左移得到 MSB-first */
    while (pio_sm_is_rx_fifo_empty(s_bsl_pio, (uint)s_bsl_pio_sm))
        tight_loop_contents();
    return *rx;
}

static void bsl_pins_take(void)
{
    /* 从 SPI 外设手里把四根线要回来：外设的引脚角色是固定的，换不了方向。 */
    gpio_init(BSL_CLK);
    gpio_init(BSL_FSS);
    gpio_init(BSL_TO_DEV);
    gpio_init(BSL_FROM_DEV);

    gpio_put(BSL_CLK, 1);             /* SPO=1：时钟空闲高 */
    gpio_set_dir(BSL_CLK, GPIO_OUT);
    gpio_put(BSL_FSS, 1);             /* 片选空闲高 */
    gpio_set_dir(BSL_FSS, GPIO_OUT);
    gpio_put(BSL_TO_DEV, 0);
    gpio_set_dir(BSL_TO_DEV, GPIO_OUT);

    gpio_set_dir(BSL_FROM_DEV, GPIO_IN);
    gpio_pull_up(BSL_FROM_DEV);

    gpio_init(PIN_SUBG_RESET);
    gpio_put(PIN_SUBG_RESET, 1);
    gpio_set_dir(PIN_SUBG_RESET, GPIO_OUT);
}

/*
 * SPI mode 3（SPO=1 / SPH=1，TRM §10.2.2.2）：时钟空闲高；数据在**下降沿**
 * 换、在**上升沿**采。MSB 先出。
 */
static uint8_t bsl_xfer(uint8_t out)
{
    if (s_use_pio) return bsl_pio_xfer(out);
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        if (s_mode3) {
            /* mode 3：时钟空闲高，下降沿换数据、上升沿采样 */
            gpio_put(BSL_CLK, 0);
            gpio_put(BSL_TO_DEV, (out >> i) & 1);
            busy_wait_us(s_half_us);
            gpio_put(BSL_CLK, 1);
            in = (uint8_t)((in << 1) | (gpio_get(BSL_FROM_DEV) ? 1u : 0u));
            busy_wait_us(s_half_us);
        } else {
            /* mode 0：时钟空闲低，数据在上升沿之前摆好、上升沿采样 */
            gpio_put(BSL_TO_DEV, (out >> i) & 1);
            busy_wait_us(s_half_us);
            gpio_put(BSL_CLK, 1);
            in = (uint8_t)((in << 1) | (gpio_get(BSL_FROM_DEV) ? 1u : 0u));
            busy_wait_us(s_half_us);
            gpio_put(BSL_CLK, 0);
        }
    }
    return in;
}

/* TRM Figure 22-9: mode 3 下 FSS 下降后要再过半个 CLK 周期，才出现首个
 * CLK 下降沿并把首位送上线。此前这里没有建立时间，FSS 与首沿几乎同时到。 */
static void bsl_fss_assert(void)
{
    gpio_put(BSL_FSS, 0);
    busy_wait_us(s_half_us);
}

/*
 * 复位进 ROM bootloader。
 *
 * sync 高低两种都试：TRM §10.1.2 的 backdoor 要在**复位那一刻**就把
 * BL_PIN_NUMBER 指定的脚摆在 BL_LEVEL 上。本板 SUBG_SYNC = DIO13 ←
 * RP2040 GPIO15，PINMAP 里本来就标着「bootloader trigger」。
 *
 * 空片上 backdoor 本身未配置（BL_PIN_NUMBER/BL_ENABLE 为默认值），但无需它：
 * IMAGE_VALID_CONF 擦除态不是合法向量地址，启动代码会自动进 bootloader。
 * sync 当变体试只是为了把「摆错电平反而挡住了自动进入」这种可能排掉。
 */
static void bsl_reset_into_rom(bool sync_high, unsigned settle_ms)
{
    gpio_put(BSL_FSS, 1);
    gpio_put(BSL_CLK, s_mode3 ? 1 : 0);

    gpio_init(PIN_SUBG_SYNC);
    gpio_put(PIN_SUBG_SYNC, sync_high ? 1 : 0);
    gpio_set_dir(PIN_SUBG_SYNC, GPIO_OUT);

    gpio_put(PIN_SUBG_RESET, 0);
    sleep_ms(5);
    gpio_put(PIN_SUBG_RESET, 1);
    sleep_ms(settle_ms);              /* 等 ROM 起来并采样 backdoor 脚 */
}

/* 未知 CCFG 的 backdoor 扫描：在 RESET_N 已拉低后，把本板实际
 * 连出的 DIO8..DIO13 同时摆成同一电平，释放复位给 ROM 采样，
 * 然后立即改回 SSI0 角色。先拉 RESET 再驱动 DIO，避免和未知应用
 * 的输出短暂对顶。这不能覆盖未引出的 DIO，但能一次性裁决六根
 * 现有信号中是否藏着可用 backdoor。 */
static void bsl_reset_all_connected_dio(bool high, unsigned settle_ms)
{
    static const uint k_dio_pins[] = {
        PIN_SUBG_MOSI, PIN_SUBG_MISO, PIN_SUBG_SCK,
        PIN_SUBG_CSN, PIN_SUBG_IRQ, PIN_SUBG_SYNC,
    };

    gpio_init(PIN_SUBG_RESET);
    gpio_put(PIN_SUBG_RESET, 0);
    gpio_set_dir(PIN_SUBG_RESET, GPIO_OUT);
    sleep_ms(2);

    for (size_t i = 0; i < sizeof k_dio_pins / sizeof k_dio_pins[0]; i++) {
        gpio_init(k_dio_pins[i]);
        gpio_put(k_dio_pins[i], high ? 1 : 0);
        gpio_set_dir(k_dio_pins[i], GPIO_OUT);
    }
    sleep_ms(2);
    gpio_put(PIN_SUBG_RESET, 1);
    sleep_ms(2);                       /* 保持到 ROM 完成 backdoor 采样 */
    bsl_pins_take();                   /* DIO8..11 恢复成 SSI0 角色 */
    if (settle_ms > 2u) sleep_ms(settle_ms - 2u);
}

/*
 * 发一个 PING 包并把回读的原始字节记下来。
 *
 * 两处 TRM 明写的坑：
 * 1. §10.2.2.2 Note：**第一包的第一个字节期间器件不回数据**——ROM 要先在
 *    SSI0_RX 上收满 1 个字节才会去配置 SSI0_TX 输出脚。所以主机在第一个字节
 *    之后必须插一小段延时，等它配完。
 * 2. §10.2.1：发送方可以一直发 0 直到收到非 0 回应；接收方在准备好 ACK/NAK
 *    之前也可以一直回 0。所以"读到一串 0"是正常的，要继续клок。
 */
static int bsl_diag_cmd(uint8_t cmd, uint8_t *log, int cap,
                        bool fss_per_byte, bool first_gap,
                        int *first_up, int *first_dn)
{
    const uint8_t pkt[3] = { 0x03, cmd, cmd };  /* size/校验和/命令 */
    int n = 0;

    if (!fss_per_byte) bsl_fss_assert();
    for (int i = 0; i < 3; i++) {
        if (fss_per_byte) bsl_fss_assert();
        uint8_t r = bsl_xfer(pkt[i]);
        if (fss_per_byte) gpio_put(BSL_FSS, 1);
        if (n < cap) log[n++] = r;
        if (i == 0 && first_gap) {
            sleep_us(s_first_gap_us);            /* 坑 1 */
            if (first_up && first_dn) {
                /* 若 ROM 已把 SSI0_TX 切成推挽输出，内部约 50k 的弱上下拉
                 * 都改变不了读值；1/0 则证明 DIO8 此刻仍是高阻。 */
                gpio_pull_up(BSL_FROM_DEV);
                sleep_us(10);
                *first_up = gpio_get(BSL_FROM_DEV);
                gpio_pull_down(BSL_FROM_DEV);
                sleep_us(10);
                *first_dn = gpio_get(BSL_FROM_DEV);
                gpio_pull_up(BSL_FROM_DEV);
            }
        }
    }
    /* 继续空转读 ACK：接收方准备好之前可以一直回 0（坑 2）。 */
    for (int i = 0; i < 12 && n < cap; i++) {
        if (fss_per_byte) bsl_fss_assert();
        uint8_t r = bsl_xfer(0x00);
        if (fss_per_byte) gpio_put(BSL_FSS, 1);
        log[n++] = r;
    }
    if (!fss_per_byte) gpio_put(BSL_FSS, 1);
    return n;
}

void cc13_bsl_diag(void)
{
    bsl_pins_take();

    printf("cc13-bsl: ROM 串行 bootloader (SSI0) —— TRM SWCU185G §10\n");
    printf("  接线: CLK=GPIO%d FSS=GPIO%d  送数据→GPIO%d(DIO9=SSI0_RX)  "
           "收数据←GPIO%d(DIO8=SSI0_TX)\n",
           BSL_CLK, BSL_FSS, BSL_TO_DEV, BSL_FROM_DEV);

    /* 先量一下"器件输出"那根线上有没有别人——和 cJTAG 那边同一套判据：
     * 我们下拉还读到 1，说明线上有强过内部 50k 的上拉（=接到了东西）。 */
    gpio_set_dir(BSL_FROM_DEV, GPIO_IN);
    gpio_pull_up(BSL_FROM_DEV);   sleep_ms(1); int up = gpio_get(BSL_FROM_DEV);
    gpio_pull_down(BSL_FROM_DEV); sleep_ms(1); int dn = gpio_get(BSL_FROM_DEV);
    gpio_pull_up(BSL_FROM_DEV);
    printf("  线探针 DIO8(器件输出): 上拉=%d 下拉=%d %s\n", up, dn,
           (up && dn) ? "← 线上有外部上拉" : "← 只有我们自己在拉");

    /* TRM §10.1.1：即便 CCFG 禁用了 bootloader，GET_STATUS 仍可执行；但先发
     * 任何其他命令，ROM 就连后续 GET_STATUS 也不再响应。因此它必须是每次
     * reset 后的第一条命令，才能把“CCFG 禁用”与“SSI 根本没被看到”分开。 */
    s_mode3 = true;
    s_half_us = 5u;
    bsl_pins_take();
    bsl_reset_into_rom(false, 10u);
    uint8_t status_log[16];
    int status_n = bsl_diag_cmd(CC13_BSL_CMD_GET_STATUS,
                                status_log, (int)sizeof status_log,
                                false, true, NULL, NULL);
    bool status_ack = false;
    printf("  首命令 GET_STATUS 回读:");
    for (int i = 0; i < status_n; i++) {
        printf(" %02X", status_log[i]);
        if (status_log[i] == CC13_BSL_ACK) status_ack = true;
    }
    printf("%s\n", status_ack ? "   ← ACK(0xCC)!" : "");

    /* 若目标并非空片、CCFG 也不是我们镜像预期的 DIO13 高有效，
     * 可能仍有 backdoor 配在已引出的 DIO8..13 之一。全低/全高各复位
     * 一次即可覆盖这六根线的两种有效电平。 */
    for (int high = 0; high <= 1; high++) {
        s_mode3 = true;
        s_first_gap_us = 10u;
        bsl_reset_all_connected_dio(high != 0, 10u);
        if (!bsl_pio_start(BSL_FROM_DEV, 500000u)) {
            printf("  backdoor DIO8..13 全%d: PIO 不可用\n", high);
            continue;
        }
        s_use_pio = true;
        uint8_t bd_log[16];
        int bd_n = bsl_diag_cmd(CC13_BSL_CMD_GET_STATUS,
                                bd_log, (int)sizeof bd_log,
                                false, true, NULL, NULL);
        bool bd_ack = false;
        printf("  backdoor DIO8..13 全%d，首命令 GET_STATUS 回读:", high);
        for (int i = 0; i < bd_n; i++) {
            printf(" %02X", bd_log[i]);
            if (bd_log[i] == CC13_BSL_ACK) bd_ack = true;
        }
        printf("%s\n", bd_ack ? "   ← ACK(0xCC)!" : "");
        bsl_pio_stop();
        bsl_pins_take();
    }
    s_first_gap_us = 500u;

    /* 用 PIO 重做相同的首命令探测。它与 bit-bang 共用接线、复位和包格式，
     * 只替换时钟/移位引擎，可直接裁决 C 循环边沿是否是无应答根因。 */
    /* 先把 PIO 输入临时映射到自己的输出脚做片内回环，验证 FIFO 字节对齐、
     * MSB-first 与状态机确实在工作；FSS 保持高，目标不会把它当成事务。 */
    if (bsl_pio_start(BSL_TO_DEV, 100000u)) {
        s_use_pio = true;
        uint8_t a5 = bsl_xfer(0xA5), c3 = bsl_xfer(0x3C);
        printf("  PIO片内回环: A5→%02X 3C→%02X %s\n", a5, c3,
               (a5 == 0xA5 && c3 == 0x3C) ? "OK" : "← PIO移位错误");
        bsl_pio_stop();
    } else {
        printf("  PIO片内回环: 无空闲状态机或指令空间，跳过\n");
    }

    bsl_pins_take();
    bsl_reset_into_rom(false, 10u);
    s_first_gap_us = 500u;
    if (bsl_pio_start(BSL_FROM_DEV, 100000u)) {
        s_use_pio = true;
        uint8_t pio_log[16];
        int pio_n = bsl_diag_cmd(CC13_BSL_CMD_GET_STATUS,
                                 pio_log, (int)sizeof pio_log,
                                 false, true, NULL, NULL);
        bool pio_ack = false;
        printf("  PIO mode3 首命令 GET_STATUS 回读:");
        for (int i = 0; i < pio_n; i++) {
            printf(" %02X", pio_log[i]);
            if (pio_log[i] == CC13_BSL_ACK) pio_ack = true;
        }
        printf("%s\n", pio_ack ? "   ← ACK(0xCC)!" : "");
        bsl_pio_stop();
        bsl_pins_take();
    } else {
        printf("  PIO mode3: 无空闲状态机或指令空间，跳过\n");
    }

    /* 最后一组纯时序排除：100 kHz 可能过慢、500 us 也可能超过 ROM 的
     * 包内等待。逐次 reset，保证 GET_STATUS 永远是第一条被尝试的命令；
     * 只打印命中或非 FF/00 回应，避免全静默时刷出几十行噪声。 */
    static const unsigned k_pio_hz[] = {
        100000u, 500000u, 1000000u, 4000000u
    };
    static const unsigned k_gap_us[] = { 0u, 2u, 10u, 50u };
    static const unsigned k_settle_ms[] = { 10u, 100u };
    unsigned pio_tries = 0, pio_nontrivial = 0;
    printf("  PIO mode3 时序扫描: 100k/500k/1M/4MHz × gap 0/2/10/50us "
           "× wait 10/100ms × sync 0/1\n");
    for (size_t hi = 0; hi < sizeof k_pio_hz / sizeof k_pio_hz[0]; hi++) {
        for (size_t gi = 0; gi < sizeof k_gap_us / sizeof k_gap_us[0]; gi++) {
            for (size_t wi = 0; wi < sizeof k_settle_ms / sizeof k_settle_ms[0]; wi++) {
                for (int sync = 0; sync <= 1; sync++) {
                    s_mode3 = true;
                    s_first_gap_us = k_gap_us[gi];
                    bsl_pins_take();
                    bsl_reset_into_rom(sync != 0, k_settle_ms[wi]);
                    if (!bsl_pio_start(BSL_FROM_DEV, k_pio_hz[hi]))
                        continue;
                    s_use_pio = true;
                    uint8_t scan_log[16];
                    int scan_n = bsl_diag_cmd(CC13_BSL_CMD_GET_STATUS,
                                              scan_log, (int)sizeof scan_log,
                                              false, true, NULL, NULL);
                    bool interesting = false;
                    for (int i = 0; i < scan_n; i++)
                        if (scan_log[i] != 0xFF && scan_log[i] != 0x00)
                            interesting = true;
                    pio_tries++;
                    if (interesting) {
                        pio_nontrivial++;
                        printf("    hz=%u gap=%uus wait=%ums sync=%d:",
                               k_pio_hz[hi], k_gap_us[gi], k_settle_ms[wi], sync);
                        for (int i = 0; i < scan_n; i++) printf(" %02X", scan_log[i]);
                        printf("\n");
                    }
                    bsl_pio_stop();
                }
            }
        }
    }
    printf("  PIO时序扫描汇总: %u 次，非 FF/00 回应 %u 次\n",
           pio_tries, pio_nontrivial);
    s_first_gap_us = 500u;
    bsl_pins_take();

    for (int v = 0; v < 16; v++) {
        s_mode3                 = (v & 1) != 0;
        const bool sync_high    = (v & 2) != 0;
        const bool fss_per_byte = (v & 4) != 0;
        const bool first_gap    = (v & 8) != 0;
        s_half_us = 5u;

        bsl_pins_take();
        bsl_reset_into_rom(sync_high, 80u);
        uint8_t log[16];
        int n = bsl_diag_cmd(CC13_BSL_CMD_PING, log, (int)sizeof log,
                             fss_per_byte, first_gap, NULL, NULL);

        bool acked = false, any = false;
        for (int i = 0; i < n; i++) {
            if (log[i] == CC13_BSL_ACK) acked = true;
            if (log[i] != 0xFF && log[i] != 0x00) any = true;
        }

        printf("  mode%d sync=%d FSS=%-4s 延时=%d 回读:",
               s_mode3 ? 3 : 0, sync_high,
               fss_per_byte ? "逐字节" : "整包", first_gap);
        for (int i = 0; i < n; i++) printf(" %02X", log[i]);
        printf("%s\n", acked ? "   ← ACK(0xCC)!" : (any ? "   ← 有非平凡回应" : ""));
    }

    /* ROM 会选择最先被访问的 UART0/SSI0。UART0_RX(DIO2) 在本板悬空，
     * 所以扫描 RESET 释放到首个 SSI 字节的等待，排除等待太久期间 UART
     * 浮空噪声抢先选中接口。这里固定使用 TRM 指定的 mode3、整包 FSS 和
     * 首字节延时，只改变等待时间与 backdoor 电平。 */
    static const unsigned k_wait_ms[] = { 0u, 1u, 2u, 5u, 10u, 20u, 40u, 80u };
    printf("  RESET释放→SSI首字节等待扫描（mode3/FSS整包/首字节延时）:\n");
    for (size_t w = 0; w < sizeof k_wait_ms / sizeof k_wait_ms[0]; w++) {
        for (int sync_high = 0; sync_high <= 1; sync_high++) {
            s_mode3 = true;
            s_half_us = 5u;
            bsl_pins_take();
            bsl_reset_into_rom(sync_high != 0, k_wait_ms[w]);
            uint8_t log[16];
            int first_up = -1, first_dn = -1;
            int n = bsl_diag_cmd(CC13_BSL_CMD_PING, log, (int)sizeof log,
                                 false, true, &first_up, &first_dn);

            printf("  wait=%ums sync=%d DIO8=%d/%d 回读:", k_wait_ms[w],
                   sync_high, first_up, first_dn);
            for (int i = 0; i < n; i++) printf(" %02X", log[i]);
            printf("\n");
        }
    }

    /* 收工：诊断最后一次尝试可能以 backdoor 高电平把目标留在 ROM，且
     * bsl_pins_take() 已把 GPIO10..13 从 SPI1 pinmux 改成 SIO。必须显式
     * 恢复 SPI1 并在 DIO13 下拉状态下再复位；spim_poll() 本身不会重配
     * pinmux。 */
    spim_hw_init();
    printf("cc13-bsl: DIAG-DONE\n");
}


/* ══ 包收发 ═══════════════════════════════════════════════════════
 *
 * SSI 是全双工：我们每送一个字节，同时收回一个字节。TRM §10.2.1 说
 * 「Neither device transfers a nonzero byte until it has received a response
 * after transmitting a packet」——所以"读到一串 0"是正常的等待态，必须继续
 * 打时钟，不能当成没人应答。
 *
 * 所有等待都有**明确上界**：本项目没开看门狗，死循环只能靠断电恢复。
 */

/* 空转直到收到一个非零字节。返回该字节，超时返回 -1。 */
static int bsl_wait_nonzero(int max_bytes)
{
    for (int i = 0; i < max_bytes; i++) {
        uint8_t r = bsl_xfer(0x00);
        if (r != 0x00 && r != 0xFF) return r;
    }
    return -1;
}

/* 发一个包，然后等 ACK。first=true 时在首字节后插延时（TRM §10.2.2.2：
 * 器件收到第一个字节后才去配置 SSI0_TX 输出脚，主机得给它这个时间）。 */
static cc13_bsl_result_t bsl_send_pkt(const uint8_t *pkt, size_t len, bool first)
{
    uint8_t trace[20];
    size_t trace_n = 0;
    bsl_fss_assert();
    for (size_t i = 0; i < len; i++) {
        const uint8_t rx = bsl_xfer(pkt[i]);
        if (trace_n < sizeof trace) trace[trace_n++] = rx;
        if (i == 0 && first) sleep_us(500);
    }
    int r = -1;
    const uint8_t cmd = len >= 3 ? pkt[2] : 0u;
    const size_t wait_bytes = cc13_bsl_response_wait_bytes(cmd);
    for (size_t i = 0; i < wait_bytes; i++) {
        const uint8_t rx = bsl_xfer(0x00);
        if (trace_n < sizeof trace) trace[trace_n++] = rx;
        if (rx != 0x00 && rx != 0xFF) {
            r = rx;
            break;
        }
    }
    gpio_put(BSL_FSS, 1);

    if (r < 0) {
        printf("cc13-bsl: cmd=0x%02X no ACK, raw:",
               cmd);
        for (size_t i = 0; i < trace_n; i++) printf(" %02X", trace[i]);
        printf("\n");
        return CC13_BSL_ERR_LINK;
    }
    if ((uint8_t)r == CC13_BSL_NACK) return CC13_BSL_ERR_PROTOCOL;
    if ((uint8_t)r != CC13_BSL_ACK)  return CC13_BSL_ERR_PROTOCOL;
    return CC13_BSL_OK;
}

/* 收一个器件发来的包（size/checksum/data），校验后回 ACK。
 * 返回 data 长度（= size-2），失败返回负的 cc13_bsl_result_t。 */
static int bsl_recv_pkt(uint8_t *out, size_t cap)
{
    bsl_fss_assert();
    int size = bsl_wait_nonzero(256);
    if (size < 3 || (size_t)size - 2u > cap) {
        gpio_put(BSL_FSS, 1);
        return -(int)CC13_BSL_ERR_PROTOCOL;
    }
    uint8_t pkt[CC13_BSL_MAX_PACKET];
    pkt[0] = (uint8_t)size;
    for (int i = 1; i < size; i++) pkt[i] = bsl_xfer(0x00);
    /* 收全了才回 ACK——先 ACK 再校验等于承认了还没看过的东西。 */
    bool ok = cc13_bsl_pkt_valid(pkt, (size_t)size);
    if (ok) (void)bsl_xfer(CC13_BSL_ACK);
    else    (void)bsl_xfer(CC13_BSL_NACK);
    gpio_put(BSL_FSS, 1);

    if (!ok) return -(int)CC13_BSL_ERR_PROTOCOL;
    for (int i = 2; i < size; i++) out[i - 2] = pkt[i];
    return size - 2;
}

static cc13_bsl_result_t bsl_cmd(uint8_t cmd, const uint8_t *params,
                                 size_t n, bool first)
{
    uint8_t pkt[CC13_BSL_MAX_PACKET];
    size_t len = cc13_bsl_pkt_build(pkt, sizeof pkt, cmd, params, n);
    if (!len) return CC13_BSL_ERR_SIZE;
    return bsl_send_pkt(pkt, len, first);
}

/* GET_STATUS：返回状态码，链路/协议出错返回 -1。 */
static int bsl_get_status(void)
{
    if (bsl_cmd(CC13_BSL_CMD_GET_STATUS, NULL, 0, false) != CC13_BSL_OK)
        return -1;
    uint8_t data[8];
    int n = bsl_recv_pkt(data, sizeof data);
    if (n != 1) return -1;
    return data[0];
}

/* 发命令并确认它真的成功了。手册反复强调：ACK 只代表"包收到了"，
 * 命令到底成没成功必须再问一次 GET_STATUS（§10.2.3.2/§10.2.3.3）。 */
static cc13_bsl_result_t bsl_cmd_checked(uint8_t cmd, const uint8_t *params,
                                         size_t n, cc13_bsl_result_t on_fail)
{
    cc13_bsl_result_t r = bsl_cmd(cmd, params, n, false);
    if (r != CC13_BSL_OK) return r;
    int st = bsl_get_status();
    if (st < 0) return CC13_BSL_ERR_LINK;
    return (st == (int)CC13_BSL_RET_SUCCESS) ? CC13_BSL_OK : on_fail;
}

/* ══ 流式烧录状态 ═════════════════════════════════════════════════ */

static bool     s_active;
static uint32_t s_total, s_sent;
static uint32_t s_crc;
static uint8_t  s_buf[CC13_BSL_MAX_DATA];
static size_t   s_buf_len;

const char *cc13_bsl_result_str(cc13_bsl_result_t r)
{
    switch (r) {
    case CC13_BSL_OK:           return "OK";
    case CC13_BSL_ERR_SIZE:     return "镜像长度非法";
    case CC13_BSL_ERR_LINK:     return "无应答(没进 bootloader/模式或接线不对)";
    case CC13_BSL_ERR_PROTOCOL: return "包不自洽(长度或校验和)";
    case CC13_BSL_ERR_TARGET:   return "目标芯片身份不是 CC1312R";
    case CC13_BSL_ERR_ERASE:    return "擦除被拒";
    case CC13_BSL_ERR_PROGRAM:  return "写入失败(GET_STATUS 非 SUCCESS)";
    case CC13_BSL_ERR_CRC:      return "CRC32 不匹配(先查多项式假设，别急着重擦)";
    case CC13_BSL_ERR_STATE:    return "调用顺序不对";
    default:                    return "?";
    }
}

bool cc13_bsl_active(void) { return s_active; }

static void bsl_release(void)
{
    s_active = false;
    /* backdoor 脚交还高阻+下拉：留着高电平会让下次复位又进 bootloader。 */
    gpio_set_dir(PIN_SUBG_SYNC, GPIO_IN);
    gpio_pull_down(PIN_SUBG_SYNC);
    /* bsl_pins_take() 把 GPIO10..13 全部切到了 SIO。仅放开 RESET/SYNC
     * 不足以回到产品态：后续 spim_poll 会继续在错误 pinmux 上产生“全零
     * 帧”。统一重建 SPI1/CS/IRQ/RESET，并在已下拉的 backdoor 下复位 U10。 */
    spim_hw_init();
}

cc13_bsl_result_t cc13_bsl_begin(uint32_t total_len)
{
    if (total_len == 0 || total_len > CC13_FLASH_BYTES) return CC13_BSL_ERR_SIZE;
    if (total_len & 3u) return CC13_BSL_ERR_SIZE;     /* flash 按字编程 */

    bsl_pins_take();
    s_active = true;
    bsl_reset_into_rom(true, 80u);       /* backdoor 拉高再复位 */

    /* PING 是唯一一条"只看 ACK"的命令——此时还没有状态可问。 */
    cc13_bsl_result_t r = bsl_cmd(CC13_BSL_CMD_PING, NULL, 0, true);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    /* 任何擦除动作前先读取 ROM 身份。CC13x2 ROM 返回 FCFG1.USER_ID；
     * 本板 CC1312R1F3RGZ 实测为 0x30828000，协议层按 TI chipinfo.c 的
     * 型号字段核对。这样即使 RP2040 身份正确，错误的 U10 也不会被误擦。 */
    r = bsl_cmd(CC13_BSL_CMD_GET_CHIP_ID, NULL, 0, false);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }
    uint8_t chip_id_bytes[4];
    const int chip_id_n = bsl_recv_pkt(chip_id_bytes, sizeof chip_id_bytes);
    if (chip_id_n != 4) { bsl_release(); return CC13_BSL_ERR_PROTOCOL; }
    const uint32_t chip_id = cc13_bsl_get_be32(chip_id_bytes);
    printf("cc13-bsl: GET_CHIP_ID=0x%08lX part=0x%04lX\n",
           (unsigned long)chip_id, (unsigned long)(chip_id & 0xFFFFu));
    if (!cc13_bsl_chip_id_is_cc1312(chip_id)) {
        bsl_release();
        return CC13_BSL_ERR_TARGET;
    }

    /* 整片擦除。BANK_ERASE 受 CCFG 的 BANK_ERASE_DIS 管；被拒时不要自作聪明
     * 退回逐扇区擦——那只会把"配置不允许"伪装成"擦了一半"。 */
    r = bsl_cmd_checked(CC13_BSL_CMD_BANK_ERASE, NULL, 0, CC13_BSL_ERR_ERASE);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    uint8_t p[8];
    cc13_bsl_put_be32(p + 0, 0x00000000u);      /* flash 从 0 开始（§10.2.3.2）*/
    cc13_bsl_put_be32(p + 4, total_len);
    r = bsl_cmd_checked(CC13_BSL_CMD_DOWNLOAD, p, 8, CC13_BSL_ERR_PROGRAM);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    s_total = total_len; s_sent = 0; s_buf_len = 0;
    s_crc = 0xFFFFFFFFu;
    return CC13_BSL_OK;
}

/* 把攒够的一片发出去。 */
static cc13_bsl_result_t bsl_flush(void)
{
    if (s_buf_len == 0) return CC13_BSL_OK;
    cc13_bsl_result_t r = bsl_cmd_checked(CC13_BSL_CMD_SEND_DATA,
                                          s_buf, s_buf_len,
                                          CC13_BSL_ERR_PROGRAM);
    if (r != CC13_BSL_OK) return r;
    s_sent += (uint32_t)s_buf_len;
    s_buf_len = 0;
    return CC13_BSL_OK;
}

cc13_bsl_result_t cc13_bsl_feed(const uint8_t *data, size_t n)
{
    if (!s_active) return CC13_BSL_ERR_STATE;
    if (s_sent + s_buf_len + n > s_total) return CC13_BSL_ERR_SIZE;

    s_crc = cc13_bsl_crc32_update(s_crc, data, n);
    while (n) {
        size_t room = CC13_BSL_MAX_DATA - s_buf_len;
        size_t take = (n < room) ? n : room;
        for (size_t i = 0; i < take; i++) s_buf[s_buf_len + i] = data[i];
        s_buf_len += take; data += take; n -= take;
        if (s_buf_len == CC13_BSL_MAX_DATA) {
            cc13_bsl_result_t r = bsl_flush();
            if (r != CC13_BSL_OK) { bsl_release(); return r; }
        }
    }
    return CC13_BSL_OK;
}

cc13_bsl_result_t cc13_bsl_finish(void)
{
    if (!s_active) return CC13_BSL_ERR_STATE;

    cc13_bsl_result_t r = bsl_flush();
    if (r != CC13_BSL_OK) { bsl_release(); return r; }
    if (s_sent != s_total) { bsl_release(); return CC13_BSL_ERR_SIZE; }

    /* CRC32 校验整段。§10.2.3.8：第三个参数是读重复次数，0 = 只读一遍；
     * 长度必须 > 8，否则器件直接回 0xFFFFFFFF。 */
    uint8_t p[12];
    cc13_bsl_put_be32(p + 0, 0x00000000u);
    cc13_bsl_put_be32(p + 4, s_total);
    cc13_bsl_put_be32(p + 8, 0x00000000u);
    r = bsl_cmd(CC13_BSL_CMD_CRC32, p, 12, false);
    if (r != CC13_BSL_OK) { bsl_release(); return r; }

    uint8_t rx[8];
    int n = bsl_recv_pkt(rx, sizeof rx);
    if (n != 4) { bsl_release(); return CC13_BSL_ERR_PROTOCOL; }
    /* 结果 4 字节 MSB first（§10.2.3.8） */
    const uint32_t dev = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                         ((uint32_t)rx[2] << 8)  |  (uint32_t)rx[3];
    const uint32_t mine = cc13_bsl_crc32_final(s_crc);
    if (dev != mine) {
        printf("cc13-bsl: CRC32 器件=0x%08lX 本地=0x%08lX\n",
               (unsigned long)dev, (unsigned long)mine);
        bsl_release();
        return CC13_BSL_ERR_CRC;
    }

    /* 复位让新固件跑起来。当前 ROM 会在 RESET 时重新采样 DIO13
     * backdoor；begin() 为进 BSL 把它拉高，因此必须先明确拉低再发 RESET，
     * 否则刚写完会再次回到 ROM，要等 RP 应用层下一轮恢复复位才可能启动。 */
    gpio_put(PIN_SUBG_SYNC, 0);
    gpio_set_dir(PIN_SUBG_SYNC, GPIO_OUT);
    sleep_ms(1);
    /* RESET 之后器件不再应答，不要等 ACK。 */
    (void)bsl_cmd(CC13_BSL_CMD_RESET, NULL, 0, false);
    bsl_release();
    return CC13_BSL_OK;
}

void cc13_bsl_abort(void)
{
    if (!s_active) return;
    bsl_release();
}

/* ══ CDC 流式接口 ═════════════════════════════════════════════════
 * 与 cjtag 那条通道同款约定（docs/firmware_update.md）：先 4 字节小端长度，
 * 再是镜像本体。不用终止符——352 KB 镜像里必然出现任何单字节值，拿某个字节
 * 当结束符一定会被截断。
 */

static bool     s_cdc_on;
static uint8_t  s_hdr[4];
static uint8_t  s_hdr_len;
static bool     s_hdr_done;
static bool     s_failed;
static uint32_t s_recv;
/* 主机声明的镜像长度。**失败之后也要留着**：用来把剩下的镜像字节吞完。
 * s_total 是烧录状态机的，begin() 失败时根本没被赋值，不能拿它当吞字节的
 * 计数——那正是下面这个坑的来源。 */
static uint32_t s_cdc_expect;

void cc13_bsl_cdc_start(void)
{
    s_cdc_on = true; s_hdr_len = 0; s_hdr_done = false;
    s_failed = false; s_recv = 0; s_cdc_expect = 0;
    printf("BSL-MODE: 送 4 字节小端长度，再送镜像（最大 %u 字节）\n",
           (unsigned)CC13_FLASH_BYTES);
}

bool cc13_bsl_cdc_busy(void) { return s_cdc_on; }

static void cdc_end(cc13_bsl_result_t r)
{
    s_cdc_on = false;
    if (r == CC13_BSL_OK) printf("BSL-DONE %lu 字节\n", (unsigned long)s_total);
    else printf("BSL-FAIL: %s\n", cc13_bsl_result_str(r));
}

void cc13_bsl_cdc_byte(uint8_t b)
{
    if (!s_cdc_on) return;

    if (!s_hdr_done) {
        s_hdr[s_hdr_len++] = b;
        if (s_hdr_len < 4) return;
        uint32_t len = (uint32_t)s_hdr[0] | ((uint32_t)s_hdr[1] << 8) |
                       ((uint32_t)s_hdr[2] << 16) | ((uint32_t)s_hdr[3] << 24);
        s_hdr_done = true;
        s_cdc_expect = len;
        cc13_bsl_result_t r = cc13_bsl_begin(len);
        if (r != CC13_BSL_OK) {
            /* ⚠ 这里**不能**直接退出烧录模式。主机已经在往下灌镜像了，
             * 一旦回到命令解析，镜像里的字节就会被当命令执行——352 KB 里
             * 必然出现 'B'，那会让 RP2040 进 BOOTSEL（仓库里记过的坑）。
             * 所以转入"吞字节"态，收满声明长度再退出。 */
            printf("BSL-FAIL: %s\n", cc13_bsl_result_str(r));
            s_failed = true;
            if (s_cdc_expect == 0) { s_cdc_on = false; printf("BSL-ABORT\n"); }
            return;
        }
        printf("BSL-RECV %lu 字节...\n", (unsigned long)len);
        return;
    }

    s_recv++;
    if (s_failed) {
        /* 已失败：继续吞掉剩余字节，别让镜像内容被当成命令解析
         * （镜像里必然有 'B'，那会让 RP2040 进 BOOTSEL）。
         * 计数用 s_cdc_expect 而不是 s_total——begin() 失败时后者没被赋值。 */
        if (s_recv >= s_cdc_expect) { s_cdc_on = false; printf("BSL-ABORT\n"); }
        return;
    }

    cc13_bsl_result_t r = cc13_bsl_feed(&b, 1);
    if (r != CC13_BSL_OK) {
        printf("BSL-FAIL @%lu: %s\n", (unsigned long)s_recv,
               cc13_bsl_result_str(r));
        s_failed = true;
        return;
    }
    if (s_recv >= s_cdc_expect) cdc_end(cc13_bsl_finish());
}
