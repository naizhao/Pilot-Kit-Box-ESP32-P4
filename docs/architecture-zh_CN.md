# Pilot Kit Box — 固件架构

英文版：[`architecture.md`](architecture.md)

本文描述当前 4.3 寸触摸版 ESP32-P4 固件的运行拓扑，包括 RP2040 UART
1090 链路、LittleFS / MicroSD / UART / BLE 输出、GT-U8
GPS NMEA/RMC、BMP388、BNO085，以及 PFD、交通、列表、设置、关于和诊断页面。

> 范围说明：当前接收路径是 **v3/v4 扩展板的 RP2040 UART 数据源**——1090 MHz
> 包络经 TLV3501 比较器整形，RP2040 以 PIO+DMA **双沿**捕获（双沿是 R11
> 融合串重建的前提：0→1 融合脉冲对只能靠沿对+脉宽重建），重建
> 56/112-bit Mode-S 帧，经 921600 波特 UART（UART2，P4 RX=46 / TX=32）
> 送入 `adsb_lnk` 任务，进入板上处理管线。捕获/解码细节见
> `firmware/rp2040`。早期的 RTL-SDR USB 数据源（v1/v2 载板与裸板方案，
> `usb_host_lib`/`sdr`/`dsp` 任务与 IQ ring buffer）已整体退役，不再画入
> 拓扑图；相关条目仅在任务表/内存表中作历史保留。

## 总览

```mermaid
flowchart LR
    subgraph HW["硬件"]
        direction TB
        RP2040["RP2040（v3/v4 扩展板）\nTLV3501 比较器 → PULSES\nPIO 双沿捕获 + DMA 块队列\nmodes_edge：前导同步 + PPM 重建\n56/112-bit（不做 CRC——P4 裁决）"]
        C6["ESP32-C6-MINI-1\nWi-Fi 6 / BLE 5"]
        SDIO_C6["SDIO\nCLK=18 CMD=19\nD0..3=14..17\nRESET=54"]
        FLASH["32 MB Nor Flash\nfactory app 12 MiB"]
        SD["MicroSD slot\nSDMMC 4-bit\nCLK=43 CMD=44\nD0..3=39..42"]
        GPS["GT-U8 GPS/BDS\nUART1 P4 TX=49 P4 RX=51\nRMC 授时；PPS(50) 已消费（时间锁定状态）"]
        BARO["BMP388\nI²C0 addr 0x76\n轮询，INT=31 未用"]
        BNO["BNO085 IMU\nSDA=7 SCL=8\n轮询，RST=28 INT=34"]
        SCREEN["ST7701 MIPI-DSI\n原生 480×800\nPPA → 800×480\nRST=27 BL=26"]
        TOUCH["GT911 触摸\nI²C0 7/8\nRST=23"]
        BLE_PEER["iPad / iPhone\nPilot Kit app"]
    end

    subgraph P4["ESP32-P4NRW32"]
        direction TB
        subgraph T_LINK["adsb_lnk 任务 — CPU1 优先级 5"]
            UART_IN["UART2 RX=46 TX=32\n921600 8N1，adsb_link 协议 v1\n（256 字节分片读；对每个 HELLO 回帧，\n1 Hz HEALTH 上报）"]
            INGEST["modes_ingest\nMode-S 24-bit 校验和门（mode_s.c；\ncheck_crc=1，不做纠错；链路外层 CRC-16\n由 codec 负责）\nICAO / 高度 / CPR 提取"]
        end
        CPR["cpr_decode 全局定位\n（64 机 CPR 表）"]
        STATE["aircraft_state\n64 slots / 60 s 窗口\n（呼号/高度/位置/速度融合）"]
        DASH["1 Hz 看板\n（报文率/架数/链路态）"]
        DISPATCH["record_dispatch\n同步 fan-out"]
        UART["UART sink\nType-C CDC log"]
        FILE["file sink\nFlash: 1 MiB 轮转，目标 12 文件\nMicroSD: 16 MiB × 64"]
        BLE["BLE raw sink\nqueue"]
        GDL["GDL90 encoder\nHeartbeat + Traffic"]
        GATT["NimBLE GATT notify"]
        IMU["imu task\nBNO085 100 Hz"]
        UI["pfd task\nPFD / TRAFFIC / LIST\nSETTINGS / ABOUT / DIAG"]
    end

    RP2040 -- "UART 协议 v1\n921600" --> UART_IN
    UART_IN --> INGEST
    INGEST --> CPR --> STATE
    INGEST --> DISPATCH
    INGEST --> DASH
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
| `usb_host_lib` | — | — | — | **已退役**（v1/v2 USB RTL-SDR 时代）：调用 `usb_host_install()` 并持续 pump `usb_host_lib_handle_events()`。不再创建；保留此行作历史参考。 |
| `sdr` | — | — | — | **已退役**（v1/v2 USB RTL-SDR 时代）：拥有 USB client，打开 RTL-SDR，配置 1090 MHz / 2 MSPS，运行 `rtlsdr_read_async()`，把 IQ 推入 ring buffer。不再创建；保留此行作历史参考。 |
| `adsb_lnk` | 1 | 5 | 8 KiB | 运行 RP2040 UART 链路（adsb_link codec @ 921600 波特，256 字节分片读）：把 RP2040 前端送来的 CRC 前置 Mode-S 帧喂进 modes_ingest（Mode-S 24-bit 校验，mode_s.c；check_crc=1，不做纠错），对每个 HELLO 回帧，并承接原 DSP 业务链（CPR/航迹/记录/1 Hz 看板）。RP2040 侧自身经 PIO+DMA 捕获双沿、用 modes_edge 解码 56/112-bit 帧；USB RTL-SDR 任务对（`usb_host_lib`/`sdr`）已退役；`adsb_link_task.c` 沿用 `dsp` TAG 保持日志检索连续。 |
| `rec_file` | 0 | 3 | 4 KiB | 文件写入任务；启动时按 NVS 设置选择 LittleFS 或 MicroSD，缺卡时回退 LittleFS，避免 DSP hot path 被存储写入阻塞。 |
| `gps` | 0 | 4 | 4 KiB | 解析 GT-U8 UART1 NMEA（RMC/GGA/GSV/TXT），维护 GPS/北斗定位、卫星/SNR、天线状态，并从 RMC 设置系统时间；GPIO50 PPS 已被固件消费（GPIO ISR 计数 + 自旋锁快照，1 Hz 采样进入时间锁定状态），授时（settimeofday 级）接线仍是后续任务。 |
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
| IQ ring buffer | 512 KiB | **已退役**（v1/v2 USB RTL-SDR 时代）：`g_iq_ringbuf` 已随退役路径删除；PSRAM 现用于地图瓦片/字体/记录等其余工作集 |
| URB pool       | ~96 KiB | **已退役**：15 × 6400 B 在途 USB 传输 |
| DSP 工作集 | 约 12 KiB | **已退役**：8 KiB IQ buffer + 4 KiB magnitude buffer |
| CPR table | 约 5 KiB | `cpr_decode.c` 中 64 架飞机的 CPR pairing 状态 |
| aircraft_state | 约 7 KiB | `aircraft_state.c` 中 64 slots，保存呼号、高度、位置、速度等 |
| 应用 framebuffer | 750 KiB | 800×480×16 bpp RGB565-swapped，位于 PSRAM |
| DPI framebuffer | 1.5 MiB | 两块 480×800×16 bpp 扫描缓冲，位于 PSRAM |
| file sink queue | 约 10 KiB | 256 × `file_record_t` |
| BLE raw queue | 约 5 KiB | 64 × 80 B raw ts-line |
| NimBLE host | 约 30 KiB | GATT DB、连接状态、事件循环等 |
| aircraft DB blob | 约 8.21 MiB PSRAM | `/sdcard/aero/pk_actdb.bin`，由 `aircraft_db.c` 懒加载进 PSRAM，用于 ICAO24 -> 机型/型号/注册号查询。已不再嵌入 flash；无卡时查询为空，拔卡即释放缓冲 |
| 航司/国家表 | 约 230 KiB + 小型 flash 表 | `airline_codes.c` 和 `icao_country.c`，生成式查找数据，用于呼号和国家显示 |

大块缓冲尽量放入 PSRAM，内部 768 KiB SRAM 留给 DMA-capable 分配、FreeRTOS 栈、ESP-Hosted 队列和 USB host descriptor。

## 故障隔离

```text
RP2040 捕获     -> 环满 DMA 停机（overrun 计数 + lost 标志）。重启前先复位
overrun/重启       PIO 状态机并清空 RX FIFO，重武装的首块带断点位——core1
                   先 modes_edge_reset 再喂，断点两侧的边沿不会拼成假帧。
                   停机窗口外的单沿丢失（RXSTALL）只损坏当前一帧，由解码
                   端自然判负丢弃。以上都进入 1 Hz HEALTH 的 ovr 计数。

链路停滞        -> RP2040 侧：core1 解码停滞 >5 s 由 core0 看门狗经链路
                   上报 ERROR(code=2)。P4 侧：曾 LINKED 后 >5 s 无合法帧
                   即判 STALLED（seq 断档/resync 按协议 v1 计数）；诊断页
                   显示链路状态。

File queue full -> file sink xQueueSend 失败，记录 drop；
                    ingest path 不等待 flash。

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

1. **1090 MHz 时序工作全部在 RP2040**：PIO + DMA 把双沿间隔收进硬件定时的块队列，捕获路径上没有 RTOS——P4 调度抖动不触碰脉冲时序。双沿是必要条件：0→1 融合脉冲对（R11）只能靠沿对+脉宽重建。
2. **单任务拥有 UART 链路**：`adsb_lnk` 是 P4 侧唯一的 codec 读取者；RP2040 侧只有 core0 发送循环发送。协议 v1 有严格的按发送方 seq、HELLO 握手与 1 Hz HEALTH。
3. **融合链不阻塞 I/O**：每一跳的背压都是有损且被计数的——P4 滞后时 RP2040 帧环丢帧（有计数），file/BLE sink 各自持有队列。解码/ingest 的前进不受 flash、BLE 对端或操作者行为影响。
4. **wire / disk / BLE raw 格式一致**：`<ts_ms> *<HEX>;` 贯穿串口、文件和 BLE Raw，降低调试和后处理成本。
