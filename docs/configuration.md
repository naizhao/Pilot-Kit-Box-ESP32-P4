# 配置参考 / Configuration Reference

本文档列出 Pilot Kit Box 固件**所有用户可能需要调整的 sdkconfig 配置**，按主题分组，并解释每个选项的影响。如果你只是想"开机即可"，按 [`docs/BUILD.md`](BUILD.md) 走默认值即可——本文档是给想做定制的人看的。

> 📍 **改配置的入口**：
> ```bash
> cd firmware
> idf.py menuconfig
> ```
> 终端 UI 会出来，方向键导航，`/` 搜索，`?` 查每个选项的帮助。
>
> 改完保存退出后，重新 `idf.py build` 即可。
>
> **永久化默认值**：如果你想让某个改动跟着 git 提交（让所有协作者都用同一配置），把对应 `CONFIG_*=...` 行加到 `firmware/sdkconfig.defaults`。`sdkconfig` 文件**不进 git**（在 `.gitignore` 里），每个人本地可以自由覆盖。

---

## 目录

1. [硬件目标 (P4 silicon revision)](#1-硬件目标-p4-silicon-revision)
2. [分区表与 Flash](#2-分区表与-flash)
3. [USB Host (RTL-SDR)](#3-usb-host-rtl-sdr)
4. [蓝牙 / BLE / ESP-Hosted](#4-蓝牙--ble--esp-hosted)
5. [存储 / LittleFS](#5-存储--littlefs)
6. [显示 / ST7789](#6-显示--st7789)
7. [IMU / BNO085](#7-imu--bno085)
8. [日志输出](#8-日志输出)
9. [FreeRTOS 调度](#9-freertos-调度)
10. [编译优化](#10-编译优化)

---

## 1. 硬件目标 (P4 silicon revision)

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `CONFIG_IDF_TARGET` | `"esp32p4"` | 目标芯片，**绝对不要改** |
| `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` | `y` | 选择支持 v0.x / v1.x silicon。当前 Waveshare ESP32-P4-WIFI6 出货的是 v1.3 silicon，必须 `y`。**如果未来你拿到了 ECO5 (v3.x) 的 Waveshare 板**，把这个改成 `n` 并把下面的 `REV_MIN_*` 改成 `300` 或 `301` |
| `CONFIG_ESP32P4_REV_MIN_100` | `y` | 最低支持 v1.0 silicon。配合上面那一项使用 |

### 二进制兼容性警告

**ESP32-P4 v1.x 和 v3.x 之间不二进制兼容**——硬件 bug 修复不同，启动序列不同。如果你的板子是 v3.x，必须改用 `CONFIG_ESP32P4_REV_MIN_300` 或 `_301`，并把 `SELECTS_REV_LESS_V3` 关掉，重新编译。

确认你的板子是哪个 revision：烧固件时 esptool 会打印：
```
Chip type:          ESP32-P4 (revision v1.3)
                                    ^^^^
```

---

## 2. 分区表与 Flash

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `CONFIG_ESPTOOLPY_FLASHSIZE_32MB` | `y` | Waveshare 板有 32 MB Nor flash |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `"32MB"` | 同上的字符串形式 |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | 用我们自己的 `partitions.csv` |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `"partitions.csv"` | 文件名（相对 firmware 根目录） |

### partitions.csv 当前布局

```
# Name,    Type, SubType,  Offset,    Size,    Flags
nvs,       data, nvs,      0x9000,    0x6000,            ← 24 KiB Wi-Fi/BLE 配置
phy_init,  data, phy,      0xf000,    0x1000,            ← 4 KiB RF 校准
factory,   app,  factory,  0x10000,   0x400000,          ← 4 MiB 主固件
storage,   data, spiffs,   ,          0x1000000,         ← 16 MiB LittleFS
```

总占用 ~20 MB / 32 MB，**剩余 12 MB 预留给未来 OTA pair**（A/B 分区双备份升级）。

### 想加 OTA？

改 `firmware/partitions.csv`，把 `factory` 替换为 `ota_0` + `ota_1` + `otadata`，例：

```
nvs,       data, nvs,      0x9000,    0x6000,
otadata,   data, ota,      ,          0x2000,
phy_init,  data, phy,      ,          0x1000,
ota_0,     app,  ota_0,    ,          0x400000,
ota_1,     app,  ota_1,    ,          0x400000,
storage,   data, spiffs,   ,          0x800000,    ← 缩到 8 MiB 给 OTA 让位
```

---

## 3. USB Host (RTL-SDR)

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `CONFIG_USB_HOST_HUBS_SUPPORTED` | `y` | 启用 USB hub 支持（某些 RTL-SDR 棒内置 hub） |
| `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` | `512` | 让我们能读全 RTL2832U 的描述符 |

### 想换中心频率？

频率写死在 `firmware/main/pilot_kit.h`：
```c
#define PK_RTLSDR_FREQ_HZ        1090000000UL  /* ADS-B Mode-S */
```

改这个常量重编即可。常见替代频率：
- `122900000` — VHF 民航语音（122.9 MHz）
- `162400000` — AIS 船舶
- `433920000` — ISM 433
- `915000000` — LoRa US

### 想换采样率？

```c
#define PK_RTLSDR_SAMPLERATE_HZ  2000000UL     /* 2 MSPS for ADS-B */
```

RTL2832U 范围 225 kSPS – 3.2 MSPS。ADS-B 必须 ≥ 2 MSPS（每比特 1 µs，每帧 120 µs）。

---

## 4. 蓝牙 / BLE / ESP-Hosted

> ⚠️ **默认 `CONFIG_PK_BLE_ENABLED=n`，BLE 启动时不初始化**。本仓库特别加了这个开关，因为 esp_hosted v2.x 的 vhci_drv.c 在 C6 不响应时直接 `ESP_ERROR_CHECK()` abort 整个固件——没有优雅降级。
>
> **使用 BLE 的前提**：
> 1. 给 C6 烧 esp_hosted slave 固件 — 见 [`docs/hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md)
> 2. `idf.py menuconfig` → Pilot Kit Box → [*] Initialise BLE GATT server at boot
> 3. 重新 build + flash
>
> 完全不要 BLE？把 `CONFIG_BT_ENABLED` 也改成 `n` 可以再省 ~500 KiB flash。

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `CONFIG_PK_BLE_ENABLED` | `n` | **我们自己加的 Kconfig**。`y` 时 app_main 会调 `ble_gatt_init()`；`n` 时跳过整个 NimBLE/hosted vhci 初始化链路，固件可以在 C6 没固件的板子上稳定运行 |
| `CONFIG_BT_ENABLED` | `y` | 启用 BT 子系统（编进 NimBLE host）。即使 `PK_BLE_ENABLED=n` 也保持 `y`，因为关掉它会让 ble_gatt.c 编不过 |
| `CONFIG_BT_CONTROLLER_DISABLED` | `y` | P4 自身无原生 BT 控制器，要走 C6。**这个必须是 `y`** |
| `CONFIG_BT_NIMBLE_ENABLED` | `y` | 用 NimBLE host（不是 Bluedroid） |
| `CONFIG_BT_NIMBLE_TRANSPORT_UART` | **`n`** | **关键**：默认是 `y`，会让 NimBLE 在 GPIO 4/5 上找 UART HCI controller。我们要走 hosted VHCI 而不是 UART，所以必须显式 `n` |
| `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` | `y` | 我们做被连接端 |
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | `y` | 启用 GATT client API（用来读 iOS 的 Current Time Service 同步时钟） |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | `256` | 让 GDL90 traffic frame 一次发完不分片 |

### ESP-Hosted SDIO 引脚

针对 Waveshare ESP32-P4-WIFI6 的硬连线：

```
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_PIN_CLK=18
CONFIG_ESP_HOSTED_SDIO_PIN_CMD=19
CONFIG_ESP_HOSTED_SDIO_PIN_D0=14
CONFIG_ESP_HOSTED_SDIO_PIN_D1=15
CONFIG_ESP_HOSTED_SDIO_PIN_D2=16
CONFIG_ESP_HOSTED_SDIO_PIN_D3=17
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW=y
CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y
```

**改板子要换引脚**：menuconfig 里 Component config → ESP-Hosted config → SDIO Pin Configuration。

### ESP-Hosted 队列大小

默认 SDIO queue 是 20×1536 字节 per direction，约 47 KiB DMA-capable 内部 RAM per pool。在 ESP-Hosted constructor 早期启动阶段（在 `__libc_init_array()` 里跑、`main()` 之前），内部 RAM 还很碎片化，47 KiB 连续 64-byte aligned 块可能凑不出。

我们把队列缩到 **8**（我们只用 BLE 不用 Wi-Fi 远程，8 完全够）：

```
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8
```

如果将来要走 Wi-Fi 高吞吐场景，可以试着提到 12 或 16，但需要同时增加 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`。

### BLE 设备名

写死在 `firmware/main/ble_gatt.c`：
```c
#define BLE_DEVICE_NAME      "PilotKitBox"
```

如果你要做多块板，可以改成包含芯片 MAC 后 6 位的形式（方便识别）。

### BLE GATT UUID

定义在 `firmware/main/ble_gatt.c` 顶部的 `s_svc_uuid` 等数组。**改 UUID 会让 Pilot Kit 移动 App 认不出来**，除非 App 也同步更新。详见 [`docs/ble_protocol.md`](ble_protocol.md) 第 3 节。

---

## 5. 存储 / LittleFS

定义在 `firmware/main/record_sink_file.c`：

```c
#define FILE_QUEUE_DEPTH    256                   /* 排队上限 */
#define FILE_ROTATE_BYTES   (1 * 1024 * 1024)    /* 每个文件 1 MiB */
#define FILE_KEEP_COUNT     12                   /* 保留 12 个文件 = 12 MiB */
```

调整后果：

| 改动 | 影响 |
|------|------|
| ↑ `FILE_QUEUE_DEPTH` | 短时高速写入时不丢数据，代价是 ~40B × N RAM |
| ↑ `FILE_ROTATE_BYTES` | 文件个数减少，每个文件更大；flash 擦除单位也更大 |
| ↑ `FILE_KEEP_COUNT` | 历史数据保留更久，总占用更大；超过 16 MiB 分区容量会丢老的 |
| 改文件命名前缀 `FILE_NAME_PREFIX` | 要同步改 `Pilot-Kit/scripts/adsb_to_track.py` 的 glob，否则 Python 端找不到 |

---

## 6. 显示 / ST7789

### 引脚

写死在 `firmware/main/display.h`：

```c
#define PK_LCD_SPI_HOST       SPI2_HOST
#define PK_LCD_PIN_SCLK       47    ← FSPICLK (IO_MUX direct)
#define PK_LCD_PIN_MOSI       45    ← FSPID (IO_MUX direct)
#define PK_LCD_PIN_CS         46
#define PK_LCD_PIN_DC         48
#define PK_LCD_PIN_RST        49
#define PK_LCD_PIN_BL         50    ← LEDC PWM
```

改板子重新分配引脚时**保持 SCLK / MOSI 用 SPI2 IO_MUX direct 引脚**（GPIO47/45）能跑满 80 MHz，走 GPIO matrix 的话顶到 40 MHz 左右。

### SPI 时钟

```c
#define PK_LCD_SPI_HZ          (40 * 1000 * 1000)  /* 40 MHz */
```

ST7789 datasheet 最大 80 MHz。我们默认 40 MHz 留 50% margin。

### 背光 PWM 频率

```c
#define PK_LCD_BL_PWM_FREQ_HZ  20000  /* 20 kHz，超出听觉范围 */
```

LED 调光可闻啸叫一般在 <16 kHz。20 kHz 安全且不耗 LEDC 太多分辨率。

### 屏幕方向

`firmware/main/display.c::pk_display_init` 里调用：
```c
ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
```

如果你的屏显示颜色反了（红/青对调），改成 `false`。如果上下颠倒：
```c
esp_lcd_panel_mirror(s_panel, false, true);     /* mirror Y */
esp_lcd_panel_swap_xy(s_panel, true);            /* rotate 90° */
```

---

## 7. IMU / BNO085

写死在 `firmware/main/imu_task.c`：

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `IMU_I2C_PORT` | `I2C_NUM_0` | 与 ES8311 codec 共享 I²C0 |
| `IMU_I2C_HZ` | `400000` | I²C fast mode (400 kHz) |
| `IMU_I2C_ADDR` | `0x4A` | BNO085 默认地址；SA0 拉高变 `0x4B` |
| `IMU_PIN_RST` | `21` | BNO085 RST 接 P4 GPIO21 |
| `IMU_PIN_INT` | `20` | HOST_INT 接 P4 GPIO20（目前仅占位，未中断驱动） |

### 报告速率

`bno_enable_rotation_vector()` 里：
```c
.report_interval_us  = 10000,    /* 100 Hz */
```

可以拉到 200 Hz (`5000` us) 或降到 30 Hz (`33333` us)。BNO085 内部 sensor fusion 最快约 400 Hz。

### 欧拉角约定

`quat_to_euler()` 实现的是 **ZYX Tait-Bryan**（航空标准），输出：
- `roll` (X 轴旋转): -180..+180，右翼下沉为正
- `pitch` (Y 轴旋转): -90..+90，机头上抬为正
- `yaw` (Z 轴旋转): 0..360，从上方看顺时针为正

如果传感器安装方向跟航空体轴不一致，可以在 `quat_to_euler` 之后加一个固定坐标变换。

---

## 8. 日志输出

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `CONFIG_LOG_DEFAULT_LEVEL_INFO` | `y` | 默认输出 INFO 及以上 |
| `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` | `y` | 编译进 DEBUG 级别代码，运行时可动态开 |

### 关掉某模块 INFO 日志

代码里：
```c
esp_log_level_set("pfd", ESP_LOG_WARN);   /* 只看 W 以上 */
```

### 打开某模块 DEBUG 日志

```c
esp_log_level_set("rtlsdr_async", ESP_LOG_DEBUG);
```

通常加在 `app_main()` 开头。

### TAG 列表（常用）

| TAG | 模块 |
|-----|------|
| `pilot_kit` | main.c 启动序列 |
| `sdr` | sdr_task RTL-SDR 控制流 |
| `dsp` | dsp_task DSP 解码 |
| `adsb` | dsp_task 每帧解码结果 |
| `rec_file` | LittleFS 文件 sink |
| `record_sink` | sink 注册 |
| `ble_gatt` | NimBLE host + GATT |
| `rtlsdr_async` | librtlsdr 异步 IO |
| `display` | ST7789 驱动 |
| `imu` | BNO085 驱动 |
| `pfd` | PFD 渲染 |

---

## 9. FreeRTOS 调度

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `CONFIG_FREERTOS_HZ` | `1000` | 1 ms tick |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | `4096` | `app_main()` 任务栈 |

### Task 优先级 (代码里硬编)

| Task | CPU | Prio | Stack | 文件 |
|------|-----|------|-------|------|
| `usb_host_lib` | 0 | 5 | 4 KiB | main.c |
| `sdr_task` | 1 | 6 | 8 KiB | main.c |
| `dsp_task` | 1 | 4 | 4 KiB | main.c |
| `imu_task` | 0 | 5 | 4 KiB | imu_task.c |
| `pfd_task` | 0 | 4 | 4 KiB | pfd.c |
| `rec_file_writer` | 0 | 3 | 4 KiB | record_sink_file.c |
| `nimble_host` | 0 | 4 | 4 KiB | NimBLE 内部 |
| `ble_emit` | 0 | 3 | 6 KiB | ble_gatt.c |

详见 [`docs/architecture.md`](architecture.md) 任务表。

---

## 10. 编译优化

| 配置项 | 默认 | 说明 |
|--------|------|------|
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | `y` | -O2 优化（默认是 -Og debug 模式） |
| `CONFIG_COMPILER_WARN_WRITE_STRINGS` | `y` | `-Wwrite-strings` |

### Size-optimize 用于 release

如果你要塞 OTA 双分区，需要把固件大小压缩。试：

```
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
# 取消 CONFIG_COMPILER_OPTIMIZATION_PERF
```

会用 -Os，固件大约小 10–15%，运行速度略慢但通常无感。

### 关 Bluetooth 省 ~500 KiB

如果你的板没有 C6，或者你不用 BLE：

```
CONFIG_BT_ENABLED=n
```

`pilot_kit_box.bin` 立即从 ~960 KiB 缩到 ~460 KiB，启动也快很多。代码里 `ble_gatt_init()` 会直接 `ESP_ERR_NOT_SUPPORTED` 返回，文件 + 串口 sink 照常工作。

### ccache 加速

```bash
brew install ccache    # macOS
sudo apt install ccache    # Linux
```

激活 ESP-IDF 后：
```bash
export IDF_CCACHE_ENABLE=1
```

加进 `~/.zshrc` 永久生效。改一两个文件后增量编译从 30 秒压到 5 秒。

---

## 11. 改完配置之后

任何 sdkconfig 改动后：

```bash
idf.py build              # 增量编译（多数情况）
# 如果 build 出错，强制重 configure：
idf.py reconfigure
idf.py build
```

对于影响**分区表**的改动（比如换 partitions.csv），第一次烧之前要：
```bash
idf.py -p <PORT> erase-flash   # 抹掉 flash
idf.py -p <PORT> flash
```

对于影响**bootloader**的改动（比如 silicon revision、flash size），同样需要 erase-flash + flash 完整序列。

---

## 12. 想加新配置项？

如果你写了一个新模块，希望让它的某个行为通过 menuconfig 配置：

1. 在 `firmware/main/` 建一个 `Kconfig.projbuild` 文件
2. 写 menuconfig 选项：
   ```
   menu "Pilot Kit Box"
       config PK_FOO_BAR
           int "Foo bar limit"
           default 42
           help
               Bla bla bla
   endmenu
   ```
3. 代码里用：
   ```c
   #include "sdkconfig.h"
   #if CONFIG_PK_FOO_BAR > 100
   ...
   #endif
   ```
4. 把推荐默认值加进 `sdkconfig.defaults`：
   ```
   CONFIG_PK_FOO_BAR=42
   ```

参考 ESP-IDF 任意 component 的 `Kconfig.projbuild` 即可。
