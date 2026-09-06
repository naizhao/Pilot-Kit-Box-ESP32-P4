# P4 ↔ RP2040 ADS-B 链路协议（UART），v1.0

状态：冻结（2026-09-05）。修改协议必须递增 minor（向前兼容新增）或 major（不兼容），
并同步更新 `firmware/components/adsb_link_codec/` 与两侧实现及测试。

物理层：UART 8N1，波特率 **921600（台架候选值）**。P4 侧 UART2：RX=GPIO46（RP2040 TXD）、
TX=GPIO32（RP2040 RXD）。RP2040 侧 UART0：TX=GPIO0、RX=GPIO1。

## 1. 帧格式（小端多字节）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 2 | magic：`0x50 0x4B`（"PK"） |
| 2 | 1 | ver_major（本版 = 1） |
| 3 | 1 | ver_minor（本版 = 0） |
| 4 | 1 | msg_type |
| 5 | 1 | seq（每发送方独立 u8 回绕递增） |
| 6 | 2 | payload_len（LE，≤ 464） |
| 8 | N | payload |
| 8+N | 2 | crc16（LE，CRC-16/CCITT-FALSE：poly 0x1021、init 0xFFFF、无反转；
覆盖偏移 0..8+N-1） |

帧总长上限 = 8 + 464 + 2 = **474 字节**。
CRC 已知答案向量：`crc16("123456789") = 0x29B1`。

## 2. 消息类型（v1）

| 值 | 名称 | 方向 | payload |
|---|---|---|---|
| 0x01 | HELLO | 双向 | `{u8 proto_min_minor; char build[16]}`（build 以 \0 结尾） |
| 0x02 | CAPABILITIES | RP→P4 | `{u32le caps_bitmask}`（bit0=1090，bit1=978 预留） |
| 0x03 | HEALTH_STATS | RP→P4，1 Hz | 见 §4 |
| 0x10 | MODES_RAW | RP→P4 | `{u8 flags; u8 rssi; u32le rp_ts_us; u8 frame[7 或 14]}`；flags bit0=112-bit 帧；rssi 单位 0.5 dB、0xFF=无值；rp_ts_us 为模 2^32 单调 µs（帧 preamble 首沿；约 71.6 分钟回绕，见下方勘误）；RP2040 MVP 始终提供有效值，0 是合法回绕值 |
| 0x20 | CONFIG_REQ | P4→RP | 预留（v1 不实现） |
| 0x21 | CONFIG_ACK | RP→P4 | 预留（v1 不实现） |
| 0x7F | ERROR | RP→P4 | `{u8 code; u8 len; u8 msg[len]}` |

> 勘误（2026-09-05，re-audit P2）：`rp_ts_us` 为**模 2^32 单调** µs（约
> 71.6 分钟回绕）。消费者必须用无符号差值 `(u32)(ts_now − ts_prev)` 解释
> 时间差，不得把回绕当作回跳。RP2040 MVP 始终提供有效值（preamble 首沿
> 即有）；0 是合法回绕值，**不是**"无值"哨兵（二次勘误：废除原 0=无值
> 特例——它与模 2^32 单调语义冲突）。

## 3. 编解码与恢复行为（两侧行为一致，由共享 codec 保证）

1. 字节流按帧头扫描重同步：magic 不符则前进 1 字节。
2. ver_major ≠ 1：本帧作废、前进 1 字节、计 version_mismatch；**不进入工作态**
   （P4 侧 `pk_adsb_link_state` 停在 LINK_PROTO_MISMATCH）。
3. payload_len > 464：作废、前进 1 字节、计 len_errors（长度攻击防御）。
4. CRC 错：作废、前进 1 字节、计 crc_errors。
5. **未知 msg_type：CRC 正常校验后必须向上递交**，由应用决定忽略（minor 前向兼容）。
6. seq：接收方检测 `(u8)(seq_now - seq_prev) != 1` 计 seq_gaps；不丢帧、不要求重传。
7. 帧被作废后，流中**后续**合法帧必须仍可解出（滑窗前进 1 字节而非清空）。

> 勘误（2026-09-05，audit P2）：实际校验顺序为 **magic → len → CRC → version**；
> `version_mismatch` 仅统计 **CRC 合法**的异版本帧。坏 CRC 一律计 `crc_errors`
> （§3.2 原文按列表序理解会先查版本，以此勘误为准）。

## 4. HEALTH_STATS payload（u32le × N，均为开机累计）

| 序 | 字段 |
|---|---|
| 0..5 | preamble_hits, frames_56, frames_112, time_degraded, dropped_noise, edge_overruns |
| 6..9 | tx_frames, tx_drops（背压丢帧）, rx_frames, rx_seq_gaps |

> 字段 3 说明（2026-09-05）：`time_degraded` 为 sticky 丢沿退化标志——
> 1 = 自启动以来发生过丢沿/断点，断点后的 rp_ts_us 时间可信度降级
> （带轻微提前偏置，单调性不受影响），重启前不清除。原 `resyncs` 槽位
> 从未使用（恒 0），v1.0 起复用；P4 侧 v1.0 可忽略该位（diag 页呈现为
> 后续项）。

## 5. 链路建立与看护

- RP 上电 200 ms 后发 HELLO，之后每 1 s 重发，直到收到任何 P4 合法帧。
- 任一侧收到首个 major 匹配的合法帧即视为 LINKED。
- P4 侧 >5 s 无合法帧 → STALLED（诊断页可见，不重启整机）。
- RP2040 重启后 seq 从 0 重新开始：按 §3.6，`(u8)(seq_now - seq_prev) != 1` 照常计入 seq_gaps——seq 回跳同样被计数器统计。但 P4 状态机只看帧活性（>5 s 无合法帧才 STALLED），从不依据 gap 计数做状态迁移。
- 实现说明（2026-09-05）：RP 侧 HELLO 由 1 Hz tick 触发，首个 HELLO 在上电后 ≤1 s（规范原文 200 ms 为设计意图，v1.0 行为以此澄清为准）。
