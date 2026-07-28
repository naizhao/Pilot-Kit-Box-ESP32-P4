# 硬件参考 — 微雪 ESP32-P4-WIFI6-Touch-LCD-4.3

英文版：[`README.md`](README.md)

本目录保存 Pilot Kit Box 当前硬件目标的厂商资料、GPIO 分配、C6 协处理器 bring-up 记录和接线说明。

## 开发板识别

| 项目 | 值 |
|---|---|
| 厂商 | Waveshare / 微雪 |
| 产品型号 | **ESP32-P4-WIFI6-Touch-LCD-4.3**（SKU 33874） |
| 主控 | **ESP32-P4NRW32**，RISC-V 双核，360 MHz，768 KB SRAM，32 MB stacked PSRAM |
| 无线协处理器 | **ESP32-C6-MINI-1**，通过 SDIO 连接 P4，提供 Wi-Fi 6 / BLE 5 |
| 显示 | 4.3 寸 480×800 IPS，ST7701，2-lane MIPI-DSI，横屏按 800×480 使用 |
| 触摸 | GT911 五点电容触摸，I²C GPIO7/8 |
| Wiki | <https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4.3/> |
| 资料页 | <https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4.3/Resources-And-Documents/> |
| 官方例程 | <https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3> |

ESP32-P4 本身没有无线能力；当前固件通过 ESP-Hosted SDIO 使用板载 C6 的 BLE controller。

## 当前外接硬件

| 模块 | 接线 | 状态 |
|---|---|---|
| RTL-SDR FC0013 USB dongle | 板载 P1 USB 2.0 HS OTG 口，专用 USB HS PHY | 已接入 |
| ST7701 480×800 MIPI-DSI LCD | 2 lane @ 500 Mbps，DPI 30 MHz，PPA 转 800×480 | bring-up 进行中 |
| GT911 电容触摸 | SDA GPIO7，SCL GPIO8，RST GPIO23，INT 默认未贴 | 驱动待接 |
| GY-BN008X / BNO085 IMU | SDA GPIO7，SCL GPIO8，INT GPIO20，RST GPIO21 | 已验证 |
| GT-U8 GPS | P4 TX GPIO32，GPS TX → P4 RX GPIO51，PPS GPIO46 | 因 BL_EN 冲突已重映射 |
| BMP388 气压计 | SDA GPIO7，SCL GPIO8，可选 INT GPIO31 | 因 LCD RESET 冲突已重映射 |
| ESP32-C6 hosted slave 固件 | H4 UART header，首次设置用外部 USB-UART 烧录 | BLE 已跑通 |

## 本目录文件

| 文件 | 内容 |
|---|---|
| [`board_pinout.md`](board_pinout.md) | 英文 GPIO 分配、板载外设、LCD / IMU / 按钮接线 |
| [`board_pinout-zh_CN.md`](board_pinout-zh_CN.md) | 板卡引脚文档中文版本 |
| [`c6_slave_firmware.md`](c6_slave_firmware.md) | ESP32-C6 hosted slave 首次烧录英文指南 |
| [`c6_slave_firmware-zh_CN.md`](c6_slave_firmware-zh_CN.md) | ESP32-C6 hosted slave 首次烧录中文指南 |
| [`c6_bringup_status.md`](c6_bringup_status.md) | P4 <-> C6 SDIO / VHCI / NimBLE bring-up 英文排障记录 |
| [`c6_bringup_status-zh_CN.md`](c6_bringup_status-zh_CN.md) | C6 bring-up 中文排障记录 |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf) | **4.3″ 触摸板原理图** —— LCD / 触摸 / 背光引脚的权威依据 |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf) | 4.3″ 外形图：114.4 × 66.8 mm，VA 94.4 × 56.96 mm，4 × M2.5 孔距 92 × 50 mm |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3.zip`](ESP32-P4-WIFI6-Touch-LCD-4.3.zip) | 官方机械资料包（尺寸 PDF + STEP + DXF），SHA-256 `8209b6aa405d4d3d8a2009e7eb545a4844e456b9cf95d8b8e53529414b03ecaf` |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md) | 厂商 wiki 正文 + 从官方 BSP 读出的面板时序实值 |
| [`ST7701-datasheet.pdf`](ST7701-datasheet.pdf) | LCD 驱动 IC —— 含 MIPI-DSI 章节与 MIPISET1-4 (D0h–D3h) 寄存器 |
| [`GT911-datasheet.pdf`](GT911-datasheet.pdf) | 触摸控制器电气特性与 I²C 寻址 |
| [`GT911-programming-guide.pdf`](GT911-programming-guide.pdf) | 触摸**坐标上报格式** —— datasheet 里没有，只在这份里 |

## 快速链接

- ESP32-P4 官方 datasheet: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf>
- ESP32-P4 技术参考手册: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf>
- 微雪 ESP-IDF 开发与烧录指南: <https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4.3/Development-Environment-Setup-IDF/>
- ESP-Hosted MCU 文档: <https://github.com/espressif/esp-hosted>
- `espressif/esp_wifi_remote` component: <https://components.espressif.com/components/espressif/esp_wifi_remote>

## 为什么本地保存厂商资料

Waveshare wiki 偶尔会在中国大陆以外限流或返回 403。本地保存原理图、接口照片和我们的 pinout 摘要，可以让后续固件和硬件工作不依赖实时网络访问。
