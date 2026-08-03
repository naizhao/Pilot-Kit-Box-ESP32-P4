# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 引脚定义

英文版：[`board_pinout.md`](board_pinout.md)

本文依据 **Waveshare Rev1.2 原理图**整理：

- [`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf)
  是本板板级连线的最高依据。
- [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md)
  记录官方 BSP 参数及本地 datasheet 索引。
- 固件行为已与 `firmware/main/`、`firmware/sdkconfig.defaults` 交叉核对。

下文使用三类标签：

- **板载**：Rev1.2 PCB 的固定连线，固件不能重映射。
- **固件**：当前 Pilot Kit 构建已经实现的行为。
- **项目**：Pilot Kit 外接模块的接线约定；变更时必须同步修改接线和固件。

### UART 方向命名约定

本文所有 UART 信号一律采用 **P4 视角**：「P4 UART1 TX」指 P4 向对端 RXD
输出的那根线。

Pilot Kit 载板 PCB（`docs/jlc/lcd-4.3in/`）的网络名采用**模块视角**，两者
方向正好相反：

| 载板 PCB 网络 | J3 脚 | P4 GPIO | 本文含义 |
|---|---:|---:|---|
| `GPS_RX` | 32 | 49 | **P4 UART1 TX** → GPS RXD |
| `GPS_TX` | 36 | 51 | GPS TXD → **P4 UART1 RX** |

固件常量同样是 P4 视角：`firmware/main/gps_task.c` 中
`GPS_TX_PIN = 49`、`GPS_RX_PIN = 51`。

> 不要把 `ESP32-P4-WIFI6-datasheet.pdf` 当成本板引脚依据。它虽然名为
> datasheet，实际是另一款 ESP32-P4-WIFI6 板的原理图。本文只采用上面的
> 4.3 寸 Rev1.2 原理图作为板级依据。

## 1. 先认清接口

旧文档曾沿用 2.4 寸载板的接口名称；这些名称不适用于 4.3 寸新板。

| 位号 | 实物接口 | 用途 |
|---|---|---|
| **H1** | USB-C，丝印 `USB TO UART` | CH343P 桥接，用于 P4 烧录和串口日志 |
| **H2** | USB-C，丝印 `USB` | ESP32-P4 原生 USB 2.0 High-Speed OTG；与 J3-25/27 同网 |
| **P1** | 1×4、2.54 mm 排针，丝印 `TX RX IO9 GND` | ESP32-C6 UART 下载口；**不是 USB** |
| **J3** | 2×20、2.54 mm 排针 | 电源、GPIO 和两组 USB 信号；仅“部分”类似树莓派 HAT |
| **P2** | 30-pin、0.5 mm 显示/触摸 FFC | MIPI-DSI LCD 及 GT911 触摸 |
| **J1** | 15-pin 摄像头 FFC | 2-lane MIPI-CSI 摄像头 |
| **J2** | 2-pin 电池座 | 3.7 V 锂电池 |
| **H8** | 2-pin RTC 电池座 | 只能接可充电 RTC 电池 |
| **H4** | 2-pin 喇叭座 | 8 Ω / 2 W 喇叭输出 |

板上只有三个按键：**Key1 RESET**、**Key2 BOOT**、**Key3 POWER**。
旧载板的四个应用按键在新板上不存在。

## 2. J3 40-pin 扩展排针

### 实物丝印视角

本节固定采用用户面对板子时的实物方向，**不做镜像**：

- 左上角是 `GND`，左下角是 `GPIO48`
- 右上角是 `ESP_3V3`，右下角是 `VCC_5V`
- 从左向右读取时，J3 脚号由 40/39 递减到 2/1

HAT 载板从对接面观察时会镜像，画 footprint 时必须再次核对。

#### 横向速查表：面对板子，从左到右

| 面对板子 | 左端 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 右端 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **上排丝印** | GND | 52 | 51 | 50 | 49 | 35 | 34 | GND | 31 | 30 | 29 | 3V3 | 28 | 4 | 3 | GND | 2 | SCL | SDA | 3V3 |
| **上排实际网络** | GND | GPIO52 | GPIO51 | GPIO50 | GPIO49 | GPIO35 | GPIO34 | GND | GPIO31 | GPIO30 | GPIO29 | ESP_3V3 | GPIO28 | GPIO4 | GPIO3 | GND | GPIO2 | GPIO8 | GPIO7 | ESP_3V3 |
| **上排 J3 脚号** | 40 | 38 | 36 | 34 | 32 | 30 | 28 | 26 | 24 | 22 | 20 | 18 | 16 | 14 | 12 | 10 | 8 | 6 | 4 | 2 |
| **下排丝印** | 48 | 47 | 46 | GND | 32 | GND | DP | DM | 25 | 24 | GND | 22 | 21 | GND | 5 | 38 | 37 | GND | 5V | 5V |
| **下排实际网络** | GPIO48 | GPIO47 | GPIO46 | GND | GPIO32 | GND | `USBD_P` | `USBD_N` | GPIO25 | GPIO24 | GND | GPIO22 | GPIO21（BAT_STAT） | GND | GPIO5 | GPIO38 | GPIO37 | GND | VCC_5V | VCC_5V |
| **下排 J3 脚号** | 39 | 37 | 35 | 33 | 31 | 29 | 27 | 25 | 23 | 21 | 19 | 17 | 15 | 13 | 11 | 9 | 7 | 5 | 3 | 1 |

#### 逐列详细表：同一实物方向

| 实物位置（左→右） | 上排 J3 脚 | 上排实际网络 | 下排 J3 脚 | 下排实际网络 |
|---:|---:|---|---:|---|
| 1 | 40 | GND | 39 | GPIO48 |
| 2 | 38 | GPIO52 | 37 | GPIO47 |
| 3 | 36 | GPIO51（GPS TXD → P4 UART1 RX；载板网络 `GPS_TX`） | 35 | GPIO46（空闲） |
| 4 | 34 | GPIO50（GPS PPS） | 33 | GND |
| 5 | 32 | GPIO49（P4 UART1 TX → GPS；载板网络 `GPS_RX`） | 31 | GPIO32（空闲） |
| 6 | 30 | GPIO35 / BOOT 与自动下载电路 | 29 | GND |
| 7 | 28 | GPIO34（IMU_INT） | 27 | `USBD_P`，原生 USB HS D+，丝印 `DP` |
| 8 | 26 | GND | 25 | `USBD_N`，原生 USB HS D−，丝印 `DM` |
| 9 | 24 | GPIO31（BARO_INT） | 23 | GPIO25 / `USB1P1_P` |
| 10 | 22 | GPIO30 | 21 | GPIO24 / `USB1P1_N` |
| 11 | 20 | GPIO29 | 19 | GND |
| 12 | 18 | ESP_3V3 | 17 | GPIO22 |
| 13 | 16 | GPIO28（IMU_RST） | 15 | GPIO21（BAT_STAT，飞线自 TP1） |
| 14 | 14 | GPIO4 | 13 | GND |
| 15 | 12 | GPIO3 | 11 | GPIO5 |
| 16 | 10 | GND | 9 | GPIO38 / P4 UART RX ← CH343P |
| 17 | 8 | GPIO2 / 经默认未贴 R35 可接 TP_INT | 7 | GPIO37 / P4 UART TX → CH343P |
| 18 | 6 | GPIO8 / 共享 I²C SCL，丝印 `SCL` | 5 | GND |
| 19 | 4 | GPIO7 / 共享 I²C SDA，丝印 `SDA` | 3 | VCC_5V |
| 20 | 2 | ESP_3V3 | 1 | VCC_5V |

```text
实物丝印正读，J3 在文字下方：

左端  上排 J3-40 ───────────────────────────── J3-2  右端
      下排 J3-39 ───────────────────────────── J3-1
```

因此 Pilot Kit 载板上的原生 USB HS 使用：

- J3-27：`USBD_P` / `DP`（载板网络 `USB_DP`）
- J3-25：`USBD_N` / `DM`（载板网络 `USB_DM`）
- J3-26：相邻 GND
- J3-1/J3-3：`VCC_5V`（载板网络 `+5V_SYS`）→ USB-A VBUS

**实际打样的载板**把 `+5V_SYS` 从 J3-1/J3-3 直接接到 USB-A pin 1，
`docs/jlc/lcd-4.3in/` 里**没有任何限流开关器件**。加 500 mA USB Host
限流开关属于后续改版的候选项，不是现有硬件。

丝印 `25`/`24` 表示 GPIO25/GPIO24，对应另一组 Full-Speed
`USB1P1_P/N`；它们不是丝印 `DP`/`DM` 的原生 USB HS 差分对。

### 按原理图脚号排列

下表为了查原理图而从 pin 1 开始，**相对于上面的“面对板子”横向表是
从右向左读取**。接线和画 HAT footprint 时应优先采用上面的实物方向表。

| 奇数脚 | 网络 / 板载用途 | 偶数脚 | 网络 / 板载用途 |
|---:|---|---:|---|
| 1 | VCC_5V | 2 | ESP_3V3 |
| 3 | VCC_5V | 4 | GPIO7 / 共享 I²C SDA |
| 5 | GND | 6 | GPIO8 / 共享 I²C SCL |
| 7 | GPIO37 / P4 UART TX → CH343P | 8 | GPIO2 / 经默认未贴 R35 可接 TP_INT |
| 9 | GPIO38 / P4 UART RX ← CH343P | 10 | GND |
| 11 | GPIO5 | 12 | GPIO3 |
| 13 | GND | 14 | GPIO4 |
| 15 | GPIO21（BAT_STAT，飞线自 TP1） | 16 | GPIO28（IMU_RST） |
| 17 | GPIO22 | 18 | ESP_3V3 |
| 19 | GND | 20 | GPIO29 |
| 21 | GPIO24 / USB1P1_N | 22 | GPIO30 |
| 23 | GPIO25 / USB1P1_P | 24 | GPIO31（BARO_INT） |
| 25 | USBD_N，原生 USB HS D− | 26 | GND |
| 27 | USBD_P，原生 USB HS D+ | 28 | GPIO34（IMU_INT） |
| 29 | GND | 30 | GPIO35 / BOOT 与自动下载电路 |
| 31 | GPIO32（空闲） | 32 | GPIO49（P4 UART1 TX → GPS；载板网络 `GPS_RX`） |
| 33 | GND | 34 | GPIO50（GPS PPS） |
| 35 | GPIO46（空闲） | 36 | GPIO51（GPS TXD → P4 UART1 RX；载板网络 `GPS_TX`） |
| 37 | GPIO47 | 38 | GPIO52 |
| 39 | GPIO48 | 40 | GND |

J3 共引出 **26 个带编号的 P4 GPIO**：
`2、3、4、5、7、8、21、22、24、25、28、29、30、31、32、34、35、37、
38、46、47、48、49、50、51、52`。

J3 **没有**引出 GPIO20、GPIO23、GPIO26、GPIO27 或 GPIO33。旧文档中
包含这些引脚的左右两列排针图属于上一代载板。

重要注意事项：

- J3 并不与完整的树莓派 40-pin 排针电气兼容。连接任何 HAT 前应逐脚核对。
- J3-25/27 与 H2 共用原生 USB HS 数据网络；H2 与载板 USB-A 只能二选一。
- GPIO24/25 是另一组 Full-Speed USB Serial/JTAG，不是 H2 的原生 HS 信号。
- GPIO35 是 BOOT，并受 CH343P 自动下载电路驱动。
- GPIO37/38 与板载 CH343P 共用，外接 UART 可能与 H1 冲突。
- 只有焊上可选 0 Ω 电阻 R35 后，GPIO2 才连接触摸中断；出厂默认开路。

### Pilot Kit 推荐 J3 分配

| 功能 | J3 接线 | 状态 |
|---|---|---|
| 共享 I²C SDA / SCL | GPIO7 / GPIO8 | **项目 + 固件** |
| BNO085 复位 | GPIO28（因 PCB 走线从 GPIO21 迁移） | **项目 + 固件** |
| BNO085 中断 | GPIO34（J3 pin 28，载板网络 `IMU_INT`） | 载板已接线，但固件仍轮询 |
| 充电状态（BAT_STAT） | GPIO21（J3 pin 15，飞线自 TP1） | **项目 + 固件** |
| BMP388 中断 | GPIO31（J3 pin 24，载板网络 `BARO_INT`） | 载板已接线，但固件仍轮询 |
| GPS UART1 TX / RX | P4 TX GPIO49（J3 pin 32）/ P4 RX GPIO51（J3 pin 36）；P4 TX 由 GPIO32 迁至 GPIO49 | **项目 + 固件**，9600 8N1 |
| GPS PPS | 可选 GPIO50（因 PCB 走线从 GPIO46 迁移） | 仅预留接线；当前固件不读取 PPS |
| RTL-SDR USB | J3-27 `DP` / J3-25 `DM` | **载板 USB-A 插头**；H2 保持空置。VBUS 直接取自 J3 `VCC_5V`，载板上没有限流开关 |

不启用上述可选项目功能时，较适合作通用扩展的引脚有：
GPIO5、GPIO22、GPIO29、GPIO30、GPIO32、GPIO46、GPIO47、GPIO48、GPIO52。

下列引脚使用前必须评估副作用：

- GPIO2/3/4 与默认 JTAG 功能重叠。
- GPIO24/25 与 USB Serial/JTAG 重叠。
- GPIO21 用于 BAT_STAT（TP1 飞线）；配置为输入上拉，ETA6098 充电时拉低。
- GPIO31 上是载板 `BARO_INT` 网络（BMP388 中断）。
- GPIO34 上是载板 `IMU_INT` 网络（BNO085 中断）。
- GPIO49/51 是当前 GPS UART（P4 TX 在 GPIO49，P4 RX 在 GPIO51）。
- GPIO50 为 GPS PPS 预留（GPIO46 现空闲）。
- GPIO35、GPIO37/38 通常应留给启动和调试串口电路。

## 3. 全部 GPIO 的板级归属

本表用于防止把板内信号误当成空闲引脚。

| GPIO | Rev1.2 板级连接 | J3 |
|---:|---|:---:|
| 0、1 | 32.768 kHz 晶体 | 否 |
| 2 | 经 R35 可选连接 GT911 TP_INT（默认未贴） | 8 |
| 3 | 扩展；默认 JTAG MTDI | 12 |
| 4 | 扩展；默认 JTAG MTMS | 14 |
| 5 | 扩展 | 11 |
| 6 | 经 R33 0 Ω 连接 ESP32-C6 IO2 | 否 |
| 7 | 共享 I²C SDA：触摸、音频、摄像头及 J3 | 4 |
| 8 | 共享 I²C SCL：触摸、音频、摄像头及 J3 | 6 |
| 9 | I²S DSDIN，P4 → ES8311 | 否 |
| 10 | I²S LRCK | 否 |
| 11 | I²S ASDOUT，ES7210 → P4 | 否 |
| 12 | I²S SCLK | 否 |
| 13 | I²S MCLK | 否 |
| 14–17 | P4↔C6 SDIO D0–D3 | 否 |
| 18、19 | P4↔C6 SDIO CLK、CMD | 否 |
| 20 | BAT_ADC，电池电压 1/3 分压 | 否 |
| 21、22 | 扩展（GPIO21 = BAT_STAT，TP1 飞线；IMU_RST 已迁至 GPIO28） | 15、17 |
| 23 | GT911 TP_RST | 否 |
| 24、25 | USB1P1_N/P，Full-Speed USB Serial/JTAG | 21、23 |
| 26 | LCD_BL_PWM | 否 |
| 27 | LCD RESET | 否 |
| 28–32 | 扩展（GPIO28 = IMU_RST，GPIO31 = BARO_INT） | 16、20、22、24、31 |
| 33 | BL_EN，100 kΩ 上拉 | 否 |
| 34 | 扩展（GPIO34 = IMU_INT） | 28 |
| 35 | BOOT 及 CH343P 自动下载 | 30 |
| 36 | 仅 10 kΩ 上拉，未引出 | 否 |
| 37 | P4 UART TX → CH343P RXD | 7 |
| 38 | P4 UART RX ← CH343P TXD | 9 |
| 39–42 | microSD D0–D3 | 否 |
| 43、44 | microSD CLK、CMD | 否 |
| 45 | microSD 电源开关控制 | 否 |
| 46–52 | 扩展（GPIO49 = P4 UART1 TX → GPS，GPIO50 = GPS PPS，GPIO51 = GPS → P4 UART1 RX） | 35、37、39、32、34、36、38 |
| 53 | NS4150B PA_CTRL | 否 |
| 54 | 经 R34 0 Ω 连接 ESP32-C6 CHIP_PU/EN | 否 |

MIPI-DSI、MIPI-CSI、原生 USB HS 和外置 Flash 使用的是专用非 GPIO
网络，不应为这些网络编造 GPIO 编号。

## 4. 显示、触摸与背光

### ST7701 MIPI-DSI 显示

面板原生方向为 480×800 竖屏，当前固件对外提供 800×480 横屏 UI。

| 项目 | 板载 / 当前固件 |
|---|---|
| DSI | 2 lane，每 lane 500 Mbit/s |
| DPI framebuffer | 480×800、RGB565、30 MHz pixel clock |
| 水平 porch | back 42 / pulse 12 / front 42 |
| 垂直 porch | back 2 / pulse 8 / front 60 |
| DSI PHY 电源 | ESP LDO_VO3 channel 3，2.5 V |
| LCD 复位 | GPIO27，低有效；10 kΩ 下拉 |
| 背光 PWM | GPIO26，注入 AP3032 反馈网络 |
| 背光使能 | GPIO33；100 kΩ 上拉，默认开启 |
| 固件变换 | PPA 逆时针 270°，等效顺时针 90°，同时 byte swap |
| framebuffer | 两个原生 480×800 DPI buffer，VSYNC 时切换 |

GPIO26 的 LEDC 输出必须反相，因为 PWM 注入的是 AP3032 反馈节点。当前
固件使用 5 kHz、10-bit duty，应用亮度范围 0–255，启动值 180；固件没有
主动驱动 BL_EN。

### P2 显示/触摸 FFC

| 脚 | 网络 | 脚 | 网络 |
|---:|---|---:|---|
| 1 | VLED− | 16 | NC |
| 2 | VLED+ | 17 | NC |
| 3 | NC | 18 | NC |
| 4 | ESP_3V3 | 19 | NC |
| 5 | NC | 20 | NC |
| 6 | DSI_D0_P | 21 | VCC_1.8V / IOVCC |
| 7 | DSI_D0_N | 22 | TE，未继续连接 P4 |
| 8 | NC | 23 | LCD RESET |
| 9 | DSI_D1_P | 24 | NC |
| 10 | DSI_D1_N | 25 | TP_RST |
| 11 | NC | 26 | TP_SDA |
| 12 | DSI_CLK_P | 27 | TP_SCL |
| 13 | DSI_CLK_N | 28 | TP_INT |
| 14 | NC | 29 | ESP_3V3 / TP_VDD |
| 15 | NC | 30 | GND |

### GT911 触摸

- **板载：**SDA GPIO7、SCL GPIO8、复位 GPIO23。
- **板载：**TP_INT 可经 R35 接 GPIO2，但 R35 出厂未贴。
- **固件：**400 kHz I²C；先探测地址 `0x5D`，再探测 `0x14`；轮询模式。
- **固件：**把原生 480×800 坐标映射到逻辑 800×480 UI。
- **硬件能力：**GT911 最多支持五点触控。
- **当前固件：**只消费第一个触点。

## 5. ESP32-C6 无线协处理器

ESP32-P4 本身不带 Wi-Fi 或蓝牙。板载 ESP32-C6-MINI-1-N4 通过
ESP-Hosted 提供 Wi-Fi 6 与 Bluetooth 5 LE。

| 功能 | P4 GPIO | C6 pad |
|---|---:|---|
| SDIO D0 | 14 | IO20 |
| SDIO D1 | 15 | IO21 |
| SDIO D2 | 16 | IO22 |
| SDIO D3 | 17 | IO23 |
| SDIO CLK | 18 | IO19 |
| SDIO CMD | 19 | IO18 |
| C6 IO2 | 6 | IO2 |
| C6 CHIP_PU / EN | 54 | EN |

六根 SDIO 信号在 C6 侧均有 51 kΩ 上拉。GPIO54 经 R34 0 Ω 直接连接
C6 EN；Rev1.2 **不存在复位反相器**。当前项目经过实机验证的配置仍为：

```text
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

不要再用不存在的板上反相器解释该软件选项。

C6 UART 下载口是 **P1**：

| P1 | 信号 | USB-UART 接法 |
|---:|---|---|
| 1 | C6_U0TXD | 转接器 RX |
| 2 | C6_U0RXD | 转接器 TX |
| 3 | C6_IO9 | C6 上电/复位时拉低以进入下载模式 |
| 4 | GND | 转接器 GND |

H4 是喇叭座，不是 C6 排针。

## 6. USB 路径与调试串口

| 路径 | 电气连接 | 用途 |
|---|---|---|
| H1 `USB TO UART` | USB-C → CH343P → P4 GPIO38 RX / GPIO37 TX | P4 烧录及 `idf.py monitor` |
| H2 `USB` | USB-C → 专用 USBD_N/P | 原生 USB 2.0 HS OTG；与 J3 HS 数据线同网 |
| J3-21/23 | GPIO24/25，USB1P1_N/P | 另一组 Full-Speed USB Serial/JTAG |
| J3-25/27 | 专用 USBD_N/P | Pilot Kit 载板的 RTL-SDR USB HS 数据路径 |

**当前接法**：Pilot Kit 载板（`docs/jlc/lcd-4.3in/`，位号 `RTL-SDR`，
4-pin USB Type-A 插头）把 J3-27 `DP`、J3-25 `DM` 引到该插头，VBUS 直接
取自 J3-1/J3-3 `VCC_5V`。因此 dongle 插在载板上，**H2 必须保持空置**。

没有载板时（台面上的裸 Waveshare 板），H2 `USB` 走的是同一对数据线，但
**H2 不对外供 VBUS**：2026-08-03 台面实测，H1、H2 两个座子都不向外输出
5 V。dongle 直接插 H2 根本不上电，也就永远不可能枚举。所以裸板必须用
**自供电（有源）USB Hub**，或另行给 dongle 喂 5 V；无源 OTG 转接头不够。
H2 与 J3-25/27 是同一组网络，同一时刻只能占用其中一个。

这正是载板接好之前 RTL-SDR 一次都没枚举成功的原因：此前每次尝试都走 H2，
dongle 压根没电。这不能作为“固件曾经有问题”的证据。

已知限制：载板 VBUS 上**没有限流开关**，dongle 直接吃板子的 5 V。
加 500 mA USB Host 限流开关是载板后续改版的候选项。不要继续使用旧文档
中的四针 P1/MX1.25 USB 线缆。

## 7. microSD

板载卡座使用 SDMMC Slot 0、4-bit 模式。

| 信号 | GPIO |
|---|---:|
| D0 | 39 |
| D1 | 40 |
| D2 | 41 |
| D3 | 42 |
| CLK | 43 |
| CMD | 44 |
| 电源开关控制 | 45 |

CMD 和全部数据线通过 51 kΩ 上拉到 ESP_LDO_VO4。没有独立的 card-detect
GPIO；卡座 CD 与 D3 共用。卡电源由 P 沟道 MOSFET 控制，其 gate 为
GPIO45，并有 10 kΩ 默认下拉。

**固件：**ESP-Hosted 已为 C6 Slot 1 初始化共享 SDMMC 控制器，因此
microSD 驱动使用 Slot 0，并提供空 `init`/`deinit` 回调，避免重复初始化。

## 8. 摄像头、音频与存储

### J1 MIPI-CSI 摄像头

| 脚 | 网络 | 脚 | 网络 |
|---:|---|---:|---|
| 1 | GND | 9 | CSI_CLK_P |
| 2 | CSI_D0_N | 10 | GND |
| 3 | CSI_D0_P | 11 | CSI_IO0 |
| 4 | GND | 12 | CSI_IO1 |
| 5 | CSI_D1_N | 13 | ESP_I2C_SCL |
| 6 | CSI_D1_P | 14 | ESP_I2C_SDA |
| 7 | GND | 15 | ESP_3V3 |
| 8 | CSI_CLK_N |  |  |

CSI_IO0 有 10 kΩ 上拉；CSI_IO1 的可选上拉未贴。原理图没有为 CSI_IO0/1
标出 P4 GPIO 编号。当前 Pilot Kit 固件尚未实现摄像头采集。

### 音频

| 设备 / 网络 | 连接 |
|---|---|
| ES8311 codec | 官方 BSP 使用 I²C 地址 `0x18` |
| ES7210 四通道 ADC | I²C 地址 `0x40`；连接双板载麦克风及回放参考 AEC 路径 |
| I²C | SDA GPIO7 / SCL GPIO8 |
| I²S MCLK / SCLK / LRCK | GPIO13 / GPIO12 / GPIO10 |
| I²S DSDIN / ASDOUT | GPIO9 / GPIO11 |
| NS4150B 功放使能 | PA_CTRL GPIO53 |
| 喇叭 | H4，建议 8 Ω / 2 W |

当前 Pilot Kit 固件尚未初始化音频子系统。

### 存储

- ESP32-P4NRW32：封装内 32 MB PSRAM。
- GD25Q256EYIGR：256 Mbit / 32 MB 外置 NOR Flash，使用专用 Flash 引脚。
- ESP32-C6-MINI-1-N4：无线协处理器；其 Flash 容量由 N4 模组型号表示，
  原理图没有另行标注。

## 9. 电源与电池

- J2 接 3.7 V 锂离子/锂聚合物电池。ETA6098 负责充电，SCT12 把电池电压
  升压并入板上 5 V 系统。
- BAT_ADC 经 200 kΩ / 100 kΩ 分压连接 GPIO20，ADC 看到的约为电池电压
  的三分之一。
- Key3 POWER：短按开机，长按约两秒关机（官方板卡行为）。
- Key1 RESET 拉低 ESP_EN；Key2 BOOT 拉低 GPIO35。
- H8 **只能接可充电 RTC 电池**。板上存在充电路径，禁止接不可充电纽扣电池。
- PWR LED 固定接 Core_5V，不能由软件控制。

## 10. Pilot 外接模块

### BNO085 IMU

| BNO085 引脚 | 连接 |
|---|---|
| VCC / GND | J3 ESP_3V3 / GND |
| SDA / SCL | GPIO7 / GPIO8 |
| RST | GPIO28（因 60mm PCB 走线从 GPIO21 迁移） |
| INT | GPIO34（J3 pin 28，载板网络 `IMU_INT`）；已接线，但当前驱动仍轮询 |
| AD0 / PS1 / PS0 | GND / GND / GND |
| CS | ESP_3V3 |

当前驱动只支持地址 `0x4A`，AD0 必须接低。GPIO20 不能接 BNO085 INT：
它没有从 J3 引出，且已硬连到 BAT_ADC。

当前固件姿态变换假定 IMU 板竖直安装：芯片面朝飞行员、排针位于飞行员
左侧、VCC 在顶部。此时芯片 +X 对应飞行器向上，+Y 对应向左，+Z 对应
向后；固件应用 `q_body_fix = (0, 0.7071068, 0, -0.7071068)`。若按标准
飞行器轴向安装传感器，必须同步修改变换并重新验证姿态，不能只改文档。

### BMP388 气压计

| BMP388 引脚 | 连接 |
|---|---|
| VCC / GND | ESP_3V3 / GND |
| SDA / SCL | GPIO7 / GPIO8 |
| SDO / CSB | GND / ESP_3V3，对应地址 `0x76` |
| INT | GPIO31（J3 pin 24，载板网络 `BARO_INT`）；已接线，但当前驱动仍轮询 |

### GPS

下表左列是 **GPS 模块自身的引脚名**，GPIO 列是 P4 侧（方向约定见本文开头）。

| GPS 引脚 | 连接 |
|---|---|
| TXD | GPIO51，GPS → P4 UART1 RX（J3 pin 36；未迁移） |
| RXD | GPIO49，P4 UART1 TX → GPS（从 GPIO32 迁移） |
| PPS | 仅可选接 GPIO50（从 GPIO46 迁移） |
| VCC / GND | ESP_3V3 / GND |

当前固件使用 UART1、9600 8N1，并从 NMEA RMC 获取时间；没有实现 PPS
GPIO 中断或授时纪律。

## 11. Bring-up 检查表

1. P4 烧录和日志使用 H1；P1 只用于烧录 C6。
2. 日志应出现 `ST7701 DSI ready: logical 800x480 -> PPA 90 CW`。
3. 检查触摸覆盖全屏；除非有意补焊 R35，否则 TP_INT 应保持开路。
4. 检查 BNO085 地址 `0x4A`、BMP388 地址 `0x76`，以及 NMEA 是否落在
   P4 RX GPIO51 上。
5. 确认 C6 ESP-Hosted 在 DSI 之前启动，且没有 SDIO CMD5 错误。
6. 确认 microSD 通过 Slot 0 挂载，且没有重复初始化共享 SDMMC host。
7. RTL-SDR 接载板的 USB-A 插头，确认 J3 原生 USB HS 枚举；H2 保持空置。
   裸板时改接 H2。
8. 修改接线后执行多次冷启动，确认启动稳定。

## 12. 旧硬件边界

旧 2.4 寸 SPI 屏与四按键载板仅是 `docs/jlc/` 下的历史材料。其
GPIO28/29/30/31/50 显示连线、GPIO26/5/22/23 应用按键和旧接口名称，
不得复制到 Rev1.2 4.3 寸构建。

`firmware/main/button_task.c` 实现的正是这四个旧按键，因此已从
`firmware/main/CMakeLists.txt` **移出编译**：它写死的 GPIO26、GPIO23 在
Rev1.2 上是 LCD_BL_PWM 与 GT911 TP_RST，调用 `pk_button_init()` 会把背光
和触摸复位脚配成输入上拉。保留该文件只是作为「按键 → 动作」路由的参考；
重新映射引脚是重新加回编译的前提。
