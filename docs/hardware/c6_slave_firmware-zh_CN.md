# 给 C6 模组烧录 ESP-Hosted slave 固件

英文版：[`c6_slave_firmware.md`](c6_slave_firmware.md)

## 这是什么

每块新出厂的 Waveshare ESP32-P4-WIFI6，如果要启用 Bluetooth / BLE，都需要做一次 C6 固件烧录。P4 固件通过 ESP-Hosted 协议和板载 ESP32-C6 通信；但出厂 C6 跑的是 factory AT 固件，不能直接配合本项目。

烧完 hosted slave 后，C6 固件会持久保留。后续反复烧 P4 主固件不会影响 C6。

不想现在启用 BLE，可以先在 P4 固件里关闭：

```bash
cd firmware
idf.py menuconfig
# Pilot Kit Box -> 取消 Initialise BLE GATT server at boot
```

## 需要的东西

- 3.3 V TTL USB-UART 转接器，CP2102 / CH340 / FT232 等都可以。
- 3 根杜邦线：GND / RXD / TXD。
- 1 根短接线或回形针：让 C6 IO9 在冷启动时接 GND，进入 download mode。
- P4 板子的 Type-C 供电线。

## H4 接线

H4 是板背面的 C6 debug header：

| H4 pin | 板上标注 | 接到 |
|---|---|---|
| 1 | C6_IO9 | 烧录期间短接到板上任意 GND |
| 2 | GND | USB-UART GND |
| 3 | C6_RXD | USB-UART TX |
| 4 | C6_TXD | USB-UART RX |

IO9 不接 USB-UART 信号，只在烧录期间短到 GND。

## 准备固件

推荐使用 ESPHome 预编译好的 ESP-Hosted slave binary：

```bash
curl -L -o /tmp/network_adapter_esp32c6.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.12.7/network_adapter_esp32c6.bin
```

如果你要自己改 slave 配置，也可以从 `firmware/managed_components/espressif__esp_hosted/slave` 复制工程到仓库外编译，避免和 P4 工程分区表冲突。

## 进入 C6 download mode

顺序很重要：

1. 拔掉 P4 Type-C，整板断电。
2. 接 USB-UART：GND -> H4-2，TX -> H4-3，RX -> H4-4。
3. 用短接线把 H4-1（C6_IO9）接到板上 GND。
4. 先把 USB-UART 插到电脑，确认出现 `/dev/cu.usbserial-*` 或对应串口。
5. 按住 P4 板正面的 BOOT 按钮。
6. 保持 BOOT 按住，插上 P4 Type-C 给板子上电。
7. 松开 BOOT。

这时：

- P4 因 BOOT 低电平进入 download mode，不会运行固件去 reset C6。
- C6 因 IO9 低电平进入 ROM download mode。

## 烧录

H4 没有 C6 RESET 线，因此 esptool 要用 `--before no-reset`：

```bash
esptool --chip esp32c6 -p /dev/cu.usbserial-XXXX -b 460800 \
    --before no-reset --after hard-reset write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x10000 /tmp/network_adapter_esp32c6.bin
```

看到以下输出表示写入成功：

```text
Hash of data verified.
```

## 烧完后收线

1. 拔掉 P4 Type-C。
2. 移除 IO9 到 GND 的短接。忘记移除会导致 C6 下次仍进 download mode。
3. 可以拆掉 USB-UART 杜邦线。
4. 重新插上 P4 Type-C。

如果 P4 固件中的 BLE 已启用，正常日志应包含：

```text
transport: Identified slave [esp32c6]
ble_gatt: advertising as "Pilot Kit Box-XXXXXX"
```

## 常见问题

### esptool 连接不上

- 确认 C6 IO9 在上电瞬间已经短接到 GND。
- 确认 RX/TX 交叉：USB-UART TX 接 C6_RXD，USB-UART RX 接 C6_TXD。
- 确认 USB-UART 是 3.3 V TTL，不是 RS232 电平。
- H4 没有 reset 线，必须用 `--before no-reset`。

### P4 固件启动后 BLE abort

C6 还没烧 hosted slave，或者 IO9 短接没拔。先确认 C6 启动正常，再看 [`c6_bringup_status-zh_CN.md`](c6_bringup_status-zh_CN.md) 的 SDIO / reset 极性排障。

### CMD5 返回 0x107

Waveshare 板上 P4 GPIO54 到 C6 EN 的极性与 C6 silicon 原始 EN 语义相反。P4 配置必须是：

```text
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

如果被改成 active-low，P4 会把 C6 一直 hold 在 reset 附近，导致 SDIO 初始化失败。

## 何时需要重烧

通常只需要每块新板烧一次。只有以下情况需要重做：

- 你拿到另一块新板。
- 你手动把 C6 刷回 AT 固件或其他固件。
- ESP-Hosted 主从协议发生不兼容升级，且 P4 端也同步升级。
