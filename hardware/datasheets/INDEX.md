# 器件手册库 · 判据索引

本目录存放扩展板（v3/v4 同源）所用器件的**厂商原始手册**。PDF 本体不进 git（见 `.gitignore`），
本文件是**判据索引**，进 git——因为真正防错的是判据，不是文件。

## 为什么有这份索引

2026-08-30 的教训：判定 GNSS 天线切换电路时，先用搜索摘要 + 对方框图的目测下了"接法正确"的结论，
结果与事实相反。根因是 **Skyworks AS179-92LF 的新版手册（200176J, 2022）删掉了真值表**，只留方框图；
而 2004 版第 3 页有明确的 Truth Table。摘要把方向说反了，方框图也被读反。

**规则：引脚定义、真值表、极性这三类判据，只认本目录里手册原文的表格。搜索摘要、方框图目测、
"pin-to-pin 兼容"的口头说法，一律不作为判据。** 同一器件如果新旧版手册内容有出入，两版都留。

## 索引

| 器件 | 位号 | 文件 | 已提取的关键判据 |
|---|---|---|---|
| XA17-G4K（信路达）| U16, U17 | `XA17-G4K.pdf`（中文 2020）<br>`XA17-G4K_lcsc.pdf`（英文 Rev 2.3 2023）| **实际贴片料**。引脚：1=J3, 2=GND, 3=J2, 4=V1, 5=J1, 6=V2。真值表：**Vcont1 高/Vcont2 低 → J1-J3 通**；Vcont1 低/Vcont2 高 → J1-J2 通。高电平 2.0–5.3V，低 0–0.2V。<br>**两份文档判据逐字一致**（中/英、相隔三年），可互为交叉验证。体积差 7 倍只是排版差异：中文版是高分辨率图片排版（1.96MB），英文版是矢量重排（285KB）。<br>⚠️ 立创那份的 PDF 元数据 Title 残留着 `XL9535, XL9555 Datasheet`（立创转制时模板复用没清），`pdfinfo` 一看会以为下错了文件，**正文内容确实是 XA17-G4K** |
| AS179-92LF（Skyworks）| U16, U17 备选 | `AS179-92LF_rev2004_truthtable.pdf` | **2004 版，p3 含 Truth Table**：V1 高/V2 = 0 → J1-J2 隔离、J1-J3 导通。引脚与 XA17-G4K 一致 |
| AS179-92LF（Skyworks 2016）| — | `AS179-92LF_rev2016.pdf` | **200176H，Table 4 有真值表**，与 2004 版一致。**独有脚注（重要）**：「Any state other than described in this table places the device in an **undefined state**. An undefined state **does not damage the device**.」→ 上电默认 V1=V2=高属未定义态，但**不会损坏器件** |
| AS179-92LF（新版）| — | `AS179-92LF_200176J_2022.pdf` | **2022 版已删除真值表**，只有 Figure 1 方框图。查真值表请用 2004 或 2016 版 |
| TOKMAS 仿厂 | U16, U17 可替代 | `AS179_clone_TOKMAS.pdf` | 真值表与原厂一致（V1=1/V2=0 → J1-J3 通）。明确写「V1 与 V2 电压**反相**」 |
| TECH PUBLIC TPAS179-92LF | U16, U17 可替代 | `AS179_clone_TechPublic.pdf` | 逻辑与原厂一致，但**引脚命名不同且反直觉**：`RF2=pin1`、`RF1=pin3`、`RFC=pin5`、`VC1=pin4`、`VC2=pin6`。真值表写作「VC1 High → RFC-RF2 通」，RF2 即 pin1，等价于原厂「V1 高 → J1-J3 通」。⚠️ 按"RF1 该在 pin1"的直觉读会得出**相反**结论 |
| AO3401A（AOS）| Q2–Q5 | `AO3401A.pdf` | P 沟道 MOSFET，SOT-23：1=G, 2=S, 3=D。**低电平导通**（Vgs 需为负），用作高边开关时源极接电源 |
| TPS7A20（TI）| U2, U3 | `TPS7A20.pdf` | SOT-23-5(DBV)：1=IN, 2=GND, 3=EN, 4=N/C, 5=OUT。EN 内部 500k 下拉，默认关断，须外部拉高 |
| RP2040 | U8 | `RP2040.pdf` | 引脚表在 §5.5.2（p612）。GPIO0=pin2 起连续，GPIO8=pin11（**pin10 是 IOVDD，编号不连续**）。IOVDD=1,10,22,33,42,49；DVDD=23,50；VREG_VIN=44；VREG_VOUT=45；USB_VDD=48；ADC_AVDD=43；**TESTEN=19 必须接地**；GND=57(EP)。USB_DP=47/USB_DM=46，**须外接 27Ω 串阻** |
| W25Q128JV（Winbond）| U9 | `W25Q128JV.pdf` | SOIC-8：1=/CS, 2=DO(IO1), 3=/WP(IO2), 4=GND, 5=DI(IO0), 6=CLK, 7=/HOLD(IO3), 8=VCC |
| BNO085 | U4 | `BNO085.pdf` | LGA-28 引脚表见 Figure 1-6。**协议选择 Figure 1-5：PS1=0,PS0=0 → I²C**（复位时采样）。**时钟源 Figure 1-8：CLKSEL0=1 且 CLKSEL1=0/悬空 → 内部振荡器**。SA0(pin17) 接地 → 地址 0x4A。I²C 模式下 H_CSN(pin18) 接高 |
| BMP388（Bosch）| U5 | `BMP388.pdf` | LGA-10 Table 50：1=VDDIO, 2=SCK/SCL, 3=VSS, 4=SDI/SDA, 5=SDO/SA0, 6=CSB, 7=INT, 8=VSS, 9=VSS, 10=VDD。**I²C 模式 CSB 必须接 VDDIO**；SDO 接地 → 0x76 |
| AD8313（ADI）| U14 | `AD8313.pdf` | MSOP-8 Table 4：1,4=VPOS, 2=INHI, 3=INLO, 5=PWDN, 6=COMM, 7=VSET, 8=VOUT。**PWDN 接地才是正常工作**；**RSSI 模式须把 VSET 与 VOUT 短接** |
| TLV3501（TI）| U15 | `TLV3501.pdf` | SOT-23-6(DBV)：1=−IN, 2=V−, 3=+IN, 4=V+, 5=OUT, 6=SHDN |
| SY6970（矽力杰）| U19 | `SY6970_DS.pdf`（**正式 DS Rev0.9B**）<br>`SY6970QCC-shot-1.jpg`（引脚图）<br>`SY6970QCC-shot-0.jpg`（典型应用电路）<br>`SY6970.pdf`（AN 版）| 正式版与 AN 版引脚表逐脚一致。典型应用值：BUS 1µF、PMID **10µF**、LX 1µH、BST-LX 47nF、SYS 10µF×2、BAT 10µF、REGN 4.7µF、NTC 分压 REGN→5.52k→节点→31.23k→GND 并联 10k(103-AT)。**STAT/INT 均需 10kΩ 上拉**。引脚：1=BUS, 2=DP, 3=DM, 4=STAT, 5=SCL, 6=SDA, 7=INT, 8=OTG(高有效), 9=/CE(**低有效，不可悬空**), 10=ILIM, 11=NTC, 12=QON, 13-14=BAT, 15-16=SYS, 17-18=PGND, 19-20=LX, 21=BST, 22=REGN, 23=PMID, 24=DSEL, EP=PGND。**NTC 分压必须从 REGN 取**；BST-LX 间 47nF；PMID 对地 10µF；BAT 对地 ≥10µF |
| CH224（沁恒）| U18 | `CH224.pdf` | CH224K 引脚表 Table 4-2：0=GND(底板), 1=VDD, 2=CFG2, 3=CFG3, 4=DP, 5=DM, **6=CC2, 7=CC1**, 8=VBUS, 9=CFG1, 10=PG。**⚠️ CH224K 不用电阻档位，用 §6.2 的三位 I/O 电平表**：`CFG1/CFG2/CFG3 = 000 → 9V`、`001→12V`、`011→15V`、`010→20V`、`1XX→5V`。§7.1 另注明「**CH224K 的 CFG2/CFG3 内部无上拉**」，故三脚必须都给确定电平（板上 R37/R48/R49 三颗 0R 拉低）。<br>❌ **不要用电阻档位表**：§5.2.1 表 5-1（`6.8k=9V/24k=12V/56k=15V/120k=20V/210k=28V`）只适用 **CH224Q/CH224A**；而「NC=20V」出自 §6.3 的 **CH224D** 表。本索引 2026-08-31 前误把这两张表挂在 CH224K 名下，已更正。VDD 须串电阻至 VBUS + 1µF 对地；VBUS 脚须串电阻至外部 VBUS。**§6.2 的 CH224K 参考原理图中 CC1/CC2 直连 Type-C，未画外部 5.1k**（而 §6.3 CH224D、§6.4 CH221K 的图里画了 5.1k）|
| QPL9547（Qorvo）| U11 | `QPL9547.pdf` | DFN-8 2x2，p3 Pad Configuration：**1=Vbias（设定偏置电流，不是电源）**、2=RF In、**6=Shut Down（高于 1.17V 关断，接地或低于 0.63V 才 ON）**、**7=RF Out 同时是 VDD 供电脚**、3/4/5/8=NC、背面焊盘=GND。p4 评估板原理图：VDD 经 **L1 18nH 扼流圈馈到 pin7**，另经 **R4 3.32K 到 pin1 Vbias**，pin1 再经 100pF 旁路到地。VDD 范围 3.15–5.25V |
| BGA2817（NXP，已停产）| U12 | `BGA2817.pdf` | p1 Table 1 Pinning：1=VCC, 2 和 5=GND2, 3=RF_OUT, 4=GND1, 6=RF_IN。内部匹配 50Ω，**不需要输出电感**，输入输出隔直电容 ≤100pF |
| ME6211（南京微盟）| U1 | `ME6211.pdf` | SOT23-5：1=VIN, 2=VSS, 3=CE, 4=NC, 5=VOUT。CE 接 VIN = 常使能 |
| ATGM336H-6N（中科微）| U7 | `GNSS_C5804601.pdf`（**正确型号 6N，文中含 6N-74**）<br>`ATGM336H.pdf`（5N 版，备查）| LCC-18，两版引脚一致：1=GND, 2=TXD, 3=RXD, 4=1PPS, **5=ON/OFF（低有效关断，须拉高）**, 6=VBAT(RTC 备份), 7=NC, 8=VCC, **9=nRESET（低有效，不用时悬空）**, 10=GND, 11=RF_IN, 12=GND, **14=VCC_RF（输出 +3.3V，可直接给有源天线供电）**, 16=RXD1, 17=TXD1, 7/13/15/18=Reserved(悬空) |
| CC1312R（TI）| U10 | `CC1312R.pdf` | QFN-48(RGZ)，Table 7-1：1=RF_P, 2=RF_N, 3=RX_TX(LNA 可选偏置), 4=X32K_Q1, 5=X32K_Q2, 6–12=DIO_1..7, 13=VDDS2, 14–21=DIO_8..15, 22=VDDS3, 23=DCOUPL, 24=JTAG_TMSC, 25=JTAG_TCKC, 26–32=DIO_16..22, 33=DCDC_SW, 34=VDDS_DCDC, **35=RESET_N（低有效，无内部上拉）**, 36–43=DIO_23..30, 44=VDDS, 45=VDDR, 46=X48M_N, 47=X48M_P, 48=VDDR_RF, EGP=唯一接地。注(2) DCOUPL/VDDR **不得给外部电路供电**；注(5) 不用内部 DC/DC 时 VDDR_RF 必须接 VDDR |
| SY7069（矽力杰）| U20 | `SY7069.pdf` | TSOT23-6：**1=FB, 2=IN, 3=GND, 4=OUT, 5=LX, 6=EN**。VOUT=1.2×(1+RH/RL)；电感接在 **IN 与 LX 之间**；OUT 对地 ≥22µF、IN 对地 ≥1µF；**EN 拉高开启，不可悬空** |
| QMC5883P（QST）| U6 | `QMC5883P.pdf` | LGA-16，Table 5：1=SCK, 2=VDD, 3–8=NC, 9=GND, 10=C1(储能电容), 11=GND, 12–15=NC, 16=SDA。I²C 上拉建议：总线 <10cm 用 2.7k、<5cm 用 4.7k |
| TA0970A（TAI-SAW）| FL1, FL2 | `TA0970A.pdf` | 1090MHz SAW 滤波器，六脚：**B=输入, E=输出, A/C/D/F=地**（见手册测量电路）。50Ω 系统**无需匹配网络**；插损 typ 2.3dB；最大输入 20dBm、**DC 电压 0V**（前级必须隔直）|
| TPESD8L3.3CT5G | D2, D3 | `TPESD8L3.3.pdf` | **双向 TVS（Bi-directional），无极性要求**，两个方向装都对。0402 二脚，结电容 typ 0.3pF，工作电压 3.3V |
| BK4819（Beken）| U21 | `BK4819_Datasheet_V1.0.pdf`（22 页正式 DS）<br>`BK4819_AppNote_v1.0.pdf`（寄存器/驱动 + §23 硬件设计参考原理图）| QFN-32 引脚（AppNote §23 参考原理图 + DS Table 引脚表）：1=GPIO5, 2=GPIO6, **3=VCC**, **4=XO(输出)**, **5=XI(输入)**, 6/7=NC, **8=EARO(模拟音频输出)**, 9/10=NC, **11=VCC**, 12=NC, 13=MICN, 14=MICP, **15=LNAIN**, 16=NC, 17=PAOUT, 18=VRAMP, 19–24=NC, 25=SCK, 26=SCN, 27=SDATA, 28–32=GPIO0–4, EP(33)=GND。<br>**§1.2.3：默认晶体 26MHz，「frequency tolerance of the crystal should be within 2.5ppm」**；§2.2 注再次强调「**within ±2.5ppm during all operating conditions**」（即含温漂，不是只算 25°C 初始容差）。<br>§1.2.4 输入匹配：C1 隔直推荐 4.7pF，**PIN15 在 RX 模式直流约 0V**，L1 推荐 NC（该值针对 UHF；120MHz 需另配）。<br>§1.2.5 晶振口是**并联谐振晶体**设计（C1/C2 对地 + Cvar，Cpar≈1pF），**手册未给用 TCXO 外部注入 XI 的电路** —— 用 TCXO 打 XI 属超出手册范围的常见用法，必须交流耦合。<br>Table 2 VDD **2.6–3.6V**；Table 3 **IRX typ 42mA** / ITX 34mA / 休眠 200µA |
| PCM1808（TI）| U22 | `PCM1808.pdf`（SLES177B）| TSSOP-14 §5：1=VREF(**去耦用，=0.5×VCC，非电源脚**), 2=AGND, **3=VCC 模拟 5V**, **4=VDD 数字 3.3V**, 5=DGND, 6=SCKI, 7=LRCK, 8=BCK, 9=DOUT, 10=MD0, 11=MD1, 12=FMT, **13=VINL**, 14=VINR。<br>**VCC 推荐 4.5/5/5.5V**（不是 3.3V 容忍）；VDD 2.7/3.3/3.6V。SCKI 须 256/384/512 fS。<br>脚注：**pins 10–12 内部自带 50kΩ 下拉**，MD0/MD1/FMT 全接地 = Slave + I²S 24-bit；SCKI/MD0/MD1/FMT 均 5V 容忍。停 SCKI/BCK/LRCK 即掉电复位 |
| TCXO 26MHz（Seiko Epson）| Y4 | `TG-5006CJ_CG_CE_brief_en.pdf`（系列 brief sheet）<br>`TG2520SMN_TG2016SMN_brief_en.pdf`（候选替换件）<br>`Epson_TCXO_product-search_tcxo_{a,b}.json`（**Epson 官方选型库，单品级参数的唯一来源**）| 实贴料 **TG-5006CG-12H 26.000MHz，订货号 X1G004211000300 / 立创 C1998253**。<br>**⚠️ 型号里的 ③Spec segment 在 brief sheet 上写的是「Please contact us」，单品参数手册上查不到**，只能查选型库（抓法见下面第 5 条）。库里该行（`pn=X1G0042110003`）：**`function=TCXO`（不是 VC-TCXO）、`fcfYN=N`（无压控）** → **pin1 = N.C.，悬空正确**；**`supplyVoltage 2.85 / 3.0 / 3.15 V`**；`frequencyTol25C=±2ppm`、`frequencyTolOpTemp=±0.5ppm`(-30~+85°C)；`Icc≤1.5mA`；2.5×2.0×0.8mm。<br>❌ **DigiKey 把这颗标成「VCTCXO」是错的**，立创标成「Standard Oscillator」也不准；两家都写 Vcc「1.7–3.3V」（那是系列总范围，不是本档额定）。**分销商参数栏不作为判据**。<br>brief sheet 通用判据：Pin map **1=N.C.(TCXO)/Vc(VC-TCXO), 2=GND, 3=OUT, 4=Vcc**，N.C. 只能悬空或接地、不能当地用；**Output load = Load_R 10kΩ // Load_C 10pF，条件栏写死 `DC cut capacitor = 0.01 µF`**（Symmetry 的测试条件也是 `GND level (DC cut)`）→ **输出必须交流耦合**；Vcc 需 0.01–0.1µF 旁路且尽量靠近；输出削峰正弦 Vpp≥0.8V；系列供电总范围 1.7–3.465V（**不是 4.0V**）。<br>✅ **频差判定：这颗料合格，不要换。** BK4819 要求晶振 ≤±2.5ppm，本料 ±2.0ppm@25°C + ±0.5ppm 温漂，两项头条指标都在线内。曾有一版审计把两项线性相加再外推一个超压项判它"超线"并建议换 `TG2520SMN` —— **该结论已撤回**：①那个超压外推超出 `f0-Vcc` 规格自己写的 `Vcc±5%` 窗口，无手册依据；②把"25°C 容差+温漂+首年老化≤2.5ppm"拿去筛全库 19 款 26MHz 削峰正弦 TCXO，**只有 5×3.2mm 的 TG5032 系列能过，所有 2520/2016 封装全挂，包括那个被推荐的替换件本身**——判据否掉自己推荐的解，说明判据错了；③BK4819 自带 AFC（AppNote `REG_73<4>`，默认 Enable），初始容差本可吸收。<br>⚠️ **`X1G0054210304 TG2520SMN` 不能拿来当替换件**：同一份选型库里它 **`fcfYN='Y'`（有频率控制功能）**，按本行上面立的判据就意味着 pin1 是 Vc 而非 N.C.，而本电路 `Y4.1` 悬空 —— 换了正好制造出"压控脚浮空"这个故障。**选替换件必须把 `fcfYN` 再查一遍，别只比频差和封装。**<br>📌 未决的是**供电**不是选型：`Y4.4` 接 3.3V 而本档额定 2.85/3.0/3.15V。不损坏（系列范围 1.7–3.465V），影响按 `f0-Vcc` 量级是 sub-ppm，**打板后台架实测频偏即可**。库内 26MHz 的 TG-5006 共 6 个订货号，**全部是 3.0V 档，没有 3.3V 档** |

## 待补手册的器件

| 器件 | 位号 | 情况 |
|---|---|---|
| 晶振 ×3 / NTC / USB-C 座 / MX1.25 | Y1–Y3, RT1, J4, J9 | 无源件与机械连接器，引脚由封装标准约束，风险低 |

（QMC5883P 与 TA0970A 由产品负责人从立创商城手动下载补齐，对应立创料号 C2847467 / C7115531。）

## 怎么把手册抓下来（实测有效的路径）

第一次尝试时误判成"拿不到"，实际都是可以的，方法记在这里：

1. **立创系（有 C 编号的料）**：`https://www.lcsc.com/datasheet/{C编号}.pdf` **不是 PDF**，是个 Nuxt 渲染的查看器页面。
   把这个页面抓下来，里面有真实地址 `https://datasheet.lcsc.com/datasheet/pdf/{32位hash}.pdf`，带 Referer 再取即可。
   ME6211 / ATGM336H / TPESD8L3.3 / SY7069 都是这么下到的。
2. **Qorvo**：站点挂 Vercel Security Checkpoint（JS challenge），`curl` 一律 429/拦截，**必须用真浏览器**。
   产品页 `/products/p/{型号}` 上的 "Product Data Sheet" 链接指向 `/products/r/{id}`，同域 fetch 后用
   `a[download]` 触发浏览器下载即可落盘。注意 `/products/d/{id}` 是另一套资源 ID，猜错会下到别的产品的 Gerber 包。
3. **已停产料（如 BGA2817，NXP 官网 404）**：`datasheet4u.com` 的详情页带逐页 PNG
   （`/pdfhtml/{dir}/{id}/page-00000N.png`），直接抓图看比找 PDF 快。
   ⚠️ **它只放前 3 页做预览**，第 4 页起一律 404（315 字节的 HTML，不是图）。BK4819 的
   §1.2.3 正好在第 4 页 —— 别据此以为"手册里没这一节"。
4. 通用：`pdftotext -layout` 转文本后 grep，比读图快也不会看错方向。
   **`pdftotext` 输出 0 行 = 扫描件**（立创转制的 Epson 那份就是），改用
   `pdftoppm -r 200 -png x.pdf out` 转图再读，别以为是文件坏了。
5. **Epson（单品级参数，手册上查不到的那种）**：Epson 只发**系列 brief sheet**，型号里的
   spec segment 写着 "Please contact us"。但官网选型工具的数据是**静态 JSON**，可以直接取：
   ```bash
   # 路径写在 https://www.epsondevice.com/crystal/assets/js/ps_common.js 的 jsonpath 变量里
   curl -o tcxo_a.json https://download.epsondevice.com/td/ps/tcxo_a.json   # TCXO，另有 tcxo_b.json
   # 同目录还有 vcxo_*.json / spxo_*.json / ss_osc.json 等，文件名列在 ps_xo.js 里
   ```
   按订货号（`pn` 字段，注意库里是 13 位 `X1G0042110003`，而卖家写的是 15 位
   `X1G004211000300`，**要用子串匹配**）查 `function` / `fcfYN` / `supplyVoltage*` /
   `frequencyTol*`。**这是判定「是 TCXO 还是 VC-TCXO」「额定 Vcc 是哪档」的唯一权威来源**，
   分销商参数栏两家能给出互相矛盾的答案（见上表 Y4 行）。
6. **国产 RF 芯片官网要注册才给下载（如 Beken）**：先看社区镜像。BK4819 的正式 DS 在
   `github.com/amnemonic/Quansheng_UV-K5_Firmware`（`hardware/` 目录），把 `/blob/` 换成
   `/raw/` 直接 curl 即可；alldatasheet 挂 Cloudflare challenge，别浪费时间。

## 用法

```bash
# 手册转文本后 grep，比读图快也比读图准
pdftotext -layout hardware/datasheets/XA17-G4K.pdf - | grep -A10 "真值表"
```
