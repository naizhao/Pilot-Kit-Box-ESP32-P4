# rp_cc13xx_codec — RP2040↔CC1312R Sub-GHz 链路共享编解码器

规范唯一权威：`firmware/PROTOCOL_RP2040_CC1312R_SPI.md`（v1.0，冻结）。
本组件是规范 §3/§4/§5 的可执行镜像；golden vectors 见规范附录 B，其可执行
形式在 `firmware/test/test_rp_cc13xx_codec.c`（向量 id ↔ 附录 B 条目一一对应）。

## 边界

- 只做帧编解码 + master 侧分片重组原语：纯 C99、无 malloc、无 FreeRTOS、
  不触碰 CSN/IRQ/RESET 时序（WP-E 数据链路的职责，Task 3 无调用点）。
- 事务模型（§2.1）：一次事务 = 定长 512 B 命令/事件对。本组件产出/解析
  帧本体（≤512 B）；帧尾补 0x00 与丢弃填充由 SPI 驱动完成。
- 全 0x00 事务是合法『无帧』（§2.3 规则 5）：`rp_cc13xx_decode_frame` 返回
  `RP_CC13XX_NO_FRAME`，不计错误。

## CRC

`rp_cc13xx_crc16` 直接转发 `adsb_link_crc16`（`adsb_link_codec.c:4-13`）——
规范 §3.2 指定的实现唯一权威，"一仓一族 CRC"，不复制、不另立参数。
KAT：`crc16("123456789") = 0x29B1`。

## 校验顺序（§5.1）

magic → len → CRC → ver → type 分发。返回值与 §5.3 计数器一一对应：
`ERR_MAGIC`→resyncs、`ERR_LEN`→len_errors、`ERR_CRC`→crc_errors、
`ERR_VERSION`→version_mismatch、`UNKNOWN_TYPE`→unknown_types（容忍忽略）。
作废粒度 = 整事务（§5.2），无滑窗、无逐字节前进（对照 P0a 的字节流模型，
CSN 边界天然分帧）。

## 保留位纪律

- `UPGRADE_STATUS.reserved[3]`、`QUEUE_FULL.reserved`、`RX_DESCRIPTOR.flags`
  保留位：接收方非 0 → payload 违规计 len_errors（§4.8 明文，§4.5/§4.3 同纪律）。
- `HELLO.reset_reason` 保留位：接收方**不检查**——§4.1 明文
  "接收方不解释未定义位"。

## 编译

- host 测试：`python3 firmware/test/run_host_tests.py`（自动发现）。
- RP2040：`firmware/rp2040/CMakeLists.txt` 直接列 `rp_cc13xx_codec.c`
  与 include 目录（与 adsb_link_codec.c 同款；当前无调用点，无害编译）。
- ESP-IDF：本目录经 `EXTRA_COMPONENT_DIRS` 自动注册，
  `REQUIRES adsb_link_codec`（CRC 同源）。
