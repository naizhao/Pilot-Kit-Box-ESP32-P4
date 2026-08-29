# 给 C6 模组烧录 ESP-Hosted slave 固件

英文版：[`c6_slave_firmware.md`](c6_slave_firmware.md)

## 这是什么

每块新出厂的 Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3，如果要启用
Bluetooth / BLE，都需要做一次 C6 固件烧录。P4 固件通过 ESP-Hosted
协议和板载 ESP32-C6 通信；但出厂 C6 跑的是 factory AT 固件，不能直接
配合本项目。

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
- 接 H1 `USB TO UART` 的 USB-C 数据/供电线。

## P1 接线

P1 是板背面的 C6 下载排针，丝印从 1 脚到 4 脚为
`TX / RX / IO9 / GND`：

| P1 pin | 板上标注 | 接到 |
|---|---|---|
| 1 | C6_TXD | USB-UART RX |
| 2 | C6_RXD | USB-UART TX |
| 3 | C6_IO9 | C6 上电时短接到板上任意 GND |
| 4 | GND | USB-UART GND |

IO9 不接 USB-UART 信号，只在烧录期间短到 GND。

C6 EN 经 R34 0 Ω 直接连接 P4 GPIO54，Rev1.2 原理图上没有反相器或电平
转换器。项目实机验证仍要求
`CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`；这是驱动配置事实，不能再
用不存在的板级反相器解释。

## 准备固件

推荐使用 ESPHome 预编译好的 ESP-Hosted slave binary：

```bash
curl -L -o firmware/network_adapter_esp32c6.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.12.7/network_adapter_esp32c6.bin
```

如果你要自己改 slave 配置，也可以从 `firmware/managed_components/espressif__esp_hosted/slave` 复制工程到仓库外编译，避免和 P4 工程分区表冲突。

## 推荐：使用项目脚本

项目内置了单板和批量烧录脚本：

```bash
firmware/tools/flash_c6_hosted.sh
```

脚本会先校验镜像必须是 ESP32-C6 的 `network_adapter` 2.12.7、4 MB /
DIO / 80 MHz，并验证镜像哈希。随后它每 1 秒自动探测一次
`/dev/cu.usbserial-*`；串口出现后只启动一次 esptool 连接，避免 macOS
USB-UART 被反复打开时出现 `termios.error: (22, 'Invalid argument')`。

常用模式：

```bash
# 只校验 ESP-IDF 环境和镜像，不接触硬件
firmware/tools/flash_c6_hosted.sh --check-only

# 指定串口烧录一块
firmware/tools/flash_c6_hosted.sh --port /dev/cu.usbserial-0001

# 连续烧录多块；每块完成后拔出 USB-UART，脚本会自动等待下一块
firmware/tools/flash_c6_hosted.sh --batch
```

整个等待过程不要求按 Enter。批量模式会每秒检测当前串口是否已拔出，确认
移除后再进入下一块板的探测循环。

## 进入 C6 download mode

顺序很重要：

1. 拔掉 H1 `USB TO UART`，整板断电。
2. 接 USB-UART：RX -> P1-1，TX -> P1-2，GND -> P1-4。
3. 用短接线把 P1-3（C6_IO9）接到板上 GND。
4. 先把 USB-UART 插到电脑，确认出现 `/dev/cu.usbserial-*` 或对应串口。
5. 按住 P4 板正面的 BOOT 按钮。
6. 保持 BOOT 按住，插上 H1 给板子上电。
7. 松开 BOOT。

这时：

- P4 因 BOOT 低电平进入 download mode，不会运行固件去 reset C6。
- C6 因 IO9 低电平进入 ROM download mode。

## 烧录

优先使用上面的项目脚本。下面是等价的手动命令，适合排障。P1 没有 C6
RESET 线，因此前后都不要让 esptool 自动复位：

```bash
python -m esptool --chip esp32c6 -p /dev/cu.usbserial-XXXX -b 115200 \
    --before no-reset --after no-reset --connect-attempts 0 write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x10000 firmware/network_adapter_esp32c6.bin
```

看到以下输出表示写入成功：

```text
Hash of data verified.
```

## 烧完后收线

1. 拔掉 H1。
2. 移除 IO9 到 GND 的短接。忘记移除会导致 C6 下次仍进 download mode。
3. 可以拆掉 USB-UART 杜邦线。
4. 重新插上 H1。

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
- P1 没有 reset 线，必须用 `--before no-reset`。

### 串口节点还在，但 USB-UART 已经卡死

实测遇到过这种情况：`/dev/cu.usbserial-*` 仍然存在，但打开端口时报
`termios.error: (22, 'Invalid argument')`，或者 esptool 一直无法同步。
反复按 P4 RESET、重新让 P4/C6 进入 download mode，甚至给 P4 板断电重启，
都不能恢复串口。

原因是 USB-UART 转接器由电脑的 USB 口独立供电。重启 P4 或 C6 只会复位
板上芯片，**不会给已经卡死的 USB-UART 桥断电**。这不是 Python、镜像或
ESP32-C6 本身的问题。

恢复步骤：

1. 关闭可能占用串口的 monitor、串口终端和旧 esptool 进程。
2. 把 **USB-UART 转接器的 USB 插头从电脑上拔掉**，等待 2–3 秒。
3. 重新插入 USB-UART，等待 `/dev/cu.usbserial-*` 重新出现。
4. 再运行 `firmware/tools/flash_c6_hosted.sh`；脚本会每 1 秒自动发现恢复后的端口。

如果不改接线、只拔插 USB-UART 就立刻恢复，基本可以确认故障点在 USB-UART
桥或主机串口驱动状态。项目脚本的批量模式也会等待串口节点消失后才进入下一
块板，正好保证每轮都能观察到一次完整的 USB-UART 重新枚举。

### 烧录中途随机断开（"The chip stopped responding"）

症状：`write-flash` 跑到一半突然报错，而且**每次断在不同进度**（例如一次停在 0%、另一次停在 21%）：

```text
Writing at 0x0006c190 [=====>     ]  21.0% 147456/702923 bytes...
A fatal error occurred: The chip stopped responding.
```

这不是固件文件损坏（日志里 `Compressed N bytes` 和文件大小对得上就说明上传已经开始），也**不一定是波特率太高**。先看断开规律判断方向：

- **每次断在不同百分比** → 接触不良 / 供电 / 自动复位电路干扰。
- **每次都断在同一百分比** → flash 擦写电流拉高时供电塌陷。

按下面顺序排查（成本从低到高）：

1. **接触与供电**：杜邦线全部重新插紧、USB-UART 直插电脑（不走 hub / 延长线）、拔掉其他大功率 USB 设备给烧录让电。随机断开最常见的就是这一类。
2. **降波特率**：`-b` 从 460800 往下试 `230400 → 115200 → 57600`。ROM 握手固定在 115200，`-b` 只影响 stub 加载后的传输速度，低于 115200 反而会拖慢整个传输。
3. **手动进 download mode + 全程禁用复位**（最可靠）：按上面《进入 C6 download mode》的步骤手动让 C6/P4 进下载模式，再把《烧录》那条命令里的 `--after hard-reset` 改成 `--after no-reset`（`-b` 按第 2 步降档）。P1 本来就没有 RESET 线，esptool 出错时会尝试用 RTS 复位（traceback 前那行 `Hard resetting via RTS pin…`），禁用 after-reset 把这个干扰去掉；写入完成后自己手动给板子断电重启即可。

### P4 固件启动后 BLE abort

C6 还没烧 hosted slave，或者 IO9 短接没拔。先确认 C6 启动正常，再核对 SDIO 引脚和复位极性——本板要求 `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`（sdkconfig.defaults 已设好，不要改）。

### CMD5 返回 0x107

P4 GPIO54 经 R34 0 Ω 直接连接 C6 EN。项目实机验证的 P4 配置必须是：

```text
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

如果被改成 active-low，P4 会把 C6 一直 hold 在 reset 附近，导致 SDIO 初始化失败。

## 何时需要重烧

通常只需要每块新板烧一次。只有以下情况需要重做：

- 你拿到另一块新板。
- 你手动把 C6 刷回 AT 固件或其他固件。
- ESP-Hosted 主从协议发生不兼容升级，且 P4 端也同步升级。
