# 扩展板 V1 — 权威引脚/网络映射（原理图单一事实来源）

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

固件侧改动量：仅新增一路 UART 驱动（GPIO46/32），其余零改动。

## 2. RP2040 引脚分配〔D，PIO 分组对齐 A 的架构〕

| RP2040 GPIO | 网络 | 功能 |
|---|---|---|
| GPIO0 / GPIO1 | ADSB_TXD / ADSB_RXD | UART0 → P4（GDL90/raw 报文 + 控制） |
| GPIO2 / GPIO3 | BIAS_EN_1090 / BIAS_EN_978 | 天线偏置 Tee PMOS 栅极（低有效，10k 上拉默认关断）〔D〕 |
| GPIO10 / 11 / 12 / 13 | SUBG_SCK / SUBG_MOSI / SUBG_MISO / SUBG_CSN | SPI1 master → CC1312R slave〔A〕 |
| GPIO14 / 15 | SUBG_IRQ / SUBG_SYNC | CC1312R 中断 + 同步/bootloader 触发〔A〕 |
| GPIO16 / 17 / 18 | SUBG_TMSC / SUBG_TCKC / SUBG_RESET | cJTAG 代刷 CC1312R 固件〔A〕 |
| GPIO19 | PULSES | 比较器输出 → PIO0 前导检测〔A〕 |
| GPIO20–23 | DEMOD0–3 | PIO 解调状态机联络/调试〔A〕 |
| GPIO24 | RECOVERED_CLK | 调试用恢复时钟〔A〕 |
| GPIO25 | TL_PWM | 比较器阈值 PWM（RC 滤波成 LEVEL_BIAS）〔A〕 |
| GPIO26 / ADC0 | LEVEL_BIAS_SENSE | 阈值直流回读〔A〕 |
| GPIO27 / ADC1 | RSSI | AD8319 VOUT 回读（自适应灵敏度）〔A〕 |
| GPIO28 / ADC2 | 备用 ADC | 预留（如 978 RSSI） |
| QSPI 专用脚 | W25Q128JVSIQ | 16MB 固件+配置 |
| XIN | 12MHz 晶振（C9002） | |
| USB_DP/DM | USB-C 座 | UF2 拖拽刷机 + CDC 调试〔D〕 |
| RUN + BOOTSEL | 轻触开关 ×2 | 开发板必备〔D〕 |
| SWD（SWCLK/SWDIO） | ~~3-pin 测试点~~ **V4 已删除** | 测试点 TP1–7 于 2026-08-23 移除，理由见 REVIEW_TODO.md A 节；固件调试在已打样的 v3.2 上做 |

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

978 差分匹配起点值〔A〕：L 7.5nH / 27nH / 6.8nH×2；C 3.6pF×2 / 2.7pF / 6.2pF。915→978 频点偏移，V1 全部留可调位。

## 4. 电源树〔D，比 A 多拆一路 RF LDO〕

```
J3 VCC_5V ──┬── ME6211C33 (500mA) ──→ 3V3_DIG：RP2040、Flash、CC1312R(经磁珠)、BNO085、BMP388、QMC5883P
            ├── TPS7A2033 #1 ──────→ 3V3_RF：QPL9547、BGA2817、AD8319/AD8313、MCP 比较器域
            └── TPS7A2033 #2 ──────→ 3V3_GNSS：ATGM336H-6N-74 + 两路天线偏置 Tee
偏置 Tee（×2：1090 / 978）〔A〕：PMOS 高侧开关 + 6V/200mA 保险丝 + 100nH 馈电电感 + ESD(0.6pF 级)
```

## 5. 1090 接收链逐级网络〔A 拓扑 + 我方选型〕

```
板载IFA → ZP1并 → ZS1串 → ZP2并 → C53 100pF ─┐
                                                ├→ U16射频开关 → C54 → QPL9547(LNA①)
J6外接U.FL → 偏置Tee → C30 100pF ─────────────┘
→ C 12pF → TA0970A(SAW①) → 同上匹配 → BGA2817(LNA②) → C 12pF → TA0970A(SAW②)
→ MM8930-2620RJ4(产测座, 串联) → C 3pF 耦合 → AD8319(主) / AD8313(备, 双封装位)
→ RSSI/VOUT ─┬→ RP2040 ADC1
             └→ TLV3501 IN+；IN- = LEVEL_BIAS(TL_PWM 经 100k+0.1µF RC) + 10k 迟滞〔A〕
→ TLV3501 OUT = PULSES → RP2040 GPIO19
测试点：W.FL ×2（LNA② 后 / 检波器入口）〔A〕
```

全部 LC 值为〔A〕起点，V1 每个位号留 0402 可调位；SAW 换型（TFS1090F→TA0970A）后匹配值需在板上实调。

板载IFA的J7挂在π网络之后、C53之前。用J7接VNA测量时C53不贴，避免后级并联进读数。
当前π网络默认ZS1=0R、ZP1/ZP2=DNP只是直通；完整六层rev2 HFSS首轮可从ZS1=3.6nH、ZP2=3.3pF、
ZP1=DNP起扫，最终值以装盒实测为准。生成器/封装库/PCB内嵌ANT1已统一为
52.0mm画长/53.5mm外包络，taper和5.103mm馈电路径已落板，详见
`../expansion-board-v3/IFA_HFSS_2026-08-24.md` §8。

## 6. 传感器 / GNSS（直连 P4，不过 RP2040）〔F 架构不变〕

| 器件 | 总线/脚 | 备注 |
|---|---|---|
| BNO085 | I2C addr 0x4A/0x4B + IMU_INT(GPIO34) + IMU_RST(GPIO28) | PS0/PS1 拉低选 I2C 模式；addr 脚接法建库时定 |
| BMP388 | I2C addr 0x76（baro_task.c:29）+ BARO_INT(GPIO31) | 壳体留通气孔 |
| QMC5883P | I2C（addr 建库时从 datasheet 定）| 远离电感/大电流走线 |
| ATGM336H-6N-74 | UART0(RXD0/TXD0) → J3；1PPS → GPIO50；VCC_RF 馈有源天线 | 18 脚 LCC，pin 图已核（手册 §2.3）|
| GNSS 天线 | 第三个 U.FL | V1 全外置天线 |

## 7. 已知冲突/待核清单

1. BNO085 的 I2C 地址脚（SA0）与 PS0/PS1 接法 — 建库时从 CEVA datasheet 核。
2. CC1312R 片内 48M 负载电容阵列范围 → 决定 Abracon 7pF 或 KDS 12pF。
3. TA0970A 的输入/输出阻抗与匹配值（datasheet 拿到后重算，当前值仅为占位）。
4. J3 排母堆叠后 USB-C（RP2040 刷机口）的开口方向与 4.3 寸外壳（docs/jlc/lcd-4.3in/3d-case）干涉检查 — PCB 阶段做。
5. QMC5883P 与 RP2040/DCDC 的距离约束（磁力计洁净区）— PCB 阶段布局约束文档里定。
6. ~~QPL9547 EN 脚极性~~ **已核实（Qorvo DS Rev.D）：SD 脚 <0.63V=LNA ON，接地使能正确**。
7. （原理图阶段新增）TLV3501 SHDN 已拉高使能（TI 口径低电平关断），TOKMAS 版本极性一致性复核。
8. （原理图阶段新增）KiCad 官方 CC1312R 符号无 DIO_0——布 PCB 前逐脚对 TI SWRS210 复核一遍 QFN48 脚位。
9. （原理图阶段新增）AD8319 CLPF 悬空为 V1 起点（视频带宽最大），若脉冲底噪高则加电容位。
10. ~~978 天线口无 ESD 管~~ **已处理：D3 已入图（与 1090 对齐，航空产品外露端口一律加 ESD）**。
11. （原理图阶段新增）32k 晶振负载电容 12/15pF 不对称照抄原图，正常应对称——评审讨论项。
12. （IFA实板待办）冻结输入已同步封装/PCB/馈线；实板dF/dL、装盒偏移、
    π网络量产值和接收效率仍待打板验证。

## 8. 器件版本核对清单（用户要求：确保 datasheet 版本 = 实际采购版本；到货逐项核对后才允许贴片）

| 器件 | 建库依据的 datasheet 版本 | 采购 SKU | 到货核对点 |
|---|---|---|---|
| BNO085 | CEVA 1000-3927 **v1.16**（BNO080/085/086 引脚相同） | 淘宝 ¥56 档散新 | 丝印 "BNO085"（警惕 "BN0085" 仿标）；LGA-28 3.8×5.2mm 实measure |
| AD8319 | ADI Rev.D | 淘宝 ¥21.9 原装配单档 | 丝印 "Q2"；LFCSP-8 2×3mm 带裸焊盘 |
| AD8313 | ADI Rev.F | 立创 C578690（仅此渠道） | 丝印 "J1A"；MSOP-8 |
| BMP388 | Bosch DS001-07 Rev 1.7 | 立创 C779278 原厂 | LGA-10 2×2mm；到货抽测 CHIP_ID=0x50 |
| QMC5883P | QST 13-52-19 RevA（**即立创 C2847467 商品页挂的同一份**） | 淘宝 ¥5.5 档或立创 | LGA-16 **3×3mm**（勿与 L 版小封装混）；I2C 0x2C 应答 |
| ATGM336H-6N-74 | 中科微 6N 用户手册（pin 表 §2.3）+ 5N 手册 land pattern（§2.2，两代同封装 pin 兼容） | 淘宝 ¥11 档（认"全模/GALILEO"文案） | 丝印含 "6N-74"；上电 NMEA 里 GSV 应出现 GA（Galileo）语句——这是单北斗/全模最硬的判据 |
| MM8930-2620 | Murata O30E 目录 2025-12 版（RJ4/RK15 仅包装卷带差异，同一料） | 立创 C6227587 (RJ4) | 1.6×1.6mm；直通导通测试（不插探头 R-C 通） |
| TA0970A | TST Rev 3.0（**6 焊盘**） | 立创 C7115531（标 "8P"，冲突已备案） | **到货第一件事：数焊盘**。若实物 8 焊盘则封装作废重画，PCB 留到实物确认后再投 |
| CC1312R1F3RGZR | KiCad 官方符号（画原理图时逐脚对 TI SWRS210 复核） | 淘宝 ¥11 原装档 | 丝印 CC1312R1F3；QFN48 7×7 |
| W25Q128JVSIQ | Winbond JV 系（官方符号 W25Q128JVS） | 立创 C97521 | SOIC-8 208mil 宽体（封装选型点，勿按 150mil 画） |

**规则**：核对未通过的料一律不上板；TA0970A 未实物确认前，PCB 不投产。
