# Pilot Kit Box — BLE 集成规范

英文版：[`ble_protocol.md`](ble_protocol.md)

| 项目 | 值 |
|---|---|
| 文档版本 | 1.1 |
| 状态 | 固件已实现，可供客户端集成 |
| 读者 | Pilot Kit Box 移动端 / 桌面端客户端开发者 |
| 固件参考 | `firmware/main/ble_gatt.c` |
| 相关文档 | [`architecture-zh_CN.md`](architecture-zh_CN.md)，[`hardware/c6_slave_firmware-zh_CN.md`](hardware/c6_slave_firmware-zh_CN.md) |

## 1. 概述

Pilot Kit Box 是基于 ESP32-P4 的 ADS-B 接收器。它通过 RTL-SDR 接收 1090 MHz Mode-S / ADS-B 广播，在固件内完成解码和飞机状态聚合，再通过 BLE 把交通态势广播给附近客户端。

协议设计刻意保持简单：

- 一个自定义 128-bit GATT service。
- 四个 characteristic：Traffic Report、Heartbeat、Raw ts-line、Time Sync。
- v1.1 当前不做 bonding、pairing 或加密；必须把它视为开放的近距离遥测链路。ADS-B 交通本身是公开广播，但不要把此 BLE 链路用于保密数据、飞机控制或安全关键命令。
- 交通帧使用 GDL90 标准格式。
- iOS 可通过系统 Current Time Service 自动校时；Android 和跨平台客户端可写自定义 Time Sync characteristic。

Pilot Kit 移动 App 是参考客户端，但任何 EFB、分析工具或测试程序都可以按本文接入。

## 2. Advertising

设备未连接时持续 advertising，断开后重新 advertising。

Advertisement:

| 字段 | 值 |
|---|---|
| Flags | LE General Discoverable + BR/EDR Not Supported |
| Complete Local Name | `Pilot Kit Box-AABBCC`（出厂默认）或 `<用户名>`（改过名，无后缀） |

Scan response:

| 字段 | 值 |
|---|---|
| Complete List of 128-bit Service UUIDs | `1090AD5B-0000-1000-8000-1090AD5B0000` |

`AABBCC` 是设备 BLE MAC 最后 3 字节的大写十六进制，跨重启稳定、跨设备不同。
它**只跟着出厂默认名走**，见下。

### 用户可改的设备名

固件 v0.9.4 起，机主可以在**设置页 →「设备名」**里改名（屏上的受限编辑器，
字符集 A–Z / 0–9 / `-` / `_`，最长 **26** 字符）。用户设了名字，广播出去的
**就是他输入的那一串，一个字符都不加**：

```
出厂默认       Pilot Kit Box-0B5A8A          20 字节
改成 N123AB    N123AB                         6 字节
最长情况       PILOT-KIT-BOX-HANGAR-01-AB    26 字节
```

**为什么默认名带 MAC 后缀、自定义名不带**：后缀解决的是「一个机库停着好几台
盒子，扫描列表里分不出哪台是自己的」，而这只在所有设备同名时才成立——出厂
默认名恰恰如此。用户自己取的名重不重名由他自己负责；BLE 连接认的是设备地址
不是名字，扫描结果里本来就带着 MAC，去掉后缀既不影响可连接性也不影响可区分性
（客户端本来也不许按名字识别设备，见下一节）。

26 字符的上限是按广播包算出来的：31 字节总长 − 3（Flags AD）− 2（Name AD 头）
= **26 字节**可用。自定义名不再拼后缀，于是上限直接顶到这个预算本身，
26 ≤ 26 **正好装满**——固件里有编译期断言钉着，多一个字符构建就红。

清空名字即逐字节恢复出厂默认（MAC 后缀跟着回来）。改名**立即生效**——固件会
把广播停掉重开，不需要重启设备。

### 过滤方式（对按名称前缀过滤的客户端是破坏性变更）

客户端**必须**按 **128-bit Service UUID**
`1090AD5B-0000-1000-8000-1090AD5B0000` 过滤扫描结果（它在 scan response 里）。

**不要按名称过滤。** 名称前缀 `Pilot Kit Box-` 曾经是一条可选路径，但改过名的
设备根本不带这个前缀——按前缀过滤的客户端会直接扫不到它。名称**只用于显示**。

## 3. GATT Service

```text
Service:                                  1090AD5B-0000-1000-8000-1090AD5B0000
  ├─ Traffic Report   (GDL90 / NOTIFY)    1090AD5B-0000-1000-8000-1090AD5B0001
  ├─ Heartbeat        (GDL90 / NOTIFY)    1090AD5B-0000-1000-8000-1090AD5B0002
  ├─ Raw ts-line      (ASCII / NOTIFY)    1090AD5B-0000-1000-8000-1090AD5B0003
  └─ Time Sync        (R/W binary)        1090AD5B-0000-1000-8000-1090AD5B0004
```

低 32 bit 表示资源：

| 后缀 | 资源 |
|---|---|
| `0000` | Primary service |
| `0001` | Traffic Report notify |
| `0002` | Heartbeat notify |
| `0003` | Raw ts-line notify |
| `0004` | Time Sync read/write |

未来版本可以在同一 service 下增加 `0005` 之后的可选 characteristic。客户端必须忽略未知 UUID。

## 4. 连接生命周期

```text
客户端扫描 -> 按名称前缀或 service UUID 过滤
客户端连接 -> GATT discover
固件尝试读取 iOS CTS；客户端也可以写 Time Sync
客户端订阅 Traffic / Heartbeat / Raw
固件按订阅状态发送 notify
断开 -> 固件重新 advertising
```

### ATT MTU

固件请求 preferred ATT MTU = 256，确保一条 GDL90 Traffic Report 可以放进单个 notification。客户端建议协商至少 64 字节 MTU；本协议版本不定义跨 notification 的 GDL90 重组，低于该值应视为不支持。

### 多客户端

当前固件一次支持一个 BLE peer。已有 peer 连接时，第二个客户端可能被 NimBLE controller 拒绝；客户端应优雅处理连接失败，并在短暂退避后重试。

## 5. 时间同步

固件没有 RTC，每次启动时从 Unix epoch 0 开始。ADS-B frame 带时间戳，因此连接后需要从客户端学习真实 UTC 时间。

### iOS Current Time Service

iOS 默认暴露 SIG Current Time Service（UUID `0x1805`）。固件在连接后会：

1. discover service `0x1805`
2. discover characteristic `0x2A2B`
3. read 10-byte Bluetooth Date Time
4. 解析 UTC 时间并调用 `settimeofday()`

iOS App 不需要额外操作。

### 自定义 Time Sync characteristic

UUID `...0004` 支持 WRITE 一个 8 字节 little-endian `uint64`，表示 UTC Unix epoch milliseconds。

| 字段 | 编码 | 说明 |
|---|---|---|
| `epoch_ms` | `uint64_le` | 自 1970-01-01T00:00:00Z 以来的毫秒数 |

READ 返回固件当前认为的 epoch milliseconds，可用于确认写入生效。

小于 `1704067200000`（2024-01-01 UTC）的值会被拒绝，ATT error 为 `0x13 Value Not Allowed`。

Dart 示例：

```dart
final bytes = ByteData(8);
bytes.setUint64(0, DateTime.now().toUtc().millisecondsSinceEpoch, Endian.little);
await timeSyncCharacteristic.write(bytes.buffer.asUint8List(), withoutResponse: false);
```

校时前产生的帧仍会发送，但 `ts_ms` 很小。客户端可以丢弃这些 pre-sync frame。

## 6. Characteristic Payloads

### 6.1 Traffic Report (`...0001`)

每个 notification 是完整 GDL90 Ownship Report（msg ID `0x0A`）或
Traffic Report（msg ID `0x14`）：

```text
0x7E | <0x0A or 0x14> | <27-byte payload> | <CRC-LSB> | <CRC-MSB> | 0x7E
```

GDL90 byte stuffing 规则：

- `0x7D` -> `0x7D 0x5D`
- `0x7E` -> `0x7D 0x5E`

CRC 为 CCITT-16，多项式 `0x1021`，init `0x0000`，低字节先发。

发送节奏：存在有效本机来源时，每秒先发送一条 Ownship Report；手动绑定
ADS-B 本机目标优先，GT-U8 GPS 作兜底。随后对最近 60 秒内仍新鲜的每架
飞机发送一条 Traffic Report。

关键字段：

| 字节 | 含义 |
|---|---|
| 0 | Alert Status + Address Type |
| 1..3 | 24-bit ICAO 地址，大端 |
| 4..6 | Latitude，signed 24-bit，`180 / 2^23` deg/LSB |
| 7..9 | Longitude，同上 |
| 10..11 | Altitude + misc indicators |
| 12 | NIC / NACp |
| 13..15 | Horizontal velocity + vertical velocity |
| 16 | Track / heading |
| 17 | Emitter category |
| 18..25 | 8 字节 callsign，空格补齐 |
| 26 | Emergency / priority code |

### 6.2 Heartbeat (`...0002`)

GDL90 Heartbeat（msg ID `0x00`），6 字节 payload，同样使用 GDL90 framing。连接且订阅后每秒发送一次。客户端可把超过 5 秒无 Heartbeat 视为链路失效。

`gps_valid` 跟随 GT-U8 RMC 定位状态，`uat_initialised = 1`。当前
`v0.8.0` 实现即使已经通过 GPS/BLE 校准系统时间，Heartbeat 的
`utc_ok` 位仍保持 0；客户端应使用时间戳值，并把该标志视为尚未实现。

### 6.3 Raw ts-line (`...0003`)

ASCII 诊断格式，每个 notification 是一行，没有换行和 NUL：

```text
1715432198765 *8D4CA1BD9908F19A5841E08E4DFC;
^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   ts_ms       AVR-format raw Mode-S hex
```

这与 LittleFS / MicroSD 文件和离线脚本使用的格式一致。典型密度取决于空域流量，可能为每秒数条到数十条。

### 6.4 Time Sync (`...0004`)

| 操作 | Payload |
|---|---|
| READ | `uint64_le` epoch_ms |
| WRITE | `uint64_le` epoch_ms，必须 >= 2024-01-01 UTC |

错误：

| Code | 含义 |
|---|---|
| `0x0D` Invalid Attribute Value Length | 写入长度不是 8 字节 |
| `0x13` Value Not Allowed | epoch 早于最小允许值 |

### 6.5 演示模式下客户端看到什么

固件 v0.9.5 起，盒子有一个用户可选的**演示模式**（设置 → 演示模式）。打开后，
屏上的一切读数——姿态、GPS、气压、ADS-B 目标——都来自固件内置的一套合成数据，
而不是真实传感器。它是给「没接外设的真机上演示与核对 UI」用的。

**线上行为是刻意收紧的：**

| 特征 | 演示模式开启时的行为 |
|---|---|
| Traffic Report (`…0001`) | **一条都不发。**没有 Ownship (`0x0A`)，没有 Traffic (`0x14`)。 |
| Heartbeat (`…0002`) | 照常 1 Hz 发送，但 **`gps_valid`（Status Byte 1 bit 7）强制为 0**。 |
| Raw ts-line (`…0003`) | 不受影响。合成目标从来没进过解码器，这条流里只会有真实 Mode-S 报文（没插 SDR 时就是空的）。 |
| Time Sync (`…0004`) | 不受影响。系统时钟绝不会由演示数据校准。 |

**为什么不「照发 + 打个标记」？** 因为 GDL90（FAA 560-1058-00 Rev A）的
Heartbeat / Ownship / Traffic 三种 payload 里**没有任何一位**表示「这是模拟
数据」，也没有任何一款 EFB 会去渲染这种标记。ForeFlight 上凭空多出来的一架
迎头飞机，与真实目标在像素级别完全一致——盒子屏上那枚红色 `DEMO` 徽标救不了
正在看手机的飞行员。既然接收机没有办法把数据标成假的，就不能把它送上线。

**客户端要做什么：** 什么都不用做。这个状态与「设备已连接、无 GPS 定位、无
目标」完全一致，任何 EFB 本来就要处理这一态。客户端**不得**尝试探测演示模式
——协议里没有对应字段，而自己发明启发式判据（比如「有心跳但一直没有目标」）
会把一台放在室内、没接天线的正常盒子也误判进去。

第一方 App 若确实需要展示这个状态，应在后续协议修订（§8）里新增一个可选特征，
而不是靠推断。

## 7. 集成示例

```dart
final svcUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0000');
final trafficUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0001');
final heartbeatUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0002');
final rawLineUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0003');
final timeSyncUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0004');

await device.connect();
await device.requestMtu(247);
final services = await device.discoverServices();
final svc = services.firstWhere((s) => s.uuid == svcUuid);

final timeChr = svc.characteristics.firstWhere((c) => c.uuid == timeSyncUuid);
final bytes = ByteData(8)
  ..setUint64(0, DateTime.now().toUtc().millisecondsSinceEpoch, Endian.little);
await timeChr.write(bytes.buffer.asUint8List(), withoutResponse: false);

final traffic = svc.characteristics.firstWhere((c) => c.uuid == trafficUuid);
await traffic.setNotifyValue(true);
traffic.lastValueStream.listen(onGdl90TrafficFrame);
```

## 8. 版本兼容

以下变化需要提升协议版本并更新本文：

- characteristic UUID 改变
- payload 格式不兼容变化
- 新增客户端必须支持的 characteristic

新增可选 characteristic 不需要破坏兼容性，客户端应忽略未知 UUID。

## 9. 后续工作

当前未包含但在路线图中的项目：

- Bluetooth Device Information Service (`0x180A`) 暴露固件版本
- 配置写 characteristic：从 App 调整 RTL-SDR frequency / gain / sample rate
- 在加入非公开、可识别用户、控制类或座舱敏感数据前，必须补 bonding + LESC encryption
- 多客户端同时连接

## 10. 参考

1. FAA, GDL 90 Data Interface Specification 560-1058-00 Rev A.
2. Bluetooth SIG, Current Time Service 1.1.0.
3. RTCA DO-260B.
4. SoftRF `gdl90.c`.
5. Stratux `gen_gdl90.go`.
