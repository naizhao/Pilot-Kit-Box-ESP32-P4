# 硬件参考 — 微雪 ESP32-P4-WIFI6-Touch-LCD-4.3

英文版：[`README.md`](README.md)

本目录保存 Pilot Kit Box 当前硬件目标的权威本地资料。

## 当前开发板

| 项目 | 值 |
|---|---|
| 开发板 | 微雪 ESP32-P4-WIFI6-Touch-LCD-4.3，Rev1.2，SKU 33874 |
| 主控 | ESP32-P4NRW32，768 KB SRAM、封装内 32 MB PSRAM |
| 外置 Flash | GD25Q256EYIGR，256 Mbit / 32 MB NOR |
| 无线 | ESP32-C6-MINI-1-N4，通过 4-bit ESP-Hosted SDIO 连接 |
| 显示 | ST7701，4.3 寸 480×800 IPS，2-lane MIPI-DSI；横屏 UI 800×480 |
| 触摸 | GT911，I²C GPIO7/8，复位 GPIO23，中断电阻默认未贴 |
| P4 调试口 | H1 USB-C `USB TO UART`，经 CH343P |
| 原生 USB HS | H2 USB-C `USB` 与 J3-27/25 同网；RTL-SDR 走这组信号 |
| C6 下载口 | P1 1×4 排针 `TX RX IO9 GND` |
| 扩展口 | J3 2×20 排针；接线前必须核对项目 pinout |

ESP32-P4 本身没有无线电。当前固件先启动 C6 上的 ESP-Hosted，再初始化
显示，并通过 C6 使用蓝牙 controller。

官方链接：

- 文档：<https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3>
- 资料：<https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3/Resources-And-Documents>
- 例程：<https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3>

## 当前 Pilot Kit 接线

| 模块 | 连接 | 固件状态 |
|---|---|---|
| RTL-SDR FC0013 | 载板 USB-A 插头走 J3-27/25（原生 USB 2.0 HS），H2 空置；裸板调试时改接 H2 加 USB-C OTG 转接头或 Hub | 已集成 |
| ST7701 显示 | 板载固定 MIPI-DSI，2 lane @ 500 Mbit/s | 已集成 |
| GT911 触摸 | 板载 GPIO7/8、复位 GPIO23；轮询 | 已集成，当前只取第一触点 |
| BNO085 IMU | GPIO7/8、复位 GPIO28、INT GPIO34、地址 0x4A | 已集成，轮询 |
| BMP388 | GPIO7/8、INT 接 GPIO31（载板网络 `BARO_INT`） | 已集成，轮询 |
| GPS | P4 TX GPIO49、P4 RX GPIO51；可选 PPS GPIO50 | UART/RMC 已集成；PPS 未实现 |
| ESP32-C6 | P4 GPIO14–19 SDIO、GPIO54 EN | Wi-Fi/BLE 传输已集成 |
| 音频 / 摄像头 | 板上硬件具备 | Pilot 固件未初始化 |

## 文档

| 文件 | 用途 |
|---|---|
| [`board_pinout.md`](board_pinout.md) / [`board_pinout-zh_CN.md`](board_pinout-zh_CN.md) | 完整 J3、GPIO0–54、接口及 Pilot 接线参考 |
| [`c6_slave_firmware.md`](c6_slave_firmware.md) / [`c6_slave_firmware-zh_CN.md`](c6_slave_firmware-zh_CN.md) | 使用 P1 首次烧录 C6 slave |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf) | **板级最高依据：**Rev1.2 原理图和装配图 |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md) | 官方资料离线摘要及 BSP 参数 |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf) | 机械尺寸：114.4 × 66.8 mm，4 × M2.5 |
| [`ST7701-datasheet.pdf`](ST7701-datasheet.pdf) | 显示控制器参考 |
| [`GT911-datasheet.pdf`](GT911-datasheet.pdf) | 触摸电气和地址参考 |
| [`GT911-programming-guide.pdf`](GT911-programming-guide.pdf) | 触摸上报及寄存器格式 |

`ESP32-P4-WIFI6-datasheet.pdf` **不是** ESP32-P4 芯片 datasheet，也不是
4.3 寸 Rev1.2 原理图；它描述另一款微雪 ESP32-P4-WIFI6 板，不能用于本
目标的引脚分配。

## 扩展板

| 板卡 | 状态 | 入口 |
|---|---|---|
| 扩展板 **V4**（6 层集成） | 当前硬件目标——布局布线完成，尚未打样 | [`../../hardware/expansion-board-v4/README.md`](../../hardware/expansion-board-v4/README.md)（双语；每份文档均有 EN + `-zh_CN.md`） |
| 扩展板 **V3**（4 层） | 已归档（v3.2 已打样；仍用于固件调试） | [`../../hardware/expansion-board-v3/README.md`](../../hardware/expansion-board-v3/README.md) |

旧 2.4 寸载板及其四按键接线仅作为历史设计源保存在 `docs/jlc/`，不是当前
硬件。
