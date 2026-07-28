# C6 hosted slave bring-up — 已解决

英文版：[`c6_bringup_status.md`](c6_bringup_status.md)

2026-05-14 已确认：BLE GATT server 可以以 `Pilot Kit Box-XXXXXX` 广播，P4 host 与 C6 slave 的 SDIO / VHCI 链路打通，NimBLE 能通过 C6 BLE controller 工作，Pilot Kit 移动端可以扫描并连接。

## 解决的问题

原始故障表现是：

```text
sdmmc_init_ocr: send_op_cond returned 0x107
```

最终发现有四层问题叠在一起。

## 1. Reset 极性反了

Waveshare ESP32-P4-WIFI6 上，P4 GPIO54 到 C6 EN 之间存在板级反相 / 电平转换。对 P4 软件来说，reset 是高有效：

```text
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

不是 active-low。

如果配置反了，ESP-Hosted driver 会在 SDIO 初始化期间把 C6 hold 在 reset 状态，CMD5 会一直返回 `0x107 INVALID_RESPONSE`。

## 2. 缺少 BT controller init / enable

SDIO transport 起来后，如果直接 `nimble_port_init()`，会看到 HCI timeout：

```text
NimBLE: HCI wait for ack returned 19
BLE_HS_ETIMEOUT_HCI
```

原因是 C6 slave 的 BT controller 不会在 boot 时自动启动。P4 host 必须先调用：

```c
esp_hosted_connect_to_slave();
esp_hosted_bt_controller_init();
esp_hosted_bt_controller_enable();
nimble_port_init();
```

顺序不能跳过。

## 3. ESP-Hosted 队列占用内部 RAM 太多

ESP-Hosted 的默认 SDIO queue 是 20×1536 B per direction，启动期会要求较大的 DMA-capable internal RAM 连续块。P4 工程同时启用 PSRAM、USB、LCD、BLE 后，早期 constructor 阶段容易因为内部 RAM 碎片化而失败。

本项目把 SDIO queue 缩到 8：

```text
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8
```

只使用 BLE 的场景下 8 足够，且明显降低启动期内存压力。

## 4. GDL90 emitter 里 `vTaskDelayUntil(..., 0)`

BLE 起来以后，`ble_emit` 任务第一次真正运行，暴露出一个潜伏 bug：`vTaskDelayUntil()` 的 `xTimeIncrement` 传了 0，FreeRTOS 会 assert。

修复方式是使用标准周期任务写法：

```c
TickType_t last_wake = xTaskGetTickCount();
const TickType_t period = pdMS_TO_TICKS(BLE_EMIT_PERIOD_MS);
while (1) {
    /* work */
    vTaskDelayUntil(&last_wake, period);
}
```

## 5. Advertising 31 字节溢出

BLE controller 起来后，`ble_gap_adv_set_fields` 曾返回 `BLE_HS_EINVAL`。原因是 flags、完整设备名和 128-bit service UUID 同时塞进 advertisement 超过 31 字节。

当前做法是：advertisement 放设备名 `Pilot Kit Box-XXXXXX`，scan response 放 128-bit service UUID。

## 6. 4.3″ 板回归：DSI PHY 抢 LDO 让 C6 起不来（2026-07-28）

迁到 4.3″ MIPI-DSI 屏之后这条链路再次断掉，症状与前面几条都不一样：

```text
I (11863) H_SDIO_DRV: Card init success, TRANSPORT_RX_ACTIVE
I (11865) transport: Waiting for esp_hosted slave to be ready
D (11916) H_SDIO_DRV: --- Wait for SDIO intr ---
I (24926) transport: Not able to connect with ESP-Hosted slave device
I (26442) H_SDIO_DRV: Host is resetting itself, to avoid any sdio race condition
```

SDIO **物理层全部正常**——CMD5、CIS、Function 1 就绪位 `IOR: 0x06`、
块大小 512、4-bit 总线协商 `BUS_WIDTH: 0x42` 一个不缺。卡住的是应用层：
主机永远等不到 C6 的 INIT event，13 秒超时后复位重试，重试撞上
`failed to read registers`，最终 hosted 自己把整机重启，26 秒一个循环。

### 定位

排除法，每一步都实测：

| 实验 | 结果 |
|---|---|
| esp_hosted 2.12.11 → 2.12.7（与 C6 镜像同版本） | 仍失败 |
| BLE init 提到 PFD 渲染任务之前 | 仍失败 |
| `SDIO_RESET_DELAY_MS` 1500 → 200 | 仍失败 |
| SDIO 时钟 40 → 20 MHz | 仍失败 |
| **跳过 PFD 渲染任务**（LVGL/PPA/GT911/温度全不跑） | 仍失败 |
| **跳过 `pk_display_init()`** | **72 ms 内握手成功** |

同时用 2.4″ 板做对照：同一套 hosted 配置（引脚、slot、复位极性、队列大小
逐行相同），`Open data path at slave` 之后 **32 ms** 就收到
`Received ESP_PRIV_IF type message` → `Identified slave [esp32c6]`。
那块板走 SPI 屏，根本不碰 DSI。

### 根因

DSI PHY 要独占 **LDO channel 3 并把它拉到 2.5 V**
（`display.h` 的 `PK_LCD_DSI_PHY_LDO_CHANNEL` / `_MV`）。这一下扰动足以让
刚被 GPIO54 复位、正在启动的 C6 起不来：它的 SDIO 外设仍能应答卡层命令
（那部分早就绪），但上层固件跑不到发 INIT event 那一步。

于是现象极具误导性——看起来「C6 在响应」，实际上响应的只是外设。排查时
一度怀疑 C6 没烧 slave 固件，其实固件好好的。

### 修复

`main.c` 里把 `ble_gatt_init()` 排到 `pk_display_init()` **之前**。先握手、
BLE 起来，再点屏。开机多花的那一秒落在 splash 显示窗口内，用户看不出差别。

改完实测：`Identified slave [esp32c6]` → `advertising as "Pilot Kit Box-XXXXXX"`
→ `ST7701 DSI ready` → `PFD render task running`，BLE 与显示同时正常，
连续运行不再重启。

**注意**：这个顺序是硬约束，不是风格偏好。以后往 app_main 里加初始化步骤时，
任何会动 LDO 或造成电源扰动的操作都得排在 hosted 握手之后。

## 当前正确配置

关键配置应包含：

```text
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
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8
```

## 正常日志

P4 主固件启动且 C6 已烧 hosted slave 后，应看到：

```text
transport: Identified slave [esp32c6]
vhci_drv: Host BT Support: Enabled | BT Transport Type: VHCI
ble_gatt: NimBLE host task running
ble_gatt: GDL90 emitter task running
ble_gatt: advertising as "Pilot Kit Box-XXXXXX"
```

## 排障顺序

1. 确认 C6 已按 [`c6_slave_firmware-zh_CN.md`](c6_slave_firmware-zh_CN.md) 烧入 hosted slave。
2. 确认 IO9 到 GND 的短接已经拔掉。
3. 确认 reset 极性是 `ACTIVE_HIGH`。
4. 确认 SDIO pin 与 Waveshare 板一致。
5. 确认 ESP-Hosted queue size 为 8。
6. 确认 `ble_gatt.c` 中有 connect -> controller init -> controller enable -> NimBLE init 的顺序。

## 已知边界

- 这份记录只覆盖当前 Waveshare ESP32-P4-WIFI6 板。
- 其他 ESP32-P4 开发板的 C6 / H2 / radio 连接方式可能不同。
- 如果未来 ESP-Hosted 升级导致主从 wire protocol 变化，需要同步更新 P4 component 和 C6 slave 固件。
- 当前 BLE v1.0 没有 pairing / bonding；在设计并记录 bonding / encryption 之前，不应通过该链路传输私有、控制类或座舱敏感数据。
