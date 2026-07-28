# ESP32-P4-WIFI6-Touch-LCD-4.3 引脚与 GPIO 映射

英文版：[`board_pinout.md`](board_pinout.md)

本文是 4.3 寸一体板固件 GPIO 分配的硬件事实源。显示、触摸和背光以
[`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf)
及微雪官方 BSP 为准；Pilot Kit 外接 IMU、气压计和 GPS 的重映射以阶段 1
冲突表为准。

## 1. 关键分配

| 功能 | GPIO / 参数 | 说明 |
|---|---|---|
| LCD RESET | GPIO27 | ST7701 active-low reset |
| LCD 背光 PWM | GPIO26 | AP3032 FB 注入；LEDC `output_invert=1` |
| LCD 背光使能 | GPIO33 | 100 kΩ 上拉，固件通常不驱动 |
| DSI PHY | 2 lane @ 500 Mbps | LDO_VO3（channel 3）@ 2.5 V |
| DPI | 480×800 RGB565 @ 30 MHz | PPA 旋转为逻辑 800×480 |
| 触摸 I²C | SDA GPIO7 / SCL GPIO8 | GT911 地址先探 0x5D，再探 0x14 |
| 触摸 RESET | GPIO23 | 板载固定 |
| 触摸 INT | GPIO2 | R35 默认不贴，当前按轮询设计 |
| BNO085 | SDA 7 / SCL 8 / INT 20 / RST 21 | 与触摸、codec 共用 I²C0 |
| BMP388 | SDA 7 / SCL 8 / INT 31 | 当前轮询；INT 从 GPIO27 重映射 |
| GPS UART1 | P4 TX GPIO32 / P4 RX GPIO51 | RX 从 GPIO33 重映射 |
| GPS PPS | GPIO46 | 1 Hz 授时 |
| RTL-SDR | P1 USB HS VBUS/D−/D+/GND | 不是 GPIO24/25 |

GPIO23/26/27/33 已被一体板固定占用，不能再用于旧实体键、气压中断或
GPS RX。4.3 寸版本不启动 `button_task.c`。

## 2. 板载固定外设

### MicroSD（SDMMC Slot 0）

| 信号 | GPIO |
|---|---:|
| CLK | 43 |
| CMD | 44 |
| D0 | 39 |
| D1 | 40 |
| D2 | 41 |
| D3 / CD | 42 |

Slot 1 已由 P4↔C6 的 ESP-Hosted SDIO 占用。IDF 6.0 下必须保持
`pk_sdcard.c` 的 **Slot 0 + dummy `host.init` / `host.deinit`** 方案，
避免重复初始化同一个 SDMMC 控制器。

### ESP32-C6（ESP-Hosted SDIO slave）

| C6 pad | 功能 | P4 GPIO |
|---|---|---:|
| IO19 | CLK | 18 |
| IO18 | CMD | 19 |
| IO12 | D0 | 14 |
| IO13 | D1 | 15 |
| IO14 | D2 | 16 |
| IO15 | D3 | 17 |
| EN | RESET | 54 |
| IO2 | boot strap | 6 |

必须保持：

```text
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

### CH343P 烧录串口

| 信号 | P4 GPIO |
|---|---:|
| PC TX → P4 RX | 37 |
| P4 TX → PC RX | 38 |

烧录使用靠近 BOOT 键的 Type-C 口。

### I²C0

| 信号 | GPIO |
|---|---:|
| SDA | 7 |
| SCL | 8 |

当前设备：

- ES8311 codec：`0x18`
- BNO085：`0x4A`，AD0 拉高时 `0x4B`
- BMP388：`0x76`
- GT911：动态探测 `0x5D` / `0x14`

## 3. 显示与触摸

### ST7701 MIPI-DSI

| 项目 | 值 |
|---|---|
| 原生方向 | 480×800 竖屏 |
| 应用方向 | 800×480 横屏 |
| DSI | 2 lane @ 500 Mbps |
| DPI pixel clock | 30 MHz |
| 色彩 | RGB565 |
| H porch | back 42 / pulse 12 / front 42 |
| V porch | back 2 / pulse 8 / front 60 |
| RESET | GPIO27 |
| BL PWM | GPIO26，反相 LEDC |
| BL EN | GPIO33，默认上拉使能 |

固件的应用 framebuffer 为 800×480 RGB565-swapped。每帧经 PPA
顺时针旋转 90°并 `byte_swap` 到两个 480×800 DPI framebuffer 之一，
随后在 VSYNC 切换，防止扫描中写入造成撕裂。

### GT911

| 信号 | GPIO |
|---|---:|
| SDA | 7 |
| SCL | 8 |
| RESET | 23 |
| INT | 2（R35 默认不贴） |

坐标上报格式以 `GT911-programming-guide.pdf` 为准，不能只参考 datasheet。

## 4. Pilot Kit 外接模块

### BNO085

| 引脚 | 连接 |
|---|---|
| VCC / GND | 3V3 / GND |
| SCL / SDA | GPIO8 / GPIO7 |
| INT / RST | GPIO20 / GPIO21 |
| AD0 / PS1 / PS0 | GND / GND / GND |
| CS | 3V3 |

安装方向：芯片 +X 向设备前方、+Y 向右、+Z 向下。

### BMP388

| 引脚 | 连接 |
|---|---|
| SCL / SDA | GPIO8 / GPIO7 |
| SDO / CSB | GND / 3V3（地址 `0x76`） |
| INT | GPIO31（可选；驱动当前约 10 Hz 轮询） |

### GPS（GT-U8 / ATGM336H）

| GPS 引脚 | 连接 |
|---|---|
| TXD | GPIO51（GPS → P4 UART RX） |
| RXD | GPIO32（P4 UART TX → GPS） |
| PPS | GPIO46 |
| VCC / GND | 3V3 / GND |

GPIO51 是必须重映射：GPIO33 已是 LCD `BL_EN`，继续接 GPIO33 会让 GPS
完全收不到 NMEA。

## 5. 旧硬件与空闲 GPIO

2.4 寸载板的 SPI LCD（GPIO28/29/30/31/50）和四实体键
（GPIO26/5/22/23）只作为 `docs/jlc/` 中的历史 PCB 参考。当前固件不初始化
实体键，旧 SPI LCD 组件也不参与显示路径。

当前无冲突的常用扩展脚：

```text
GPIO5 GPIO22 GPIO28 GPIO29 GPIO30 GPIO47 GPIO48 GPIO49 GPIO50 GPIO52
```

GPIO31 预留 BARO_INT；GPIO32/51/46 属于 GPS。GPIO2/3/4 与 GPIO24/25
可用但会影响 JTAG 或 USB Serial/JTAG，不列入首选。

## 6. Bring-up 检查

1. 串口出现 `ST7701 DSI ready: logical 800x480 -> PPA 90 CW`。
2. 启动画面覆盖全屏，颜色正常、方向正确、连续刷新无撕裂。
3. 日志确认旧实体键任务未启动，GPS 显示 RX=GPIO51，BARO INT=GPIO31。
4. microSD 保持 Slot 0 + dummy host init，C6 SDIO 无 CMD5 错误。
5. RTL-SDR 接 P1 USB HS 排针并成功枚举。
6. 诊断页核对 IMU / BARO / GPS / SDR / BLE / microSD。
7. 完成 10 次断电冷启动，确认无 ESP-Hosted boot loop。
