# P4 ↔ RP2040 ADS-B 链路协议（UART），v1.2

状态：v1.0 冻结（2026-09-05）；v1.1（2026-09-08）与 v1.2（2026-09-11）均为
向前兼容新增，见 §6 / §7 变更清单。修改协议必须递增 minor（向前兼容新增）或 major（不兼容），
并同步更新 `firmware/components/adsb_link_codec/` 与两侧实现及测试。

物理层：UART 8N1，波特率 **921600（台架候选值）**。P4 侧 UART2：RX=GPIO46（RP2040 TXD）、
TX=GPIO32（RP2040 RXD）。RP2040 侧 UART0：TX=GPIO0、RX=GPIO1。

## 1. 帧格式（小端多字节）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 2 | magic：`0x50 0x4B`（"PK"） |
| 2 | 1 | ver_major（本版 = 1） |
| 3 | 1 | ver_minor（本版 = 2；v1.1 为 1、v1.0 为 0。接收方只校验 major，§3.2） |
| 4 | 1 | msg_type |
| 5 | 1 | seq（每发送方独立 u8 回绕递增） |
| 6 | 2 | payload_len（LE，≤ 576；v1.0 为 464，v1.1 扩容见 §6） |
| 8 | N | payload |
| 8+N | 2 | crc16（LE，CRC-16/CCITT-FALSE：poly 0x1021、init 0xFFFF、无反转；
覆盖偏移 0..8+N-1） |

帧总长上限 = 8 + 576 + 2 = **586 字节**（v1.0 为 474）。
CRC 已知答案向量：`crc16("123456789") = 0x29B1`。

## 2. 消息类型（v1）

| 值 | 名称 | 方向 | payload |
|---|---|---|---|
| 0x01 | HELLO | 双向 | `{u8 proto_min_minor; char build[16]}`（build 以 \0 结尾） |
| 0x02 | CAPABILITIES | RP→P4 | `{u32le caps_bitmask}`（bit0=1090，bit1=978 预留） |
| 0x03 | HEALTH_STATS | RP→P4，1 Hz | 见 §4 |
| 0x10 | MODES_RAW | RP→P4 | `{u8 flags; u8 rssi; u32le rp_ts_us; u8 frame[7 或 14]}`；flags bit0=112-bit 帧；rssi 单位 0.5 dB、0xFF=无值；rp_ts_us 为模 2^32 单调 µs（帧 preamble 首沿；约 71.6 分钟回绕，见下方勘误）；RP2040 MVP 始终提供有效值，0 是合法回绕值 |
| 0x11 | UAT_UPLINK | RP→P4 | v1.1 新增，固定 557 B，见 §6 |
| 0x20 | CONFIG_REQ | P4→RP | `{u8 n; {u8 key; u8 val;} item[n]}`（v1.2，§7） |
| 0x21 | CONFIG_ACK | RP→P4 | `{u8 applied; u8 rejected;}`（v1.2，§7） |
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
3. payload_len > 576（v1.0 上限 464；v1.1 扩容见 §6）：作废、前进 1 字节、
   计 len_errors（长度攻击防御）。
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

## 6. v1.1 变更（2026-09-08）：UAT_UPLINK

v1.1 只做向前兼容新增，不改变任何 v1.0 消息的字节行为：

1. **payload 上限 464 → 576**（§1/§3.3）：新消息 0x11 的 payload 为 557 B。
   576 = 552 B UAT 帧 + 24 B 余量（覆盖元数据扩展与未来 978 下行 48 B 帧族），
   帧总长上限随之 474 → 586。v1.0 接收方对 557 B 帧按 §3.3 计 len_errors
   作废，滑窗重同步语义不变（§3.7），后续帧仍可解——降级安全。
2. **ver_minor 0 → 1**（§1）：仅编码侧自述；接收方校验规则不变（§3.2 只看
   major，minor 前向兼容）。
3. **新增消息类型 0x11 UAT_UPLINK**（RP→P4）：类型号取 0x1x 原始 RF 帧
   族的下一空位（0x10 = MODES_RAW）。

### 6.1 UAT_UPLINK payload（固定 557 B，小端多字节）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 1 | rssi：0.5 dB/LSB、无符号，0xFF=无值——与 SPI RX_DESCRIPTOR 同族单位（PROTOCOL_RP2040_CC1312R_SPI.md §4.3），不发明新单位 |
| 1 | 4 | rp_ts_us：模 2^32 单调 µs，**时钟域 = CC1312R 描述符时钟**（SPI §4.3 `ts_us`，RP2040 原样转发；字名沿用 v1.1 载荷族惯例）。差值按 `(u32)(now − prev)` 无符号解释，0 是合法回绕值。**与 §2 MODES_RAW 的 rp_ts_us（RP2040 preamble 沿时钟）不同源——跨链时效比较不得混用** |
| 5 | 552 | frame：978 UAT 上行解调帧**原样**（交织、含各块 20 B RS 校验字） |

设计依据（字段裁决记录）：

- **帧原样透传，RS/解交织/消息层解码在 P4**——WP-E Task 1 事实卡
  （docs/internal/firmware-v3v4/UAT-PROTOCOL-FACTS.md）§7 的边界裁决：
  CC1312R 交付 552 B 含校验字的交织帧，RS 纠错统计跨链路保留。
- **无长度字段**——UAT 上行只有一种帧长 552 B（事实卡 §1；长/短帧是下行
  概念），payload 总长恒 557 B。接收方校验 `payload_len == 557`，不符即
  整帧丢弃（P4 侧 uat_ingest 不收半帧）。
- **无 flags 字节**——MODES_RAW 需要 flags bit0 区分 56/112-bit 帧长，
  UAT 上行单一帧长，无此需求（最小元数据）。
- **rssi/ts_us 元数据复用 SPI 描述符的语义**——RP2040 转发（T3）时直接
  取自 CC1312R 链路 RX_DESCRIPTOR 的同名字段（SPI §4.3），UART 段不改写
  单位与回绕语义，P4 侧可与 1090 路径共用同一套解释代码。

完整性：UART 帧级 crc16（§1）即传输校验；UAT payload 本身**无 CRC**，业务
完整性由 RS(92,72)×6 承担（事实卡 §6 裁决，三方上游一致）。

### 6.2 golden vector（V1-UPLINK-CLEAN）

帧 = UP-CLEAN 无错上行帧（事实卡 §8：真实捕获 + 独立 Python 编码器重构，
test_uat_decode.c 同一 552 B），元数据 rssi=0x37、rp_ts_us=0x11223344：

```
50 4B 01 02 11 00 2D 02          ; magic ver=1.2 type=0x11 seq=0 plen=0x022D
37 44 33 22 11                   ; rssi=0x37, rp_ts_us=0x11223344 (LE)
35 F0 FC 07 30 00 … C1 F7 00 00  ; 552 B 交织帧（UP-CLEAN 全文见
                                  ;   test_uat_decode.c VEC_UP_CLEAN_552）
A4 21                            ; crc16 = 0x21A4（LE）
```

帧总长 567 B。CRC 由独立 Python 实现（CCITT-FALSE，KAT "123456789"→
0x29B1 对拍）计算。**注意 ver_minor 在帧头里、也进 CRC**：每次递增 minor
这个向量的末两字节都会变（v1.1 时是 `AD 5F`），这属于预期，不是回归；
codec 测试再以第三实现钉死（test_adsb_link_codec.c
case 19-21 / test_uat_ingest.c 向量节）。

---

## 7. v1.2 新增：CONFIG_REQ / CONFIG_ACK（2026-09-11）

v1.0/v1.1 把 0x20/0x21 列为"预留不实现"，RP2040 收到一律回 ERROR code=1。
v1.2 给出定义，用于把**天线选择**这类持久配置从 P4 推到 RP2040。

### 7.1 payload

```
CONFIG_REQ (P4→RP):  u8 n;  然后 n 组 { u8 key; u8 val; }        长度 = 1 + 2n
CONFIG_ACK (RP→P4):  u8 applied;  u8 rejected;                    长度 = 2
```

`n ≤ 16`（`ADSB_LINK_CFG_MAX_ITEMS`）。**声明条数与 payload 长度必须严格
相符**，多一字节少一字节都整包拒收并回 ERROR code=2——这不是"宽容"问题，
照单全收等于把来路不明的字节应用到 RF 通路上。

| key | 含义 | val |
|---|---|---|
| 0x01 | 1090 天线 | 0 = 板载 IFA（默认）/ 1 = 外接 J6 |
| 0x02 | GNSS 天线 | 0 = 外接 J2（默认）/ 1 = 板载 patch J8 |

未知 key 计入 `rejected` 但不影响其它项——与 §3.5「未知 msg_type 必须递交」
同一条前向兼容原则：对面比我们新的时候，该忽略的是不认识的那一项，不是整包。

### 7.2 状态归属与重推时机

**NVS 里 P4 那一份是真源**，RP2040 上的是它的投影。RP2040 自己不持久化，
上电一律回到 `rf_safety` 的上电安全向量。

key 的取值刻意让 **val=0 等于上电默认**。这样"配置还没送到"和"用户选的就是
默认值"落在同一个 RF 状态上，不会出现半套配置——RF 通路上一半新一半旧最难
查：屏上一切正常，实际接的是另一根天线。

P4 在两个时机下发：用户在设置页改值；以及**每次收到 RP2040 的 HELLO**。
挂在 HELLO 上而不是 linked 边沿，是因为 RP 侧未 linked 时就是 1 Hz 发
HELLO，边沿判定漏一次，用户的选择要等到下次开机才生效。

### 7.3 与 CDC 调试键的关系

RP2040 的 CDC `A/a/N/n` 仍可切天线，但那是**易失**的台架入口：P4 下一次
下发就会覆盖回设置页里存的值。串口提示里已标注。


## 7. v1.2 / v1.3 扩展（向前兼容，接收方只看 major）

### 7.1 CONFIG_REQ / CONFIG_ACK（v1.2）

v1.0/v1.1 里这两个 type 是「预留不实现」，RP 侧收到一律回 ERROR code=1。v1.2 起：

```
CONFIG_REQ (P4→RP): { u8 n; struct { u8 key; u8 val; } item[n] }     n ≤ 16
CONFIG_ACK (RP→P4): { u8 applied; u8 rejected }
```

| key | 含义 | 值 |
|---|---|---|
| 0x01 | ANT_1090 | 0 = 板载 IFA（上电默认）/ 1 = 外置 |
| 0x02 | ANT_GNSS | 0 = 外置（上电默认）/ 1 = 板载 patch |

键值编号刻意让**全 0 的 payload 等于上电默认**（与 rf_safety.c 的开机安全向量
一致）：这样「配置没送到」和「配置就是默认值」落到同一个 RF 状态，不会出现
一半新一半旧的天线组合——RF 通路上半套配置最难查，屏上一切正常而实际接的是
另一根天线。

未知 key 计入 `rejected` 但不影响其它项（与 §3.5「未知 msg_type 必须递交」
同一条前向兼容原则：对面比我们新时，该忽略的是不认识的那一项，不是整包）。

声明条数与 payload 长度必须严格相符，多一字节即拒收并计 ERROR code=2——
这不是洁癖：照单全收等于把来路不明的字节当配置应用到 RF 通路上。

### 7.2 HEALTH_STATS 扩展到 11 个字段（v1.3）

payload 由 10×u32（40 B）扩到 **11×u32（44 B）**，新增字段 10：

| 字段 | 含义 |
|---|---|
| 10 | `USB_VBUS_SENSE` **分压中点**电压（mV）——**不是 VBUS 本身** |

分压网物理在 RP2040 的 ADC2（U8 pad 40，PCB 焊盘已核）。RP2040 是两块板共用的
**单一构建、没有板型检测**，而分压比 V4=30k/10k(4.0)、V3=10k/10k(2.0) 差整整
两倍，所以本侧只报中点电压，换算由 P4 用 `pk_board_profile()` 完成
（`firmware/main/vbus_sense.c`）。

⚠ 该读数**未经标定**：ADC 输入阻抗对分压网有负载，实际中点比理想值低约 7%。
判 5V/9V 档位应直接在中点电压上比门限，不要拿换算后的电压去比 7 V——那会把
偏差带进判据。详见 `vbus_sense.h`。

**前向兼容**：接收方必须用 `payload_len >= 44` 判断该字段是否存在，v1.0/v1.1 的
RP2040 只发 40 字节。用 `>= 40` 继续解析前 10 个字段的旧接收方不受影响。
