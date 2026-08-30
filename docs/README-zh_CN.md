# Pilot Kit Box 文档索引

本目录采用统一语言约定：

- 默认 `*.md` 文档为英文。
- 简体中文译文使用 `-zh_CN.md` 后缀。

项目根目录 README 仍保留中英双语，因为它是公开入口页。`docs/` 下的详细文档按语言拆分，方便读者和工具稳定链接。这个约定适用于公开文档、内部说明和放在 `docs/` 下的计划文件。

## 快速入口

| 主题 | 英文 | 简体中文 | 适用读者 |
|---|---|---|---|
| 构建与烧录 | [`BUILD.md`](BUILD.md) | [`BUILD-zh_CN.md`](BUILD-zh_CN.md) | 首次构建固件的开发者 |
| 配置参考 | [`configuration.md`](configuration.md) | [`configuration-zh_CN.md`](configuration-zh_CN.md) | 固件定制 |
| 固件架构 | [`architecture.md`](architecture.md) | [`architecture-zh_CN.md`](architecture-zh_CN.md) | 固件开发者 |
| BLE 协议 | [`ble_protocol.md`](ble_protocol.md) | [`ble_protocol-zh_CN.md`](ble_protocol-zh_CN.md) | 移动端 / EFB 客户端开发者 |
| 数据库维护 | [`database_maintenance.md`](database_maintenance.md) | [`database_maintenance-zh_CN.md`](database_maintenance-zh_CN.md) | 固件维护者 |
| 用户指南 | [`user_guide.md`](user_guide.md) | [`user_guide-zh_CN.md`](user_guide-zh_CN.md) | 设备使用者 |
| 固件发布与网页刷写 | [`firmware_update.md`](firmware_update.md) | [`firmware_update-zh_CN.md`](firmware_update-zh_CN.md) | 维护者和终端用户 |
| 硬件资料索引 | [`hardware/README.md`](hardware/README.md) | [`hardware/README-zh_CN.md`](hardware/README-zh_CN.md) | 硬件搭建者 |
| 板卡引脚 | [`hardware/board_pinout.md`](hardware/board_pinout.md) | [`hardware/board_pinout-zh_CN.md`](hardware/board_pinout-zh_CN.md) | 硬件开发者 |
| ESP32-C6 slave 烧录 | [`hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md) | [`hardware/c6_slave_firmware-zh_CN.md`](hardware/c6_slave_firmware-zh_CN.md) | BLE bring-up |
| 4.3 寸触摸 UX 规格 | [`ux/box-4.3-ux-spec.md`](ux/box-4.3-ux-spec.md) | [`ux/box-4.3-ux-spec-zh_CN.md`](ux/box-4.3-ux-spec-zh_CN.md) | UI / 固件界面实现者 |
| 扩展板 V4（当前硬件目标） | [`../hardware/expansion-board-v4/README.md`](../hardware/expansion-board-v4/README.md) | 同一文件（双语） | 装板 / 复刻者 |

## 当前功能基线

所有公开文档应与以下事实保持一致：

- 安全边界：Pilot Kit Box 不是经过适航认证的航电设备，文档不得把它写成主飞行仪表、备用仪表、导航源或防撞系统。
- 目标开发板是 **Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3**。P4 负责 USB、DSP、UI 和存储；C6 负责 BLE。
- ADS-B 接收按板代分两种形态：**v1/v2 载板与裸板**把 RTL-SDR dongle 接到原生 USB 2.0 HS 差分对（装载板时插载板 USB-A，走 J3-27/25；裸 Waveshare 板则用 H2 Type-C 加 OTG 转接头或 Hub；两者同网，只能二选一），而 **v3/v4 扩展板**自带 1090 MHz 接收链、由 RP2040 解码。dongle 路径默认 1090 MHz、2 MSPS。
- 当前推荐的 SDR dongle tuner 是 **FC0013**，主要原因是 BOM 成本低。
- 识别数据库：ICAO24 飞机数据库随 microSD 卡分发，路径 `/sdcard/aero/pk_actdb.bin`（仓库内产物 `datafiles/data/pk_actdb.bin`，读取端 `firmware/main/aircraft_db.c`），更新是拷文件、不是刷固件；`firmware/main/airline_codes.c` 的航司代码表和 `firmware/main/icao_country.c` 的 ICAO24 国家地址段表仍编进固件。
- LCD 是 **4.3 寸 ST7701 480×800 MIPI-DSI 屏**，通过 PPA 作为 800×480 横屏使用；背光 GPIO26、复位 GPIO27、BL_EN GPIO33。
- GT911 触摸与其他设备共用 GPIO7/8 的 I²C0，复位 GPIO23；旧四实体键任务（`button_task.c`）已移出编译。
- IMU 是 **BNO085 / GY-BN008X**，I2C0 使用 GPIO7 / GPIO8，RST GPIO28，地址 `0x4A`；INT 接 GPIO34（J3 pin 28）但驱动轮询。GPIO20 是板载 BAT_ADC。
- 4.3 寸触摸 FAB 打开**全屏导航网格**直接切页：第 1 页 **PFD、交通、地图、
  列表、搜索、记录、工具**，第 2 页 **诊断、设置、关于**；网格底部动作条承载
  长按「调平」。旧的横向 dock 已废弃。
- GT-U8 GPS/北斗、BMP388 气压高度/升降率、交通雷达和 DIAG 实时诊断均已接入运行时；GPIO50 PPS 已接线但固件不读取。
- Settings 可调整语言、QNH、地图朝向、雷达量程和日志后端；MicroSD 支持插拔探测、容量状态和受保护格式化。
- Settings、About、Diagnostics、Compass Calibration 页面已有中英文固件 UI 字符串，配置保存到 NVS。

## 翻译维护规则

新增或更新 `docs/` 下的文档时：

1. 先写或更新英文默认文件。
2. 再新增或更新同路径、同文件名加 `-zh_CN.md` 后缀的简体中文版本。
3. 除非存在语言特定说明，否则两个版本的章节结构和命令示例保持对齐。
4. 面向用户的文档新增后，同步更新本文档索引和根目录 README。
5. 描述座舱相关行为时，必须明确安全和适航认证边界。
