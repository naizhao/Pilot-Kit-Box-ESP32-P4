# Pilot Kit Box (ESP32-P4 Edition)

<p align="center">
  <strong>开源低成本飞行数据盒子与态势感知设备 | Open-source low-cost flight data box and situational-awareness device</strong><br>
  官方网站 / Official website: <a href="https://air.club">air.club</a><br>
  网页刷机 / Web flasher: <a href="https://updater.pilotkit.app">updater.pilotkit.app</a>
</p>

## 项目概览 / Overview

**Pilot Kit Box** 是一个开源、低成本、便携式的飞行数据盒子和航空态势感知设备，可与 [Pilot Kit](https://air.club) 深度结合使用，也可以作为独立设备运行。

如果你熟悉 Stratux、Sentry、Garmin GDL 系列或 uAvionix ping 这类便携式 ADS-B / 飞行数据盒子，可以把 Pilot Kit Box 理解成一个更开放、更低成本、可自己搭建和二次开发的替代方案。它面向飞行员、飞行学员、飞行爱好者以及通航开发者，用常见硬件完成飞行记录、姿态显示、周边交通接收和后续数据分析。

它的核心价值不是单纯“看天上有哪些飞机”，而是帮助用户低成本记录、理解和管理自己的飞行数据。它可以随飞机一起使用，记录每一次飞行中的关键数据，用于飞行日志、回放、分享和分析；也可以接收附近飞机公开广播的 ADS-B / Mode-S 信号，在本机屏幕上显示周边交通信息。

你可以把它理解成一个“飞行员自己的小型数据记录与接收盒子”：它既能独立工作，也能和 Pilot Kit 紧密结合；未来也可以通过标准航空数据格式与 Garmin Pilot、ForeFlight 等第三方 EFB 软件配合使用。

Pilot Kit Box can work as a standalone device or integrate closely with [Pilot Kit](https://air.club). If you know products such as Stratux, Sentry, Garmin GDL, or uAvionix ping, Pilot Kit Box is best understood as a more open, lower-cost, builder-friendly flight data and ADS-B box. It is intended for pilots, student pilots, aviation enthusiasts, and general-aviation developers who want local flight recording, attitude display, nearby traffic reception, and data they can later review or share.

The main point is not just watching aircraft overhead. Pilot Kit Box is meant to help users record, understand, and manage their own flying at a much lower cost. It can record each flight for logs, replay, sharing, and analysis; it can also receive public ADS-B / Mode-S broadcasts from nearby aircraft and show surrounding traffic on the built-in display.

Think of it as a pilot's own small flight data recorder and receiver: it can run independently, integrate tightly with Pilot Kit, and evolve toward standard aviation data outputs for third-party EFB apps such as Garmin Pilot and ForeFlight.

## 使用场景 / Use Cases

Pilot Kit Box 的核心用途是：让普通飞行员、飞行学员和飞行爱好者，也能用较低成本拥有一个自己的飞行记录和态势感知盒子。

Pilot Kit Box is designed to give pilots, student pilots, and aviation enthusiasts an affordable personal flight recording and situational-awareness box.

- 记录自己的每一次飞行，为飞行日志、回放、分享和分析提供数据基础。<br>Record each flight as source data for logs, replay, sharing, and analysis.
- 在本机屏幕上显示姿态、航向、高度、速度、垂直速度等飞行状态信息。<br>Display flight state such as attitude, heading, altitude, speed, and vertical speed on the local screen.
- 接收附近飞机的 ADS-B / Mode-S 广播，显示周边交通、航班号、国家、航司、机型和注册号等信息。<br>Receive nearby ADS-B / Mode-S traffic and show callsign, country, airline, aircraft type, and registration when known.
- 在没有互联网的情况下，本地接收和记录航空数据，而不是依赖在线航班网站。<br>Receive and record aviation data locally without relying on online flight-tracking websites.
- 与 Pilot Kit 软件结合，把硬件采集到的数据用于更完整的飞行记录、回放、分析和分享体验。<br>Use captured device data with Pilot Kit for richer flight records, replay, analysis, and sharing.
- 作为通用飞行数据盒子继续扩展；路线图中的 GDL90 over Wi-Fi 目标是连接 Garmin Pilot、ForeFlight 等 EFB。<br>Continue evolving as a general-purpose flight data box; the roadmap GDL90 over Wi-Fi path targets EFBs such as Garmin Pilot and ForeFlight.
- 给通航、航电、嵌入式和 SDR 爱好者提供一个完整、开源、可学习的参考实现。<br>Provide a complete open-source reference for general aviation, avionics, embedded systems, and SDR builders.

## 为什么要做这个设备 / Why Build This

市面上已经有 Stratux、Sentry、Garmin GDL、uAvionix ping 等便携式 ADS-B 接收器、飞行记录盒子和通航辅助设备，但很多产品要么价格较高，要么生态相对封闭，要么不方便用户理解、修改和扩展内部的数据链路。

Pilot Kit Box 想解决的是这个问题：

- **低成本**：用 ESP32-P4、RTL-SDR、屏幕、IMU 和常见电源模块，做出一个普通用户也能负担的飞行数据盒子。
- **通用性**：它不是只能配合某一个 App 使用的封闭硬件；它可以独立使用，也可以面向标准航空数据接口继续扩展。
- **与 Pilot Kit 紧密结合**：Pilot Kit 可以充分利用 Box 采集的数据，提供更完整的飞行记录、回放、分析和分享能力。
- **本地记录**：飞行数据可以在设备本地记录，为飞行复盘和个人飞行档案提供基础。
- **态势感知**：除了记录自己的飞行，也能接收附近 ADS-B / Mode-S 交通，帮助理解周边空域情况。
- **开源透明**：从 SDR 接收、ADS-B 解码、状态聚合、屏幕显示到数据库维护，整个链路都可以检查、修改和复现。
- **可继续扩展**：它可以是一个低成本替代方案，也可以作为移动端、云端分析、EFB 集成、Wi-Fi GDL90 输出和硬件外壳迭代的平台。

Portable ADS-B receivers and flight data boxes already exist, including Stratux, Sentry, Garmin GDL, and uAvionix ping. Many of them are effective products, but they can be expensive, relatively closed, or hard for users to inspect, modify, and extend.

Pilot Kit Box focuses on:

- **Low cost**: ESP32-P4, RTL-SDR, display, IMU, and common power modules keep the hardware approachable.
- **General-purpose use**: it is not locked to one app; it can run on its own and evolve toward standard aviation data interfaces.
- **Tight Pilot Kit integration**: Pilot Kit can use Box data for richer flight records, replay, analysis, and sharing.
- **Local recording**: flight data can be recorded on the device for review and personal archives.
- **Situational awareness**: the same box can receive nearby ADS-B / Mode-S traffic and help users understand surrounding airspace.
- **Open implementation**: SDR reception, ADS-B decoding, state fusion, display rendering, and database maintenance are all inspectable and reproducible.
- **Room to grow**: it can be a low-cost alternative today and a platform for mobile apps, cloud analysis, EFB integration, Wi-Fi GDL90 output, and enclosure iterations later.

## 技术概览 / Technical Overview

当前 ESP32-P4 版本把传统依赖 Linux 板卡的 ADS-B 接收链路压缩到单片机 + RTOS 架构：ESP32-P4 通过原生 USB 2.0 HS 直接驱动 RTL-SDR，实时接收 1090 MHz ADS-B / Mode-S 信号，在本机完成 dump1090 派生的 DSP 解码、CPR 定位融合、飞机状态聚合，并通过 BLE GATT、串口和本地 LittleFS 文件输出给移动端或调试工具。

The ESP32-P4 edition removes the Linux SBC from the ADS-B path: the P4 drives an RTL-SDR receiver over native USB 2.0 HS, decodes 1090 MHz ADS-B / Mode-S frames on-device, fuses per-aircraft state, and publishes traffic over BLE GATT, serial output, and local LittleFS logs.

## 安全与适航边界 / Safety And Certification Boundary

Pilot Kit Box 是开源原型和态势感知设备，当前仓库没有 FAA、EASA、CAAC 或其他适航/TSO 认证。它不能作为主飞行仪表、备用飞行仪表、导航源或防撞系统使用；任何飞行决策必须以认证航电、机载仪表、目视观察和适用法规为准。

Pilot Kit Box is an open-source prototype and situational-awareness device. This repository does not represent FAA, EASA, CAAC, TSO, or other airworthiness certification. Do not use it as a primary flight instrument, backup flight instrument, navigation source, or collision-avoidance system; flight decisions must remain based on certified avionics, installed instruments, visual scan, and applicable regulations.

## 当前状态 / Current Status

截至 **2026-05-25**，主固件已经覆盖 ADS-B 接收、解码、BLE 分发、本地记录、LCD PFD、ADS-B 列表内置航空识别数据库、BNO085 姿态融合、四按钮交互、中英文 UI 和 MODE 长按深睡眠。

As of **2026-05-25**, the main firmware includes ADS-B reception and decode, BLE distribution, local recording, LCD PFD rendering, embedded aviation identity databases for the ADS-B list, BNO085 attitude fusion, four-button interaction, English/Chinese UI pages, and MODE long-press deep sleep.

## 核心特性 / Features

| 功能 | Feature | 状态 / Status |
|---|---|---|
| ESP32-P4 + FreeRTOS 固件，无 Linux 启动链路 | ESP32-P4 + FreeRTOS firmware, no Linux boot chain | 已实现 / Implemented |
| USB 2.0 HS 直连 RTL-SDR，1090 MHz，2 MSPS IQ8 数据流 | USB 2.0 HS RTL-SDR path at 1090 MHz, 2 MSPS IQ8 | 已实现 / Implemented |
| 512 KiB IQ ring buffer、非阻塞 USB 回调、DSP 任务解码 | 512 KiB IQ ring buffer, non-blocking USB callback, DSP decode task | 已实现 / Implemented |
| dump1090 派生 Mode-S 解码、CRC 过滤、CPR 全球定位 | dump1090-derived Mode-S decode, CRC filtering, CPR global position decode | 已实现 / Implemented |
| 最多同时跟踪 64 个 ADS-B / Mode-S 目标，并聚合呼号、高度、位置、速度、垂直速度、应答机码和机型信息 | Tracks up to 64 ADS-B / Mode-S targets at once, aggregating callsign, altitude, position, velocity, vertical rate, squawk, and aircraft type | 已实现 / Implemented |
| UART、LittleFS 轮转文件、BLE raw ts-line 三路记录输出 | UART, rotating LittleFS files, and BLE raw ts-line output | 已实现 / Implemented |
| BLE GATT：GDL90 Traffic、Heartbeat、Raw、Time Sync 特征 | BLE GATT: GDL90 Traffic, Heartbeat, Raw, and Time Sync characteristics | 已实现 / Implemented |
| iOS Current Time Service 自动校时，Android/跨平台可写 Time Sync | iOS Current Time Service auto-sync, Android/cross-platform Time Sync writes | 已实现 / Implemented |
| TK024F3036 / ST7789 320x240 SPI 屏幕，PFD 约 30 FPS | TK024F3036 / ST7789 320x240 SPI display, PFD around 30 FPS | 已实现 / Implemented |
| G1000 风格 PFD：姿态、航向/HSI、高度带、GS/VS、ADS-B 数量 | G1000-style PFD: attitude, heading/HSI, altitude tape, GS/VS, ADS-B count | 已实现 / Implemented |
| ADS-B 列表页：ICAO、呼号、国家、ALT、SPD、HDG、VS、SQK、TYPE 和详情面板 | ADS-B list page: ICAO, callsign, country, ALT, SPD, HDG, VS, SQK, TYPE, and detail pane | 已实现 / Implemented |
| 内置航空识别数据库：航司 ICAO/IATA、运营人名称、ICAO24 国家、注册号、机型和型号 | Embedded aviation identity databases: airline ICAO/IATA, operator name, ICAO24 country, registration, type, and model | 已实现 / Implemented |
| TARE 在 ADS-B 列表中绑定 own-ship，PFD 可用本机 ADS-B 数据显示 ALT/GS/VS | TARE binds own-ship in ADS-B list; PFD can source ALT/GS/VS from bound ADS-B traffic | 已实现 / Implemented |
| BNO085 100 Hz 姿态融合、校准向导、TARE 归零/持久化/工厂重置 | BNO085 100 Hz attitude fusion, calibration wizard, TARE zero/persist/factory reset | 已实现 / Implemented |
| Settings / About / Compass Calibration 中英文 UI，语言写入 NVS | English/Chinese Settings, About, and Compass Calibration UI, persisted in NVS | 已实现 / Implemented |
| Noto Sans SC 字形生成、中文 LCD 锐化曲线、英文硬像素路径 | Noto Sans SC glyph generation, sharpened CJK LCD alpha curve, crisp English bitmap path | 已实现 / Implemented |
| MODE 短按切换 PFD -> ADS-B LIST -> SETTINGS -> ABOUT；长按进入深睡眠，MODE 唤醒 | MODE short-press cycles PFD -> ADS-B LIST -> SETTINGS -> ABOUT; long-press sleeps, MODE wakes | 已实现 / Implemented |
| RTL-SDR IQ stall 触发软重连，多次失败后才重启整机 | RTL-SDR IQ-stall soft re-init before full restart fallback | 已实现 / Implemented |

## 硬件清单 / Hardware Bill of Materials

### 硬件预览 / Hardware Preview

| PFD / Primary Flight Display | ADS-B LIST / Aircraft List |
|---|---|
| <img src="images/PFD.jpg" alt="Pilot Kit Box PFD hardware preview" width="360"> | <img src="images/adsb-list.jpg" alt="Pilot Kit Box ADS-B list hardware preview" width="360"> |

### 必备硬件 / Required

| 硬件 | Hardware | 说明 / Notes |
|---|---|---|
| Waveshare ESP32-P4-WIFI6 | Waveshare ESP32-P4-WIFI6 | ESP32-P4NRW32 主控，32 MB Nor Flash，32 MB PSRAM，板载 ESP32-C6-MINI-1 无线协处理器。<br>Main ESP32-P4NRW32 board with 32 MB Nor Flash, 32 MB PSRAM, and on-board ESP32-C6-MINI-1 radio co-processor. |
| USB-C 数据线 | USB-C data cable | 用于 P4 烧录、串口监视和供电；必须是数据线。<br>Used for P4 flashing, serial monitoring, and power; it must support data, not charge-only. |
| RTL-SDR FC0013 USB Dongle | RTL-SDR FC0013 USB dongle | 当前推荐 FC0013 tuner 版本，成本低，适合本项目 1090 MHz ADS-B 接收；固件按 1090 MHz / 2 MSPS 配置，并使用最大手动增益。<br>Currently recommended with the FC0013 tuner because it is inexpensive and suitable for this 1090 MHz ADS-B receiver; firmware defaults to 1090 MHz / 2 MSPS and max manual gain. |
| USB Type-A 母座到 MX1.25 4-pin OTG 线 | USB Type-A female to MX1.25 4-pin OTG cable | 接到板载 P1 USB HS OTG 口，给 RTL-SDR 走高速 USB 数据路径。<br>Connects the RTL-SDR to the board's P1 USB HS OTG port for the high-speed USB data path. |
| 1090 MHz ADS-B 天线 | 1090 MHz ADS-B antenna | 接 RTL-SDR；实际接收距离强依赖天线位置和供电噪声。<br>Connects to the RTL-SDR; real-world range depends strongly on antenna placement and power noise. |
| 2.4 寸 TK024F3036 / ST7789 半透反射 SPI 屏 + `TK024F304189-SPI` 转接板 | 2.4-inch TK024F3036 / ST7789 transflective SPI display with `TK024F304189-SPI` breakout | 已验证；SPI2 左排 GPIO 28/29/30/31，背光 GPIO50。<br>Verified; SPI2 is wired on the left header at GPIO 28/29/30/31 with backlight on GPIO50. |
| GY-BN008X / BNO085 IMU 模块 | GY-BN008X / BNO085 IMU module | I2C0：SDA GPIO7、SCL GPIO8；INT GPIO20、RST GPIO21。<br>I2C0 wiring: SDA GPIO7, SCL GPIO8, INT GPIO20, RST GPIO21. |
| 4 个常开轻触按键 | Four normally-open tact buttons | TARE GPIO26，MODE GPIO5，UP GPIO22，DOWN GPIO23；按下接 GND。<br>TARE GPIO26, MODE GPIO5, UP GPIO22, DOWN GPIO23; each button shorts its GPIO to GND when pressed. |

### BOM 成本参考 / BOM Cost Reference

以下为当前原型的人民币成本参考，实际价格会随采购渠道、数量、运费和替代料变化。美元价格按粗略汇率 **¥1 ≈ $0.15** 估算；反向参考 **$1 ≈ ¥6.78**。

The following RMB costs are reference prices for the current prototype and will vary with supplier, quantity, shipping, and substitutions. USD prices are rough estimates using **¥1 ≈ $0.15**; reverse reference is **$1 ≈ ¥6.78**.

| 物料 | Part | 人民币参考 / RMB Reference | 美元估算 / USD Estimate |
|---|---|---:|---:|
| 微雪 ESP32P4C6 | Waveshare ESP32P4C6 | ¥76 | ~$11.40 |
| BNO085 | BNO085 | ¥76 | ~$11.40 |
| RTL-SDR FC0013 | RTL-SDR FC0013 | ¥10 | ~$1.50 |
| IPEX、MCX、SMA 线、座子等 | IPEX / MCX / SMA cables, sockets, and RF adapters | ¥2 | ~$0.30 |
| 5V 2A / 2.4A Type-C 口充电模块 | 5V 2A / 2.4A Type-C charging module | ¥4 | ~$0.60 |
| 3.7V 10000mAh 锂电池 | 3.7V 10000mAh lithium battery | ¥25 | ~$3.75 |
| 2.4 寸半透反射屏 | 2.4-inch transflective display | ¥38 | ~$5.70 |
| USB-A 母座 | USB-A female socket | ¥0.3 | ~$0.05 |
| **合计** | **Total** | **约 ¥231.3** | **约 $34.70** |

### 首次设置或选配 / Setup And Optional

| 硬件 | Hardware | 说明 / Notes |
|---|---|---|
| USB-UART 转接器 | USB-UART adapter | 每块新 Waveshare 板首次烧 ESP32-C6 hosted slave 固件时需要；详见 [`docs/hardware/c6_slave_firmware.md`](docs/hardware/c6_slave_firmware.md)。<br>Required once per fresh Waveshare board to flash the ESP32-C6 hosted slave firmware; see [`docs/hardware/c6_slave_firmware.md`](docs/hardware/c6_slave_firmware.md). |
| 杜邦线 / 短接线 | Jumper wires / shorting wire | 连接 LCD、IMU、按钮，以及 C6 首次烧录时短接 IO9 到 GND。<br>Used for LCD, IMU, and button wiring, plus shorting C6 IO9 to GND during first-time C6 flashing. |
| 5V 2A / 2.4A Type-C 口充电模块 | 5V 2A / 2.4A Type-C charging module | 电池供电版本使用；给系统提供稳定 5V 输入。<br>Used in battery-powered builds to provide a stable 5V system input. |
| 3.7V 10000mAh 锂电池 | 3.7V 10000mAh lithium battery | 便携版本的电源选项；容量可按外壳和续航目标调整。<br>Portable power option; capacity can be adjusted for enclosure size and endurance target. |
| 5 V 外部供电 | External 5 V power | RTL-SDR 功耗约数百 mA，电脑 USB 口供电不稳时建议使用更可靠供电。<br>RTL-SDR dongles can draw a few hundred mA; use a reliable 5V supply if a computer USB port is unstable. |
| 外壳、支架、屏蔽和固定件 | Enclosure, mounts, shielding, fixtures | 当前仓库主要维护固件和接线文档，机械结构可按实际安装补充。<br>This repository mainly maintains firmware and wiring docs; mechanical parts should be adapted to the actual installation. |

### 板载资源 / On-board Resources

- ESP32-C6-MINI-1 通过 SDIO 作为 Wi-Fi 6 / BLE 5 协处理器；当前固件使用 BLE。
- MicroSD 卡座已在硬件上可用，当前记录后端默认使用 16 MiB LittleFS 分区。
- CH343P USB-UART 桥用于 P4 烧录和串口监视。
- ES8311 音频 codec、麦克风、扬声器功放为板载资源，当前航电路径尚未使用。

- The ESP32-C6-MINI-1 is connected over SDIO as the Wi-Fi 6 / BLE 5 co-processor; the current firmware uses BLE.
- The MicroSD slot is present on the board; the current recording backend defaults to a 16 MiB LittleFS partition.
- The CH343P USB-UART bridge handles P4 flashing and serial monitoring.
- The ES8311 codec, microphone, and speaker amplifier are on-board resources, not currently used by the avionics path.

## 目录结构 / Repository Structure

| 路径 | Path | 内容 / Contents |
|---|---|---|
| `firmware/` | `firmware/` | ESP-IDF v6.0.1 固件工程 / ESP-IDF v6.0.1 firmware project |
| `firmware/main/` | `firmware/main/` | 应用层 C 源码和内置航空识别数据库 / Application C sources and embedded aviation identity databases |
| `firmware/components/esp32-rtl-sdr/` | `firmware/components/esp32-rtl-sdr/` | RTL-SDR USB/SDR 组件 / RTL-SDR USB/SDR component |
| `firmware/scripts/` | `firmware/scripts/` | 字体、数据库和测试脚本 / Font, database, and test scripts |
| `docs/` | `docs/` | 构建、协议、架构、用户和硬件文档 / Build, protocol, architecture, user, and hardware docs |
| `web/flasher/` | `web/flasher/` | ESP Web Tools 网页刷机页面 / ESP Web Tools web flasher |
| `tools/firmware_release/` | `tools/firmware_release/` | 固件发布打包工具 / Firmware release packaging tools |

## 内置航空识别数据库 / Embedded Aviation Identity Databases

固件内置三类本地识别数据库，用于把 ADS-B / Mode-S 中收到的 ICAO24 地址和呼号显示为更容易核对的航空信息。它们不是航班计划、航线、时刻表或实时联网数据；终端用户通过更新完整固件获得新的数据库快照。

The firmware embeds three local identity databases so ICAO24 addresses and ADS-B callsigns can be rendered as operationally useful aviation information. These are not flight-plan, route, timetable, or live network databases; end users receive refreshed snapshots through full firmware updates.

| 数据库 | Database | 用途 / Purpose | 仓库位置 / Repository Location | 更新脚本 / Update Script |
|---|---|---|---|---|
| 飞机 ICAO24 数据库 | Aircraft ICAO24 database | ICAO24 -> 注册号、ICAO 机型代码、型号名称、Doc 8643 技术描述；当前快照约 570k 条记录、8.16 MiB。<br>ICAO24 -> registration, ICAO type code, model name, and Doc 8643 descriptor; the current snapshot is about 570k records / 8.16 MiB. | `firmware/main/aircraft_db.bin`, `firmware/main/aircraft_db.c`, `firmware/main/aircraft_db.h` | `firmware/scripts/gen_aircraft_db.py` |
| 航司代码表 | Airline code table | ADS-B 呼号前三位 ICAO 航司代码 -> IATA 代码和运营人名称，用于把 `CSN1234` 等显示为更常见的航班号形式。<br>ADS-B callsign ICAO prefix -> IATA code and operator name, used to render callsigns such as `CSN1234` in a more familiar form. | `firmware/main/airline_codes.c`, `firmware/main/airline_codes.h` | `firmware/scripts/gen_airline_codes.py --update-source` |
| ICAO24 国家地址段 | ICAO24 country ranges | ICAO24 地址段 -> ISO 3166-1 alpha-2 国家/地区代码和名称，用于 ADS-B LIST 的 CT 列和详情面板。<br>ICAO24 address range -> ISO 3166-1 alpha-2 country/region code and name for the ADS-B LIST CT column and detail pane. | `firmware/main/icao_country.c`, `firmware/main/icao_country.h` | `firmware/scripts/gen_icao_country.py` |

维护流程详见 / Maintenance workflow: [`docs/database_maintenance.md`](docs/database_maintenance.md) / [`docs/database_maintenance-zh_CN.md`](docs/database_maintenance-zh_CN.md)。

## 快速开始 / Quick Start

| 我想做什么 | What I want | English | 中文 |
|---|---|---|---|
| 从零安装、编译、烧录 | Set up, build, and flash from scratch | [`docs/BUILD.md`](docs/BUILD.md) | [`docs/BUILD-zh_CN.md`](docs/BUILD-zh_CN.md) |
| 用网页更新 ESP32-P4 固件 | Update ESP32-P4 firmware from browser | [updater.pilotkit.app](https://updater.pilotkit.app) | [updater.pilotkit.app](https://updater.pilotkit.app) |
| 发布维护者固件包 | Publish a maintainer firmware release | [`docs/firmware_update.md`](docs/firmware_update.md) | [`docs/firmware_update-zh_CN.md`](docs/firmware_update-zh_CN.md) |
| 看运行时任务和数据流 | Understand runtime tasks and data flow | [`docs/architecture.md`](docs/architecture.md) | [`docs/architecture-zh_CN.md`](docs/architecture-zh_CN.md) |
| 调整 sdkconfig | Tune sdkconfig options | [`docs/configuration.md`](docs/configuration.md) | [`docs/configuration-zh_CN.md`](docs/configuration-zh_CN.md) |
| 集成移动端 BLE | Integrate a mobile BLE client | [`docs/ble_protocol.md`](docs/ble_protocol.md) | [`docs/ble_protocol-zh_CN.md`](docs/ble_protocol-zh_CN.md) |
| 维护内置航空识别数据库 | Maintain embedded aviation identity databases | [`docs/database_maintenance.md`](docs/database_maintenance.md) | [`docs/database_maintenance-zh_CN.md`](docs/database_maintenance-zh_CN.md) |
| 使用四按钮和 PFD | Use the four buttons and PFD | [`docs/user_guide.md`](docs/user_guide.md) | [`docs/user_guide-zh_CN.md`](docs/user_guide-zh_CN.md) |
| 接 LCD、IMU、按钮、RTL-SDR | Wire LCD, IMU, buttons, and RTL-SDR | [`docs/hardware/board_pinout.md`](docs/hardware/board_pinout.md) | [`docs/hardware/board_pinout-zh_CN.md`](docs/hardware/board_pinout-zh_CN.md) |
| 首次烧 ESP32-C6 slave 固件 | Flash ESP32-C6 slave firmware once | [`docs/hardware/c6_slave_firmware.md`](docs/hardware/c6_slave_firmware.md) | [`docs/hardware/c6_slave_firmware-zh_CN.md`](docs/hardware/c6_slave_firmware-zh_CN.md) |
| 浏览全部文档语言覆盖 | Browse all docs and language coverage | [`docs/README.md`](docs/README.md) | [`docs/README-zh_CN.md`](docs/README-zh_CN.md) |

最短上手路径 / Minimal path:

```bash
# 1. 安装 ESP-IDF v6.0.1 / Install ESP-IDF v6.0.1
curl -L https://dl.espressif.com/dl/eim/eim-installer.sh | bash

# 2. 拉代码 / Clone the repository
git clone --recursive https://github.com/naizhao/Pilot-Kit-Box-ESP32-P4.git
cd Pilot-Kit-Box-ESP32-P4/firmware

# 3. 激活 IDF 并构建 / Activate IDF and build
source ~/.espressif/tools/activate_idf_v6.0.1.sh
./build.sh set-target esp32p4
./build.sh build

# 4. 烧录并监视 / Flash and monitor
./build.sh -p /dev/cu.usbmodem* flash monitor
```

> 新板如果要启用 BLE，必须先给板载 ESP32-C6 烧一次 hosted slave 固件。If BLE is needed on a fresh board, flash the on-board ESP32-C6 hosted slave firmware once first. See [`docs/BUILD.md`](docs/BUILD.md) section 3.

## 已知限制 / Known Limits

- BLE pairing-window 手势已检测；移动端配对窗口 UI 尚未实现。
- BLE Device Information Service 尚未暴露固件版本；版本目前显示在 boot splash 和 ABOUT 页。
- Wi-Fi 分发、配置写特征、MicroSD 记录后端和 OTA A/B 分区仍是后续工作。
- 当前 own-ship 数据来自配置或 ADS-B 列表中手动绑定的 ICAO；没有 GPS 输入时不会生成 GDL90 Ownship Report。
- ESP32-P4 v1.x 深睡眠 GPIO hold 有 silicon 限制，MODE 长按可进入深睡眠并唤醒，但背光残余电流仍受硬件版本影响。

- The BLE pairing-window gesture is detected; the mobile pairing-window UI is not implemented yet.
- BLE Device Information Service does not yet expose firmware version; the version is shown on the boot splash and ABOUT page.
- Wi-Fi distribution, configuration-write characteristics, MicroSD recording, and OTA A/B partitions are future work.
- Own-ship data currently comes from a configured or list-bound ICAO; without GPS input the firmware does not emit a GDL90 Ownship Report.
- ESP32-P4 v1.x has deep-sleep GPIO-hold limitations; MODE long-press sleep/wake works, but residual backlight current depends on hardware revision.

## 致谢 / Credits

- [`kvhnuke/esp32-rtl-sdr`](https://github.com/kvhnuke/esp32-rtl-sdr) — librtlsdr API 封装参考 / librtlsdr API wrapper reference
- [`XTR1984/xtrsdr`](https://github.com/XTR1984/xtrsdr) — ESP32 USB DMA 分包与吞吐调优参考 / ESP32 USB DMA transfer tuning reference
- dump1090 社区实现 — ADS-B / Mode-S 解码算法基础 / ADS-B / Mode-S decode algorithm foundation

## 开源协议 / License

MIT License
