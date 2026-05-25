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
