# 构建与烧录指南

英文版：[`BUILD.md`](BUILD.md)

本文档面向**第一次接触 ESP32-P4 / Pilot Kit Box 项目**的开发者，从零开始一步步带你完成：

1. 安装编译环境 (ESP-IDF v6.0.1)
2. 拉取源码 (含 git submodule)
3. 配置 + 编译固件
4. 通过 H1 `USB TO UART` 烧录到 Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
5. 第一次（可选）烧录板载 ESP32-C6 协处理器的 esp_hosted slave 固件，启用蓝牙

如果你是经验丰富的 ESP-IDF 用户，可以直接跳到第 5 节看具体 `idf.py` 命令；如果你是第一次接触这个项目，请按顺序读完。

---

## 0. 先决条件 / Prerequisites

### 硬件 / Hardware

| 必备 | 描述 |
|------|------|
| **Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3** | P4NRW32 + C6 一体板，板载 4.3 寸 ST7701 DSI 屏、GT911 触摸、32 MB Flash 与 32 MB PSRAM。 |
| **USB-C 数据线** | 用于供电、烧录、串口监视。注意是数据线，不是只能充电的线。 |
| **macOS / Linux / Windows 主机** | 任何能装 ESP-IDF 的开发机。本文以 **macOS** 为主要示例，Linux 步骤几乎一样，Windows 下用 PowerShell + 安装包路径会略有差异，在每一步会标注。 |

### 选配（启用对应功能时需要）

| 选配硬件 | 用途 | 对应功能 |
|----------|------|-------------|
| RTL-SDR FC0013 USB dongle | 1090 MHz ADS-B 接收；当前推荐 FC0013，主要因为成本低 | ADS-B 数据链路 |
| USB-C OTG 转接头或有源 USB Hub | 仅裸板需要：把 H2 原生 USB HS Type-C 转成 USB-A 母座接 RTL-SDR。Pilot Kit 载板自带 USB-A 插头（走 J3-27/25），不需要转接头 | ADS-B USB 数据链路 |
| BNO085 IMU 模块 | 姿态融合 | PFD 姿态显示 |
| USB-UART 转接器 (CP2102 / FTDI / CH340 任一即可) | 烧录 C6 hosted slave 固件 | BLE bring-up |

### 软件 / Software prerequisites

- **Git** 2.20+
- **Python 3.10+**（使用 [ServBay](https://servbay.com) 安装或 IDF Manager 自带）
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
git clone --recursive https://github.com/naizhao/Pilot-Kit-Box-ESP32-P4.git
cd Pilot-Kit-Box-ESP32-P4
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

## 3. 新板首次设置：烧 ESP32-C6 hosted slave 固件（**一次性**）

> ✅ **BLE 已经全链路跑通**（`CONFIG_PK_BLE_ENABLED=y` 是默认值）。每块全新的 Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 第一次用都要做这一步——出厂的 C6 上面跑的是工厂 AT 命令固件，跟我们 P4 上的 ESP-Hosted / NimBLE host 协议对不上。**这一步耗时 ~30 分钟、每块板只做一次**，做完之后 C6 的 hosted slave 固件**永久驻留**（除非你重新烧 AT 固件覆盖），所有后续 P4 固件迭代都不再碰 C6。bring-up 过程的完整诊断记录见 [`docs/hardware/c6_bringup_status.md`](hardware/c6_bringup_status.md)。
>
> **能不能跳过？** 可以，但 BLE 就不能用。如果你**确定不要 BLE**（比如只做 ADS-B 数据路径开发、没买 USB-UART 转接器、跑 CI），跳到 Section 4 之前先做：
>
> ```bash
> cd firmware
> idf.py menuconfig
> # → Pilot Kit Box → 取消 [*] Initialise BLE GATT server at boot
> ```
>
> 然后再做 Section 4 编译。**不关 BLE 又跳过这一节**的话 P4 固件第一次启动就会 `abort()`（vhci_drv 找不到 C6 时强制 abort，没有 graceful degrade）。

### 所需器材

| 必备 | 描述 |
|------|------|
| **USB-UART 转接器** | CP2102 / FTDI FT232 / CH340 / CH343 都行，输出 3V3 TTL |
| **3 根杜邦线（母对公）** | 转接器 ↔ P1 上的 TX / RX / GND |
| **1 段回形针 / 短线** | 板内短接 IO9 到任意 GND（让 C6 进 download mode）|

### 接线（板子背面的 4-pin P1 下载排针）

| P1 pin | 板上标注 | 接到 |
|--------|---------|------|
| 1 | C6_TXD | 转接器的 RX |
| 2 | C6_RXD | 转接器的 TX |
| 3 | C6_IO9 | **板内** 短接到任意 P4 GND 针脚（C6 download mode strap，烧完再拔） |
| 4 | GND | 转接器的 GND |

P1-3 (IO9) **不接**转接器信号 —— 它只需要在烧录期间被板内短到 GND
就行，跟 USB-UART 之间没有信号关系。

### 准备 C6 slave 固件二进制

两条路，选一条：

**(A) 最快路径 ——用 esphome 预编译好的二进制**（推荐，省 ~10 分钟，跟我们自己编译的功能完全等价）：

```bash
curl -L -o firmware/network_adapter_esp32c6.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.12.7/network_adapter_esp32c6.bin
# sha256: ee7c546eb726ba92aa583448969c05f378a0deae5b3556699122761b7a595f51
```

**(B) 自己编译 slave**（如果你想改 slave config）：

```bash
# 复制本仓库 vendored 的 slave 工程到外面（slave 的 partitions.esp32c6.csv
# 会跟 P4 工程的 partitions.csv 冲突，必须在仓库外构建）
cp -R firmware/managed_components/espressif__esp_hosted/slave ~/hosts/c6_slave
cd ~/hosts/c6_slave

source ~/.espressif/tools/activate_idf_v6.0.1.sh
export PATH="$IDF_PATH/tools:$PATH"   # eim 的 activate 不会加这条
idf.py set-target esp32c6
# sdkconfig.defaults.esp32c6 已经包含正确的 SDIO + BLE controller 配置，
# 默认开 CONFIG_ESP_SDIO_HOST_INTERFACE=y + CONFIG_BT_LE_HCI_INTERFACE_USE_RAM=y，
# 不需要再 menuconfig。
idf.py build
# 产物：build/network_adapter.bin (~1.2 MB)
```

### 烧录步骤（**顺序很重要**）

**关键点**：P1 只有 UART 与 IO9，没有引出 C6 的 RESET。要进 download
mode 只能“启动时 IO9 拉低”——这要求 C6 在 IO9 已经短接到 GND 的状态下
**冷启动**。同时还得防止 P4 在烧 C6 期间通过 GPIO54 干扰 C6，所以也要
让 P4 进 download mode 站着不动。Rev1.2 上 GPIO54 经 R34 0 Ω 直连 C6
EN，没有反相器或电平转换器。

按这个顺序：

1. **断开所有电源**：拔掉 H1 `USB TO UART`，板子完全不通电。

2. **接 3 根杜邦线**（按 [§3 接线表](#接线板子背面的-4-pin-p1-下载排针)）：UART 转接器 RX→P1-1、TX→P1-2、GND→P1-4。

3. **板内短接 IO9 → GND**：回形针 / 短跳线，P1-3 短到板上任意 P4
   GND 针脚。IO9 不接 UART 控制信号。

4. **先把 UART 转接器插上电脑**。此时只有转接器通电（电脑 USB → 转接器），P4 板还没电。`ls /dev/cu.usbserial-*` 应该看到设备出现（macOS 上类似 `/dev/cu.usbserial-0001`，CP2102/CH340 各家命名不同）。

5. **按住板子正面的 BOOT 按钮不放**。

6. **保持按住 BOOT** 的同时，把 H1 插上电脑给板子上电。这一瞬间发生的事：
   - P4 看到 BOOT 是低电平 → 进 download mode、不会跑我们的固件，于是 GPIO54 浮空（不会去乱 reset C6）
   - C6 看到 IO9 是低电平（你的短接）→ 进 download mode、停在 ROM bootloader 等命令

7. **松开 BOOT 按钮**（已经上电完成、strap 已锁存，松开没影响）。

8. **运行项目烧录脚本**。它会每 1 秒自动探测串口、校验镜像，并用已经
   真机验证过的 115200 baud / no-reset 序列写入。串口出现后只启动一次
   esptool，避免 USB-UART 被反复打开时出现 macOS `termios EINVAL`：

   ```bash
   firmware/tools/flash_c6_hosted.sh --port /dev/cu.usbserial-XXXX

   # 连续烧多块：每块成功后拔出 USB-UART，脚本会自动等待下一块
   firmware/tools/flash_c6_hosted.sh --batch
   ```

   脚本等待期间不要求按 Enter；不接硬件时可先用 `--check-only` 校验
   ESP-IDF 环境与镜像。它会强制设置 `ESP_IDF_VERSION=6.0`，避免
   ESP-Hosted Kconfig 回退到 H2/SPI。

   （路径 A 只烧 app 一个 bin；路径 B 还要加上 bootloader + partition table，详见
   [`docs/hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md) §4。）

   看到 `Hash of data verified.` 即烧录成功。

9. **断电收线**：
   - 拔掉 H1（断开整板电源）
   - 拔掉 IO9 ↔ GND 短接（不拔的话下次上电 C6 又会进 download mode）
   - 杜邦线可以留着也可以拆，下次烧 C6 还能用

10. **重新插上 H1**。C6 这次 IO9 是高电平了，正常引导执行 hosted slave 固件；P4 也正常引导执行我们的固件。验证日志里能看到：

    ```
    I (xxxx) transport: Identified slave [esp32c6]
    I (xxxx) ble_gatt: advertising as "Pilot Kit Box-XXXXXX"
    ```

详细分步说明 + 故障排查见 [`docs/hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md)。

---

## 4. 配置（多数情况下用默认即可）

### 设置目标芯片

第一次构建前必须告诉 ESP-IDF 目标芯片：

```bash
cd Pilot-Kit-Box-ESP32-P4/firmware
idf.py set-target esp32p4
```

这一步会生成 `sdkconfig` 文件（基于 `sdkconfig.defaults`）。**如果之前已经存在 `sdkconfig`，建议先改名备份再重新生成**：

```bash
[ -f sdkconfig ] && mv sdkconfig sdkconfig.backup-$(date +%Y%m%d-%H%M)
idf.py set-target esp32p4
```

### 关键配置项（已在 sdkconfig.defaults 写好，无需手动改）

如果你只是要把固件跑起来，**这一节可以跳过**。完整的配置参考见 [`docs/configuration.md`](configuration.md)。

- `CONFIG_IDF_TARGET="esp32p4"` — 目标芯片
- `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y` — Waveshare 板有 32 MB Nor flash
- `CONFIG_PARTITION_TABLE_CUSTOM=y` + `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` — 使用自定义分区表（含 10 MiB LittleFS 区；另支持可选 MicroSD 文件后端）
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

## 5. 编译

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

pilot_kit_box.bin binary size 0x989a40 bytes. Smallest app partition is 0xa00000 bytes. 0x765c0 bytes (5%) free.
```

产物位于 `firmware/build/`：
- `bootloader/bootloader.bin` — 二级 bootloader (~23 KiB)
- `partition_table/partition-table.bin` — 分区表 (~3 KiB)
- `pilot_kit_box.bin` — 主固件 (~960 KiB)

---

## 6. 连接板子 + 找到串口

### 接线

把 USB-C 数据线一端接 H1、也就是丝印为 `USB TO UART` 的 Type-C 口，
另一端接电脑。

> H1 走 CH343P USB-UART 桥。RTL-SDR 走 P4 原生 USB 2.0 HS：装上 Pilot Kit
> 载板时插载板的 USB-A（J3-27/25），H2 保持空置；裸板时改插丝印为 `USB`
> 的 H2 Type-C（同一组网络，二者只能占一个）。P1 是 C6 下载排针，不是 USB。

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

## 7. 烧录 P4 + 串口监视

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

## 8. 看到什么算成功

烧完后，串口应该有类似这样的开机日志（**没接任何外设的空板** + 已完成 Section 3 的 C6 烧录情况）：

```
I (1559) pilot_kit:   Pilot Kit Box (ESP32-P4) boot
I (1564) pilot_kit:   Free internal heap at boot: 350371 B
I (1569) pilot_kit:   IQ ring buffer ready: 524288 B (BYTEBUF)
I (1575) pilot_kit:   Installing USB host stack on peripheral_map=0x1
I (1611) pilot_kit:   USB host stack installed
I (1611) pilot_kit:   USB host stack online — spawning SDR + DSP tasks
I (1612) record_sink: registered sink 'uart' (1 total)
I (1615) record_sink: registered sink 'ble_raw' (2 total)
I (1620) pk_sd:       no microSD card at boot (will keep probing)      ← 没插卡时正常
I (3236) rec_file:    LittleFS mounted at /storage: 32/10240 KiB used   ← 第一次启动会自动格式化 LittleFS
I (3470) record_sink: registered sink 'file_littlefs' (3 total)
I (3472) rec_file:    logging ADS-B to /storage/pilot_kit_ts_1.txt (rotate every 1024 KiB, keep 12 files)
I (3474) pilot_kit:   ADS-B sinks ready (UART + file at /storage)
I (3480) sdr:         USB client registered, waiting for RTL-SDR enumeration
I (3486) dsp:         dsp_task running (dump1090-derived edge decode)
I (3617) display:     ST7701 DSI ready: logical 800x480 -> PPA 90 CW -> native 480x800, 2 DPI buffers, app framebuffer 750 KiB PSRAM
E (4401) imu:         enable_rotation_vector: ESP_ERR_INVALID_RESPONSE         ← 预期（没接 BNO085）
W (4402) pilot_kit:   IMU init failed (ESP_ERR_INVALID_RESPONSE) — PFD will run without attitude
I (4405) pfd:         pfd_task running (G1000 landscape)
I (4417) pilot_kit:   PFD render task running
I (4490) transport:   Identified slave [esp32c6]
I (4510) vhci_drv:    Host BT Support: Enabled | BT Transport Type: VHCI
I (4520) ble_gatt:    NimBLE host task running
I (4530) ble_gatt:    GDL90 emitter task running
I (4540) ble_gatt:    BLE address 8c:fd:49:0b:5a:8a type=0
I (4550) ble_gatt:    advertising as "Pilot Kit Box-0B5A8A"
I (4560) main_task:   Returned from app_main()
I (4544) dsp:         stream 0.00 MB/s | msgs/s 0 (...) | aircraft 0           ← 1Hz dashboard 心跳
I (5439) pfd:         PFD 32 FPS  | roll= +0.00 pitch= +0.00 yaw=  0.00 ...    ← 1Hz FPS 心跳
... (PFD + DSP 心跳每秒一行持续输出)
```

**关键确认点**：

| 看到这条 | 说明 |
|---------|------|
| `Pilot Kit Box (ESP32-P4) boot` | 主应用启动正常 |
| `Found 32MB PSRAM device` + `Speed: 200MHz` | PSRAM 32 MB 起来了（之前在 1.5 节里看到）|
| `Reserving pool of 128K of internal memory for DMA/internal allocations` | DMA 内部内存预留 OK |
| `LittleFS mounted` | 存储分区 OK |
| `USB host stack online` | USB host 就绪（等 RTL-SDR） |
| `Returned from app_main()` | 所有初始化完成 |
| `PFD 30+ FPS` 周期出现 | 显示渲染管线 OK |
| `dsp: stream 0.00 MB/s` 1Hz 重复 | DSP task 运行中 |
| `IMU init failed` | 预期（没接 BNO085） |
| `advertising as "Pilot Kit Box-XXXXXX"` | BLE 起来了，手机能扫到（XXXXXX = 本机 C6 MAC 后 3 字节） |

> 💡 **跳过 Section 3 的情况**（没烧 C6 hosted slave）：`PK_BLE_ENABLED` 改成 `n` 之后，上面 `advertising as ...` 那行会换成 `BLE disabled at build time (CONFIG_PK_BLE_ENABLED=n) — UART + file sinks only`。其他都一样。

### 接外设的预期

| 操作 | 预期 |
|------|------|
| RTL-SDR dongle 插载板 USB-A（裸板则经 H2 USB-C OTG 转接头或 Hub） | `sdr: USB NEW_DEV at addr 1` → `Tuned to 1090000000 Hz` → `Sampling at 2000000 S/s` → `rtlsdr_async: starting async stream` → `dsp: stream 2.00 MB/s` |
| 给 4.3 寸一体板上电 | 800×480 全屏显示 Pilot Kit boot splash，最少停留约 3 秒 → 显示 PFD |
| 装 GT-U8 GPS | DIAG 更新 GPS/北斗卫星、SNR、天线状态和系统时间；定位后 TRAFFIC 获得本机位置 |
| 装 BMP388 | PFD / DIAG 显示压力、QNH 修正高度和升降率 |
| 插入 FAT32 MicroSD | DIAG 显示容量；Settings 把 LOG 改成 MICROSD 并重启后，日志写入 `/sdcard` |
| 接 BNO085 模块到 I²C0 + INT/RST | 串口出现 `imu: rpy = +1.23 / -0.45 / 187.66 (acc=3 valid=98)` 周期更新，晃动板子时 PFD 的 horizon 跟着翻 |
| C6 刷完 hosted slave 固件（见第 3 节） | 串口 `ble_gatt: advertising as "Pilot Kit Box-XXXXXX"`，手机 BLE 扫描器按 `Pilot Kit Box-` 前缀过滤能看到 |

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
[ -f sdkconfig ] && mv sdkconfig sdkconfig.bad-rev-$(date +%Y%m%d-%H%M)
idf.py build
```
重新烧。

### 启动早期 `HS_MP: mempool create failed: no mem` panic

**原因**：ESP-Hosted 在 `__libc_init_array()` 阶段（main 之前）调用 SDIO mempool 初始化，需要约 47 KiB DMA-capable 内部 RAM。如果默认 SDIO queue（20×1536）+ 默认 SPIRAM reserve（32 KiB）+ PSRAM 未启用三个条件中任一不满足，allocator 凑不出连续内存。

**修复**：sdkconfig.defaults 已经包含修复（PSRAM 启用、SDIO queue 缩到 8、reserve 增到 128 KiB）。如果你改过这些配置导致问题复发，恢复以下默认：
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=131072
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8
```

### 启动早期 `spi_mempool_create spi_drv.c:141 (buf_mp_g)` panic

**症状**：开机几乎立即崩溃，串口出现：
```
E (1534) HS_MP: mempool create failed: no mem
assert failed: spi_mempool_create spi_drv.c:141 (buf_mp_g)
```
然后栈展开里看到 `bus_init_internal at spi_drv.c:302` ← 注意这里是 **SPI**，跟上一节的 SDIO 看起来像但**根因完全不同**。

**根因**：ESP-Hosted 把 co-processor 当成 **ESP32-H2 走 SPI transport** 而不是预期的 **ESP32-C6 走 SDIO**。后果是 SPI 队列默认要 20×1536 B 内部 RAM，凑不出 → assert。

为什么 H2/SPI 替代了 C6/SDIO？因为 ESP-IDF v6.0.1 不再把 `ESP_IDF_VERSION` 环境变量塞进 Kconfig build env（只塞 `IDF_VERSION`），但 `managed_components/espressif__esp_wifi_remote/Kconfig` 仍然写着：

```kconfig
orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"
```

变量为空 → `Kconfig.idf_v6.0.in` silently 不加载 → `SLAVE_IDF_TARGET_ESP32C6` 选项不存在 → `ESP_HOSTED_CP_TARGET_ESP32C6` 因 `depends on SLAVE_IDF_TARGET_ESP32C6` 而隐藏 → choice 唯一未隐藏的 H2 选项被默认选中 → H2 默认走 SPI → 崩溃。

**快速识别**：看 `firmware/sdkconfig` 是否含：
```
CONFIG_ESP_HOSTED_CP_TARGET_ESP32H2=y          ← 错的
CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=12    ← 应该是 54
# CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE is not set
```

如果是，说明踩到了。

**修复**：

1. 备份再删 `sdkconfig`：
   ```sh
   cd firmware
   mv sdkconfig sdkconfig.broken-$(date +%Y%m%d-%H%M)
   ```

2. **设 `ESP_IDF_VERSION=6.0`** 后再 reconfigure：
   ```sh
   ESP_IDF_VERSION=6.0 idf.py reconfigure
   ```

   或更省事 —— 用项目自带的 `firmware/build.sh` wrapper，它自动设这个 env：
   ```sh
   ./build.sh reconfigure
   ./build.sh build
   ./build.sh -p /dev/cu.usbserial-XXX flash monitor
   ```

3. 检查 `sdkconfig` 现在写的是 C6/SDIO：
   ```sh
   grep -E "ESP_HOSTED_CP_TARGET_ESP32C6|ESP_HOSTED_SDIO_HOST_INTERFACE|ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=54" sdkconfig
   ```
   全部要在输出里。

**为什么不在应用层做"容错"绕过？** 这个 panic 发生在 `__libc_init_array`（C++ 全局构造器阶段），早于 `app_main()`，连 ESP_LOG 之外的应用代码都没机会跑。`CONFIG_PK_BLE_ENABLED` 那个 guard 只挡得住 `ble_gatt_init()` 这种 main-time 调用，挡不住 esp_hosted 的全局构造器。这是**配置层 bug**，应用层无解。

### `assert failed: vApplicationGetTimerTaskMemory` 启动期 panic

**原因**：内部 RAM 紧张到连 FreeRTOS timer task 的 8 KiB 栈都凑不出。通常是大型静态数组占了 `.bss` 内部段。

**修复**：把大数组（>1 KiB）的 `static` 声明加 `EXT_RAM_BSS_ATTR`（在 `esp_attr.h`）把它们丢到 PSRAM `.ext_ram.bss` 段。本仓库 `pfd.c` 和 `ble_gatt.c` 的 64 槽 `aircraft_t` 表已经这样处理。需要 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`。

### `assert failed: hci_h4_frame_start hci_h4.c` 启动期 panic

**原因**：NimBLE 默认走 H4 UART HCI（GPIO 4/5 @ 921600 baud）找 BT controller。我们的 controller 在 C6 上通过 SDIO 接出来，不是 UART。NimBLE 在 GPIO 4/5 上收到的全是垃圾，hci_h4 解码 assert。

**修复**：`CONFIG_BT_NIMBLE_TRANSPORT_UART=n` 已在 sdkconfig.defaults。这样 NimBLE 会自动用 esp_hosted 提供的 VHCI transport。

### `ble_transport_ll_init` 处 `ESP_ERROR_CHECK` 导致 abort

**原因**：ESP-Hosted 的 `transport_drv_reconfigure()` 在 C6 没有响应（没刷 hosted slave 固件）时返回失败；vhci_drv.c 第 154 行用 `ESP_ERROR_CHECK()` 包裹这个返回值，强制 abort。

**修复**：先按 [§3](#3-新板首次设置烧-esp32-c6-hosted-slave-固件一次性) 烧 C6 hosted slave 固件。已经烧过、还出这个错，再去 [`docs/hardware/c6_bringup_status.md`](hardware/c6_bringup_status.md) 对一下 GPIO54 / SDIO pin / 复位极性。如果暂时不想接 C6，跑：`idf.py menuconfig → Pilot Kit Box → [ ] Initialise BLE GATT server at boot`，或直接编辑 `sdkconfig` 把 `CONFIG_PK_BLE_ENABLED` 改成 `n` 再编。

### CMD5 `sdmmc_init_ocr: send_op_cond returned 0x107` 然后 reboot loop

**原因**：P4 host 发了 SDIO CMD5 但 C6 没正确响应（具体是
`INVALID_RESPONSE`，CRC 错或者根本没回）。项目实机验证要求 ESP-Hosted
使用 `RESET_ACTIVE_HIGH`；Rev1.2 原理图同时确认 P4 GPIO54 经 R34 0 Ω
直连 C6 EN，板上没有 inverter / level shifter，不能再用反相器解释该
软件选项。

**修复**：sdkconfig.defaults 已经设置 `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`。如果你改过这一项，恢复默认即可。详细排查过程（包括为什么不是 SDIO 时钟、pull-up、LDO 那些常见嫌疑）见 [`docs/hardware/c6_bringup_status.md`](hardware/c6_bringup_status.md)。

### `BLE_HS_ETIMEOUT_HCI (controller unresponsive)` 反复打印

**原因**：SDIO transport 已经起来（你能看到 `transport: Identified slave [esp32c6]`），但 C6 的 BT controller 没有 enable。ESP-Hosted-MCU 的 slave **不会**在 boot 时自动启 BT controller —— host 要先通过 RPC 显式调用 `esp_hosted_bt_controller_init()` + `esp_hosted_bt_controller_enable()`，再 `nimble_port_init()`。如果你 fork 了 `ble_gatt.c` 删掉了这两步，HCI 命令就会全部 timeout。

**修复**：恢复 `ble_gatt.c::ble_gatt_init()` 里的 connect → init → enable → nimble_port_init 顺序，参照 `managed_components/.../examples/host_nimble_bleprph_host_only_vhci/main/main.c`。

### Task "pfd" / "ble_emit" Stack protection fault

**原因**：栈太小放不下大型局部数组（`aircraft_t scratch[64]` ≈ 4.5 KiB on 4 KiB stack）。

**修复**：本仓库已经把这两个数组改成 `static EXT_RAM_BSS_ATTR`（PSRAM BSS），并把任务栈从 4 KiB 提到 6 KiB。如果你新加自己的渲染或采样任务，遵循同样的约定。

### `Failed to connect to ESP32-P4: Wrong boot mode detected`

esptool 自动复位失败。手动按 BOOT + RESET（见第 6 节末尾）。

### `nimble_port_init` 这一步串口卡住不动

C6 没有 hosted slave 固件。本仓库我们已经把 `ble_gatt_init()` 放在 `main()` 最后一步，所以**前面 LCD / PFD / 存储都会正常起来**。如果你 100% 不想用 BLE，可以临时关掉：

```bash
idf.py menuconfig
# → Component config → Bluetooth → 取消 [*] Bluetooth
```

或回到第 3 节烧 C6 slave 固件。

### `LittleFS mount failed`

99% 是分区表没烧。重跑：
```bash
idf.py -p <PORT> erase-flash
idf.py -p <PORT> flash
```

### MicroSD 已插入但日志仍写 Flash

日志后端只在启动时选择。确认卡是 FAT32，在 Settings 把 `LOG` 改为
`MICROSD` 后重启。启动时卡未挂载成功会自动回退 LittleFS；可在 DIAG
查看 MicroSD 容量和当前 `LOG` 后端。若要格式化卡，先把 LOG 切回 Flash
并重启，再在 `FORMAT SD` 行 5 秒内按两次 UP 或 DOWN 确认。

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
[ -d managed_components ] && mv managed_components managed_components.backup-$(date +%Y%m%d-%H%M)
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
