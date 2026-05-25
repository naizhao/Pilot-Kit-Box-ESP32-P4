# Pilot Kit Box — BLE 集成规范

英文版：[`ble_protocol.md`](ble_protocol.md)

| 项目 | 值 |
|---|---|
| 文档版本 | 1.0 |
| 状态 | 固件已实现，可供客户端集成 |
| 读者 | Pilot Kit Box 移动端 / 桌面端客户端开发者 |
| 固件参考 | `firmware/main/ble_gatt.c` |
| 相关文档 | [`architecture-zh_CN.md`](architecture-zh_CN.md)，[`hardware/c6_slave_firmware-zh_CN.md`](hardware/c6_slave_firmware-zh_CN.md) |

## 1. 概述

Pilot Kit Box 是基于 ESP32-P4 的 ADS-B 接收器。它通过 RTL-SDR 接收 1090 MHz Mode-S / ADS-B 广播，在固件内完成解码和飞机状态聚合，再通过 BLE 把交通态势广播给附近客户端。

协议设计刻意保持简单：

- 一个自定义 128-bit GATT service。
- 四个 characteristic：Traffic Report、Heartbeat、Raw ts-line、Time Sync。
- v1.0 当前不做 bonding、pairing 或加密；必须把它视为开放的近距离遥测链路。ADS-B 交通本身是公开广播，但不要把此 BLE 链路用于保密数据、飞机控制或安全关键命令。
- 交通帧使用 GDL90 标准格式。
- iOS 可通过系统 Current Time Service 自动校时；Android 和跨平台客户端可写自定义 Time Sync characteristic。

Pilot Kit 移动 App 是参考客户端，但任何 EFB、分析工具或测试程序都可以按本文接入。

## 2. Advertising

设备未连接时持续 advertising，断开后重新 advertising。

Advertisement:

| 字段 | 值 |
|---|---|
| Flags | LE General Discoverable + BR/EDR Not Supported |
| Complete Local Name | `Pilot Kit Box-AABBCC` |

Scan response:

| 字段 | 值 |
|---|---|
| Complete List of 128-bit Service UUIDs | `1090AD5B-0000-1000-8000-1090AD5B0000` |

`AABBCC` 是设备 BLE MAC 最后 3 字节的大写十六进制，跨重启稳定、跨设备不同。客户端应按名称前缀 `Pilot Kit Box-` 或 service UUID 过滤，不要匹配完整固定字符串。

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

每个 notification 是完整 GDL90 Traffic Report（msg ID `0x14`）：

```text
0x7E | 0x14 | <27-byte payload> | <CRC-LSB> | <CRC-MSB> | 0x7E
```

GDL90 byte stuffing 规则：

- `0x7D` -> `0x7D 0x5D`
- `0x7E` -> `0x7D 0x5E`

CRC 为 CCITT-16，多项式 `0x1021`，init `0x0000`，低字节先发。

发送节奏：客户端订阅后，每秒对最近 60 秒内仍新鲜的每架飞机发送一条 Traffic Report。

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

当前固件在校时前 `utc_ok = 0`，校时后 `utc_ok = 1`。当前没有 GPS / ownship 位置源，因此 `gps_valid = 0`。

### 6.3 Raw ts-line (`...0003`)

ASCII 诊断格式，每个 notification 是一行，没有换行和 NUL：

```text
1715432198765 *8D4CA1BD9908F19A5841E08E4DFC;
^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   ts_ms       AVR-format raw Mode-S hex
```

这与 LittleFS 文件和离线脚本使用的格式一致。典型密度取决于空域流量，可能为每秒数条到数十条。

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

- GDL90 Ownship Report（需要 GPS 或其他可信 ownship 位置源）
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
