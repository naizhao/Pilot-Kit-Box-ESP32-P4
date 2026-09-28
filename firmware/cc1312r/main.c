/*
 * main.c — CC1312R UAT 接收协处理器固件（978 MHz RX-only）。
 *
 * 职责：TI RF Driver 连续接收 UAT，按冻结协议
 * （PROTOCOL_RP2040_CC1312R_SPI.md）交给 RP2040 master。
 * 纯逻辑在 spi_slave.c（host 全测：含 master↔slave 互通）；本文件
 * 只做器件胶水：SSI0 slave mode3、IOC 映射（pinmap_978.md）、IRQ
 * GPIO、主循环的预载/收发/交付节奏。
 *
 * RF 命令图只含 RADIO_DIV_SETUP、FS、RX_ADV 与 RX 中的 SET_LEN，
 * 没有发射命令。SSI 使用 1 MHz、mode 3 和 uDMA；主循环只轮询 FSS
 * 边沿并管理事务，硬件 watchdog 覆盖 FSS 卡死。
 */
#include <stdint.h>
#include <stdbool.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/prcm.h)
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include DeviceFamily_constructPath(driverlib/ssi.h)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include DeviceFamily_constructPath(driverlib/udma.h)
#pragma GCC diagnostic pop
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/gpio.h)
#include DeviceFamily_constructPath(driverlib/watchdog.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_ssi.h)
#include DeviceFamily_constructPath(inc/hw_types.h)

#include <NoRTOS.h>
#include <ti/drivers/Power.h>
#include <ti/drivers/dpl/HwiP.h>

#include "spi_slave.h"
#include "rp_cc13xx_codec.h"
#include "uat_rf.h"

/* 引脚（docs/hardware/pinmap_978.md——每条有 PDF 页码取证）：
 * DIO_10 = SSI0 SCLK（slave 输入）
 * DIO_8  = SSI0 MOSI/SSI_D0（master 出 → slave 入，即本侧 RX）
 * DIO_9  = SSI0 MISO/SSI_D1（slave 出，即本侧 TX）
 * DIO_11 = SSI0 FSS（硬件从选，RP2040 CSN）
 * DIO_12 = SUBG_IRQ（本侧输出，电平高有效） */
#define PIN_SCLK     IOID_10
#define PIN_MOSI_RX  IOID_8
#define PIN_MISO_TX  IOID_9
#define PIN_FSS      IOID_11
#define PIN_IRQ      IOID_12

#define SSI_BASE     SSI0_BASE

static cc13s_slave_t g_slave;
static uint8_t g_miso[CC13S_TXN_LEN];
static uint8_t g_mosi[CC13S_TXN_LEN];

/* The official NoRTOS linker script fixes the complete uDMA control table at
 * 0x20001800 and gives SSI0 RX/TX their architectural channel offsets.  Use
 * the SDK allocation macro so the linker, driverlib and silicon all agree on
 * those addresses; ordinary BSS allocation is not sufficiently aligned. */
#define UDMA_CONTROL_BASE 0x20001800u
__attribute__((section(".dmaSpi0RxControlTableEntry"), used))
static volatile tDMAControlTable g_dma_spi0_rx = {0};
__attribute__((section(".dmaSpi0TxControlTableEntry"), used))
static volatile tDMAControlTable g_dma_spi0_tx = {0};

#define WATCHDOG_RELOAD_TICKS (2u * 48000000u)
#define CC13_ASYNC_RF_INTERNAL 0x04u

static void irq_set(bool level);

static void watchdog_feed(void)
{
    WatchdogIntClear();
}

static void rf_frame_received(const uat_phy_packet_t *packet, void *user)
{
    cc13s_slave_t *slave = (cc13s_slave_t *)user;
    uintptr_t key = HwiP_disable();
    (void)cc13s_push_rx(slave, packet->kind, packet->frame,
                        packet->frame_len, packet->rssi_dbm,
                        packet->cc_time);
    /* RF callback runs asynchronously to the FSS polling loop.  Raising the
     * physical line here avoids waiting indefinitely for a main-loop pass
     * when RP has no reason to initiate another transaction yet.  Keep the
     * GPIO write inside the same critical section as the source mutation:
     * otherwise main could publish a stale low after this callback's high. */
    irq_set(cc13s_irq(slave));
    HwiP_restore(key);
}

/* TI SPI driver uses these test registers to empty the transmit FIFO between
 * peripheral-mode transfers.  They are documented by the SDK driver but not
 * exported by the public driverlib header. */
#define SSI_O_TCR               0x00000080u
#define SSI_O_TDR               0x0000008Cu
#define SSI_TCR_TESTFIFO_ENABLE 0x00000002u

static void ssi_flush_fifos(void)
{
    while (HWREG(SSI_BASE + SSI_O_SR) & SSI_RX_NOT_EMPTY) {
        (void)HWREG(SSI_BASE + SSI_O_DR);
    }
    HWREG(SSI_BASE + SSI_O_TCR) = SSI_TCR_TESTFIFO_ENABLE;
    while (!(HWREG(SSI_BASE + SSI_O_SR) & SSI_TX_EMPTY)) {
        (void)HWREG(SSI_BASE + SSI_O_TDR);
    }
    HWREG(SSI_BASE + SSI_O_TCR) = 0;
}

static void ssi_dma_init(void)
{
    /* Make the fixed linker sections live under --gc-sections and start from
     * a known STOP descriptor before the controller is enabled. */
    g_dma_spi0_rx.pvSrcEndAddr = NULL;
    g_dma_spi0_rx.pvDstEndAddr = NULL;
    g_dma_spi0_rx.ui32Control = 0;
    g_dma_spi0_rx.ui32Spare = 0;
    g_dma_spi0_tx.pvSrcEndAddr = NULL;
    g_dma_spi0_tx.pvDstEndAddr = NULL;
    g_dma_spi0_tx.ui32Control = 0;
    g_dma_spi0_tx.ui32Spare = 0;
    uDMAEnable(UDMA0_BASE);
    uDMAControlBaseSet(UDMA0_BASE, (void *)UDMA_CONTROL_BASE);
    uDMAChannelAttributeDisable(UDMA0_BASE, UDMA_CHAN_SSI0_RX,
                                UDMA_ATTR_ALL);
    uDMAChannelAttributeDisable(UDMA0_BASE, UDMA_CHAN_SSI0_TX,
                                UDMA_ATTR_ALL);
}

static void ssi_dma_arm(uint8_t mosi[CC13S_TXN_LEN],
                        const uint8_t miso[CC13S_TXN_LEN])
{
    SSIDMADisable(SSI_BASE, SSI_DMA_RX | SSI_DMA_TX);
    uDMAChannelDisable(UDMA0_BASE, UDMA_CHAN_SSI0_RX);
    uDMAChannelDisable(UDMA0_BASE, UDMA_CHAN_SSI0_TX);
    ssi_flush_fifos();

    uDMAChannelControlSet(UDMA0_BASE, UDMA_CHAN_SSI0_RX | UDMA_PRI_SELECT,
                          UDMA_SIZE_8 | UDMA_SRC_INC_NONE |
                          UDMA_DST_INC_8 | UDMA_ARB_4);
    uDMAChannelTransferSet(UDMA0_BASE,
                           UDMA_CHAN_SSI0_RX | UDMA_PRI_SELECT,
                           UDMA_MODE_BASIC,
                           (void *)(SSI_BASE + SSI_O_DR), mosi,
                           CC13S_TXN_LEN);
    uDMAChannelControlSet(UDMA0_BASE, UDMA_CHAN_SSI0_TX | UDMA_PRI_SELECT,
                          UDMA_SIZE_8 | UDMA_SRC_INC_8 |
                          UDMA_DST_INC_NONE | UDMA_ARB_4);
    uDMAChannelTransferSet(UDMA0_BASE,
                           UDMA_CHAN_SSI0_TX | UDMA_PRI_SELECT,
                           UDMA_MODE_BASIC, (void *)miso,
                           (void *)(SSI_BASE + SSI_O_DR), CC13S_TXN_LEN);

    /* RX first, then TX.  Enabling TX while FSS is still high lets DMA fill
     * the eight-byte FIFO before the master produces its first clock edge. */
    SSIDMAEnable(SSI_BASE, SSI_DMA_RX | SSI_DMA_TX);
    uDMAChannelEnable(UDMA0_BASE, UDMA_CHAN_SSI0_RX);
    uDMAChannelEnable(UDMA0_BASE, UDMA_CHAN_SSI0_TX);
}

/* SSI slave 收发一个 512 B 定长事务（§2.1）。两路 uDMA 在 FSS 下降前
 * 装好，因此 RF Driver 的 HWI/SWI 即使打断 CPU，也不会让八字节 FIFO
 * 溢出或欠载。CPU 只负责等待物理事务边界。 */
static void ssi_txn(uint8_t mosi[CC13S_TXN_LEN],
                    const uint8_t miso_pre[CC13S_TXN_LEN])
{
    for (size_t i = 0; i < CC13S_TXN_LEN; i++) mosi[i] = 0;
    ssi_dma_arm(mosi, miso_pre);
    while (GPIO_readDio(PIN_FSS) != 0) watchdog_feed();
    while (GPIO_readDio(PIN_FSS) == 0) watchdog_feed();

    /* The physical completion edge is CSN↑, not DMA STOP.  In peripheral
     * mode the SSI receive threshold can leave a short tail in its FIFO, so
     * waiting for an exact 512-item basic descriptor deadlocks even though
     * the master has finished.  Freeze DMA, derive the completed prefix from
     * its remaining count, then copy only that FIFO tail into the zero-filled
     * buffer.  The protocol CRC still rejects every genuinely short frame. */
    SSIDMADisable(SSI_BASE, SSI_DMA_RX | SSI_DMA_TX);
    uDMAChannelDisable(UDMA0_BASE, UDMA_CHAN_SSI0_RX);
    uDMAChannelDisable(UDMA0_BASE, UDMA_CHAN_SSI0_TX);
    uint32_t remaining = uDMAChannelSizeGet(
        UDMA0_BASE, UDMA_CHAN_SSI0_RX | UDMA_PRI_SELECT);
    size_t received = remaining <= CC13S_TXN_LEN
        ? CC13S_TXN_LEN - remaining : 0;
    while ((HWREG(SSI_BASE + SSI_O_SR) & SSI_RX_NOT_EMPTY) &&
           received < CC13S_TXN_LEN) {
        mosi[received++] = (uint8_t)HWREG(SSI_BASE + SSI_O_DR);
    }
}

static void irq_set(bool level)
{
    GPIO_writeDio(PIN_IRQ, level ? 1 : 0);
}

int main(void)
{
    /* TI Power.h requires Power_init() before every other TI-Drivers API and
     * before OS initialization.  The first RX-only image skipped it, so
     * RF_open() entered the Power dependency path with uninitialised manager
     * state; the hardware then reset before it could answer the first SPI
     * HELLO (RP observed legal=0/noframe increasing). */
    (void)Power_init();

    /* RF Driver 的 HWI/SWI/DPL 必须先由 NoRTOS 初始化。Power policy 在
     * ti_platform.c 明确关闭：本固件持续 RX，不进入 standby。 */
    NoRTOS_start();

    /* 时钟：SDK 启动默认 48 MHz RCOSC（骨架不调频、不进 standby）。
     *
     * 电源域/外设时钟（审计 round-WP-E-1 P1-1）：CC13x2 复位后
     * PERIPH/SERIAL 域与 GPIO/SSI0 运行时钟默认关闭，裸启动代码
     * （startup_gcc.c）只做 trim/BSS/FPU——不使能就访问 GPIO/SSI
     * 是未定义行为（实机可能首访即停）。SDK 的域就绪查询是
     * PRCMPowerDomainsAllOn()（全部就绪布尔值，非逐域状态）。 */
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
    /* PRCMPowerDomainsAllOn 返回域状态位图（POWER_ON=0x1），须与
     * PRCM_DOMAIN_POWER_ON 比较而非当布尔用（验证轮抓过空转反例：
     * POWER_OFF=0x2 也非零，!x 恒假）。 */
    while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)
           != PRCM_DOMAIN_POWER_ON) { }
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMPeripheralRunEnable(PRCM_PERIPH_SSI0);
    PRCMPeripheralRunEnable(PRCM_PERIPH_UDMA);
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }

    IOCPortConfigureSet(PIN_SCLK, IOC_PORT_MCU_SSI0_CLK,
                        IOC_STD_INPUT | IOC_HYST_ENABLE);
    IOCPortConfigureSet(PIN_MOSI_RX, IOC_PORT_MCU_SSI0_RX,
                        IOC_STD_INPUT | IOC_HYST_ENABLE);
    IOCPortConfigureSet(PIN_MISO_TX, IOC_PORT_MCU_SSI0_TX,
                        IOC_STD_OUTPUT);
    IOCPortConfigureSet(PIN_FSS, IOC_PORT_MCU_SSI0_FSS,
                        IOC_STD_INPUT | IOC_HYST_ENABLE);

    /* IRQ：DIO_12 输出，低（无源）。 */
    IOCPinTypeGpioOutput(PIN_IRQ);
    irq_set(false);

    /* SSI0：slave、mode 3、8-bit、时钟由 master 提供。mode 3 与同一组
     * 物理线上的 ROM BSL 实机配置一致；应用 mode 0 无法形成合法帧。
     * bitrate 参数在 slave 模式不驱动时钟，但参与 SDK 的分频计算
     * （ssi.c 直接 ui32SSIClk/ui32BitRate——传 0 是除零，审计
     * round-WP-E-1 P1-2）；双方传入 1 Mbps。TI 要求 slave 模式
     * FSSI >= 12×bitrate：48 MHz/12 = 4 MHz 上限；实板链路当前以
     * 1 MHz 留出完整事务与 DMA 重装载裕量，
     * bitrate 参数在 slave 模式仅用于合法性/分频检查。 */
    SSIConfigSetExpClk(SSI_BASE, 48000000, SSI_FRF_MOTO_MODE_3,
                       SSI_MODE_SLAVE, 1000000, 8);
    SSIEnable(SSI_BASE);
    ssi_dma_init();

    cc13s_init(&g_slave, CC13_FW_VER_MAJOR, CC13_FW_VER_MINOR);
    /* 48 MHz / 96 M = 2 s 首超时；若主循环无法清中断，再次超时复位。
     * 在 RF_open/FS 之前启用，使 RF Core 启动卡死也在覆盖范围内；此外
     * 覆盖 master 把 FSS 长期拉低、SSI 状态机卡死等主线程故障。 */
    WatchdogUnlock();
    WatchdogReloadSet(WATCHDOG_RELOAD_TICKS);
    WatchdogResetEnable();
    WatchdogEnable();
    WatchdogLock();

    bool rf_started = false;
    uint32_t rf_health_published = 0;

    for (;;) {
        /* CSN 高（事务间隙）：pending 预载 + IRQ 同步（§1 生命周期）。 */
        uintptr_t key = HwiP_disable();
        cc13s_pending(&g_slave, g_miso);
        irq_set(cc13s_irq(&g_slave));
        HwiP_restore(key);

        ssi_txn(g_mosi, g_miso);            /* 512 B 全双工（阻塞） */

        /* CSN↑：交付沿清源 + 处理 MOSI + 装载下一 pending（§1/§2.3）。 */
        key = HwiP_disable();
        cc13s_txn_done(&g_slave, g_mosi);
        HwiP_restore(key);
        /* RF Driver power-up/FS may take multiple RF SWI turns.  Starting it
         * before the pipelined HELLO response is physically delivered can
         * starve the next SSI transaction and make a healthy slave look like
         * all-zero MISO.  Two legal transactions are the first point at which
         * the master has clocked out the initial HELLO response. */
        if (!rf_started && g_slave.state == CC13S_LINKED &&
            g_slave.legal_frames >= 2u) {
            rf_started = true;
            if (!uat_rf_start(rf_frame_received, &g_slave)) {
                cc13s_raise_async(&g_slave, CC13_ASYNC_RF_INTERNAL);
            }
        }
        if (rf_started) {
            uint16_t rf_fault = uat_rf_poll();
            if (rf_fault) {
                cc13s_raise_async_detail(&g_slave, CC13_ASYNC_RF_INTERNAL,
                                          rf_fault);
            }
            if (uat_rf_reset_required()) {
                /* RF_cancelCmd failed terminally or its documented completion
                 * callback never arrived.  Continuing the loop would keep
                 * feeding the watchdog forever while 978 RX stays dead. */
                SysCtrlSystemReset();
            }
            rp_cc13xx_rf_health_t rf_health;
            uint32_t rf_health_generation;
            uat_rf_health_status(&rf_health, &rf_health_generation);
            if (rf_health_generation != rf_health_published) {
                key = HwiP_disable();
                cc13s_publish_rf_health(&g_slave, &rf_health);
                irq_set(cc13s_irq(&g_slave));
                HwiP_restore(key);
                rf_health_published = rf_health_generation;
            }
        }
        watchdog_feed();
    }
}
