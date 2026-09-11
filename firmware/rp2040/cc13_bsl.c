/* cc13_bsl.c — 见 cc13_bsl.h。 */
#include "cc13_bsl.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include "board_pins.h"

/* ROM bootloader 的固定角色 ↔ 本板网络（推导见头文件）。
 * 注意 TO_DEV / FROM_DEV 是**按 ROM 的角色**命名的，与板上 MOSI/MISO 的名字
 * 正好相反——这正是本模块存在的理由，不要"顺手改回来"。 */
#define BSL_CLK       PIN_SUBG_SCK    /* GPIO10 → DIO10 = SSI0_CLK  */
#define BSL_FSS       PIN_SUBG_CSN    /* GPIO13 → DIO11 = SSI0_FSS  */
#define BSL_TO_DEV    PIN_SUBG_MISO   /* GPIO12 → DIO9  = SSI0_RX   */
#define BSL_FROM_DEV  PIN_SUBG_MOSI   /* GPIO11 → DIO8  = SSI0_TX   */

static unsigned s_half_us = 5;        /* ~100 kHz，远低于 4 MHz 上限 */

/*
 * SPI 模式。TRM §10.2.2.2 白纸黑字写「Motorola format with SPH set to 1 and
 * SPO set to 1」= mode 3，但 **实机不是这样**：
 *
 * PantsForBirds/adsbee（同样是 RP2040 + CC1312 的开源项目，GPLv3——这里只取
 * 事实、不抄代码）在 cc1312.cc 里把 SPI 初始化成 CPOL_0/CPHA_0 = **mode 0**，
 * 而那段「进 bootloader 后切成 CPOL_1/CPHA_1」的代码是**被注释掉的**。也就是
 * 说他们试过手册写的 mode 3，最后跑通的是 mode 0。
 *
 * 手册和实机冲突时以实机为准，但两个都留着当变体试——这正是变体矩阵的用途。
 */
static bool s_mode3 = false;

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

/*
 * 复位进 ROM bootloader。
 *
 * sync 高低两种都试：TRM §10.1.2 的 backdoor 要在**复位那一刻**就把
 * BL_PIN_NUMBER 指定的脚摆在 BL_LEVEL 上。本板 SUBG_SYNC = DIO13 ←
 * RP2040 GPIO15，PINMAP 里本来就标着「bootloader trigger」。
 *
 * ⚠ 空片上 backdoor 其实用不了（CCFG 擦除态 BL_PIN_NUMBER=0xFF、
 * BL_ENABLE=0xFF），指望的是「无有效镜像 → 自动进 bootloader」那一条。
 * sync 当变体试只是为了把「摆错电平反而挡住了自动进入」这种可能排掉。
 */
static void bsl_reset_into_rom(bool sync_high)
{
    gpio_put(BSL_FSS, 1);
    gpio_put(BSL_CLK, s_mode3 ? 1 : 0);

    gpio_init(PIN_SUBG_SYNC);
    gpio_put(PIN_SUBG_SYNC, sync_high ? 1 : 0);
    gpio_set_dir(PIN_SUBG_SYNC, GPIO_OUT);

    gpio_put(PIN_SUBG_RESET, 0);
    sleep_ms(5);
    gpio_put(PIN_SUBG_RESET, 1);
    sleep_ms(80);                     /* 等 ROM 起来并采样 backdoor 脚 */
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
static int bsl_ping(uint8_t *log, int cap, bool fss_per_byte, bool first_gap)
{
    const uint8_t pkt[3] = { 0x03, BSL_CMD_PING, BSL_CMD_PING };  /* size/校验和/命令 */
    int n = 0;

    if (!fss_per_byte) gpio_put(BSL_FSS, 0);
    for (int i = 0; i < 3; i++) {
        if (fss_per_byte) gpio_put(BSL_FSS, 0);
        uint8_t r = bsl_xfer(pkt[i]);
        if (fss_per_byte) gpio_put(BSL_FSS, 1);
        if (n < cap) log[n++] = r;
        if (i == 0 && first_gap) sleep_us(500);   /* 坑 1 */
    }
    /* 继续空转读 ACK：接收方准备好之前可以一直回 0（坑 2）。 */
    for (int i = 0; i < 12 && n < cap; i++) {
        if (fss_per_byte) gpio_put(BSL_FSS, 0);
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

    for (int v = 0; v < 16; v++) {
        s_mode3                 = (v & 1) != 0;
        const bool sync_high    = (v & 2) != 0;
        const bool fss_per_byte = (v & 4) != 0;
        const bool first_gap    = (v & 8) != 0;
        s_half_us = 5u;

        bsl_pins_take();
        bsl_reset_into_rom(sync_high);
        uint8_t log[16];
        int n = bsl_ping(log, (int)sizeof log, fss_per_byte, first_gap);

        bool acked = false, any = false;
        for (int i = 0; i < n; i++) {
            if (log[i] == BSL_ACK) acked = true;
            if (log[i] != 0xFF && log[i] != 0x00) any = true;
        }

        printf("  mode%d sync=%d FSS=%-4s 延时=%d 回读:",
               s_mode3 ? 3 : 0, sync_high,
               fss_per_byte ? "逐字节" : "整包", first_gap);
        for (int i = 0; i < n; i++) printf(" %02X", log[i]);
        printf("%s\n", acked ? "   ← ACK(0xCC)!" : (any ? "   ← 有非平凡回应" : ""));
    }

    /* 收工：把线交还给 SPI master（它会在下一次 spim_poll 里重新配好）。 */
    gpio_put(PIN_SUBG_RESET, 1);
}
