/*
 * cc13_bsl.h — 经 CC1312R 的 **ROM 串行 bootloader**（SSI0）烧录，不走 cJTAG。
 *
 * 为什么有这条路
 * --------------
 * 交接文档写着「UART ROM bootloader 不可用（DIO_0/1 没接主控），cJTAG 是唯一
 * 恢复通道」。这个前提是错的：TRM SWCU185G §10 说得很清楚——
 *
 *   §10（ROM 概述）「The ROM contains a serial bootloader with **SPI and UART**
 *   support」；§10.1「The bootloader either executes automatically **if no valid
 *   image has been written to the FLASH**, or ... through a configurable GPIO
 *   backdoor.」
 *
 * 也就是说 SPI 也能进 bootloader，而且**空片会自动进**。本盒子的 CC1312R 现在
 * SPI 链路是死的（rx 冻在 1），与"没有可用镜像"一致，正好落在自动进入那一档。
 *
 * 引脚对得上吗
 * ------------
 * ROM bootloader 的 SSI0 引脚是**固定的**（TRM Table 10-2）：
 *
 *   SSI0_CLK = DIO10   SSI0_FSS = DIO11   SSI0_RX = DIO9   SSI0_TX = DIO8
 *
 * 本板（sheet_subghz.py / board_pins.h）：
 *
 *   DIO10 = SUBG_SCK  ← RP2040 GPIO10      ✔ 正好是 SSI0_CLK
 *   DIO11 = SUBG_CSN  ← RP2040 GPIO13      ✔ 正好是 SSI0_FSS
 *   DIO8  = SUBG_MOSI ↔ RP2040 GPIO11      ✘ 板上当"主机输出"，ROM 当**器件输出**
 *   DIO9  = SUBG_MISO ↔ RP2040 GPIO12      ✘ 板上当"主机输入"，ROM 当**器件输入**
 *
 * 时钟和片选这两根**天生就对**——这俩要是错了，软件再怎么写都救不回来。
 * 只有数据两根的方向与 ROM 的固定角色相反：板子是按**应用固件**的 SSI 引脚
 * 分配画的（应用可以用 IOC 自由 mux），而 ROM 的分配改不了。
 *
 * 而方向恰恰是软件能改的：RP2040 的 GPIO 方向随便设，所以这里**不用硬件 SPI**
 * （SPI 外设的引脚角色是固定的，换不过来），全部位脉冲，把两根数据线对调：
 *   往器件送数据 → GPIO12（走 SUBG_MISO 这根网络到 DIO9 = SSI0_RX）
 *   从器件收数据 → GPIO11（走 SUBG_MOSI 这根网络到 DIO8 = SSI0_TX）
 *
 * 帧格式（TRM §10.2.2.2）：Motorola，SPH=1 / SPO=1 → **SPI mode 3**，
 * 时钟上限 4 MHz（48 MHz / 12）。
 *
 * 与 SPI master / cJTAG 互斥：本模块独占 GPIO10~13，调用期间 spim 必须暂停。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TRM Table 10-3：命令码 */
#define BSL_CMD_PING           0x20
#define BSL_CMD_DOWNLOAD       0x21
#define BSL_CMD_SEND_DATA      0x24
#define BSL_CMD_SECTOR_ERASE   0x26
#define BSL_CMD_GET_STATUS     0x23
#define BSL_CMD_RESET          0x25

#define BSL_ACK                0xCC
#define BSL_NAK                0x33

/* 诊断：复位 CC1312R 进 ROM bootloader，发 PING，把收到的原始字节打出来。
 * 一次跑完几个接线/时序变体，上板只需要按一个键。 */
void cc13_bsl_diag(void);
