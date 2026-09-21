# 扩展板 V4.10 — 权威引脚/网络映射（原理图单一事实来源）

英文版：[`PINMAP.md`](PINMAP.md)


> 规则：本表先于原理图存在；原理图/PCB 与本表冲突时，以本表为准并同步修订。
> 器件具体 pin 号（QFN48 等）在阶段 2 建符号时从 datasheet 填入，并按 PAD_NET 教训逐脚复核。
> 来源标注：〔A〕=同类参考设计原理图拓扑（仅事实参考）；〔F〕=本仓库固件现有定义；〔D〕=本项目决策。

## 1. 对主板接口：2×20 排母，直插微雪载板 J3（HAT 式堆叠）〔D〕

只列扩展板实际使用的脚，其余脚过孔留空：

| J3 脚 | P4 GPIO | 网络 | 方向（P4 视角） | 依据 |
|---|---|---|---|---|
| 1, 3（下排末端） | — | VCC_5V | 供电入 | 载板 pinout 下排全表复核〔F〕 |
| 5,10,13,19,26,29,33,40 | — | GND | — | 上排 40/26/10 + 下排 33/29/19/13/5〔F〕 |
| 4 | GPIO7 | I2C_SDA | 双向 | imu_task.c:49〔F〕 |
| 6 | GPIO8 | I2C_SCL | 输出 | imu_task.c:50〔F〕 |
| 32 | GPIO49 | GNSS_RXD（P4 TX → GNSS） | 输出 | gps_task.c:20〔F〕 |
| 36 | GPIO51 | GNSS_TXD（GNSS → P4 RX） | 输入 | gps_task.c:21〔F〕 |
| 34 | GPIO50 | GNSS_PPS | 输入 | gps_task.c:21 注释〔F〕 |
| 28 | GPIO34 | IMU_INT | 输入 | imu_task.c:56〔F〕 |
| 16 | GPIO28 | IMU_RST | 输出 | imu_task.c:57〔F〕 |
| 24 | GPIO31 | BARO_INT（可选） | 输入 | baro_task.c:32〔F〕 |
| 35 | GPIO46 | ADSB_TXD（RP2040 → P4 RX）**新增** | 输入 | 载板标空闲〔F〕+〔D〕 |
| 31 | GPIO32 | ADSB_RXD（P4 TX → RP2040，配置/控制）**新增** | 输出 | 载板标空闲〔F〕+〔D〕 |

### V4.6 AIRBAND 新增（BK4819 三线 SPI + PCM1808 走 P4 第二路 I2S）

| J3 脚 | P4 GPIO | 网络 | 方向（相对 P4） | 依据 |
|---|---|---|---|---|
| 20 | GPIO29 | VHF_SCN（三线 SPI 片选） | 输出 | J3 空闲脚〔F〕+〔D〕 |
| 22 | GPIO30 | VHF_SCK（三线 SPI 时钟） | 输出 | J3 空闲脚〔F〕+〔D〕 |
| 37 | GPIO47 | VHF_SDATA（三线 SPI 数据） | 双向 | J3 空闲脚〔F〕+〔D〕 |
| 11 | GPIO5 | VHF_MCLK（PCM1808 系统时钟） | 输出 | J3 空闲脚〔F〕+〔D〕 |
| 17 | GPIO22 | VHF_BCK（I2S 位时钟） | 输出 | J3 空闲脚〔F〕+〔D〕 |
| 38 | GPIO52 | VHF_LRCK（I2S 帧时钟） | 输出 | J3 空闲脚〔F〕+〔D〕 |
| 39 | GPIO48 | VHF_DOUT（PCM1808 → P4） | 输入 | J3 空闲脚〔F〕+〔D〕 |

BK4819 的三线 SPI 由 P4 软件模拟（SDATA 双向，不占用硬件 SPI 引脚）。PCM1808 走 P4 的
**第二路** I2S 控制器（载板的 ES8311/ES7210 占着 I2S0），信号经上述 J3 脚引出。航空波段音频
经 BLE 回放到 App；载板的 ES8311 + NS4150B + H4 喇叭是可选项（纯固件）。

固件侧改动量：新增一路 UART 驱动（GPIO46/32）+ VHF 驱动组（BK4819 SPI + 第二路 I2S 采集），其余零改动。

## 2. RP2040 引脚分配〔D，PIO 分组对齐 A 的架构〕

| RP2040 GPIO | 网络 | 功能 |
|---|---|---|
| GPIO0 / GPIO1 | ADSB_TXD / ADSB_RXD | UART0 → P4（GDL90/raw 报文 + 控制） |
| GPIO2 / GPIO3 | BIAS_EN_1090 / BIAS_EN_978 | 天线偏置 Tee PMOS 栅极（低有效，10k 上拉默认关断）〔D〕 |
| GPIO10 / 11 / 12 / 13 | SUBG_SCK / SUBG_MOSI / SUBG_MISO / SUBG_CSN | SPI1 master → CC1312R slave〔A〕 |
| GPIO14 / 15 | SUBG_IRQ / SUBG_SYNC | CC1312R 中断 + 同步/bootloader 触发〔A〕 |
| GPIO16 / 17 / 18 | SUBG_TMSC / SUBG_TCKC / SUBG_RESET | cJTAG 代刷 CC1312R 固件〔A〕 |
| GPIO19 | PULSES | 比较器输出 → PIO0 前导检测〔A〕 |
| GPIO20–23 | DEMOD0–3 | PIO 解调状态机联络/调试〔A〕。V4.9 起落到 **J11 的 10/12/14/13 脚**（见下节） |
| GPIO24 | RECOVERED_CLK | 调试用恢复时钟〔A〕。V4.9 起落到 **J11 的 15 脚**；固件把它借作自检脉冲输出 |
| GPIO25 | TL_PWM | 比较器阈值 PWM（RC 滤波成 LEVEL_BIAS）〔A〕 |
| GPIO26 / ADC0 | LEVEL_BIAS_SENSE | 阈值直流回读〔A〕 |
| GPIO27 / ADC1 | RSSI | AD8313 VOUT 回读（自适应灵敏度）〔A〕 |
| GPIO28 / ADC2 | 备用 ADC | 预留（如 978 RSSI） |
| QSPI 专用脚 | W25Q128JVSIQ | 16MB 固件+配置 |
| XIN | 12MHz 晶振（C9002） | |
| USB_DP/DM | USB-C 座 | UF2 拖拽刷机 + CDC 调试〔D〕 |
| RUN + BOOTSEL | 轻触开关 ×2 | 开发板必备〔D〕 |
| SWD（SWCLK/SWDIO） | **J11 的 6 / 8 脚** | V4.9 起随 J11 调试排针一并引出（原 TP1/TP2） |

### RP2040 的 USB 路径（2026-08-23 查证）

| USB 外设（ESP32-P4） | 默认引脚 | 能否做 host | 微雪 J3 是否引出 |
|---|---|---|---|
| USB Serial/JTAG | GPIO24 / 25 | ❌ 纯 device | ✅ pin23 / pin21 |
| USB 2.0 Full-Speed OTG | GPIO26 / 27 | ✅ | ❌ **未引出** |
| USB 2.0 High-Speed OTG | 专用脚 49/50 | ✅ | ✅ pin27 / pin25（与 H2 同网） |

⚠️ **J3-23/21 引出的 GPIO24/25 是 USB Serial/JTAG，不是 OTG FS**，不能做 host。
要切换须烧 eFuse `USB_PHY_SEL`（不可逆且废掉 USB-JTAG）。若要 P4 代刷 RP2040，
只能用 HS 那组（pin27/25），代价是永久占掉 H2 与载板 USB-A。详见
[VARIANTS.md](VARIANTS.md) §4–§5。

## J11 调试焊盘阵列（V4.9 新增；V4.10 改为 2x10 裸焊盘，B 面）

板上此前**没有任何办法接触 CC1312R 的 cJTAG**：`SUBG_TMSC`/`SUBG_TCKC`/`SUBG_RESET`
在 `U8`(RP2040) 与 `U10` 之间点对点直连，中途既不能断开也无处下探针。J11 把三根
引出，并取代了原来的 `TP1`~`TP7` 与 `SW1`/`SW2`（九件已删除）。

🔴 **V4.10 相对 V4.9 有两处会让人读错板的改动**：

1. **不是排针，是裸焊盘阵列**（自制封装 `TestPad_2x10_P2.54mm`，20 个 φ1.0mm
   圆形裸铜，2.54mm 网格，不贴任何元件）。配 2x10p 的 PCB 测试夹使用，探针
   直接压上去。**没有排针可插，也就没有跳线帽**——三对短接改用镊子或锡桥。
2. **脚号整体后移 4 位**：V4.9 的 pin1~16 现在是 **pin5~20**，新的 pin1~4 给
   airband。照 V4.9 的图读，每一脚都会错位 4 个。

⚠️ **板面丝印用的是缩写**（0.8mm 字高放不下 `RECOVERED_CLK` 这种全名），
拿着板子对不上名字时查这张表：

| 脚 | 板面丝印 | 网络 | V4.9 时的脚号 | 说明 |
|---|---|---|---|---|
| 1 | `TCXO` | `VHF_TCXO_TP` | — | BK4819 的 26MHz 参考（V4.10 新增）。**经 `R66`（0Ω，默认不贴）才接到 `VHF_TCXO`** |
| 2 | `AUD` | `VHF_AUDIO` | — | BK4819 解调后的模拟音频（V4.10 新增） |
| 3 | `VSCK` | `VHF_SCK` | — | BK4819 的 SPI 时钟（V4.10 新增） |
| 4 | `VSDA` | `VHF_SDATA` | — | BK4819 的 SPI 数据（V4.10 新增） |
| 5 | `GND` | GND | 1 | 短接 5-6 = 复位 |
| 6 | `RUN` | `RP_RUN` | 2（原 **SW1** RESET 短接焊盘） | RP2040 复位 |
| 7 | `GND` | GND | 3 | 短接 7-8 = BOOTSEL |
| 8 | `BOOT` | `QSPI_SS` | 4（原 **SW2** BOOTSEL 短接焊盘） | 拉低则上电进 UF2 刷机模式 |
| 9 | `TMS` | `SUBG_TMSC` | 5 | **CC1312R cJTAG**，V4.9 首次引出 |
| 10 | `SWCLK` | `SWCLK` | 6（原 TP1） | RP2040 的 SWD 时钟 |
| 11 | `TCK` | `SUBG_TCKC` | 7 | **CC1312R cJTAG** |
| 12 | `SWDIO` | `SWDIO` | 8（原 TP2） | RP2040 的 SWD 数据 |
| 13 | `RST` | `SUBG_RESET` | 9 | **CC1312R 复位** |
| 14 | `D0` | `DEMOD0` | 10（原 TP3） | PIO 解调状态输出 |
| 15 | `GND` | GND | 11 | cJTAG 侧参考地 |
| 16 | `D1` | `DEMOD1` | 12（原 TP4） | 同上 |
| 17 | `D3` | `DEMOD3` | 13（原 TP6） | 同上 |
| 18 | `D2` | `DEMOD2` | 14（原 TP5） | 同上 |
| 19 | `RCLK` | `RECOVERED_CLK` | 15（原 TP7） | GPIO24，固件借作自检脉冲输出 |
| 20 | `PLS` | `PULSES` | 16 | GPIO19，TLV3501 比较器 → PIO 的 1090 脉冲 |

🔴 **pin1（26MHz 参考）默认是断开的**，中间串着 `R66`（0Ω，0603，F 面
(138.10, 103.80)，位号在 (138.10, 106.13)）。要用测试夹看 26MHz 时手焊一颗
0Ω 上去，看完拆掉。

为什么不能直连：到 J11 那根分支有 **119.9mm 走在 In2**（夹在 In1-GND 与
In3-3V3 两个平面之间的带状线，全板电容密度最高的层），估算 **17pF**。而
`TG-5006CG` 手册 Specifications 表的 `Output load` 是 **10 kΩ // 10 pF**，
`fo-Load` 那行的条件栏更写死 **±10%** —— 直连等于把负载翻到两三倍。频率倒不是
主要风险（TCXO 晶体在片内、输出是缓冲的，何况 AM 航空波段对频差宽松），
**要命的是幅度**：0.8Vpp Min 是在 10pF 下保证的，27pF 在 26MHz 只剩 227Ω 阻抗，
缓冲级灌不动就会塌；而 BK4819 的 XI 本来就是按并联谐振晶体口设计的、手册没给
外部注入门限。reference 起不来 = airband 整条收不了。

R66 **必须在 TCXO 那一端**（紧挨 U21/Y4），放 J11 端的话那 120mm 仍然挂在振荡
节点上，等于白做。不贴时振荡节点上的残留是 11.0mm + 2 个过孔 ≈ **1.8pF**，
总负载约 11.8pF —— 比手册 ±10% 窗口的上限 11pF 略高 7%，按线性外推负载牵引约
±0.35ppm（在 130MHz 上是 48Hz，AM 话音无感），负载阻抗 612Ω→520Ω、幅度约掉
10%。与直连的 218Ω / −65% 不是一个量级。

pin2~4 三根（音频、SPI 时钟与数据）是直连的：`VHF_AUDIO` 后面串着 10µF 耦合、
音频频段对这 10pF 无感，SPI 也慢。

**三对短接**（V4.9 是插跳线帽；V4.10 是裸焊盘，要用镊子或锡桥）：

| 短接 | 等效于 | 用途 |
|---|---|---|
| 5-6 | 原 SW1 | 复位 RP2040 |
| 7-8 | 原 SW2 | 进 BOOTSEL（UF2 刷机） |
| 19-20 | 原先要飞线 | 自检回环 `GPIO24 → GPIO19` |

三对都是同一行的左右两个焊盘，中心距 2.54mm、净间距 1.54mm。

🔴 做 19-20 自检回环前**必须先拆 `R57`**（33Ω，`PULSES_RAW`→`PULSES` 的串阻）。
不拆的话 TLV3501 的推挽输出与 GPIO 对驱，GPIO 既打不动电平又在跟比较器抢，
判据见固件 `firmware/rp2040/selftest_gen.h` 的 audit round 5 勘误。

脚位顺序**不是按功能分组排的，是按既有走线的进线顺序排的**——原 TP/SW 那束
8 条线从座子右上方平行进来、自身已按 y 排好序，焊盘照同序往下排才能平行落下
互不交叉。改动脚位前先看 `internal/tools/sheet_mcu.py` 里的说明。

V4.10 扩成 2x10 时**中心上移了 2.54mm**（y 88.44 → 85.90），不是随手挪的：
2x10 的 pad5 相对中心是 -6.35、2x8 的 pad1 是 -8.89，上移正好让新 pin5~20 落在
原 pin1~16 的物理位置上，16 根已布走线一根都不用重布。往下扩会撞 B 面的 RT1
（y 99.41~102.46）和贯穿的 H3 安装孔。

---

## 3. CC1312R（QFN48）逻辑分配〔D；DIO→pin 号建库时从 datasheet 填〕

| 逻辑信号 | CC1312R 侧 | 对端 |
|---|---|---|
| SPI slave：SCLK/MOSI/MISO/CSN | DIO10 / DIO8 / DIO9 / DIO11 | RP2040 SPI1 |
| IRQ / SYNC | DIO12 / DIO13 | RP2040 GPIO14/15 |
| cJTAG TMSC / TCKC | 专用 JTAG_TMSC / JTAG_TCKC 脚 | RP2040 GPIO16/17 |
| RESET_N | 专用脚 | RP2040 GPIO18 |
| RF_P / RF_N | 差分 RF | LC 匹配网络 → J3(U.FL 978) |
| X48M | 48MHz 晶振（Abracon ABM8W 7pF 首选；KDS 12pF 待核片内电容阵列范围） | |
| X32K | 32.768kHz（EPSON FC-135） | |

978 差分匹配起点值〔A〕：L 7.5nH / 27nH / 6.8nH×2；C 3.6pF×2 / 2.7pF / 6.2pF。915→978 频点偏移，V4 全部留可调位。

## 4. 电源树〔D，比 A 多拆一路 RF LDO〕

```
J3 VCC_5V ──┬── ME6211C33 (500mA) ──→ 3V3_DIG：RP2040、Flash、CC1312R(经磁珠)、BNO085、BMP388、QMC5883P
            ├── TPS7A2033 #1 ──────→ 3V3_RF：QPL9547、BGA2817、AD8313、TLV3501 比较器域
            └── TPS7A2033 #2 ──────→ 3V3_GNSS：ATGM336H-6N-74
GNSS 有源天线偏置（×2：内置 patch / 外接 SMA）：ATGM336H VCC_RF(U7.14) → PMOS 高侧开关(Q4/Q5) → 6V/200mA 保险丝 → 33nH 馈电电感 → 天线。模块按 VCC_RF 电流判定 ANTENNA OK/OPEN/SHORT。
偏置 Tee（×2：1090 / 978）〔A〕：PMOS 高侧开关 + 6V/200mA 保险丝 + 100nH 馈电电感 + ESD(0.6pF 级)
```

## 5. 1090 接收链逐级网络〔A 拓扑 + 我方选型〕

```
板载IFA → ZP1并 → ZS1串 → ZP2并 → C53 100pF ─┐
                                                ├→ U16射频开关 → C54 → QPL9547(LNA①)
J6外接U.FL → 偏置Tee → C30 100pF ─────────────┘
→ C 100pF → TA0970A(SAW①) → 同上匹配 → BGA2817(LNA②) → C 100pF → TA0970A(SAW②)
→ C 100pF C0G 耦合(C34/C35) → AD8313(U14，2026-08-26 定为唯一检波方案，AD8319 已删)
→ RSSI/VOUT ─┬→ RP2040 ADC1
             └→ TLV3501 IN+；IN- = LEVEL_BIAS(TL_PWM 经 10k+100nF RC) + 10k 迟滞〔A〕
→ TLV3501 OUT = PULSES → RP2040 GPIO19
测试点：V1 已删 MM8930 产测座与射频段 W.FL（1090MHz 短截线）；板载 IFA 侧保留 J7 调试口
```

全部 LC 值为〔A〕起点，V1 每个位号留 0402 可调位；SAW 换型（TFS1090F→TA0970A）后匹配值需在板上实调。

板载IFA的J7挂在π网络之后、C53之前。只要后级接收路径保持断路，从J7接VNA就是隔离测量。
V4.0实测板虽然已经贴了C53，但U16等后级元件未贴，C53没有建立负载通路，因此与本次读数无关；
若另一块板已贴通后级，则测量前应先隔离后级。
当前π网络默认ZS1=0R、ZP1/ZP2=DNP，即直通。V4.0实板最终切到**铜箔外包络50.0mm、
辐射臂中心线跨度48.5mm**（由53.5mm逐刀切短、50.0mm停刀）；装盒+电池实测
**1082.5MHz、SWR 1.09、45+j0.5Ω**；拆盒（仍装电池）读数在1080–1090MHz间漂移。V4.4已把该几何统一回灌到生成器、封装库和PCB内嵌ANT1，
taper与5.103mm馈电路径保持不变。

## 6. 传感器 / GNSS（直连 P4，不过 RP2040）〔F 架构不变〕

| 器件 | 总线/脚 | 备注 |
|---|---|---|
| BNO085 | I2C addr 0x4A + IMU_INT(GPIO34) + IMU_RST(GPIO28) | PS0/PS1/SA0 均接地，CLKSEL0/H_CSN 拉高；V4.4 封装旋转 90° |
| BMP388 | I2C addr 0x76（baro_task.c:29）+ BARO_INT(GPIO31) | 壳体留通气孔 |
| QMC5883P | I2C addr 0x2C | 远离电感/大电流走线；引脚与外围已按手册复核 |
| ATGM336H-6N-74 | UART0(RXD0/TXD0) → J3；1PPS → GPIO50；VCC_RF(pin 14) 作两路偏置 Tee PMOS(Q4/Q5) 源极电源，供模块检测有源天线 | 18 脚 LCC，pin 图已核（手册 §2.3、§2.7）|
| GNSS 天线 | 第三个 U.FL | V1 全外置天线 |

## 7. 已知冲突/待核清单

1. ~~BNO085 的 I2C 地址与模式绑带~~ **已用导出网表复核：SA0/PS0/PS1=GND，地址 0x4A，I2C 模式。**
2. CC1312R 片内 48M 负载电容阵列范围 → 决定 Abracon 7pF 或 KDS 12pF。
3. TA0970A 的输入/输出阻抗与匹配值（datasheet 拿到后重算，当前值仅为占位）。
4. J3 排母堆叠后 USB-C（RP2040 刷机口）的开口方向与 4.3 寸外壳（docs/jlc/lcd-4.3in/3d-case）干涉检查 — PCB 阶段做。
5. QMC5883P 与 RP2040/DCDC 的距离约束（磁力计洁净区）— PCB 阶段布局约束文档里定。
6. ~~QPL9547 EN 脚极性~~ **已核实（Qorvo DS Rev.D）：SD 脚 <0.63V=LNA ON，接地使能正确**。
7. （原理图阶段新增）TLV3501 SHDN 已拉高使能（TI 口径低电平关断），TOKMAS 版本极性一致性复核。
8. （原理图阶段新增）KiCad 官方 CC1312R 符号无 DIO_0——布 PCB 前逐脚对 TI SWRS210 复核一遍 QFN48 脚位。
9. ~~AD8319 CLPF 悬空~~ —— 2026-08-26 改用 AD8313（无 CLPF 脚），本条作废。
10. ~~978 天线口无 ESD 管~~ **已处理：D3 已入图（与 1090 对齐，航空产品外露端口一律加 ESD）**。
11. （原理图阶段新增）32k 晶振负载电容 12/15pF 不对称照抄原图，正常应对称——评审讨论项。
12. ~~IFA长度bring-up~~ **已于2026-09-03在V4.0实板完成：** 从约54mm切到50.0mm外包络后，
    中心频点约1090MHz；π网络直通状态下得到上述实测值。辐射效率及实际报文数量/距离A/B仍待验证。

## 8. 器件版本核对清单（用户要求：确保 datasheet 版本 = 实际采购版本；到货逐项核对后才允许贴片）

| 器件 | 建库依据的 datasheet 版本 | 采购 SKU | 到货核对点 |
|---|---|---|---|
| BNO085 | CEVA 1000-3927 **v1.16**（BNO080/085/086 引脚相同） | 淘宝 ¥56 档散新 | 丝印 "BNO085"（警惕 "BN0085" 仿标）；LGA-28 3.8×5.2mm 实measure |
| ~~AD8319~~ | — | — | **2026-08-26 弃用**：改 AD8313 单一方案（动态范围 70dB vs 40dB，且实购更便宜） |
| AD8313 | ADI Rev.F | 立创 C578690（仅此渠道） | 丝印 "J1A"；MSOP-8 |
| BMP388 | Bosch DS001-07 Rev 1.7 | 立创 C779278 原厂 | LGA-10 2×2mm；到货抽测 CHIP_ID=0x50 |
| QMC5883P | QST 13-52-19 RevA（**即立创 C2847467 商品页挂的同一份**） | 淘宝 ¥5.5 档或立创 | LGA-16 **3×3mm**（勿与 L 版小封装混）；I2C 0x2C 应答 |
| ATGM336H-6N-74 | 中科微 6N 用户手册（pin 表 §2.3）+ 5N 手册 land pattern（§2.2，两代同封装 pin 兼容） | 淘宝 ¥11 档（认"全模/GALILEO"文案） | 丝印含 "6N-74"；上电 NMEA 里 GSV 应出现 GA（Galileo）语句——这是单北斗/全模最硬的判据 |
| MM8930-2620 | Murata O30E 目录 2025-12 版（RJ4/RK15 仅包装卷带差异，同一料） | 立创 C6227587 (RJ4) | 1.6×1.6mm；直通导通测试（不插探头 R-C 通） |
| TA0970A | TST Rev 3.0（**6 焊盘**） | 立创 C7115531（标 "8P"，冲突已备案） | **到货第一件事：数焊盘**。若实物 8 焊盘则封装作废重画，PCB 留到实物确认后再投 |
| CC1312R1F3RGZR | KiCad 官方符号（画原理图时逐脚对 TI SWRS210 复核） | 淘宝 ¥11 原装档 | 丝印 CC1312R1F3；QFN48 7×7 |
| W25Q128JVSIQ | Winbond JV 系（官方符号 W25Q128JVS） | 立创 C97521 | SOIC-8 208mil 宽体（封装选型点，勿按 150mil 画） |

**规则**：核对未通过的料一律不上板；TA0970A 未实物确认前，PCB 不投产。
