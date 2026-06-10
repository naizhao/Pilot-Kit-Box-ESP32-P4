# Waveshare ESP32-P4-WIFI6 — 引脚与 GPIO 映射

英文版：[`board_pinout.md`](board_pinout.md)

本文是固件 GPIO 分配的硬件事实源。当前已验证接线：

- LCD：左排 GPIO28/29/30/31 + 背光 GPIO50。
- BNO085：I2C0 GPIO7 / GPIO8，INT GPIO20，RST GPIO21。
- 按钮：TARE GPIO26，MODE GPIO5，UP GPIO22，DOWN GPIO23。
- MODE 使用 GPIO5 是为了支持 deep sleep 低电平唤醒。

修改固件里的 `*_PIN` define 前，先更新本文。

## 1. 用户 header 物理布局

面向板子，USB-C / SD 卡边在上方，ESP32-C6 模组在下方：

```text
              ┌───────────────────────┐
       (top)  │ ◯ Type-C  ◯ MicroSD ◯ │
              ├───────────────────────┤
    GPIO52  ──┤                       ├──  VBUS
    GPIO51  ──┤                       ├──  VSYS
       GND  ──┤                       ├──  GND
    GPIO31  ──┤                       ├──  EN
    GPIO30  ──┤  ┌──────────────┐     ├──  3V3
    GPIO29  ──┤  │   ESP32-P4   │     ├──  GPIO20
    GPIO28  ──┤  └──────────────┘     ├──  GPIO21
       GND  ──┤                       ├──  GND
    GPIO50  ──┤                       ├──  GPIO22
    GPIO49  ──┤                       ├──  GPIO23
     GPIO5  ──┤                       ├──  RUN
     GPIO4  ──┤                       ├──  GPIO26
       GND  ──┤                       ├──  GND
     GPIO3  ──┤                       ├──  GPIO27
     GPIO2  ──┤                       ├──  GPIO32
     GPIO8  ──┤    H4: IO9 GND RX TX ├──  GPIO33
     GPIO7  ──┤                       ├──  GPIO46
       GND  ──┤                       ├──  GND
    GPIO24  ──┤    P1 USB: V D- D+ G ├──  GPIO47
    GPIO25  ──┤                       ├──  GPIO48
              └───────────────────────┘
```

两排 header 都从 USB-C 侧向下计数。

### 左排 header

| 丝印 | GPIO / Net | 当前用途 | 说明 |
|---|---|---|---|
| 52 | GPIO52 | 空闲 | 可用于后续扩展 |
| 51 | GPIO51 | 空闲 | 可用于后续扩展 |
| GND | GND | 地 | 推荐 LCD GND 回流 |
| **31** | GPIO31 | **LCD DC** | 普通 GPIO 输出 |
| **30** | GPIO30 | **LCD SCK** | SPI2_CK_PAD，IO_MUX direct |
| **29** | GPIO29 | **LCD MOSI** | SPI2_D_PAD，IO_MUX direct |
| **28** | GPIO28 | **LCD CS** | SPI2_CS_PAD，IO_MUX direct |
| GND | GND | 地 | LCD 推荐接地 |
| **50** | GPIO50 | **LCD BL** | LEDC PWM 背光 |
| 49 | GPIO49 | 空闲 | 曾作为 LCD RST，当前屏幕转接板自带 RC reset |
| **5** | GPIO5 | **BTN2 / MODE** | LP_IO，可用于 deep sleep 唤醒 |
| 4 | GPIO4 | 空闲 | 默认 JTAG MTMS；使用会影响 JTAG |
| GND | GND | 地 |  |
| 3 | GPIO3 | 空闲 | 默认 JTAG MTDI |
| 2 | GPIO2 | 空闲 | 默认 JTAG MTCK |
| **SCL/8** | GPIO8 | **I2C0 SCL** | ES8311 + BNO085 共用 |
| **SDA/7** | GPIO7 | **I2C0 SDA** | ES8311 + BNO085 共用 |
| GND | GND | 地 |  |
| DM/24 | GPIO24 | 保留 | USB Serial/JTAG FS PHY，不用于 RTL-SDR |
| DP/25 | GPIO25 | 保留 | USB Serial/JTAG FS PHY，不用于 RTL-SDR |

### 右排 header

| 丝印 | GPIO / Net | 当前用途 | 说明 |
|---|---|---|---|
| VBUS | +5 V | 输入 | USB-C VBUS |
| VSYS | +5 V | 输入 | 电池 / 外部 5 V |
| GND | GND | 地 |  |
| EN | CHIP_PU | 复位 | 拉低让 P4 reset |
| 3V3 | +3.3 V | 输出 | 板载 LDO 输出 |
| **20** | GPIO20 | **IMU INT** | 当前轮询，不作为 IRQ |
| **21** | GPIO21 | **IMU RST** | BNO085 active-low reset |
| GND | GND | 地 |  |
| **22** | GPIO22 | **BTN3 / UP** | 列表 / 页面上滚 |
| **23** | GPIO23 | **BTN4 / DOWN** | 列表 / 页面下滚 |
| RUN | RUN | 系统复位 | 板载 reset net |
| **26** | GPIO26 | **BTN1 / TARE** | IMU tare / 语言切换 / own-ship 绑定 |
| GND | GND | 地 |  |
| **27** | GPIO27 | **BARO_INT**（BMP388，载板）| data-ready 中断；驱动已实现（`baro_task.c`，轮询 ~10 Hz）|
| **32** | GPIO32 | **GPS RX**（载板）| P4 UART **TX** → GPS RXD；驱动已实现（`gps_task.c`，UART1 NMEA）|
| **33** | GPIO33 | **GPS TX**（载板）| GPS TXD → P4 UART **RX**；旧 LCD MOSI |
| **46** | GPIO46 | **GPS PPS**（载板）| 1 Hz 秒脉冲，用于授时 |
| GND | GND | 地 | 位于 GPIO46 与 GPIO47 之间，容易误短路 |
| 47 | GPIO47 | 空闲 | 旧 LCD SCK，已废弃 |
| 48 | GPIO48 | 空闲 | 旧 LCD DC，已废弃 |

## 2. 板载外设

### MicroSD 卡槽

| 信号 | ESP32-P4 GPIO |
|---|---:|
| CLK | GPIO43 |
| CMD | GPIO44 |
| D0 | GPIO39 |
| D1 | GPIO40 |
| D2 | GPIO41 |
| D3 / CD | GPIO42 |

卡槽走 **SDMMC Slot 0**（GPIO39-44 是 P4 Slot 0 专用脚；Slot 1 被 ESP-Hosted
连 C6 的 SDIO 链路占用）。IDF ≥ 6.0 的 SDMMC 控制器只能 init 一次（IDF
issue #16233）：ESP-Hosted 先 init，SD 挂载需把 `host.init`/`host.deinit`
置为空函数复用控制器（见 `firmware/main/pk_sdcard.c`）。

记录后端默认 LittleFS；设置页 LOG 行可切到 MicroSD（重启生效，缺卡自动回退）。

### ESP32-C6 协处理器

C6 作为 ESP-Hosted SDIO slave，提供 BLE / Wi-Fi radio。

| C6 pad | 功能 | ESP32-P4 GPIO |
|---|---|---:|
| IO19 | CLK | GPIO18 |
| IO18 | CMD | GPIO19 |
| IO12 | D0 | GPIO14 |
| IO13 | D1 | GPIO15 |
| IO14 | D2 | GPIO16 |
| IO15 | D3 | GPIO17 |
| EN | RESET | GPIO54 |
| IO2 | boot strap | GPIO6 |

关键配置：

```text
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

在 Waveshare 板上，P4 软件视角的 C6 reset 是高有效。改错会导致 CMD5 `INVALID_RESPONSE`。

### USB-to-UART bridge

| 信号 | ESP32-P4 GPIO |
|---|---:|
| RXD (PC -> P4) | GPIO37 |
| TXD (P4 -> PC) | GPIO38 |

这是 `idf.py flash monitor` 使用的 Type-C 口。

### I2C0

| 信号 | ESP32-P4 GPIO |
|---|---:|
| SDA | GPIO7 |
| SCL | GPIO8 |

当前从设备：

- ES8311 audio codec：`0x18`
- BNO085 IMU：`0x4A`（AD0 拉高时为 `0x4B`）
- BMP388 气压计：`0x76`（SDO→GND）—— 载板，驱动已实现（`baro_task.c`，轮询 ~10 Hz）

### USB HS OTG P1

P1 是 RTL-SDR 数据路径，接到 ESP32-P4 专用 USB HS PHY，不是 GPIO24/25。

```text
P1: VBUS / D- / D+ / GND
```

## 3. 外接模块

### TK024F3036 / ST7789 SPI 屏

当前验证屏幕：2.4 寸 TK024F3036 / ST7789 320x240 半透反射 SPI 屏，配 `TK024F304189-SPI` 转接板。

| 转接板 label | ESP32-P4 pin | 说明 |
|---|---|---|
| GND | 任意 GND | 建议短地线 |
| 3V3 | 3V3 | 板载 LDO 输出 |
| BL | GPIO50 | LEDC PWM 背光 |
| D/C | GPIO31 | 命令 / 数据选择 |
| CS | GPIO28 | SPI2_CS_PAD |
| SCK | GPIO30 | SPI2_CK_PAD |
| MISO | 不接 | `PK_LCD_PIN_MISO = -1` |
| MOSI | GPIO29 | SPI2_D_PAD |

左排 GPIO28/29/30/31 是当前推荐接线。旧右排 GPIO33/46/47/48 方案已废弃：它走 GPIO matrix，并且 GPIO46 与 GPIO47 中间夹 GND，排线容易误短路。

### BNO085 / GY-BN008X IMU

| 引脚 | 连接 | 说明 |
|---|---|---|
| VCC | 3V3 | 模块有板载 LDO，3.3 V 可用 |
| GND | GND |  |
| SCL | GPIO8 | I2C0 SCL |
| SDA | GPIO7 | I2C0 SDA |
| AD0 | GND | 地址 `0x4A` |
| CS | 3V3 | I2C 模式下禁用 SPI |
| INT | GPIO20 | 当前未作为 IRQ |
| RST | GPIO21 | active-low reset |
| PS1 | GND | 协议选择 |
| PS0 | GND | `PS1=0, PS0=0` 为 I2C 模式 |

`PS0`、`PS1`、`AD0`、`CS` 不要悬空，否则 BNO085 可能进入 UART / SPI 模式，I2C 无响应。

安装方向建议：

- 芯片 +X 指向设备前方。
- 芯片 +Y 指向设备右侧。
- 芯片 +Z 指向设备下方。

### 按钮

4 个常开轻触按钮，按下时把对应 GPIO 接到 GND。固件启用内部 pull-up，不需要外部电阻。

| 按钮 | GPIO | 功能 |
|---|---:|---|
| TARE | 26 | IMU tare / 持久化 / 工厂重置；Settings 移动选中行；ADS-B LIST 绑定 own-ship |
| MODE | 5 | 短按循环 PFD → TRAFFIC → LIST → SETTINGS → ABOUT → DIAG；长按 deep sleep |
| UP | 22 | Traffic/List 选目标、Settings 调整、About/Diag 上滚 |
| DOWN | 23 | Traffic/List 选目标、Settings 调整、About/Diag 下滚 |

UP + DOWN 同时按住 5 秒保留给 BLE pairing window。

### BMP388 气压计（I2C0，载板）

**状态**：驱动已实现——`baro_task.c`，经 I2C0 轮询 ~10 Hz。
提供气压高度 + 升降率（QNH 可调）；结果写入 `g_baro_state`，
在 PFD 右侧面板和 DIAG 诊断 view 中渲染。

与 ES8311、BNO085 共用 I2C0（地址不冲突）。

| BMP388 引脚 | net | 接到 | 说明 |
|---|---|---|---|
| VCC | +3V3 | 3V3 | |
| GND | GND | GND | |
| SCL | I2C0_SCL | GPIO8 | 共用总线 |
| SDA | I2C0_SDA | GPIO7 | 共用总线 |
| SDO | GND | GND | 地址 `0x76` |
| CSB | +3V3 | 3V3 | 拉高选 I2C 模式 |
| INT | BARO_INT | **GPIO27** | data-ready 中断（可选）|

### GPS 模块（GT-U8 / ATGM336H，UART + PPS，载板）

**状态**：驱动已实现——`gps_task.c`，UART1 NMEA 解析。
提供本机位置 / 速度 / 航向，以及 1 PPS 精密授时脉冲。

GOOUUU **GT-U8** 模块（AT6558 / ATGM336H GNSS 核心）。板载 LDO 宽压
3.3–5 V；GNSS 核心与 UART 都是 3.3 V，UART 直连 ESP32-P4，无需电平转换。

| GPS 引脚 | net | 接到 | 说明 |
|---|---|---|---|
| V (VCC) | +3V3 | 3V3 | 丝印写 "5v"，但宽压模块接 3.3 V 即可，**不要**误以为必须 5 V |
| G (GND) | GND | GND | |
| T (TXD) | GPS_TX | **GPIO33** | GPS → **P4 UART RX**（交叉）|
| R (RXD) | GPS_RX | **GPIO32** | **P4 UART TX** → GPS（交叉）|
| P (PPS) | GPS_PPS | **GPIO46** | 1 Hz 秒脉冲，上升沿对齐 UTC 整秒 |

> UART 按标准交叉（GPS TX → P4 RX，GPS RX → P4 TX）。

## 4. 当前空闲 GPIO

扣除当前固件使用和板载固定外设后，主要可自由使用的 header GPIO：

```text
GPIO47  GPIO48
GPIO49  GPIO51  GPIO52
```

载板占用了原 free pool 的 4 个：GPIO27 → BMP388 INT、GPIO32/33 → GPS UART、
GPIO46 → GPS PPS（见 §3）。载板前为 9 个。

注意 GPIO46 与 GPIO47 中间有 GND，做排线时要避免误插导致短路。

## 5. Bring-up 快速检查

1. LCD 优先使用左排 GPIO28/29/30/31，不使用旧右排方案。
2. BNO085 的 `PS0` / `PS1` / `AD0` / `CS` 必须固定电平。
3. MODE 必须接 GPIO5，才能支持 deep sleep 唤醒。
4. RTL-SDR 必须接 P1 USB HS OTG，不是 Type-C 串口，也不是 GPIO24/25。
5. C6 首次烧录用 H4 header，烧完记得移除 IO9 到 GND 的短接。
