# Pilot Kit Box — 固件架构

英文版：[`architecture.md`](architecture.md)

本文描述当前 4.3 寸触摸版 ESP32-P4 固件的运行拓扑，包括 RTL-SDR USB
Host、Mode-S DSP 解码、LittleFS / MicroSD / UART / BLE 输出、GT-U8
GPS NMEA/RMC、BMP388、BNO085，以及 PFD、交通、列表、设置、关于和诊断页面。

## 总览

```mermaid
flowchart LR
    subgraph HW["硬件"]
        direction TB
        SDR["RTL-SDR\n1090 MHz"]
        USB["原生 USB 2.0 HS\n载板 USB-A 走 J3-27/25\n（H2 同网，裸板时用）"]
        C6["ESP32-C6-MINI-1\nWi-Fi 6 / BLE 5"]
        SDIO_C6["SDIO\nCLK=18 CMD=19\nD0..3=14..17\nRESET=54"]
        FLASH["32 MB Nor Flash\nfactory app 12 MiB"]
        SD["MicroSD slot\nSDMMC 4-bit\nCLK=43 CMD=44\nD0..3=39..42"]
        GPS["GT-U8 GPS/BDS\nUART1 P4 TX=49 P4 RX=51\nRMC 授时；PPS(50) 未读取"]
        BARO["BMP388\nI²C0 addr 0x76\n轮询，INT=31 未用"]
        BNO["BNO085 IMU\nSDA=7 SCL=8\n轮询，RST=28 INT=34"]
        SCREEN["ST7701 MIPI-DSI\n原生 480×800\nPPA → 800×480\nRST=27 BL=26"]
        TOUCH["GT911 触摸\nI²C0 7/8\nRST=23"]
        BLE_PEER["iPad / iPhone\nPilot Kit app"]
    end

    subgraph P4["ESP32-P4NRW32"]
        direction TB
        USBTASK["usb_host_lib_task\nCPU0 prio 5"]
        SDRTASK["sdr_task\nCPU1 prio 6\nrtlsdr_read_async"]
        RBUF["g_iq_ringbuf\n512 KiB BYTEBUF\nPSRAM"]
        DSPTASK["dsp_task\nCPU1 prio 4\nmagnitude -> detect -> CPR"]
        DISPATCH["record_dispatch\n同步 fan-out"]
        UART["UART sink\nType-C CDC log"]
        FILE["file sink\nFlash: 1 MiB 轮转，目标 12 文件\nMicroSD: 16 MiB × 64"]
        BLE["BLE raw sink\nqueue"]
        STATE["aircraft_state\n64 slots / 60 s fresh window"]
        GDL["GDL90 encoder\nHeartbeat + Traffic"]
        GATT["NimBLE GATT notify"]
        IMU["imu task\nBNO085 100 Hz"]
        UI["pfd task\nPFD / TRAFFIC / LIST\nSETTINGS / ABOUT / DIAG"]
    end

    SDR --> USB --> SDRTASK --> RBUF --> DSPTASK
    USBTASK --> SDRTASK
    DSPTASK --> DISPATCH
    DSPTASK --> STATE
    DISPATCH --> UART
    DISPATCH --> FILE
    DISPATCH --> BLE
    STATE --> GDL --> GATT --> SDIO_C6 --> C6 --> BLE_PEER
    BNO --> IMU --> UI
    GPS --> UI
    BARO --> UI
    TOUCH --> UI
    SCREEN --> UI
    FLASH --> FILE
    SD --> FILE
```

## 任务表

| Task | CPU | 优先级 | 栈 | 职责 |
|---|---:|---:|---:|---|
| `usb_host_lib` | 0 | 5 | 4 KiB | 调用 `usb_host_install()` 并持续 pump `usb_host_lib_handle_events()`。 |
| `sdr` | 1 | 6 | 8 KiB | 拥有 USB client，打开 RTL-SDR，配置 1090 MHz / 2 MSPS，运行 `rtlsdr_read_async()`。USB URB 回调在同一任务上下文执行，只负责把 IQ 推入 ring buffer。 |
| `dsp` | 1 | 4 | 4 KiB | 从 IQ ring buffer 取数据，运行 dump1090 派生的幅度计算、前导码检测、曼彻斯特解码和 CPR 定位，并输出 1 Hz dashboard。 |
| `rec_file` | 0 | 3 | 4 KiB | 文件写入任务；启动时按 NVS 设置选择 LittleFS 或 MicroSD，缺卡时回退 LittleFS，避免 DSP hot path 被存储写入阻塞。 |
| `gps` | 0 | 4 | 4 KiB | 解析 GT-U8 UART1 NMEA（RMC/GGA/GSV/TXT），维护 GPS/北斗定位、卫星/SNR、天线状态，并从 RMC 设置系统时间；不读取 GPIO50 PPS。 |
| `imu` | 0 | 5 | 4 KiB | 以 100 Hz 读取 BNO085 Rotation Vector，应用软件 tare，提供给 PFD 和校准向导。 |
| `baro` | 0 | 4 | 4 KiB | 轻量独立任务：以 ~10 Hz 经 I²C0 轮询 BMP388，运行温度补偿气压→高度换算并计算升降率，结果写入 `g_baro_state`（QNH 可调）。 |
| `sd_detect` | 0 | 2 | 4 KiB | MicroSD 插拔探测：无卡时每 3 秒尝试挂载，已挂载时每 2 秒探活并刷新容量缓存。 |
| `buttons` | — | — | — | 保留旧源码但 4.3 寸触摸板不启动该任务。 |
| `pfd` | 0 | 4 | 6 KiB | 把 PFD 与 UI 页面渲染到 800×480 逻辑 framebuffer。 |
| `nimble_host` | 0 | 4 | 4 KiB | NimBLE host 事件循环，通过 C6 的 SDIO / VHCI controller 处理 BLE。 |
| `ble_emit` | 0 | 3 | 6 KiB | 每秒快照 `aircraft_state`，发送 GDL90 Heartbeat 和 Traffic Report，同时发送 raw ts-line 队列。 |

## 内存预算

| 区域 | 大小 | 所有者 |
|---|---:|---|
| IQ ring buffer | 512 KiB | `g_iq_ringbuf`，大块 IQ 缓冲，当前通过 malloc 阈值放入 PSRAM |
| URB pool | 约 96 KiB | 15 × 6400 B USB in-flight transfer |
| DSP 工作集 | 约 12 KiB | 8 KiB IQ buffer + 4 KiB magnitude buffer |
| CPR table | 约 5 KiB | `cpr_decode.c` 中 64 架飞机的 CPR pairing 状态 |
| aircraft_state | 约 7 KiB | `aircraft_state.c` 中 64 slots，保存呼号、高度、位置、速度等 |
| 应用 framebuffer | 750 KiB | 800×480×16 bpp RGB565-swapped，位于 PSRAM |
| DPI framebuffer | 1.5 MiB | 两块 480×800×16 bpp 扫描缓冲，位于 PSRAM |
| file sink queue | 约 10 KiB | 256 × `file_record_t` |
| BLE raw queue | 约 5 KiB | 64 × 80 B raw ts-line |
| NimBLE host | 约 30 KiB | GATT DB、连接状态、事件循环等 |
| aircraft DB blob | 约 8.16 MiB flash | `aircraft_db.bin`，通过 `EMBED_FILES` 嵌入，用于 ICAO24 -> 机型/型号/注册号查询 |
| 航司/国家表 | 约 230 KiB + 小型 flash 表 | `airline_codes.c` 和 `icao_country.c`，生成式查找数据，用于呼号和国家显示 |

大块缓冲尽量放入 PSRAM，内部 768 KiB SRAM 留给 DMA-capable 分配、FreeRTOS 栈、ESP-Hosted 队列和 USB host descriptor。

## 故障隔离

```text
URB / IQ stall -> librtlsdr 累计 transfer error，或 dsp_task 检测 IQ 停滞后调用
                  pk_sdr_request_reinit()。sdr_task 关闭并重新打开同一 dongle；
                  连续失败达到上限后才 esp_restart()。

Ring overflow  -> on_iq 中 xRingbufferSend 失败，只累计 drop counter；
                  DSP dashboard 输出 WARN，USB 回调不阻塞。

File queue full -> file sink xQueueSend 失败，记录 drop；
                   DSP path 不等待 flash。

Storage write -> LittleFS / MicroSD 的 fwrite 或 rotation fopen 失败时记录错误；
                 UART 和 BLE sink 继续工作。MicroSD 拔出后探测任务会卸载，
                 但本次启动的文件后端不会动态切换，需重启后重新选择。

BLE peer drops -> NimBLE 处理断连；没有订阅者时 notify 被跳过；
                  重新连接后自动继续。
```

## 时间同步

固件没有持久化系统墙钟，上电时系统时间从 Unix epoch 0 开始。当前有三条
校时路径，按质量保护避免低质量来源覆盖高质量来源：

1. **GT-U8 RMC**：RMC 提供 UTC 日期时间，是首选来源。GPIO50 只是未来
   PPS 的可选接线预留；当前固件没有 PPS GPIO 输入或纪律环。
2. **iOS Current Time Service**：iOS 默认暴露 SIG 标准 CTS（UUID `0x1805`）。固件在 GAP CONNECT 后作为 GATT client 读取 `0x2A2B` 当前时间并调用 `settimeofday()`。
3. **自定义 Time Sync characteristic**：UUID `...0004`，客户端写入 8 字节 little-endian Unix epoch milliseconds。Android 和跨平台客户端推荐使用这一条。

校时前输出的 Mode-S frame 仍会进入所有 sink，只是 `ts_ms` 很小（接近开机以来毫秒数）。客户端可以识别并丢弃这些 pre-sync frame。

### 载板传感器（GPS / 气压 / microSD）

Pilot Kit 通过 UART 连接 GT-U8 GPS，通过 I²C0（`0x76`）连接 BMP388，
并使用 Rev1.2 板载 microSD 卡槽。GPIO50 可为未来 PPS 预留，但当前路径
未实现。各能力如何接入上面的架构：

- **GPS 授时**（`gps_task.c`）：GPS RMC UTC 设置 `settimeofday()`。**GPS 优先**，
  BLE 作备份；有覆盖保护，低质量源不会盖掉已校准好的 GPS 时间。设备无需手机即可
  自主校时，DIAG 显示系统时间、定位和卫星状态；当前没有 PPS 边沿时间修正。
- **GPS own-ship**（`gps_task.c`）：没有启用编译期 ADS-B 本机 ICAO 时作兜底。
  接入现有 `aircraft_state` own-ship 路径 + GDL90 ownship report。
- **BMP388 气压**（`baro_task.c`）：高度/升降率仅作**参考**显示（增压座舱内失真），不作权威高度；QNH 可在 SETTINGS 中调整。
- **microSD 记录后端**：Settings 可选 Flash / MicroSD，设置写入 NVS 并在下次启动生效。
  选择 MicroSD 但启动时未挂载会回退 LittleFS。Flash 按 1 MiB 轮转，文件数目标为 12，
  实际保留量受 10 MiB 分区限制；
  MicroSD 使用 16 MiB × 64 个文件（约 1 GiB），并支持受保护的 FAT32 格式化。

## 数据格式

所有 sink 对同一条 Mode-S frame 使用相同文本形状：

```text
1715432198765 *8D4CA1BD58C386435840BA1AD7CA;
^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    ts_ms      AVR-format Mode-S hex payload
```

这也是 Pilot Kit 离线脚本和 BLE Raw characteristic 使用的格式。GDL90 encoder 是另一路结构化输出，供 EFB / 移动端消费。

## 关键架构选择

1. **RF / USB 工作集中在 CPU1**：减少 BLE、LCD、SDIO 等 CPU0 工作对 2 MSPS IQ path 的抖动影响。
2. **单任务拥有 USB client**：同一个 USB client 不能被多个任务同时 pump。`sdr_task` 是唯一事件泵。
3. **DSP 不阻塞 I/O**：文件、BLE、串口 sink 自己处理背压，DSP loop 保证继续消费 IQ。
4. **wire / disk / BLE raw 格式一致**：`<ts_ms> *<HEX>;` 贯穿串口、文件和 BLE Raw，降低调试和后处理成本。
