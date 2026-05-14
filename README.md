# Pilot Kit Box (ESP32-P4 Edition)

<p align="center">
  <strong>开源的下一代便携式单片机航电网关 | Open-Source Portable Avionics Gateway</strong><br>
  🌐 官方网站 (Official Website): <a href="https://air.club">air.club</a>
</p>

## 📖 项目愿景 (Overview)
**Pilot Kit Box** 是一款专注于提升通用航空飞行安全与态势感知的便携式航电设备。本网关项目致力于将传统的、依赖笨重 Linux 板卡（如树莓派）的便携式 **ADS-B 接收系统 (ADS-B Receiver)**，彻底降维至 **纯 MCU (单片机) 实时操作系统 (RTOS)** 架构。

通过利用 **ESP32-P4** 芯片原生的高速 USB 2.0 (480Mbps) 接口，Pilot Kit Box 可以直接高速驱动 **RTL-SDR** (软件定义无线电) 接收机，实时监听 **1090MHz ADS-B** 飞行器广播信号。我们在芯片内部完成了 2MSPS 的高并发 I/Q 采样读取、DSP 曼彻斯特解码与 CRC 校验（边缘计算），最终将结构化的航空态势数据通过低功耗蓝牙 (BLE) 或 Wi-Fi 无缝推送至 **EFB (电子飞行包)** 等移动终端，实现精准的室外**飞行追踪 (Flight Tracking)** 与防撞预警。

## 🎯 核心特性 (Features)
* ⚡ **Bare-metal 级启动：** 彻底告别 Linux 引导，上电即刻开启 ADS-B 监听。
* 🚀 **硬件级 USB 吞吐：** 榨干 ESP32-P4 DWC2 控制器性能，稳定吃下 2MSPS 原始 SDR 射频数据流。
* 🧠 **RTOS 边缘解码 (Edge Computing)：** 深度移植并重写 `dump1090` 核心算法，在双核 400MHz 算力下实现无损解析。
* 🔋 **超低功耗航电 (Low Power Avionics)：** 专为航空座舱便携场景设计，功耗仅为传统树莓派方案的十分之一。
* 🔌 **高扩展性硬件：** 预留 I2C/SPI 接口，完美支持航天级 BNO085 IMU 与 虚拟姿态仪 (PFD) 屏幕显示。

## 🏗️ 目录结构 (Repository Structure)
本项目采用软硬件一体 (Monorepo) 管理机制，为创客与飞行员提供完整的解决方案：
- `/firmware` : 基于 ESP-IDF v6.0 的纯 RTOS 固件源码 (C 语言)。
- `/hardware` : 硬件扩展底板图纸 (PCB) 与 3D 打印外壳模型 (STL)。
- `/docs` : 开发者文档、API 说明与组装指南。

## 🚀 快速开始 (Quick Start)

| 我想… | 看这份文档 |
|------|----------|
| **从零开始把固件跑起来**（clone、装环境、编译、烧录） | 📖 [`docs/BUILD.md`](docs/BUILD.md) |
| 看系统跑起来后的任务 / 数据流架构 | 🏗️ [`docs/architecture.md`](docs/architecture.md) |
| 调整 sdkconfig（开关 BLE、改频率、改分区等） | ⚙️ [`docs/configuration.md`](docs/configuration.md) |
| 给 Pilot Kit 移动 App 做 BLE 集成 | 📱 [`docs/ble_protocol.md`](docs/ble_protocol.md) |
| 改硬件接线 / 看 GPIO 分配 | 🔌 [`docs/hardware/board_pinout.md`](docs/hardware/board_pinout.md) |
| 启用蓝牙（烧 C6 协处理器固件） | 🔵 [`docs/hardware/c6_slave_firmware.md`](docs/hardware/c6_slave_firmware.md) |

最短上手路径 — macOS / Linux：

```bash
# 1. 装 ESP-IDF v6.0.1（一次性，全局）
curl -L https://dl.espressif.com/dl/eim/eim-installer.sh | bash
# 选 v6.0.1 + target=all，等约 5-10 分钟

# 2. 拉代码（含 submodule）
git clone --recursive https://github.com/naizhao/Pilot-Kit-Box-ESP32-P4.git
cd Pilot-Kit-Box-ESP32-P4

# 3. 【新板必做】烧 ESP32-C6 协处理器的 hosted slave 固件（一次性，~30 分钟）
#    需要 USB-UART 转接器 + 4 根杜邦线接到板子背面 H4 头
#    详见 docs/BUILD.md §3 (或临时不要 BLE 可跳过: menuconfig 关 PK_BLE_ENABLED)

# 4. 编译 + 烧录 P4 主固件（约 10 分钟首次构建，之后 30 秒一轮）
cd firmware
source ~/.espressif/tools/activate_idf_v6.0.1.sh
idf.py set-target esp32p4
idf.py -p /dev/cu.usbmodem* flash monitor
```

遇到问题查 [`docs/BUILD.md` 第 9 节常见问题](docs/BUILD.md#9-常见问题--troubleshooting)。

## ⚙️ 硬件选型参考 (Hardware Setup)
- **核心算力主板：** 微雪 (Waveshare) ESP32-P4C6 (P4负责高速USB与DSP，C6负责无线射频)。
- **SDR 接收模块：** RTL-SDR 电视棒 (推荐基于 Fitipower FC0013 调谐芯片的型号，极低功耗与高抗扰性)。
- **扩展与交互 (规划中)：** CH334R 高速 USB 2.0 Hub 控制器、BNO085 航天级 IMU、1.28寸 GC9A01 SPI 圆形彩色液晶屏。

## 📚 驱动致谢 (Credits)
本项目的底层 USB 与 SDR 驱动，致敬并参考了开源社区极客们的卓越贡献：
* `kvhnuke/esp32-rtl-sdr` (提供标准的 librtlsdr API 封装)
* `XTR1984/xtrsdr` (提供 ESP32 平台 USB DMA 分包调优方案)

## ✈️ 关于我们 (About Us)
了解更多关于航空电子创新、智能飞行工具以及飞行员社区的最新动态，请访问我们的官方网站：[air.club](https://air.club)。

## 📄 开源协议 (License)
MIT License