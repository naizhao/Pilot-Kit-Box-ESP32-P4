/*
 * main.c — CC1312R UAT 接收协处理器固件骨架（WP-E T4）。
 *
 * 职责（本骨架）：SPI slave 链路层——按冻结协议 v1.0
 * （PROTOCOL_RP2040_CC1312R_SPI.md）与 RP2040 master 事务对跑。
 * 纯逻辑在 spi_slave.c（host 全测：含 master↔slave 互通）；本文件
 * 只做器件胶水：SSI0 slave mode0、IOC 映射（pinmap_978.md）、IRQ
 * GPIO、主循环的预载/收发/交付节奏。
 *
 * 明确不在本骨架（诚实边界）：
 *   - RF Core / 978 MHz PHY：结构占位，参数待台架实测（协议 §1/
 *     PLAN §5.2——不得宣称任何 RF 性能）；
 *   - SSI DMA：轮询流式收发（8 MHz 下 512 B 事务 512 µs，48 MHz
 *     CPU 轮询跟得上；DMA 优化留后续任务）；
 *   - 看门狗：骨架循环不喂狗——台架前必须补（备注：CC13x2 默认
 *     看门狗在复位后不自动使能，骨架不开启，风险面为台架项）。
 */
#include <stdint.h>
#include <stdbool.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include DeviceFamily_constructPath(driverlib/ssi.h)
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/gpio.h)

#include "spi_slave.h"
#include "rp_cc13xx_codec.h"

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

/* SSI slave 流式收发一个 512 B 定长事务（§2.1）。
 * 进入条件：FSS 即将拉低、TX FIFO 头 8 字节已由调用方预载（§2.2）；
 * 返回条件：整事务完成。阻塞轮询——骨架实现，时序余量见文件头注。 */
static void ssi_txn(uint8_t mosi[CC13S_TXN_LEN],
                    const uint8_t miso_pre[CC13S_TXN_LEN])
{
    size_t tx = 8, rx = 0;                  /* 头 8 字节已在 FIFO */
    while (rx < CC13S_TXN_LEN) {
        uint32_t st = SSIStatus(SSI_BASE);
        if (st & SSI_RX_NOT_EMPTY) {
            uint32_t d = 0;
            SSIDataGet(SSI_BASE, &d);
            mosi[rx++] = (uint8_t)d;
        }
        if ((st & SSI_TX_NOT_FULL) && tx < CC13S_TXN_LEN) {
            SSIDataPut(SSI_BASE, miso_pre[tx++]);
        }
    }
}

static void irq_set(bool level)
{
    GPIO_writeDio(PIN_IRQ, level ? 1 : 0);
}

int main(void)
{
    /* 时钟：SDK 启动默认 48 MHz RCOSC（骨架不调频、不进 standby）。 */
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

    /* SSI0：slave、mode 0（协议 §1）、8-bit、时钟由 master 提供。 */
    SSIConfigSetExpClk(SSI_BASE, 48000000, SSI_FRF_MOTO_MODE_0,
                       SSI_MODE_SLAVE, 0, 8);
    SSIEnable(SSI_BASE);

    cc13s_init(&g_slave, CC13_FW_VER_MAJOR, CC13_FW_VER_MINOR);

    uint8_t miso[CC13S_TXN_LEN];
    uint8_t mosi[CC13S_TXN_LEN];
    for (;;) {
        /* CSN 高（事务间隙）：pending 预载 + IRQ 同步（§1 生命周期）。 */
        cc13s_pending(&g_slave, miso);
        irq_set(cc13s_irq(&g_slave));

        /* §2.2 物理合同：FSS 拉低前 MISO 字节 0 必须已在 TX FIFO——
         * 预载 8 字节 FIFO 头**先于**等待 FSS（master 的 CSN→SCK
         * 亚微秒级，事后预载必致头字节欠载/帧 magic 损坏）。 */
        for (int i = 0; i < 8 && i < CC13S_TXN_LEN; i++) {
            SSIDataPut(SSI_BASE, miso[i]);
        }

        /* 等 FSS 拉低（轮询 CSN 输入电平——FSS 已路由 SSI，此处读
         * IOC 输入状态；骨架轮询，台架后可改中断）。 */
        while (GPIO_readDio(PIN_FSS) != 0) { }

        ssi_txn(mosi, miso);                /* 512 B 全双工（阻塞） */

        /* CSN↑：交付沿清源 + 处理 MOSI + 装载下一 pending（§1/§2.3）。 */
        cc13s_txn_done(&g_slave, mosi);
    }
}
