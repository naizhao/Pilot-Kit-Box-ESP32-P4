# 构建与烧录指南 / Build & Flash Guide

本文档面向**第一次接触 ESP32-P4 / Pilot Kit Box 项目**的开发者，从零开始一步步带你完成：

1. 安装编译环境 (ESP-IDF v6.0)
2. 拉取源码 (含 git submodule)
3. 配置 + 编译固件
4. 通过 USB 烧录到 Waveshare ESP32-P4-WIFI6 开发板
5. 第一次（可选）烧录板载 ESP32-C6 协处理器的 esp_hosted slave 固件，启用蓝牙

如果你是经验丰富的 ESP-IDF 用户，可以直接跳到第 5 节看具体 `idf.py` 命令；如果你是第一次接触这个项目，请按顺序读完。

---

## 0. 先决条件 / Prerequisites

### 硬件 / Hardware

| 必备 | 描述 |
|------|------|
| **Waveshare ESP32-P4-WIFI6** 开发板 | 32 MB Nor Flash + 32 MB PSRAM，板载 ESP32-C6（Wi-Fi 6 + BLE 5）。其他 P4 板（如 Espressif Function EV Board）也可以用，但引脚映射在 [`docs/hardware/board_pinout.md`](hardware/board_pinout.md) 里以 Waveshare 为准。 |
| **USB-C 数据线** | 用于供电、烧录、串口监视。注意是数据线，不是只能充电的线。 |
| **macOS / Linux / Windows 主机** | 任何能装 ESP-IDF 的开发机。本文以 **macOS** 为主要示例，Linux 步骤几乎一样，Windows 下用 PowerShell + 安装包路径会略有差异，在每一步会标注。 |

### 选配（启用对应功能时需要）

| 选配硬件 | 用途 | 启用对应 Phase |
|----------|------|-------------|
| RTL-SDR USB Dongle (R820T2 / RTL2832U) | 1090 MHz ADS-B 接收 | Phase 1–3 数据管线 |
| USB Type-A 公座 → MX1.25 4-pin 转接线 | 把 P4 板上的 4-pin USB OTG 接口转成标准 A 母座插 RTL-SDR | Phase 1+ |
| TK024F3036 / ST7789 240×320 SPI 屏 + 驱动板 | PFD 显示 | Phase 4 |
| BNO085 IMU 模块 | 姿态融合 | Phase 4 |
| USB-UART 转接器 (CP2102 / FTDI / CH340 任一即可) | 烧录 C6 hosted slave 固件 | BLE (Phase 3b) |

### 软件 / Software prerequisites

- **Git** 2.20+
- **Python 3.10+**（macOS 用 Homebrew `brew install python` 或 IDF Manager 自带）
- **CMake 3.16+**（IDF Manager 自带）
- **Ninja**（IDF Manager 自带）

---

## 1. 安装 ESP-IDF v6.0

> ⚠️ **必须用 ESP-IDF v6.0.x**。v5.x 不支持本项目用到的若干 component-manager 包；v7+（如果未来发布）我们尚未验证。

推荐用 **Espressif Installation Manager (eim)**，对新人最友好：

### macOS / Linux — Espressif Installation Manager 方式

1. **下载并运行 eim**（官方安装器）：
   ```bash
   # macOS / Linux 都可
   curl -L https://dl.espressif.com/dl/eim/eim-installer.sh | bash
   ```
   或者去 https://dl.espressif.com/dl/eim/ 下载图形化版本。

2. 安装时选择：
   - **IDF version**: `v6.0.1`
   - **Target**: `all`（或至少勾上 `esp32p4` 和 `esp32c6`）
   - **Install path**: 接受默认 `~/.espressif`

3. 安装会下载约 1.5 GB（toolchain + Python venv + 工具）。等它跑完，最后会生成一个 activation 脚本：
   ```
   ~/.espressif/tools/activate_idf_v6.0.1.sh
   ```

4. **激活环境**（每次开新终端都要做一次）：
   ```bash
   source ~/.espressif/tools/activate_idf_v6.0.1.sh
   ```
   激活后 `idf.py --version` 应输出 `ESP-IDF v6.0.1`。如果嫌每次都 source 麻烦，把这一行加到 `~/.zshrc` / `~/.bashrc`。

### Windows — Espressif IDE installer

1. 去 https://dl.espressif.com/dl/esp-idf/ 下载 ESP-IDF Windows Installer。
2. 安装时选 v6.0.1。
3. 桌面会生成 "ESP-IDF v6.0.1 PowerShell" 快捷方式。**所有 `idf.py` 命令都在这个 PowerShell 里跑**，普通 PowerShell 不行。

### 验证安装

激活环境后，跑：

```bash
idf.py --version
```

应该看到：

```
ESP-IDF v6.0.1
```

如果看到 `idf.py: command not found`，说明环境没激活，回到上面 source 那一步。

---

## 2. 拉取源码

```bash
# 选一个你喜欢的目录
cd ~/repos

# clone 主仓库 + 拉子模块（重要：esp32-rtl-sdr 是 submodule！）
git clone --recursive https://github.com/airclub/Pilot-Kit-Box-ESP32-P4.git
```

如果你已经 clone 过但忘了 `--recursive`，补一句：

```bash
cd Pilot-Kit-Box-ESP32-P4
git submodule update --init --recursive
```

### 子模块说明

`firmware/components/esp32-rtl-sdr/` 是一个独立的 git submodule，指向 [`naizhao/esp32-rtl-sdr`](https://github.com/naizhao/esp32-rtl-sdr) 的 `feat/p4-async-iq-stream` 分支。它包含我们 patch 过的 librtlsdr 异步 IO 实现。`xtrsdr` 是参考库不入仓库（在 `.gitignore` 里）。

### 目录结构速览

```
Pilot-Kit-Box-ESP32-P4/
├── docs/
│   ├── BUILD.md                        ← 你正在读的
│   ├── architecture.md                 ← 任务/数据流/内存图
│   ├── ble_protocol.md                 ← BLE GATT 服务规范（给 Pilot Kit App 团队）
│   ├── configuration.md                ← sdkconfig 配置参考
│   └── hardware/
│       ├── README.md                   ← 硬件资料索引
│       ├── board_pinout.md             ← Waveshare 板 GPIO 全表
│       ├── c6_slave_firmware.md        ← C6 hosted slave 烧录指南
│       └── ESP32-P4-WIFI6-datasheet.pdf
├── firmware/                           ← ESP-IDF 工程根
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults              ← 默认配置（项目级）
│   ├── partitions.csv                  ← 自定义分区表
│   ├── main/                           ← 应用层源码
│   └── components/
│       ├── esp32-rtl-sdr/              ← submodule
│       └── xtrsdr/                     ← gitignored
└── README.md
```

---

## 3. 配置（多数情况下用默认即可）

### 设置目标芯片

第一次构建前必须告诉 ESP-IDF 目标芯片：

```bash
cd Pilot-Kit-Box-ESP32-P4/firmware
idf.py set-target esp32p4
```

这一步会生成 `sdkconfig` 文件（基于 `sdkconfig.defaults`）。**如果之前已经存在 `sdkconfig`，建议删掉重新生成**：

```bash
rm -f sdkconfig
idf.py set-target esp32p4
```

### 关键配置项（已在 sdkconfig.defaults 写好，无需手动改）

如果你只是要把固件跑起来，**这一节可以跳过**。完整的配置参考见 [`docs/configuration.md`](configuration.md)。

- `CONFIG_IDF_TARGET="esp32p4"` — 目标芯片
- `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y` — Waveshare 板有 32 MB Nor flash
- `CONFIG_PARTITION_TABLE_CUSTOM=y` + `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` — 用我们的自定义分区表（含 16 MiB LittleFS 区）
- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y` — **针对 v1.x silicon 的 Waveshare 板**。IDF v6.0 默认编 v3.1+ 目标，而当前 Waveshare 出货的 P4 多是 v1.3 silicon，不打开这两个开关会出现：
  ```
  A fatal error occurred: 'bootloader/bootloader.bin' requires chip revision
  in range [v3.1 - v3.99] (this chip is revision v1.3).
  ```
- `CONFIG_BT_ENABLED=y` + `CONFIG_BT_CONTROLLER_DISABLED=y` + ESP_HOSTED SDIO 引脚 — BLE 走 C6 协处理器
- `CONFIG_USB_HOST_HUBS_SUPPORTED=y` — RTL-SDR 通过 USB hub 也能识别

### 想改配置？

```bash
idf.py menuconfig
```

会弹出终端 UI，可以浏览所有 Kconfig 选项。常见调整见 [`docs/configuration.md`](configuration.md)。

---

## 4. 编译

```bash
cd firmware
idf.py build
```

**第一次编译大约 5–15 分钟**（取决于网速 + CPU）：

- IDF Component Manager 会从 https://components.espressif.com 下载约 50 MB 依赖包
  （`espressif/usb`, `espressif/esp_hosted`, `joltwallet/littlefs` 等）
- 编译大约 1,200 个 .c 文件成 .o 文件再链接

**之后增量编译只需 5–30 秒**（取决于改了哪个文件）。

编译成功后会看到：

```
Project build complete. To flash, run:
 idf.py flash
or
 idf.py -p PORT flash
or
 ...

pilot_kit_box.bin binary size 0xf0xxx bytes. Smallest app partition is 0x400000 bytes. 0x3xxxxx bytes (77%) free.
```

产物位于 `firmware/build/`：
- `bootloader/bootloader.bin` — 二级 bootloader (~23 KiB)
- `partition_table/partition-table.bin` — 分区表 (~3 KiB)
- `pilot_kit_box.bin` — 主固件 (~960 KiB)

---

## 5. 连接板子 + 找到串口

### 接线

把 USB-C 数据线一端接 P4 板上**靠近 BOOT 按键那个 Type-C 口**（不是背面那个 SD 卡槽旁边的小 4-pin），另一端接电脑。

> ⚠️ Type-C 口走的是 CH343P USB-UART 桥，不是 P4 的原生 USB-OTG。原生 USB-OTG 在背面的 4-pin MX1.25 接口（之后接 RTL-SDR 用）。

### 找串口

#### macOS

```bash
ls -la /dev/cu.usbmodem*
```

应该能看到类似：
```
/dev/cu.usbmodem5B7B0255751
```

后面那一串数字是设备的序列号，每块板子不一样。**记住完整路径**，下面要用。

如果什么都没看到：
- 检查 USB 线是不是数据线（不是充电线）
- 换一个 USB 端口（最好是机器直插，别经过 hub）
- macOS 弹窗问 "Allow USB device" 时点允许

#### Linux

```bash
ls -la /dev/ttyACM* /dev/ttyUSB*
```

通常是 `/dev/ttyACM0` 或 `/dev/ttyUSB0`。可能需要把当前用户加进 `dialout` 组：

```bash
sudo usermod -aG dialout $USER
# 注销后重新登录才生效
```

#### Windows

打开「设备管理器」→「端口 (COM 和 LPT)」，看到 "USB Serial Device (COMx)" 或 "CH343 (COMx)"。记下 `COMx`。

---

## 6. 烧录 + 串口监视

### 一键搞定

```bash
cd Pilot-Kit-Box-ESP32-P4/firmware
source ~/.espressif/tools/activate_idf_v6.0.1.sh   # 每个新终端都要

# macOS / Linux：
idf.py -p /dev/cu.usbmodem5B7B0255751 flash monitor

# Windows：
idf.py -p COM3 flash monitor
```

这条命令做了 4 件事：
1. 把 bootloader、partition table、应用三个 bin 烧到指定串口
2. esptool 自动通过 DTR/RTS 让 P4 进入 download 模式
3. 烧完后自动复位
4. 启动串口监视，显示 boot log + 运行时日志

**首次烧录约 1-2 分钟**（写入 ~1 MB 数据 + 校验）。之后增量烧录只烧应用分区，~30 秒。

### 退出串口监视

按 `Ctrl-]`（macOS 是 `Control + ]`）。

### 如果 `Failed to connect` / 自动复位失败

某些 USB-C 数据线 / 转接器不支持 DTR/RTS 控制。手动进入 download 模式：

1. **按住** BOOT 按钮（板子上有标注，靠近 Type-C 口）
2. **短按一下** RESET 按钮
3. **松开** BOOT
4. 重新跑 `idf.py flash`

如果还是不行，看下面的「常见问题」。

### 只烧录不监视

```bash
idf.py -p <PORT> flash
```

### 只监视不烧录

```bash
idf.py -p <PORT> monitor
```

### 擦除 flash（恢复出厂）

```bash
idf.py -p <PORT> erase-flash
```

⚠️ 这会**抹掉 LittleFS 上存的所有 ADS-B 历史数据**。

---

## 7. 看到什么算成功

烧完后，串口应该有类似这样的开机日志（**没接任何外设的空板**情况）：

```
I (315) pilot_kit:   Pilot Kit Box (ESP32-P4) — Phase 1 boot
I (315) pilot_kit:   Free internal heap at boot: 5xx KiB
I (325) pilot_kit:   IQ ring buffer ready: 131072 B (BYTEBUF)
I (335) pilot_kit:   USB host stack installed
I (335) pilot_kit:   USB host stack online — spawning SDR + DSP tasks
I (345) rec_file:    LittleFS mounted at /storage: 0/16384 KiB used   ← 第一次启动会自动格式化 LittleFS
I (xxx) record_sink: registered sink 'uart' (1 total)
I (xxx) record_sink: registered sink 'ble_raw' (2 total)
I (xxx) record_sink: registered sink 'file_littlefs' (3 total)
I (xxx) pilot_kit:   ADS-B sinks ready (UART + file at /storage)
I (xxx) sdr:         USB client registered, waiting for RTL-SDR enumeration
I (xxx) display:     ST7789 240x320 ready, framebuffer @ 0x48xxxxxx (150 KiB PSRAM)
I (xxx) display:     test pattern flushed; backlight @ 70%
W (xxx) pilot_kit:   IMU init failed (ESP_ERR_TIMEOUT) — PFD will run without attitude
I (xxx) pfd:         pfd_task running (Phase 4c v2: ladder + arc + tape + text)
W (xxx) pilot_kit:   BLE init failed (...) — UART + file sinks only (flash C6 first)
I (xxx) pfd:         PFD 30 FPS  | roll=+0.00 pitch=+0.00 yaw=0.00  imu_valid=0 aircraft=0
I (xxx) dsp:         stream 0.00 MB/s | msgs/s 0 (...)  | aircraft 0     ← 1Hz 心跳
```

**关键确认点**：

| 看到这条 | 说明 |
|---------|------|
| `Pilot Kit Box (ESP32-P4) — Phase 1 boot` | 主应用启动正常 |
| `LittleFS mounted` | 存储分区 OK |
| `USB host stack online` | USB host 就绪（等 RTL-SDR） |
| `PFD 30 FPS` 周期出现 | 显示渲染管线 OK |
| `dsp: stream 0.00 MB/s` 1Hz 重复 | DSP task 运行中 |
| `IMU init failed` | 预期（没接 BNO085） |
| `BLE init failed` | 预期（C6 还没刷 hosted slave） |

### 接外设的预期

| 操作 | 预期 |
|------|------|
| 通过 4-pin USB 转接器接 RTL-SDR dongle | `sdr: USB NEW_DEV at addr 1` → `Tuned to 1090000000 Hz` → `Sampling at 2000000 S/s` → `rtlsdr_async: starting async stream` → `dsp: stream 2.00 MB/s` |
| 接 ST7789 SPI 屏到正确引脚 | 屏幕亮起 R→G→B 渐变 (~1 秒) → 显示 PFD 仪表 (蓝色天 + 棕色地 + 黄十字 + 黑色数字面板) |
| 接 BNO085 模块到 I²C0 + INT/RST | 串口出现 `imu: rpy = +1.23 / -0.45 / 187.66 (acc=3 valid=98)` 周期更新，晃动板子时 PFD 的 horizon 跟着翻 |
| C6 刷完 hosted slave 固件（见第 8 节） | 串口 `ble_gatt: advertising as "PilotKitBox"`，手机用 BLE 扫描器能看到 |

---

## 8. （可选）启用 BLE — 烧 C6 esp_hosted slave 固件

> 这一步只有在你**实际要用蓝牙**的时候才需要做。不做也不影响 RTL-SDR / LCD / 串口 / 存储 这些功能。

详细步骤见 [`docs/hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md)。简单流程：

1. 准备一个外接 USB-UART 转接器（CP2102 / FTDI / CH340 任一）+ 4 根杜邦线
2. 接到板子背面的 **H4** 4-pin 头：

   | H4 pin | 接 USB-UART | 备注 |
   |--------|------------|------|
   | 1 (C6_IO9) | 短接到 GND（用一根线短一下，烧录前持续短接） | C6 进 download 模式的 strap |
   | 2 (GND) | GND | |
   | 3 (C6_RXD) | TX | 转接器的 TX |
   | 4 (C6_TXD) | RX | 转接器的 RX |

3. clone esp-hosted slave 工程：
   ```bash
   git clone --recursive https://github.com/espressif/esp-hosted.git
   cd esp-hosted/slave_drv
   idf.py set-target esp32c6
   idf.py menuconfig
   # → Component config → ESP-Hosted config →
   #     Host Interface = SDIO Slave
   #     Bluetooth Support = NimBLE
   #     SDIO Slave Pin Configuration:
   #       CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17
   idf.py build
   ```

4. 烧 C6（保持 H4 pin 1 短接到 GND）：
   ```bash
   idf.py -p /dev/cu.usbserial-XXXX flash
   ```

5. 松开 H4 pin 1，按 P4 板上的 RESET 按钮重启全板。

6. 重新跑主固件的 `idf.py -p <P4 串口> monitor`，应看到：
   ```
   I (xxx) ble_gatt: NimBLE host task running
   I (xxx) ble_gatt: BLE address aa:bb:cc:dd:ee:ff type=1
   I (xxx) ble_gatt: advertising as "PilotKitBox"
   ```

7. 在手机上用任何 BLE 扫描器（nRF Connect / LightBlue）能看到 `PilotKitBox` 设备。Pilot Kit 移动 App 已知道我们的 GATT UUID，可以直接连接。

---

## 9. 常见问题 / Troubleshooting

### `A fatal error occurred: 'bootloader/bootloader.bin' requires chip revision in range [v3.1 - v3.99]`

**原因**：IDF v6.0 默认编 P4 ECO5 (v3.1+) 目标，但你板上的 P4 是 v1.x silicon。

**修复**：检查 `firmware/sdkconfig.defaults` 里是否有：
```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```
没有的话加上，然后：
```bash
cd firmware
rm sdkconfig
idf.py build
```
重新烧。

### `Failed to connect to ESP32-P4: Wrong boot mode detected`

esptool 自动复位失败。手动按 BOOT + RESET（见第 6 节末尾）。

### `nimble_port_init` 这一步串口卡住不动

C6 没有 hosted slave 固件。本仓库我们已经把 `ble_gatt_init()` 放在 `main()` 最后一步，所以**前面 LCD / PFD / 存储都会正常起来**。如果你 100% 不想用 BLE，可以临时关掉：

```bash
idf.py menuconfig
# → Component config → Bluetooth → 取消 [*] Bluetooth
```

或参考第 8 节烧 C6 slave 固件。

### `LittleFS mount failed`

99% 是分区表没烧。重跑：
```bash
idf.py -p <PORT> erase-flash
idf.py -p <PORT> flash
```

### `USB device descriptor returned errors` / RTL-SDR 上电没识别

USB 供电不足。RTL-SDR 棒功耗 ~300 mA，电脑 USB 口供电有时不够带 P4 板 + dongle。试一根 5V/2A 的 USB-C 适配器从板子的 USB-C 口供电，电脑端接 4-pin 上的 OTG 数据。

### 编译时 `idf.py: command not found`

环境没激活。每次开新终端跑：
```bash
source ~/.espressif/tools/activate_idf_v6.0.1.sh
```

### 编译时 `Component "espressif/esp_hosted" not found`

Component manager 没下载。手动触发：
```bash
cd firmware
idf.py reconfigure
```
如果还是不行：
```bash
rm -rf managed_components
idf.py reconfigure
```

### 想要清空中间产物重新编

```bash
idf.py fullclean
```

会清掉 `build/` 和 `managed_components/`。下次 build 又要重新下载依赖。如果只想清掉 build 缓存保留依赖：
```bash
idf.py clean
```

### 编译太慢

启用 ccache 大幅提速：
```bash
brew install ccache    # macOS
idf.py --ccache build
```

把 `--ccache` 加到环境变量：
```bash
echo 'export IDF_CCACHE_ENABLE=1' >> ~/.zshrc
```

---

## 10. 日常开发循环

修改代码 → 立即查看效果的最快流程：

```bash
# 终端 1，常驻 ESP-IDF 环境
cd firmware
source ~/.espressif/tools/activate_idf_v6.0.1.sh

# 改完代码后
idf.py -p /dev/cu.usbmodem5B7B0255751 flash monitor
```

`flash monitor` 是连写命令，编译 + 烧录 + 启监视一气呵成。增量编译 + 烧 ~30 秒一轮。

退出 monitor 用 `Ctrl-]`，重新进只 monitor 不烧：
```bash
idf.py -p <PORT> monitor
```

---

## 11. 参与贡献

- 改了 `firmware/` 下任何 `.c` / `.h` / `CMakeLists.txt` / `sdkconfig.defaults`：直接发 PR
- 改了 `firmware/components/esp32-rtl-sdr/`：这是 submodule，请直接对 [naizhao/esp32-rtl-sdr](https://github.com/naizhao/esp32-rtl-sdr) 的 `feat/p4-async-iq-stream` 分支发 PR，然后在本仓库 bump submodule SHA
- 加了新 doc：放 `docs/` 下，并在本文档 + `README.md` 加链接
- 加了新硬件支持：更新 `docs/hardware/board_pinout.md` + 创建对应章节

详细的代码规范见 [`docs/architecture.md`](architecture.md)。

---

## 12. 下一步

- 想了解整个系统跑起来后的数据/任务架构 → [`docs/architecture.md`](architecture.md)
- 想知道有哪些 sdkconfig 选项可调 → [`docs/configuration.md`](configuration.md)
- 想做 Pilot Kit 手机 App 的 BLE 集成 → [`docs/ble_protocol.md`](ble_protocol.md)
- 想自己改硬件接线 → [`docs/hardware/board_pinout.md`](hardware/board_pinout.md)
- 想烧 C6 启用 BLE → [`docs/hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md)
