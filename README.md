# Pilot Kit Box (ESP32-P4 Edition)

<p align="center">
  <strong>开源便携式 MCU 航电网关 | Open-source portable MCU avionics gateway</strong><br>
  官方网站 / Official website: <a href="https://air.club">air.club</a><br>
  网页刷机 / Web flasher: <a href="https://updater.pilotkit.app">updater.pilotkit.app</a>
</p>

## 项目概览 / Overview

**Pilot Kit Box** 是面向通用航空态势感知的便携式航电设备。当前 ESP32-P4 版本把传统依赖 Linux 板卡的 ADS-B 接收链路压缩到单片机 + RTOS 架构：ESP32-P4 通过原生 USB 2.0 HS 直接驱动 RTL-SDR，实时接收 1090 MHz ADS-B / Mode-S 信号，在本机完成 dump1090 派生的 DSP 解码、CPR 定位融合、飞机状态聚合，并通过 BLE GATT、串口和本地 LittleFS 文件输出给移动端或调试工具。

**Pilot Kit Box** is a portable avionics gateway for general-aviation situational awareness. The ESP32-P4 edition removes the Linux SBC from the ADS-B path: the P4 drives an RTL-SDR receiver over native USB 2.0 HS, decodes 1090 MHz ADS-B / Mode-S frames on-device, fuses per-aircraft state, and publishes traffic over BLE GATT, serial output, and local LittleFS logs.

## 安全与适航边界 / Safety And Certification Boundary

Pilot Kit Box 是开源原型和态势感知设备，当前仓库没有 FAA、EASA、CAAC 或其他适航/TSO 认证。它不能作为主飞行仪表、备用飞行仪表、导航源或防撞系统使用；任何飞行决策必须以认证航电、机载仪表、目视观察和适用法规为准。

Pilot Kit Box is an open-source prototype and situational-awareness device. This repository does not represent FAA, EASA, CAAC, TSO, or other airworthiness certification. Do not use it as a primary flight instrument, backup flight instrument, navigation source, or collision-avoidance system; flight decisions must remain based on certified avionics, installed instruments, visual scan, and applicable regulations.

## 当前状态 / Current Status

截至 **2026-05-23**，主固件已经覆盖 ADS-B 接收、解码、BLE 分发、本地记录、LCD PFD、BNO085 姿态融合、四按钮交互、中英文 UI 和 MODE 长按深睡眠。

As of **2026-05-23**, the main firmware includes ADS-B reception and decode, BLE distribution, local recording, LCD PFD rendering, BNO085 attitude fusion, four-button interaction, English/Chinese UI pages, and MODE long-press deep sleep.

## 核心特性 / Features

| 功能 | Feature | 状态 / Status |
|---|---|---|
| ESP32-P4 + FreeRTOS 固件，无 Linux 启动链路 | ESP32-P4 + FreeRTOS firmware, no Linux boot chain | 已实现 / Implemented |
| USB 2.0 HS 直连 RTL-SDR，1090 MHz，2 MSPS IQ8 数据流 | USB 2.0 HS RTL-SDR path at 1090 MHz, 2 MSPS IQ8 | 已实现 / Implemented |
| 512 KiB IQ ring buffer、非阻塞 USB 回调、DSP 任务解码 | 512 KiB IQ ring buffer, non-blocking USB callback, DSP decode task | 已实现 / Implemented |
| dump1090 派生 Mode-S 解码、CRC 过滤、CPR 全球定位 | dump1090-derived Mode-S decode, CRC filtering, CPR global position decode | 已实现 / Implemented |
| 64 架飞机状态表：呼号、高度、位置、速度、垂直速度、应答机码、机型 | 64-aircraft state table: callsign, altitude, position, velocity, vertical rate, squawk, type | 已实现 / Implemented |
| UART、LittleFS 轮转文件、BLE raw ts-line 三路记录输出 | UART, rotating LittleFS files, and BLE raw ts-line output | 已实现 / Implemented |
| BLE GATT：GDL90 Traffic、Heartbeat、Raw、Time Sync 特征 | BLE GATT: GDL90 Traffic, Heartbeat, Raw, and Time Sync characteristics | 已实现 / Implemented |
| iOS Current Time Service 自动校时，Android/跨平台可写 Time Sync | iOS Current Time Service auto-sync, Android/cross-platform Time Sync writes | 已实现 / Implemented |
| TK024F3036 / ST7789 320x240 SPI 屏幕，PFD 约 30 FPS | TK024F3036 / ST7789 320x240 SPI display, PFD around 30 FPS | 已实现 / Implemented |
| G1000 风格 PFD：姿态、航向/HSI、高度带、GS/VS、ADS-B 数量 | G1000-style PFD: attitude, heading/HSI, altitude tape, GS/VS, ADS-B count | 已实现 / Implemented |
| ADS-B 列表页：ICAO、呼号、国家、ALT、SPD、HDG、VS、SQK、TYPE 和详情面板 | ADS-B list page: ICAO, callsign, country, ALT, SPD, HDG, VS, SQK, TYPE, and detail pane | 已实现 / Implemented |
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

以下为当前原型的人民币成本参考，实际价格会随采购渠道、数量、运费和替代料变化。

The following RMB costs are reference prices for the current prototype and will vary with supplier, quantity, shipping, and substitutions.

| 物料 | Part | 参考成本 / Reference Cost |
|---|---|---:|
| 微雪 ESP32P4C6 | Waveshare ESP32P4C6 | ¥76 |
| BNO085 | BNO085 | ¥76 |
| RTL-SDR FC0013 | RTL-SDR FC0013 | ¥10 |
| IPEX、MCX、SMA 线、座子等 | IPEX / MCX / SMA cables, sockets, and RF adapters | ¥2 |
| 5V 2A / 2.4A Type-C 口充电模块 | 5V 2A / 2.4A Type-C charging module | ¥4 |
| 3.7V 10000mAh 锂电池 | 3.7V 10000mAh lithium battery | ¥25 |
| 2.4 寸半透反射屏 | 2.4-inch transflective display | ¥38 |
| USB-A 母座 | USB-A female socket | ¥0.3 |
| **合计** | **Total** | **约 ¥231.3** |

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
| `firmware/main/` | `firmware/main/` | 应用层 C 源码 / Application C sources |
| `firmware/components/esp32-rtl-sdr/` | `firmware/components/esp32-rtl-sdr/` | RTL-SDR USB/SDR 组件 / RTL-SDR USB/SDR component |
| `firmware/scripts/` | `firmware/scripts/` | 字体、数据库和测试脚本 / Font, database, and test scripts |
| `docs/` | `docs/` | 构建、协议、架构、用户和硬件文档 / Build, protocol, architecture, user, and hardware docs |
| `web/flasher/` | `web/flasher/` | ESP Web Tools 网页刷机页面 / ESP Web Tools web flasher |
| `tools/firmware_release/` | `tools/firmware_release/` | 固件发布打包工具 / Firmware release packaging tools |

## 快速开始 / Quick Start

| 我想做什么 | What I want | English | 中文 |
|---|---|---|---|
| 从零安装、编译、烧录 | Set up, build, and flash from scratch | [`docs/BUILD.md`](docs/BUILD.md) | [`docs/BUILD-zh_CN.md`](docs/BUILD-zh_CN.md) |
| 用网页更新 ESP32-P4 固件 | Update ESP32-P4 firmware from browser | [updater.pilotkit.app](https://updater.pilotkit.app) | [updater.pilotkit.app](https://updater.pilotkit.app) |
| 发布维护者固件包 | Publish a maintainer firmware release | [`docs/firmware_update.md`](docs/firmware_update.md) | [`docs/firmware_update-zh_CN.md`](docs/firmware_update-zh_CN.md) |
| 看运行时任务和数据流 | Understand runtime tasks and data flow | [`docs/architecture.md`](docs/architecture.md) | [`docs/architecture-zh_CN.md`](docs/architecture-zh_CN.md) |
| 调整 sdkconfig | Tune sdkconfig options | [`docs/configuration.md`](docs/configuration.md) | [`docs/configuration-zh_CN.md`](docs/configuration-zh_CN.md) |
| 集成移动端 BLE | Integrate a mobile BLE client | [`docs/ble_protocol.md`](docs/ble_protocol.md) | [`docs/ble_protocol-zh_CN.md`](docs/ble_protocol-zh_CN.md) |
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
