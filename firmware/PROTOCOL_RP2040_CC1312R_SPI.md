# RP2040 ↔ CC1312R Sub-GHz 链路协议（SPI），v1.0

状态：冻结（2026-09-08；定稿前修订见附录 C.3，不升版本）。修改协议必须递增
minor（向前兼容新增）或 major（不兼容），并同步更新
`firmware/components/rp_cc13xx_codec/`（WP-P0b Task 3 产出）与两侧实现及测试。

上位依据：`docs/internal/firmware-v3v4/PLAN.md` §5.2。硬件事实唯一来源：
`docs/hardware/pinmap_978.md`（下称「事实卡」）。姊妹规范：
`firmware/PROTOCOL_P4_RP2040_UART.md`（P0a，P4↔RP2040 UART）——本规范沿用其字段
家族、小端惯例与校验纪律，差异处逐条写明（§2.5、§3.1、§3.3、§3.4）。

## 1. 物理层与信号裁决

- RP2040 = SPI master（唯一时钟源、唯一发起者），CC1312R = SPI slave。四线落点
  唯一：**SPI1**，SCK=GPIO10 / MOSI=GPIO11 / MISO=GPIO12 / CSn=GPIO13 ↔
  DIO_10/8/9/11（事实卡「SPI instance verdict: SPI1, unique」——GPIO10–13 只承载
  SPI1 功能，无 SPI0 替代）。
- SPI 模式 0（CPOL=0、CPHA=0），字节内线序 MSB first；帧内多字节字段一律小端
  （LE）。RP2040 侧为 ARM PL022（RP2040.pdf §4.4），CC1312R 侧为 SSI，两者均
  原生支持模式 0。
- 时钟速率**不在本规范冻结范围**：事实卡确认 SCK/MOSI/MISO 上无串阻/缓冲/滤波，
  速率上限由 WP-E 台架实测决定（§9）。
- `SUBG_IRQ`（GPIO14 ↔ DIO_12）：**电平触发、高有效**，slave 持有驱动。高 =
  待交付事件未取毕（事件队列非空，或已交付的 RX_DESCRIPTOR 尚有未取分片）；
  队列空且分片取毕即拉低。事实卡 Unresolved #3（电平/极性未定）由本条裁决，
  WP-E 实现可复核。
- `SUBG_RESET`（GPIO18 ↔ RESET_N）：RP2040 驱动，低有效，复位脉冲 ≥ 1 ms。
  R47 10 kΩ 上拉、RESET_N 无内部上拉（事实卡「Digital interface nets」）。
- **`SUBG_SYNC`（GPIO15 ↔ DIO_13）：v1.0 裁决 = 保留闲置。** 两侧一律配置为
  输入、不驱动、不采样，协议不赋予任何语义。理由：事实卡证实该网两端均为纯
  GPIO、无任何固定硬件功能（事实卡「Semantics rulings」、Unresolved #2），且
  本阶段零台架数据；为其指派含义属于无证据发明。保留闲置不消耗线资源，未来
  增加含义不破坏 v1 互操作。WP-E 实现可复核：固件不得把 GPIO15/DIO_13 配置为
  输出。事实卡 Unresolved #2 由本条关闭。
- cJTAG（`SUBG_TCKC`/`SUBG_TMSC`，GPIO17/16 ↔ JTAG_TCKC/TMSC）与 SPI1 四线
  无引脚交叠（事实卡「cJTAG boundary」），对本协议零影响（§8）。

## 2. 事务模型（本规范裁决，无实现自由度）

1. **一次事务 = CSN 低电平期间的定长 512 B 交换；应答延迟一拍（deferred-
   response / pending 模型）。** master 在 MOSI 上发出命令帧，slave 在 MISO 上
   返回的是**上一事务命令的应答**：命令类型位于 MOSI 字节 3（magic 2 B + ver
   之后），而全双工线路上 MISO 字节 0 与 MOSI 字节 0 同时移出——slave 在解读
   命令之前 MISO 前几字节已经上线，"同一事务内请求→应答"不存在因果路径
   （§5.6 对 ERROR 的"本事务帧已定型"即此物理约束，对一切应答同样成立）。
   因此：**事务 N 的 MISO = 事务 N−1 的命令所确定的应答；事务 N 的命令决定
   事务 N+1 的 MISO。** 实施合同（一句话）：**slave 在 CSN 下降沿锁存上一事务
   确定的 MISO 缓冲，事务期间 MISO 内容不再变化**——任何实现只需在 CSN 拉低
   前预装 512 B DMA 缓冲。事务定长 512 字节：双方各移位恰好 512 字节；帧短于
   512 时发送方以 0x00 填充尾部，接收方必须丢弃帧尾之后的全部字节。两次事务
   之间 CSN 必须回高并保持 ≥ 1 µs（CSN 上升沿 = 事务边界）。
2. **pending 应答槽与发起规则**：slave 维护**一个** 512 B 应答槽（应答帧 +
   0x00 填充）。每个事务的 CSN 上升沿（事务完成）时刻，slave 依据**刚收到的
   命令**与内部状态装入下一事务的应答（装载优先级 §2.3）——槽内容在下一
   CSN 下降沿之前完全确定。**槽空（无可应答、无事件）时装载全 0x00**：接收方
   把全 0x00 事务判为合法『无帧』（§2.3 规则 5、§5.2），故无需额外 idle 标记
   ——「无应答」与「填充」共用同一全零编码。**应答相关性（seq 回显）**：应答
   类帧的 seq = 其所应答命令帧的 seq 原样回显（§3.5），master 以此把 MISO
   应答与自己的历史命令对账。**IRQ 读路径**：SUBG_IRQ 高电平 = 「存在待交付
   事件」（§1）；master 的读触发 = **IRQ_ACK 事务**（不设专用 POLL——
   IRQ_ACK 即"请装载下一事件/分片"）：事务 N 的 IRQ_ACK 使 slave 在其 CSN
   上升沿按 §2.3 规则 1/3 装载，事件帧出现在事务 N+1 的 MISO；取空 =
   IRQ_ACK 流水（§7.2 drain），每事务推进一个事件/分片。master 仅在 (a) 有
   命令待发或 (b) SUBG_IRQ 为高时发起事务；slave 无时钟即无法应答，故 slave
   从不"主动说话"，只能经 IRQ 声明（§1）。
3. **slave pending 装载规则**（CSN 上升沿时刻按序判定，唯一结果；决定下一
   事务的 MISO）。两条裁决原则：**无后备队列者先行**——直接应答（含命令性
   ERROR）被挤掉即永久丢失；**IRQ 背书者可延后零丢失**——事件与异步 ERROR
   留在队列/挂起标志中，IRQ 保持有效：
   1. 上一事务交付的 RX_DESCRIPTOR 尚有未取分片，且本事务命令为 IRQ_ACK →
      装入下一片 RX_PAYLOAD_CHUNK（§4.4；取片是 IRQ_ACK 读的延续——命令非
      IRQ_ACK 时不适用，分片保持已装载不丢失）；
   2. 否则存在**顺延的命令性 ERROR**（code 0x01–0x03，§3.5/§4.9）待交付 →
      装入该 ERROR（无后备队列，不许被事件/异步 ERROR 覆盖）；
   3. 否则本事务命令有直接应答（HELLO→HELLO、PING→PONG、RF_CONFIG→
      RF_CONFIG_STATUS、RESET_STATUS_REQ→RESET_STATUS、UPGRADE_STATUS_REQ→
      UPGRADE_STATUS、命令性违规→ERROR 0x01–0x03）→ 装入应答（seq 回显本
      命令，§3.5）；
   4. 否则按序交付 IRQ 背书事件：`queue_full_pending`（§4.5 挂起标志；
      **drain 期间跳过本分支**——§4.5 清除时机）→ 装入 QUEUE_FULL；
      否则事件队列非空 → 装入队头事件（RX_DESCRIPTOR——QUEUE_FULL 不占
      队列，见 §4.5）；否则存在待交付的**异步 ERROR**（code 0x04/0x05，§3.5——
      视为可保留事件，独立挂起标志等待，随本规则交付、延后零丢失）→ 装入
      该 ERROR（seq=0x0000 哨兵）；
   5. 否则装入全 0x00（无帧）。**全 0x00 的事务是合法的『无帧』结果**：接收方
      校验 MISO 时，magic 不符且整缓冲为全 0x00 → 合法空事务，不计任何错误；
      magic 不符且缓冲含非 0 字节 → 才计 resyncs（§5.2）。
   **延迟上界（规范性）**：事件/QUEUE_FULL/异步 ERROR 在命令连续到达时被
   推迟，其上界由 master 调度合同（§6.7）确定性封顶——IRQ 一旦观察到高
   电平，master 至多完成当前在途应答后必须连发 IRQ_ACK 直至 IRQ 读低，故
   交付延迟 ≤ 1 个直接应答事务 + 取空流水，不存在无界饥饿；QUEUE_FULL 由此
   必然可达 master，背靠背合法命令不会造成 978 丢弃信号永久滞留。走查：
   ①队列非空时 RF_CONFIG → STATUS 胜出装入，事件延后、IRQ 保持高，下一事务
   IRQ_ACK 取走；②PING 同理（PONG 胜出）；③RF_CONFIG 期间异步溢出 ERROR
   （0x04）触发 → STATUS 胜出，0x04 挂起延后交付（旧序下它会覆盖 STATUS
   造成永久丢失——已关闭）；④背靠背命令——各应答即时装入，事件在 §6.7
   上界内必达。
4. **不是寄存器模型。** 本协议是消息协议：slave 不暴露任何寄存器地址空间，
   一切交互经帧内 msg_type 语义（PLAN §0.1/§5.2 约束：不得把 CC1312R 当作
   无固件寄存器外设）。
5. **SPI 下无需 escape/重同步**（对照 P0a §3 的字节流滑窗）：CSN 边界天然
   分帧，坏帧随所在事务整体作废（§5.2），下一事务从干净边界开始，不存在
   字节流错位累积——P0a 的"前进 1 字节"扫描机制被"按事务作废"替代。

## 3. 帧格式（小端多字节）

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 2 | magic：`0x50 0x4B`（"PK"，沿用 P0a 家族） |
| 2 | 1 | ver：协议 major 版本，本版 = 1 |
| 3 | 1 | msg_type（§4） |
| 4 | 2 | seq：u16le，语义按 §3.5——命令帧=发送方序号（每发送方独立 +1，模 2^16 回绕）；应答帧=回显被答命令的 seq；异步 ERROR（0x04/0x05）=0x0000 哨兵 |
| 6 | 2 | payload_len：u16le，≤ 502 |
| 8 | N | payload |
| 8+N | 2 | crc16：u16le（§3.2） |

- **3.1 帧总长上限** = 8 + 502 + 2 = **512 字节 = 事务长度**；payload 上限 502。
  len > 502 → 整事务作废（§5.2，长度攻击防御）。填充字节不参与 seq/CRC。
- **3.2 CRC16 参数（与 P0a 共仓同族，一字不差）**：CRC-16/CCITT-FALSE——
  poly `0x1021`、init `0xFFFF`、输入/输出均不反转（MSB first）、无增强
  （无剩余异或，xorout = `0x0000`）；覆盖偏移 0..8+N−1（含帧头与 payload，
  不含 CRC 字段自身）；存储小端。实现唯一权威：
  `firmware/components/adsb_link_codec/adsb_link_codec.c:4-13`（函数
  `adsb_link_crc16`，声明 `adsb_link.h:33`）。已知答案向量：
  `crc16("123456789") = 0x29B1`。理由：一仓一族 CRC（P0a 先例），本链路与
  UART 链路无线上互通需求，无另立参数的理由。
- **3.3 版本模型差异**（对照 P0a 的 ver_major+ver_minor 2 B）：本协议 ver 仅 1 B
  major。minor 向前兼容**只通过新增 msg_type** 承载，故"未知 msg_type 必须
  容忍"（§5.4）即 minor 兼容规则；升 minor 不得改动既有类型语义。
- **3.4 seq 回绕**：接收方以 `(u16)(seq_now − seq_prev)` 做无符号差值判定（§5.5）；
  `0xFFFF → 0x0000` 为正常回绕，不是错误。
- **3.5 seq 回显（延迟应答模型，§2.2）**：应答类帧（HELLO 应答、PONG、
  RF_CONFIG_STATUS、RESET_STATUS、UPGRADE_STATUS、命令性 ERROR）的 seq =
  其所应答命令帧的 seq 原样回显；事件类帧（RX_DESCRIPTOR、QUEUE_FULL）的
  seq = slave 自有计数器，每发一帧 +1（模 2^16）。接收方 seq_gaps 判定
  （§5.5）仅对事件类帧按到达顺序执行，应答类帧不参与 gap 判定（其 seq 由
  命令决定，回显重复/乱序均合法）。**ERROR 的 seq 按 code 分两类**：
  0x01–0x03（版本/状态/RF_CONFIG，回应具体命令）回显命令 seq；0x04（溢出
  通报）/0x05（内部错误）为**异步事件**、无对应命令——seq 恒为 `0x0000`
  哨兵（『无命令』），接收方不得对其做 seq 对账或 gap 判定（§4.9）。

## 4. 消息类型（v1）

| 值 | 名称 | 方向 | payload | 频率约束 |
|---|---|---|---|---|
| 0x01 | HELLO | 双向 | §4.1 | 未 LINKED 时 master 每 500 ms 重发 |
| 0x02 | IRQ_ACK | master→slave | 空（len=0） | 每取一事件/一片一发 |
| 0x03 | PING | master→slave | 空（len=0） | LINKED 后 1 Hz 看护（drain 挂起）；WAIT_HELLO 对 PING 静默（§6.2） |
| 0x04 | PONG | slave→master | 空（len=0） | 仅 **LINKED 态**应答 PING（§6.2） |
| 0x10 | RX_DESCRIPTOR | slave→master | §4.3 | 事件到达即入队 |
| 0x11 | RX_PAYLOAD_CHUNK | slave→master | §4.4 | 仅紧随其 RX_DESCRIPTOR |
| 0x12 | QUEUE_FULL | slave→master | §4.5 | 队满置位、交付即清（drain 期间不装载，§4.5） |
| 0x20 | RF_CONFIG | master→slave | §4.6 | （re)LINKED 后 1 s 内先查询，按需写 |
| 0x21 | RF_CONFIG_STATUS | slave→master | §4.6 | 仅应答 0x20 |
| 0x22 | RESET_STATUS_REQ | master→slave | 空（len=0） | 按需；drain 期间禁止 |
| 0x23 | RESET_STATUS | slave→master | §4.7 | 仅应答 0x22 |
| 0x24 | UPGRADE_STATUS_REQ | master→slave | 空（len=0） | 按需；drain 期间禁止 |
| 0x25 | UPGRADE_STATUS | slave→master | §4.8 | 仅应答 0x24 |
| 0x7F | ERROR | 双向 | §4.9 | 事件驱动，无周期 |

`0x00` 禁用（全 0 填充不得被解析为帧）；`0xFF` 永不分配（全 `0xFF` 擦除/悬空
态检测哨兵）；其余值保留。**接收方裁决**：携带合法 magic+CRC 的 `0x00`/`0xFF`
帧属伪造/悬空态，按 §5.4 未知类型容忍处理——计 unknown_types、消息原样递交
诊断、不发 ERROR、不改变状态（v1 裁决，codec 未知类型分支即按此实现）。
**seq 语义**：应答类帧回显命令 seq、事件类帧走 slave 计数器（§3.5）。以下
payload 布局的每个字段偏移/单位均为 codec 断言的直接引用对象（附录 B 契约）。

### 4.1 HELLO（握手）

`{u16le fw_ver_major; u16le fw_ver_minor; u32le reset_reason}`，共 8 B。
协议版本在帧头 ver；本消息携带**固件**版本。`reset_reason` 位图（置 1 有效，
保留位必须为 0）：bit0=上电复位（POR）、bit1=RESET_N 引脚复位、bit2=软件复位、
bit3=看门狗/时钟安全复位。双方各自填充自己的复位原因；接收方不解释未定义位。
**发送方掩码**：reset_reason 源可能带厂商/平台位，发送方在编码边界把它掩码
到已定义低 4 位（`& 0x0F`）再上线——掩码即线上合同；接收方保持容忍（§5.4
精神：不因未定义位拒收），线外寄存器值无须预处理。

### 4.2 IRQ_ACK

len=0。语义：**读触发**（§2.2 IRQ 读路径）——事务 N 的 IRQ_ACK 命令使 slave
在其 CSN 上升沿按 §2.3 装载下一个事件/下一分片，装载结果出现在**事务 N+1 的
MISO**。若上一事务交付的是 RX_DESCRIPTOR，则装载其下一分片。IRQ 线在最后
一片被**实际交付**（而非仅装载）后拉低（§1）。seq = 所应答 IRQ_ACK 的 seq
回显（§3.5）。

### 4.3 RX_DESCRIPTOR

`{u16le desc_id; u32le freq_hz; u32le ts_us; u16le total_len; u8 rssi;
u8 flags}`，共 14 B。`desc_id`：slave 侧单调递增（模 2^16），关联其分片；
`freq_hz`：接收频率，单位 Hz；`ts_us`：模 2^32 单调 µs（帧 preamble 首沿；
消费者用无符号差值 `(u32)(t_now − t_prev)`，回绕不是回跳——同 P0a §2 勘误
精神）；`total_len`：报文总长，≤ 4096（超限报文被 slave 截断到 4096 并置
flags bit1，4096 ≥ 10× UAT 上行帧 432 B，裕量充足且 master 缓冲可静态分配）；
**该上限在发送与接收两侧均强制**（超限 → len 违规，§5.2）——截断只由 slave
在入队前执行，线上不存在 total_len > 4096 的合法描述符；
`rssi`：0.5 dB/LSB、无符号，0xFF=无值（P0a MODES_RAW 同族单位）；`flags`：
bit0=高优先级（建议 master 尽快取走）、bit1=截断，其余保留必须为 0（接收方
校验非 0 视为 payload 违规，与 len 违规同计 len_errors 并作废整事务，§5.2；
同 §4.8）。

### 4.4 RX_PAYLOAD_CHUNK

`{u16le desc_id; u16le offset; u16le total_len; u8 data[≤496]}`。
`data` 长度 = payload_len − 6，∈ **[1, 496]**（上限 = 502 − 6，§3.1；下限：
**空分片非法**——不推进重组的合法片会令取毕流水永远停滞，而取毕信号是
集齐 total_len 本身，不需要空片；编码/解码/重组三处均拒绝，len 违规 §5.2）；
`offset` 为该片在报文内的字节偏移；除最后一片外 offset 必须按 data 长度递进，
`total_len` 必须与对应 RX_DESCRIPTOR 一致。master 按 offset 升序重组；帧级
CRC16（§3.2）即传输完整性校验，不另设报文级 CRC（slave 在入队前已完成 RF
层校验，属 SimpleLink SDK 配置域，§9）。

### 4.5 QUEUE_FULL

`{u16le events_dropped; u8 queue_depth; u8 reserved}`，共 4 B。**入队机制
（out-of-queue 挂起标志，队列满时不存在自由槽位）**：事件到达而队列已满时，
队列本身不动（队头交付不受影响），slave 置单个 `queue_full_pending` 标志
（幂等，重复触发不叠加）；其交付优先级在直接应答与普通事件之间（§2.3 规则
4 首位——流控信号必须尽快到达 master，否则 drain 永不启动、978 丢弃无信号）；
交付 QUEUE_FULL **不清空队列**。**清除时机**：装载交付即清（一次交付 =
一次通报——QUEUE_FULL 是通知，不是队列成员，不存在"入队后待清"的残留）；
**drain 期间该标志完全不装载**（§2.3 规则 4 跳过）：drain 意味着 master 正
在取空队列，期间队列重新填满只使 IRQ 保持高、取空流水延长，无需重复通报
——事件事务不计入 §7.2 停滞判定，故停滞出口在任意重填场景下仍然可达；
RESET 清除一切挂起态。payload：`events_dropped` 为开机累计丢弃数（模 2^16 单调），
`queue_depth` 为当前队列占用（观测值，深度本身是 slave 资源参数、不经协议
冻结），`reserved` 必须为 0（接收方校验非 0 视为 payload 违规，与 len 违规
同计 len_errors 并作废整事务，§5.2；同 §4.8）。master 行为见 §7.2。

### 4.6 RF_CONFIG / RF_CONFIG_STATUS（同一 payload 布局）

`{u32le config_version; u8 digest[16]}`，共 20 B。`config_version`：RF 配置
版本号，**单调递增**，0 = 无配置；`digest`：配置映像 SHA-256 截取前 16 字节，
仅用于两侧对账，协议不解释其内容。**978 PHY/灵敏度/同步参数本身不在协议内**
（PLAN §5.2 原文：SimpleLink RF Driver + SysConfig 配置域）。规则：
- master→slave（0x20）：`config_version > slave 当前值` → slave 原子接受并
  存储（RAM 常驻，断电即失，master 是唯一事实源）；否则拒绝并装入 ERROR{0x03}
  （顺延到下一事务交付，§5.6）。
- `config_version = 0` 的 0x20 为**只读查询**，从不改变 slave 状态，0x21 回读。
- slave 复位后 config_version 恒为 0，master 须在每次（re)LINKED 后重发
  （§6.4）。

### 4.7 RESET_STATUS

`{u32le reset_reason; u32le uptime_ms}`，共 8 B。`reset_reason` 位图同 §4.1；
`uptime_ms` 为 slave 开机累计毫秒（u32le，模 2^32）。与 HELLO 中复位原因的
关系：HELLO 只在握手时携带，本查询允许 master 任意时刻复核。

### 4.8 UPGRADE_STATUS

`{u8 state; u8 reserved[3]; u32le running_image_version}`，共 8 B。`state`：
0x00=normal、0x01=bootloader、0x02=upgrading（WP-E 二期实现）、0x03=failed；
`reserved[3]` 必须为 0（发送方置 0；接收方校验非 0 视为 payload 违规，与
len 违规同计 len_errors 并作废整事务，§5.2）。
`running_image_version`：当前运行固件版本（与 §4.1 fw_ver 同编码）。
**cJTAG 状态不在本消息内**：cJTAG 是独立硬件调试路径，CC1312R 固件无法观测
（事实卡「cJTAG boundary」）——本消息只描述固件侧升级状态机（§8）。

### 4.9 ERROR

`{u8 code; u8 len; u8 msg[len]}`，len ≤ 32，msg 为 UTF-8 诊断串（非 NUL 结尾
也合法，以 len 为准）。code：0x01=版本不符（拒收异版本帧后的主动申报）、
0x02=命令与接收方状态机不符、0x03=RF_CONFIG 写被拒、0x04=slave RX 溢出通报、
0x05=固件内部错误。保留 code ≥ 0x06。**seq 语义（§3.5）**：code 0x01–0x03
回显其所应答命令的 seq；code 0x04/0x05 为异步事件、seq 恒 = `0x0000` 哨兵
（编码器强制置 0，调用方取值不透传）。
接收方对 ERROR 仅计数，不改变自身
状态机（发送方状态机亦不受对方 ERROR 影响——错误经计数器与诊断呈现）。

## 5. 接收校验与恢复行为（两侧行为一致，由共享 codec 保证）

1. **校验顺序：magic → len → CRC → ver → type 分发**（同 P0a 2026-09-05 勘误
   次序；CRC 先于版本，坏 CRC 一律计 crc_errors，防止对端凭噪声伪造
   version_mismatch）。
2. **作废粒度 = 整事务**：任一校验失败即丢弃本事务全部 512 字节、计对应计数
   （crc_errors / len_errors / resyncs（magic 不符且缓冲含非 0 字节）），
   **无滑窗、无逐字节前进**（§2.5）。全 0x00 事务 = 合法『无帧』，不计错误
   （§2.3 规则 5）。作废不影响下一事务。
3. 计数器集（诊断用，两侧行为一致）：tx_frames、rx_frames、resyncs、
   len_errors、crc_errors、version_mismatch、seq_gaps、unknown_types、
   prelink_reject（§6.2）、irq_spurious（§6.6）。
4. **未知 msg_type（CRC 合法）：必须容忍并忽略**，计 unknown_types，不得回
   ERROR、不得改变状态（§3.3 minor 兼容规则）。
5. seq 判定：`(u16)(seq_now − seq_prev) != 1` 计 seq_gaps；不丢帧、不重传。
   **仅事件类帧参与**（§3.5——应答类帧回显命令 seq，不判 gap）。会话基线重置
   （§6.5）不计 gap。
6. ERROR 申报一律**顺延至下一事务**的 MISO/MOSI——延迟应答模型（§2.2）下这
   是物理必然：本事务的 MISO 缓冲在命令被解读前已于上一事务结束时装定，
   ERROR 只能进 pending 槽、出现在下一事务。装载优先级分两类：命令性 ERROR
   （0x01–0x03）走 §2.3 规则 2/3；异步 ERROR（0x04/0x05）按事件规则交付
   （§2.3 规则 4，IRQ 背书延后零丢失）。

## 6. 握手、看护与会话

1. **上电路径**：master 释放 RESET_N（≥ 1 ms 低脉冲后回高，§1）→ 等待
   ≥ 100 ms（slave 启动时间由 WP-E 实测，本预算为下限）→ 进入 WAIT_HELLO。
2. **WAIT_HELLO**：master 每 500 ms 发 HELLO。复位后 slave 的 pending 槽为空
   （§2.2），故事务 1 的 MISO 为全 0x00（合法『无帧』）；slave 在事务 1 完成
   时装入 HELLO 应答（seq 回显，§3.5），事务 2——即 500 ms 后的 HELLO 重发
   ——取走：**握手在首次重发事务完成，≈ 500 ms**。
   **WAIT_HELLO 应答策略（完整裁决，仅此一处）**：HELLO → 装入 HELLO 应答
   （握手段，不删）；PING → **静默**（装入全 0——PONG 是 LINKED 态专属，
   否则 slave 自复位后 master 会永远收到合法 PONG、3 s 看护永不触发，形成
   假活/split-brain）；ERROR → 仅计数、不装载应答；其余命令（业务/查询/写）
   → 整事务作废、计 prelink_reject、装载全 0。master 在 WAIT_HELLO 态收到
   全 0x00 MISO 按 §2.3 规则 5 合法『无帧』处理：忽略、不计错误、继续重试。
   master 收到 ver=1
   的合法 slave HELLO → LINKED；slave 同理。
   **任一侧 ver ≠ 1：拒收、计 version_mismatch、装入 ERROR{0x01} 于下一事务、
   永不进入 LINKED**（禁盲目互通，对齐 P0a §3.2）。连续 10 次 HELLO 无合法
   应答（≈5 s）→ master 重发 RESET 脉冲并回到步骤 1（循环不设上限，诊断页
   呈现；slave 固件缺失时此循环即 cJTAG 首烧的入口信号，§8）。
3. **版本协商**：HELLO 帧头 ver=1 即互认；双方各自在诊断中记录对端 fw_ver
   （§4.1）。无独立版本协商消息——异 major 即拒绝，同 major 内新增类型靠
   §5.4 容忍。
4. **LINKED 后首务**：master 必须在 LINKED 后 1 s 内发 RF_CONFIG 查询
   （§4.6），发现版本落后则写；slave 不得主动要求配置。
5. **seq 基线**：收到对端 HELLO 即以该帧 seq 为新基线（对端重启检测——
   HELLO 中 reset_reason 为证），基线重置不计 seq_gaps（§5.5）。收到合法
   HELLO 时 slave 同时清理半交付态：已交付的 RX_DESCRIPTOR 及其已积累分片
   一并作废、不重新入队（master 重启即放弃该报文），后续 IRQ_ACK 不再命中
   §2.3 规则 1；事件队列与 RF 配置保留（与 §6.6 slave 侧看护同一保留范围）。
   §6.6 的 master RECOVERY 则经 RESET_N 复位 slave，其全部会话态（含队列）
   随重启清零，与本条不冲突。
6. **IRQ 丢失恢复（超时轮询兜底）**：LINKED 态 master 以 1 Hz 发 PING，
   **不看 IRQ**。延迟应答下 PING 事务 N 的 MISO 是 N−1 的结果（§2.2）；PING
   自身的应答按 §2.3 装入 pending——PONG 优先于事件（§2.3 规则 3），队列非
   空时事件顺延一拍——由下一事务取走。三态走查（无死锁/活锁）：① **drain
   态**（§7.2）——PING 挂起，全部事务为 IRQ_ACK 流水，事件流即活性证明；
   ② **WAIT_HELLO 态**——master 只发 HELLO，应答一拍后到达（§6.2），继续按
   500 ms 重试即可；**PING 在此态被 slave 静默**——这正是 slave 自复位后
   master 看护得以触发的机制（B.3 走查 B30：PING 落空 > 3 s → RECOVERY →
   RESET → 重握手 → RF_CONFIG 重发）；③ **LINKED 空闲态**——PING→PONG 一拍
   往返，1 Hz 看护成立。
   以下任一条件成立 → master 进入 RECOVERY：RESET_N 低 ≥ 1 ms → 回步骤 1：
   - LINKED 态 > 3 s 未收到任何合法 MISO 帧（drain 挂起 PING 时以事件流为
     活性证明，不计时）；
   - SUBG_IRQ 持续高电平 > 1 s 且期间所有事务 MISO 均无事件（irq_spurious；
     drain 态由 §7.2 停滞判定接管，本条仅适用于非 drain 的 LINKED 态）。
   slave 侧看护：LINKED 态 > 5 s 无任何 master 事务 → 回 WAIT_HELLO（保留
   RF 配置与事件队列）。
7. **master 调度合同（LINKED 态，规范性）**：master 在每次事务完成（CSN
   上升沿）后采样 SUBG_IRQ；一旦观察到高电平，master 至多再完成**当前已
   装载应答的在途事务**（取走刚装入槽中的直接应答），随后**必须连续发起
   IRQ_ACK 事务直至采样到 IRQ 读低**——即进入 drain 模式（§7.2：挂起 PING
   与查询/写命令），交付延迟上界由此确定性封顶（§2.3）。drain 退出条件、
   停滞判定（§7.2）与活性证明（§6.6：事件流水本身即活性，看护计时挂起）照
   既有规则执行。WAIT_HELLO 态不受本条约束（§6.2 应答策略）。本条是 master
   侧行为合同（WP-E 实现目标），不是 slave 语义。

## 7. 流控

1. **分片上限**：RX_PAYLOAD_CHUNK 单片 data ≤ 496（§4.4）；报文 total_len
   ≤ 4096（§4.3，超限截断置 flags bit1）。master 必须按 descriptor → N×chunk
   的顺序取毕一个报文再取下一事件（IRQ_ACK 驱动，§4.2）。
2. **QUEUE_FULL 退避**：master 收到 QUEUE_FULL 事件后进入 **drain 模式**：
   只发 IRQ_ACK 连续取空事件队列（挂起 PING 与一切查询/写命令）。延迟应答下
   drain 是深度为 1 的流水线（§2.2）：事务 N 的 IRQ_ACK 取走 N−1 装载的
   事件、同时令 slave 装载下一个——每事务推进一个事件/分片，直至一个事务
   满足「MISO 无事件且 IRQ 为低」→ drain 正常结束。**停滞判定（stall）**：
   连续 **8** 个事务 MISO 均无事件而 SUBG_IRQ 仍为高 → 按 §6.6 RECOVERY
   （drain 期间 `queue_full_pending` 不装载，§4.5——队列重填只延长取空
   流水，事件事务不计停滞，本出口在任意重填场景下保持可达）。
   阈值依据：按**单事务空转次数**而非墙钟计——SPI 时钟速率未冻结（§1）、
   queue_depth 未冻结（§4.5，u8 ≤ 255），任何墙钟上限都会在合法低速 + 深队列
   组合下误杀健康 drain（且 RECOVERY 的 RESET 脉冲会清空正在排水的队列）；
   事务次数与线速无关，任意时钟速率下语义恒定。停滞意味着 slave 异常（IRQ
   声明有事件却持续交付为空），此时复位清队列是期望行为，与正常 drain 的
   保护目标不冲突。drain 期间事件流水本身即链路活性证明（同 §6.6，PING 看护
   计时挂起）；§6.6 的 irq_spurious 墙钟兜底仅在非 drain 态计时。

## 8. 升级路径与 cJTAG 边界

- **首烧（R4 现状）**：cJTAG 两线（SUBG_TCKC/TMSC）是 CC1312R 唯一编程路径，
  与 SPI1 无引脚交叠、无协议影响（事实卡「cJTAG boundary」）。master 检测
  「握手循环失败」（§6.2）即为"slave 未烧固件"的运行期信号，提示走 cJTAG。
- **未来固件内升级**：本规范**只冻结 UPGRADE_STATUS 查询**（§4.8）。
  升级数据流（镜像传输、写扇区、切换引导）**明确不在 v1 范围**，留给 WP-E
  二期以 minor 增量定义（新增 msg_type，§3.3）；v1 固件收到未知升级类命令
  按未知类型容忍（§5.4），不会误入升级态。

## 9. 明确不做（非目标）

- **978 PHY/灵敏度/同步/包处理参数**：SimpleLink SDK RF Driver + SysConfig/
  SmartRF 配置域，且必须 978 MHz 实测（PLAN §5.2 原文）；协议只冻结
  config_version 协商槽位（§4.6）。
- **FCC/认证**：合规流程不属于链路协议。
- **加密/鉴权**：板内 3V3_DIG 域点对点短走线（事实卡「Power domain」——
  无电平移位、无外露连接器），威胁模型不含物理接入者。
- **寄存器式访问模型**（§2.4）、**升级数据流**（§8）、**cJTAG 协议化**（§8）、
  **SPI 时钟速率终值**（§1，WP-E 台架实测决定）。

## 附录 A：硬件事实卡引用（Task 1 产出）

`docs/hardware/pinmap_978.md`（证据：v3/v4 kicad_sch 网表 + PCB 网表 + 双侧
数据手册页码，三源一致）：

| 网 | RP2040 U8 | CC1312R U10 | 协议角色 |
|---|---|---|---|
| SUBG_SCK | pad13 = GPIO10 | pad16 = DIO_10 | SPI1 SCK |
| SUBG_MOSI | pad14 = GPIO11 | pad14 = DIO_8 | SPI1 TX（master 出） |
| SUBG_MISO | pad15 = GPIO12 | pad15 = DIO_9 | SPI1 RX（slave 出） |
| SUBG_CSN | pad16 = GPIO13 | pad17 = DIO_11 | 事务边界（R56 10 kΩ 上拉） |
| SUBG_IRQ | pad17 = GPIO14 | pad18 = DIO_12 | 电平触发、高有效（§1） |
| SUBG_SYNC | pad18 = GPIO15 | pad19 = DIO_13 | 保留闲置（§1 裁决） |
| SUBG_RESET | pad29 = GPIO18 | pad35 = RESET_N | master 驱动，低有效（§1） |
| SUBG_TCKC/TMSC | pad28/27 = GPIO17/16 | pad25/24 | cJTAG，与 SPI 无交叠（§8） |

## 附录 B：golden vectors

向量由 Task 3 依据本规范字段表产出并回填于此（Task 3 即首次具体化者），
可执行镜像在 `firmware/test/test_rp_cc13xx_codec.c`（向量 id ↔ 本附录条目
一一对应，文件头有总表）。CRC 已知答案 `crc16("123456789") = 0x29B1`
（§3.2）为 codec 首条断言。十六进制为帧本体（§3 布局），帧尾补 0x00 至
512 B 由 SPI 驱动完成、不计入向量。

### B.1 向量总表

| id | 名称 | 条款 | 方向 | 判定期望（codec 返回 / 语义） |
|---|---|---|---|---|
| B1 | CRC KAT | §3.2 | — | `crc16("123456789") = 0x29B1` |
| B2 | HELLO 正常 | §4.1 | m→s | OK，字段逐项一致 |
| B3 | IRQ_ACK 正常 | §4.2 | m→s | OK，len=0 |
| B4 | PING 正常 | §4 表 | m→s | OK |
| B5 | PONG 正常 | §4 表 | s→m | OK |
| B6 | RX_DESCRIPTOR 正常 | §4.3 | s→m | OK，978 MHz 等字段逐项一致 |
| B7 | 分片重组 | §4.4/§7.1 | s→m | 两片 OK，offset 升序重组逐字节一致 |
| B8 | 单片极限帧 | §3.1/§4.4 | s→m | 整帧恰 512 B，OK + 往返一致 |
| B9 | QUEUE_FULL + 回绕 | §4.5 | s→m | OK；0xFFFE→0x0001 无符号差值 = +3 |
| B10 | RF_CONFIG 写 | §4.6 | m→s | OK，config_version=3 |
| B11 | RF_CONFIG 只读查询 | §4.6 | m→s | OK，config_version=0 |
| B12 | RF_CONFIG_STATUS 回读 | §4.6 | s→m | OK，与 B10 同 digest |
| B13 | RESET_STATUS_REQ 正常 | §4 表 | m→s | OK |
| B14 | RESET_STATUS 正常 | §4.7 | s→m | OK |
| B15 | UPGRADE_STATUS_REQ 正常 | §4 表 | m→s | OK |
| B16 | UPGRADE_STATUS 正常 | §4.8 | s→m | OK，state=0x00 |
| B17 | ERROR 正常 | §4.9 | 双向 | OK，code=0x03、msg="DENY" |
| B18 | N1 异版本拒收 | §6.2/§5.1 | m→s | ERR_VERSION（CRC 合法，死于版本） |
| B19 | N2 len 超限拒收 | §3.1/§5.2 | m→s | ERR_LEN（len 判定先于 CRC） |
| B20 | N3 坏 CRC 拒收 | §5.1/§5.2 | 双向 | ERR_CRC；ver=2+坏 CRC 亦只计 CRC |
| B21 | N4 reserved≠0 | §4.8 | s→m | ERR_LEN（payload 违规同计 len） |
| B22 | N5 未知类型容忍 | §3.3/§5.4 | 双向 | UNKNOWN_TYPE，消息原样可读 |
| B23 | N6 全 0 事务 | §2.3 规则 5 | — | NO_FRAME（合法『无帧』，不计错） |
| B24 | seq 回绕 | §3.4/§5.5 | m→s | 两帧 OK；0xFFFF→0x0000 不是 gap |
| B25 | re-HELLO 清分片态 | §6.5 | m→s | 半交付态作废，旧分片不再命中 |
| B26 | 延迟握手走查 | §2.2/§6.2 | — | T1 MISO 全 0；T2 = slave HELLO（seq 回显 T1 命令） |
| B27 | N7 零长分片拒收 | §4.4 | s→m | encode(0)→0；decode_frame → ERR_LEN；reasm_feed → ERR_LEN |
| B28 | N8 total_len>4096 拒收 | §4.3 | s→m | encode→0；decode_frame/typed decode → ERR_LEN |
| B29 | N9 reset_reason 发送掩码 | §4.1 | — | 源值 0x1F → 线上 0x0F（&0x0F），解码见 0x0F |
| B32 | N10 异步 ERROR seq 哨兵 | §3.5/§4.9 | s→m | code=0x04、seq=0x0000 往返 OK；对账/gap 跳过 |
| B33 | N11 reasm 越界片拒收 | §4.4 | — | app 侧 data_len=497（total=4096）→ ERR_LEN，先于 memcpy |

### B.2 十六进制字面量

```
B2  HELLO           seq=0001 fw=1.2 reset=POR(0x1)
    50 4B 01 01 01 00 08 00 01 00 02 00 01 00 00 00 5A E6
B3  IRQ_ACK         seq=0002
    50 4B 01 02 02 00 00 00 D2 80
B4  PING            seq=0003
    50 4B 01 03 03 00 00 00 37 5C
B5  PONG            seq=0001
    50 4B 01 04 01 00 00 00 8B D6
B6  RX_DESCRIPTOR   seq=0004 desc_id=1 freq=978000000 ts=1000 total=20
    rssi=0xC4 flags=0x01
    50 4B 01 10 04 00 0E 00 01 00 80 18 4B 3A E8 03 00 00 14 00 C4 01 15 70
B7  分片重组：报文 20 B = "0123456789ABCDEFGHIJ"（total_len=20, desc_id=1）
    chunk0  seq=0005 offset=0    data=12 B（"0123456789AB"）
    50 4B 01 11 05 00 12 00 01 00 00 00 14 00 30 31 32 33 34 35 36 37 38 39
    41 42 C2 C5
    chunk1  seq=0006 offset=12   data=8 B（"CDEFGHIJ"）
    50 4B 01 11 06 00 0E 00 01 00 0C 00 14 00 43 44 45 46 47 48 49 4A 12 11
B8  单片极限：desc_id=2 offset=0 total=496 data[k]=k&0xFF（k=0..495）
    → plen=502、整帧 512 B = 事务长度（构造规则向量；帧头/CRC 字面：）
    50 4B 01 11 07 00 F6 01 ‖ 02 00 00 00 F0 01 ‖ data(496) ‖ 46 F3
B9  QUEUE_FULL      seq=0008 dropped=0xFFFE depth=8
    50 4B 01 12 08 00 04 00 FE FF 08 00 32 69
B9' QUEUE_FULL 回绕 seq=0009 dropped=0x0001 depth=0
    50 4B 01 12 09 00 04 00 01 00 00 00 88 23
B10 RF_CONFIG 写    seq=000A ver=3 digest=00 11 22 .. EE FF
    50 4B 01 20 0A 00 14 00 03 00 00 00 00 11 22 33 44 55 66 77 88 99 AA BB
    CC DD EE FF 47 E7
B11 RF_CONFIG 查询  seq=000B ver=0 digest=00×16（只读，§4.6）
    50 4B 01 20 0B 00 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 D9 9C
B12 RF_CONFIG_STATUS seq=0001 ver=3 digest 同 B10（回读）
    50 4B 01 21 01 00 14 00 03 00 00 00 00 11 22 33 44 55 66 77 88 99 AA BB
    CC DD EE FF CF 53
B13 RESET_STATUS_REQ seq=000C
    50 4B 01 22 0C 00 00 00 3C 2A
B14 RESET_STATUS    seq=0002 reset=RESET_N(0x2) uptime=123456 ms
    50 4B 01 23 02 00 08 00 02 00 00 00 40 E2 01 00 16 E2
B15 UPGRADE_STATUS_REQ seq=000D
    50 4B 01 24 0D 00 00 00 0D 91
B16 UPGRADE_STATUS  seq=0003 state=normal(0x00) img=0x00000102
    50 4B 01 25 03 00 08 00 00 00 00 00 02 01 00 00 0A A8
B17 ERROR           seq=000E code=0x03 len=4 msg="DENY"
    50 4B 01 7F 0E 00 06 00 03 04 44 45 4E 59 2F A0
B18 N1 HELLO ver=2  CRC 合法（B2 同 payload，帧头 ver 改 0x02 后重算 CRC）
    50 4B 02 01 01 00 08 00 01 00 02 00 01 00 00 00 F9 6B
B19 N2 len=503>502  帧头 8 B 字面 + 0x00 填充至 512 B（构造规则向量；
    len 判定先于 CRC，故无合法 CRC 亦必拒）
    50 4B 01 01 01 00 F7 01 ‖ 00×504
B20 N3a 坏 CRC      B2 末字节翻转（E6→E7）
    50 4B 01 01 01 00 08 00 01 00 02 00 01 00 00 00 5A E7
B20 N3b ver=2+坏CRC B18 末字节翻转（6B→6A）——必须计 CRC 不计 version（§5.1）
    50 4B 02 01 01 00 08 00 01 00 02 00 01 00 00 00 F9 6A
B21 N4 reserved≠0   B16 payload[1] 置 0x01 后重算 CRC（死于 payload 校验）
    50 4B 01 25 03 00 08 00 00 01 00 00 02 01 00 00 6B 10
B22 N5 未知类型     type=0x55 len=2 payload=CA FE，CRC 合法
    50 4B 01 55 0F 00 02 00 CA FE 11 57
B23 N6 全 0 事务    0x00 ×512（构造规则向量；合法『无帧』）
B24 seq 回绕        PING seq=0xFFFF 与 PING seq=0x0000
    50 4B 01 03 FF FF 00 00 2B 43
    50 4B 01 03 00 00 00 00 EB C7
B27 N7 零长分片     seq=0009 desc_id=1 offset=0 total=20 data_len=0（plen=6，
    CRC 合法——死于 §4.4 data≥1，计 len）
    50 4B 01 11 09 00 06 00 01 00 00 00 14 00 DE 7B
B28 N8 total=4097   B6 的 total 字段改 0x1001 后重算 CRC（死于 §4.3 上限）
    50 4B 01 10 04 00 0E 00 01 00 80 18 4B 3A E8 03 00 00 01 10 C4 01 94 94
B29 N9 掩码 HELLO   seq=0002 fw=1.2 reset 源值 0x1F → 线上 0x0F
    50 4B 01 01 02 00 08 00 01 00 02 00 0F 00 00 00 9F 41
B32 N10 异步 ERROR  seq=0000（哨兵）code=0x04 len=0
    50 4B 01 7F 00 00 02 00 04 00 20 34
B33 N11 越界片      构造规则向量（无线上帧——app 侧 chunk 结构体）：
    chunk{desc_id=1, offset=0, total_len=4096, data_len=497}
    → reasm_feed 必须先于 memcpy 拒绝（ERR_LEN；data[496] 容量越界防御）
```

### B.3 会话级行为向量（延迟应答模型 §2.2 下的走查）

- **B25（§6.5）**：事务级流水（每事件一事务，应答延迟一拍）：T1 IRQ_ACK
  取走 pending 的 B6 descriptor（此前已装载）并令 slave 装载 chunk0；T2
  IRQ_ACK 的 MISO = B7 chunk0（seq 回显 T1 命令 seq），积累至 12 B；此刻
  master 收到合法 HELLO → 半交付态立即作废（active=0、have=0），旧 chunk1
  不再命中（§6.5 已放弃该报文）；随后新 descriptor（新 desc_id）可干净重组
  至完成。可执行断言见测试 case 18。
- **B26（§2.2/§6.2）延迟握手走查**：复位后 slave pending 槽空 → T1 master
  HELLO（seq=A）的 MISO = 全 0x00（合法『无帧』，master 忽略不计错、继续
  重试）；slave 于 T1 完成时装入 HELLO 应答；T2 master HELLO（seq=A+1，
  500 ms 重发）的 MISO = slave HELLO（**seq 回显 A**，非 A+1——应答对应的是
  它所应答的命令）→ master 判 LINKED。帧本体复用 B2（解码路径）与 B23
  （全 0 事务路径），无独立十六进制。
- **B30（§6.2/§6.6）slave 自复位恢复走查**：LINKED 态 slave 自复位（pending
  槽清空、回 WAIT_HELLO、RF 配置清零）→ master PING 落入静默（WAIT_HELLO
  对 PING 装载全 0，§6.2 应答策略）→ > 3 s 无任何合法 MISO 帧 → §6.6
  RECOVERY（RESET_N ≥ 1 ms）→ slave 全态清零重来 → HELLO ×2（T1 全 0、
  T2 应答，同 B26）→ LINKED → 1 s 内 RF_CONFIG 查询/重发（§6.4）。旧模型
  「WAIT_HELLO 仍答 PING」造成的 PONG 假活/split-brain 路径已由 P1-b 关闭。
- **B31（§2.3 规则 3/4）应答优先走查**：队列非空时 master 发 RF_CONFIG →
  事务 N+1 的 MISO = RF_CONFIG_STATUS（直接应答胜出，seq 回显）；事件仍在
  队头、IRQ 保持高 → 事务 N+2 发 IRQ_ACK 取走该事件。PING 落在队列非空时
  同理（PONG 胜出）。无后备队列的应答不再可能被单槽覆盖丢失。**异步 ERROR
  不再抢占**（round 7）：RF_CONFIG 期间异步 0x04 触发 → STATUS 胜出装入、
  0x04 挂起延后（规则 4）——旧序下 0x04 会占据单槽使 STATUS 永久丢失，已
  关闭；0x04 最终经 IRQ 背书在 §6.7 上界内送达。

正向向量 B2–B17 覆盖 §4 表全部 14 种 msg_type（5 条空载荷命令共用
`rp_cc13xx_encode_empty`）；负路径 B18–B23、B27–B29 对应 §5.2/§5.4/§6.2/
§4.3/§4.4 的拒收与容忍语义。帧格式（§3）在延迟应答修订中未变，B2–B24 十六
进制向量逐字节仍有效；B25/B26 为事务时序走查（B9' 等事件帧的 seq 按事件类
计数器解释，应答帧按回显解释——§3.5）。

## 附录 C：自审清单（本规范冻结时已核）

### C.1 PLAN.md §5.2 逐项覆盖

| PLAN.md §5.2 条目 | 本规范章节 |
|---|---|
| 启动握手 | §6.1–§6.2 |
| 固件版本 | §4.1（HELLO）、§6.3 |
| RF 配置版本 | §4.6（RF_CONFIG/RF_CONFIG_STATUS）、§6.4 |
| 收包 descriptor | §4.3 |
| payload 分片 | §4.4、§7.1 |
| IRQ 清除 | §1（IRQ 裁决）、§4.2、§2.3 |
| 队列满 | §4.5、§7.2 |
| 复位原因 | §4.1、§4.7 |
| cJTAG/升级状态 | §4.8、§8 |
| 不得假定无固件寄存器外设 | §2.4 |
| PHY/同步/包处理/灵敏度 = SDK 配置 + 实测 | §9、§4.6 |
| （brief 增补）分片上限 | §7.1 |
| （brief 增补）QUEUE_FULL 退避 | §7.2 |
| （brief 增补）seq 回绕 | §3.4、§5.5 |
| （brief 增补）IRQ 丢失恢复 | §6.6 |
| （brief 增补）版本不匹配拒绝 | §6.2 |
| （brief 增补）明确不做：FCC/认证/加密 | §9 |

### C.2 裁决完整性

- 事务形状唯一裁决：512 B 定长事务 + **单槽延迟应答**（§2.1、§2.2、§2.3）；
  应答 seq 回显对账（§3.5）；IRQ_ACK 即读触发、无专用 POLL（§2.2）。
- SYNC 唯一裁决：保留闲置（§1），关闭事实卡 Unresolved #2；IRQ 极性裁决
  关闭 Unresolved #3。
- CRC 参数与 `adsb_link_codec.c:4-13` 逐字一致（§3.2），未另立参数。
- 全文无实现自由度留白语句；唯一显式延后项为 SPI 时钟速率终值（§1/§9，
  非协议语义）。
- 每个字段偏移/单位/常量（496/502/512/4096/0.5 dB/0xFF/0x29B1）均可直接
  成为 codec 断言；消息布局与 §3 帧格式联合可导出逐字节向量。

### C.3 v1.0 定稿前修订记录（2026-09-08，同日，不升版本）

| 修订 | 内容 | 条款 |
|---|---|---|
| R1（`595e11c`） | UPGRADE_STATUS `reserved[3]` 约束；re-HELLO 分片态清理 | §4.8、§6.5 |
| R2（本提交） | drain 退出改为事务粒度停滞判定（连续 8 事务空转），废除墙钟 100 ms 上限——线速/队列深度的合法组合下墙钟上限会误杀健康 drain | §7.2、§6.6 |
| R2（`e33db63`） | 0x00/0xFF 合法帧（magic+CRC 合法）接收行为入文（按未知类型容忍）；RX_DESCRIPTOR flags 与 QUEUE_FULL reserved 的接收校验规则补齐（同 §4.8 句式） | §4 表注、§4.3、§4.5 |
| R3（`af3a5cb`） | **事务模型修订为延迟应答（pending）**：同事务请求→应答在 SPI 全双工线上无因果路径（命令类型在 MOSI 字节 3，MISO 字节 0 已先移出）——事务 N 的 MISO = 事务 N−1 命令的应答；单 pending 槽、空槽装全 0；应答 seq 回显（§3.5）；IRQ_ACK 即读触发；握手改为 T2（首次重发）完成；PING/drain 走查入 B.3。依据：独立审计指出物理不可实现性（SPI 从机 CSN 下降沿预装 DMA 缓冲的实现合同随之入文 §2.1） | §2、§3.5、§4.2、§5.5、§5.6、§6.2、§6.6、§7.2、B.3、C.2 |
| R4（本提交） | 边界强制补齐：零长分片拒绝（§4.4，防重组活锁）；total_len ≤ 4096 编码/解码两侧强制（§4.3，此前仅重组侧拒绝）；reset_reason 发送边界掩码 &0x0F（§4.1，线上合同）。新负向量 B27/B28/B29 | §4.1、§4.3、§4.4、B.1/B.2 |
| R5（本提交） | 审计 round 6 状态机裁决：①**直接应答优先于事件**（§2.3——单槽下无后备队列的应答被事件挤掉即永久丢失；事件有队列 + IRQ 兜底，延后零丢失；饥饿权衡诚实声明 + B31 走查）；②**WAIT_HELLO 对 PING 静默**（§6.2 完整应答策略；关闭 slave 自复位后 PONG 假活/split-brain，3 s 看护自此可触发；PING/PONG 表行限定 + B30 走查）；③**异步 ERROR seq 哨兵**（§3.5/§4.9——code 0x01–0x03 回显命令 seq，0x04/0x05 恒 0x0000） | §2.3、§3.5、§4 表、§4.9、§6.2、§6.6、B.3 |
| R6（本提交） | 审计 round 6 codec/测试修复：reasm_feed 补 data_len > 496 越界防御（P1-c，ASan 复现项——app 侧 chunk 先于 memcpy 拒绝）；B28 typed-decode 测试改为显式构造合法 msg（P2-b，旧写法复用未初始化对象属假通过）；异步 ERROR seq=0 哨兵向量 B32 + 越界片向量 B33 + 回归测试 case 23/24 | rp_cc13xx_codec.c、test case 21/23/24、B.1/B.2 |
| R7（本提交） | 审计 round 7 裁决：①**异步 ERROR 降位**（§2.3 规则 2 限定命令性 0x01–0x03；0x04/0x05 视为可保留事件走规则 4，关闭其覆盖直接应答的永久丢失路径，B31 走查补充）；②**§6.7 master 调度合同**（规范性：IRQ 高电平后至多完成在途应答必须连发 IRQ_ACK 直至读低——事件/QUEUE_FULL/异步 ERROR 交付延迟确定性封顶，关闭背靠背命令无限饥饿）；③**queue_full_pending 挂起标志**（§4.5——队满时队列不动、置单个标志，交付优先级在直接应答与普通事件之间、不清队列、drain/RESET 清除）；④§3 seq 表行语义改指 §3.5（命令/回显/哨兵三分类） | §2.3、§3 表、§4.5、§5.6、§6.7（新）、B.3 |
| R8（本提交） | 审计 round 7 P2-b：encode_error 对 code 0x04/0x05 强制 seq=0x0000 上线（调用方取值不透传——线上合同固定）；回归测试证伪透传（code=0x04/seq=0x1234 → 线上 00 00）并钉住命令码回显（0x03/0x1234 → 0x1234） | rp_cc13xx_codec.c、test case 24、§4.9 |
| R9（本提交） | 验证轮修订：①**queue_full_pending 交付即清 + drain 不装载**（§4.5——QUEUE_FULL 是通知不是队列成员；旧"drain 进入才清"在 drain 中重填时标志反复装载、MISO 永不空转、§7.2 停滞出口永不触发 → 只能 RESET 逃逸的非终止 drain，已关闭）；②§2.3 规则 4 陈旧括注修正（QUEUE_FULL 已是队列外标志，不再列为队列成员）；③§4 表 0x12 频率约束格由「入队一次/清空后复置」改为「队满置位、交付即清、drain 不装载」，与 §4.5 对齐（grep 全文复查，其余「入队」均指 RX_DESCRIPTOR 或 slave RF 侧，无残留）；§7.2 重填场景出口可达性注记 | §2.3、§4 表、§4.5、§7.2 |
