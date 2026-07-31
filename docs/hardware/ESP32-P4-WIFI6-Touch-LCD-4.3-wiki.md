# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 — 硬件资料存档

本文是微雪官方 wiki 正文的离线存档 + 从原理图 PDF 实读出的引脚归属。

- 官方文档页：<https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3>
- 资料下载页：<https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3/Resources-And-Documents>
- 商品页：<https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4.3.htm>（SKU 33874 = 标准版，33875 = 带 OV5647 摄像头版）
- 官方例程仓库：<https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3>
- 注：`https://www.waveshare.com/wiki/...` 与 `https://www.waveshare.net/wiki/...` 均已失效（403 / 404），内容已迁移到 `docs.waveshare.com`。

## 本目录内的配套文件

| 文件 | 说明 | 来源 |
|---|---|---|
| `ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf` | 官方原理图（Rev1.2，2 页：第 1 页电路图 / 第 2 页装配丝印图），SHA-256 `3697baa3ded0089446baf09705f437d13cf0324874031ccc57fd9b72cd9dfe53` | <https://www.waveshare.net/w/upload/b/b8/ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf> |
| `ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf` | 外形尺寸图（20260411） | 官方 `ESP32-P4-WIFI6-Touch-LCD-4.3.zip` 内解出 |
| `ESP32-P4-WIFI6-Touch-LCD-4.3.zip` | 官方机械资料包（尺寸 PDF + STEP + DXF），SHA-256 `8209b6aa405d4d3d8a2009e7eb545a4844e456b9cf95d8b8e53529414b03ecaf` | <https://www.waveshare.net/w/upload/3/36/ESP32-P4-WIFI6-Touch-LCD-4.3.zip> |
| `ST7701-datasheet.pdf` | 屏驱动 IC 规格书（Sitronix ST7701 SPEC V1.2，303 页，含 MIPI-DSI 章节） | Crystalfontz 镜像 <https://www.crystalfontz.com/controllers/uploaded/ST7701.pdf> |
| `GT911-datasheet.pdf` | Goodix GT911 触控 IC 数据手册 Rev.09 | <https://files.waveshare.com/wiki/common/GT911_EN_Datasheet.pdf> |
| `GT911-programming-guide.pdf` | GT911 编程指南 Rev.10（寄存器表 / 坐标上报格式） | <https://www.lcd-module.de/fileadmin/eng/pdf/zubehoer/GT911_Programming_Guide_Rev.10.pdf> |

## 板载资源（官方 Hardware Description 原文摘译）

1. **ESP32-P4-Core** — ESP32-P4NRW32 + 32MB Nor Flash（片内叠封 32MB PSRAM）
2. **ESP32-C6-MINI-1 模组** — SDIO 接口，提供 Wi-Fi 6 / Bluetooth 5 (LE)
3. **ES7210** 回声消除芯片
4. **ES8311** 低功耗音频编解码芯片
5. **MIPI CSI 接口** — 15PIN / 0.5mm 间距，支持 MIPI 2-lane 摄像头
6. **2.54mm 4PIN 焊盘** — 用于给 ESP32-C6 烧录固件
7. **RTC 电池座** — 仅支持可充电 RTC 电池
8. **POWER 按键** — 长按 2s 关机，短按开机
9. **BOOT 按键** — 上电/复位时按住进入下载模式
10. **RESET 按键**
11. **TF 卡槽** — SDIO 3.0
12. **MIPI DSI LCD 接口** — 接 MIPI 2-lane 屏
13. **喇叭座** — GH 1.25 2PIN（带锁），推荐 8Ω 2W
14. **Type-C（USB TO UART）** — 供电 / 烧录 / 调试
15. **Type-C（USB OTG）** — USB OTG 2.0 High Speed
16. **板载双麦克风阵列**
17. **PWR LED 电源指示灯**
18. **MX1.25 锂电池座** — 3.7V，支持充放电
19. **40PIN 排针扩展口** — 2.54mm，兼容部分树莓派 HAT

屏幕：4.3 inch 电容触摸 IPS，**480 × 800**。

外形尺寸（来自 dimensions.pdf）：盖板 114.4 × 66.8 mm，可视区 94.4 × 56.96 mm，整体厚 11.15 mm，4×M2.5 安装孔，孔距 92 × 50 mm。

## 引脚定义（实读自 schematic.pdf 第 1 页 "4.3inch Display" / "4.3 INCH" 区块）

原理图里屏和触摸的信号全部经 0R 电阻跳接到 P4 的 GPIO：

| 网络 | 跳接电阻 | ESP32-P4 GPIO | 说明 |
|---|---|---|---|
| `ESP_I2C_SCL` / `TP_SCL` | 直连 | **GPIO8** | 与板上 codec 共用同一条 I²C |
| `ESP_I2C_SDA` / `TP_SDA` | 直连 | **GPIO7** | 同上 |
| `RESET`（LCD 复位） | R60 = 0R | **GPIO27** | 该网另有 R102 = 10K 下拉到 GND |
| `BL_EN`（背光使能） | R32 = 0R | **GPIO33** | AP3032 的 EN 脚，已有 R57 = 100K 上拉到 Core_5V，默认使能 |
| `TP_RST`（触摸复位） | R37 = 0R | **GPIO23** | |
| `TP_INT`（触摸中断） | R35 = **NC/0R，默认不贴** | GPIO2（贴上才通） | 另引到测试点 TP2 |
| `LCD_BL_PWM`（背光调光） | R43 = 0R | **GPIO26** | 经 R42 = 10K 注入 AP3032 的 FB 节点 |

> **背光是「注入 FB」式调光，不是直接开关 LED。** 官方 BSP 里 LEDC 通道配了 `.flags = {.output_invert = 1}`，即 duty 越大 → 引脚实际输出越低 → 越亮。自己写驱动时必须带上这个反相，否则亮度曲线是反的。
>
> **`TP_INT` 默认断开**，所以官方 BSP 写的是 `BSP_LCD_TOUCH_INT = GPIO_NUM_NC`，触摸只能轮询。要用中断得自己补贴 R35。

屏 FPC 座 P2（0.5mm 间距 30PIN，后翻盖式，2.0H）：

| Pin | 网络 | Pin | 网络 |
|---|---|---|---|
| 1 | VLED- | 21 | VCC_1.8V / IOVCC |
| 2 | VLED+ | 22 | TE |
| 4 | ESP_3V3 | 23 | RESET |
| 6 / 7 | DSI_D0_P / DSI_D0_N | 25 | TP_RST |
| 9 / 10 | DSI_D1_P / DSI_D1_N | 26 | TP_SDA |
| 12 / 13 | DSI_CLK_P / DSI_CLK_N | 27 | TP_SCL |
| | | 28 | TP_INT |
| | | 29 | ESP_3V3 / TP_VDD |

其余板载资源引脚（来自官方 BSP 头文件 `bsp/esp32_p4_wifi6_touch_lcd_4_3.h`）：

| 功能 | GPIO |
|---|---|
| I2S SCLK / MCLK / LCLK / DOUT / DSIN | 12 / 13 / 10 / 9 / 11 |
| 功放使能 `BSP_POWER_AMP_IO` | 53 |
| SD D0–D3 / CMD / CLK | 39 / 40 / 41 / 42 / 44 / 43 |

## 面板参数（来自官方 BSP，可直接照抄）

驱动 IC 是 **ST7701**（BSP 依赖 `espressif/esp_lcd_st7701`），MIPI-DSI 接入：

| 参数 | 值 |
|---|---|
| 分辨率 | 480 (H) × 800 (V) |
| MIPI-DSI data lane 数 | 2 |
| lane bit rate | 500 Mbps |
| DPI 像素时钟 | 30 MHz |
| 像素格式 | RGB565（16bpp），可选 RGB888 |
| hsync back / pulse / front porch | 42 / 12 / 42 |
| vsync back / pulse / front porch | 2 / 8 / 60 |
| DSI PHY 供电 | LDO_VO3 通道 3，2500 mV |
| DBI 命令位宽 | cmd 8 bit / param 8 bit |

厂商初始化序列（39 条命令，`0xFF` 换页 + `0xB0/0xB1` gamma + `0xC0~0xCC` 等）在官方仓库
`examples/esp-idf/08_lvgl_demo_v9/components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c`
第 24–73 行 `vendor_specific_init_default[]`，配合 `ST7701-datasheet.pdf` 的寄存器章节对照阅读。

## 触摸（GT911）

- I²C 总线与 codec / 其它 I²C 从机共用：SCL = GPIO8，SDA = GPIO7
- 地址：官方 BSP 先探 **0x5D**（`ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS`），探不到再退到 **0x14**（`..._ADDRESS_BACKUP`）——这块板两种都可能，必须做探测
- 复位：GPIO23；中断：默认 NC（见上）
- 坐标上报格式、状态寄存器 `0x814E`、配置区 `0x8047–0x80FE` 校验和刷新流程见 `GT911-programming-guide.pdf`

## 本项目迁移结果（对照 `board_pinout.md`）

下表中的“旧用途”只描述迁移前 2.4 寸载板；当前 Rev1.2 4.3 寸固件已经
解除这些冲突。

| GPIO | 旧 2.4 寸用途 | Rev1.2 板载用途 | 当前处理 |
|---|---|---|---|
| 7 / 8 | I²C0（BNO085） | GT911 + audio + camera 共享 I²C | BNO085/BMP388 继续共享，地址不冲突 |
| 20 | BNO085 INT | BAT_ADC，且未从 J3 引出 | BNO085 INT 不接，固件轮询 |
| 23 | DOWN 按键 | TP_RST | 旧四按键任务停用 |
| 26 | TARE 按键 | LCD_BL_PWM | 调平迁移到触摸 UI |
| 27 | BMP388 INT | LCD RESET | BMP388 改为轮询，可选 INT 预留 GPIO31 |
| 33 | GPS RX | BL_EN | GPS UART RX 用 GPIO51（不占用 GPIO33）；GPIO50 留给 PPS |

必须同时区分以下接口：

- H1：P4 烧录/串口日志的 `USB TO UART` Type-C。
- H2：P4 原生 USB 2.0 HS OTG Type-C，也是 RTL-SDR 数据口。
- P1：C6 的 `TX RX IO9 GND` UART 下载排针，不是 USB。
- H4：喇叭座，不是 C6 调试排针。

## J3 引脚再分配（2026-07-31，因 60mm PCB 走线）

原始引脚分配中，IMU_RST（GPIO21/Pin15）、GPS_RX（GPIO32/Pin31）、GPS_PPS（GPIO46/Pin35）都在
J3 下排（奇数 pin）。由于 60mm 窄板下排紧贴 PCB 边缘没有走线空间，将这三个功能信号与相邻上排
引脚对调：

| 功能信号 | 原 GPIO / Pin | 新 GPIO / Pin |
|---|---|---|
| IMU_RST | GPIO21 / Pin15（下排） | GPIO28 / Pin16（上排） |
| GPS_RX | GPIO32 / Pin31（下排） | GPIO49 / Pin32（上排） |
| GPS_PPS | GPIO46 / Pin35（下排） | GPIO50 / Pin34（上排） |

GPS_TX 保留在 Pin36（GPIO51，上排），未变动。固件中的引脚定义已同步更新。

## FAQ（官方原文摘译）

- **推荐的 ESP-IDF 版本？** v5.5.1 ~ v5.5.4。
- **烧录报 `bootloader.bin requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)` 怎么办？** 在 menuconfig 里改 chip revision。
- **ESP32-P4 最大支持多大摄像头？** 200 万像素。P4 内置 ISP 与 H.264 编码器，编码性能上限 1080p@30fps。
- **怎么给 ESP32-C6 烧固件？** 上电时把 `C6_IO9` 拉低让 C6 进下载模式，同时让 P4 也进下载模式，然后通过 `C6_U0RXD` / `C6_U0TXD` 烧录。C6 出厂已带固件。
- **能用 PlatformIO / MicroPython 吗？** 暂不推荐，官方建议现阶段用 ESP-IDF。
