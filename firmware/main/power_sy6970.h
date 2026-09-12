/*
 * power_sy6970.h — SY6970 充电芯片寄存器解码纯层（WP-D Task 3）。
 *
 * v4 powered 板上 I²C 0x6A 的 SY6970 充电芯片（见 power_service.h 文件头）。
 * 本单元只做**纯解码**：把 REG0B..REG12 连续寄存器窗口的字节帧翻译成
 * 状态/故障/ADC 换算结果，host 单测直编（SY6970_HOST_TEST 隔离，模式同
 * qmc5883p.c）；I²C 胶水与 power_service 注册是 Task 4 的事，这里一概
 * 不碰（连 main.c 都不改）。寄存器事实全部现场取自 hardware/datasheets/
 * 的三份 SY6970 文档互核，逐条页码引用见 power_sy6970.c 文件头取证表。
 *
 * ── 窗口合同（Task 4 的轮询方按此连读）──────────────────────────────
 * regs 是从 SY6970_WIN_REG0 起连续读 SY6970_WIN_LEN 字节的一次 I²C
 * 顺序读的原始结果：regs[0]==REG0B、regs[1]==REG0C、……
 * regs[7]==REG12。窗口含 REG0D（VINDPM）/REG0F（SYSV）两个本层不消费
 * 的寄存器——保留它们换来"一次顺序读拿全状态"，比分段读省总线时间。
 * n >= SY6970_WIN_LEN 即可（多出的字节忽略），不足则拒绝。
 *
 * ── 全 0xFF 容错（QMC 教训的硬约束）─────────────────────────────────
 * 器件缺席/总线被上拉时读回全 0xFF，若照位定义硬解会得到"处处故障
 * 但数据有效"的垃圾态。decode 对整窗全 0xFF 返回 false 并把 *out 清零
 * ——调用方拿到 false 就当作"本拍无数据"，绝不许往下编造。
 *
 * ── 不是电量计 ──────────────────────────────────────────────────────
 * SY6970 只报电压/电流/NTC 百分比，没有库仑计。这里**不出 pct**：
 * 状态栏的电量估算一律由别处按 batt_mv 估（power_service 的 pct_est
 * 语义），Task 4 的 backend 负责把 batt_mv 接过去，别在本层发明换算。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 寄存器窗口：起点 REG0B，连续 8 字节到 REG12（取证：寄存器表里
 * 0B~12 连续无洞；逐址页码引用见 power_sy6970.c 文件头取证表）。 */
#define SY6970_WIN_REG0 0x0B
#define SY6970_WIN_LEN  8

/* 一次解码的结果（字段最小集，消费方：Task 4 的 backend → 状态栏）。 */
typedef struct {
    /* REG0B（只读状态） */
    uint8_t  bus_stat;       /* BUS_STAT[7:5] 原样 0..7（枚举见取证表）*/
    bool     charging;       /* CHRG_STAT=01 预充 或 10 快充           */
    bool     term_done;      /* CHRG_STAT=11 充电终止完成              */
    bool     power_good;     /* PG_STAT：输入电源好                    */
    /* REG0C（只读故障，锁存到被读——语义见取证表，消费在 Task 4）     */
    bool     wd_fault;       /* WATCHDOG_FAULT bit7：看门狗超时        */
    bool     boost_fault;    /* BOOST_FAULT bit6                       */
    uint8_t  chrg_fault;     /* CHRG_FAULT[5:4] 0..3（0 正常/1 输入/
                              * 2 热关断/3 安全定时器超时）            */
    bool     bat_ovp_fault;  /* BAT_FAULT bit3：BATOVP                 */
    uint8_t  ntc_fault;      /* NTC_FAULT[2:0] 原样 0..7               */
    /* ADC 换算结果（REG0E/REG10/REG11/REG12，公式见取证表）           */
    bool     therm_reg;      /* REG0E bit7：正在热调节（降流）         */
    uint16_t batt_mv;        /* BATV：2304 mV + code×20 mV（2.304~4.844）*/
    uint32_t ntc_pct_x1000;  /* NTCPCT：(21% + code×0.465%)×1000，
                              * 21000~80055（0.465×1000=465/code，
                              * 整数无浮点；uint16 装不下 80055）      */
    uint16_t vbus_mv;        /* BUSV：2600 mV + code×100 mV（2.6~15.3V）*/
    uint16_t ichg_ma;        /* ICHGR：code×50 mA（0~6350；VBAT<VSHORT
                              * 时芯片读回 0——见取证表，非解码层过错）*/
    /* 派生位（不是寄存器原值，口径见下）                              */
    bool     vbus_present;   /* BUS_GD=1 且 BUS_STAT≠111(OTG)：OTG 是
                              * 电池对外供电，BUS 在位≠BUS 在供电      */
} sy6970_status_t;

/*
 * 解码一帧寄存器窗口。regs 布局见文件头窗口合同；成功返回 true 并填
 * *out；失败（regs/out 为 NULL、n 不足、整窗全 0xFF）返回 false，且
 * 失败路径已把 *out 清成全零——调用方不需要在 false 后再去猜 out 里
 * 是什么。
 */
bool sy6970_decode_status(const uint8_t *regs, size_t n, sy6970_status_t *out);

/* ── F1 初始化序列（本阶段唯一的写操作豁免：看门狗管理）──────────────
 *
 * SY6970 的 I²C 看门狗超时会把寄存器整体打回默认值（POR 态），1 Hz
 * 轮询的后端必须先把它关掉。序列同时保留喂狗步：即便关狗写失败，喂狗
 * 也能续命到下一轮重试。写前后各有一次 REG00 回读校验：写前是"器件
 * 应答且不在 HIZ"的在位证据；写后按计划约束「写入后必须回读 REG00
 * 验证」，证明 RMW 真的落到了寄存器里。执行器在 Task 4 的目标端胶水里。
 */

/* 序列步骤的操作语义。 */
typedef enum {
    SY6970_SEQ_VERIFY = 0,   /* 回读 reg，要求 (reg & mask) == val      */
    SY6970_SEQ_RMW_CLEAR,    /* reg = reg & ~mask（保其余位，清指定位） */
    SY6970_SEQ_RMW_SET,      /* reg = reg |  mask（保其余位，置指定位） */
    SY6970_SEQ_RMW_FIELD,    /* reg = (reg & ~mask) | val（写位域）     */
} sy6970_seq_op_t;

/* ── IINLIM 输入限流（2026-09-12 实板定值）────────────────────────────
 *
 * 实测 REG00 回读 0x48：IINLIM[5:0]=001000=8=500mA，就是 POR 值——
 * 在此之前固件从未配过这个寄存器，CH224K 谈下来的功率被芯片自己卡在
 * 500mA。板上 U19 的 DP/DM 悬空（V4.4 PCB 焊盘已核对），AUTO_DPDM_EN
 * 的 BC1.2 检测只能判成 SDP，所以这个 500mA 不会自己变好（DS p.15：
 * "IINLIM will be changed according to the adapter type after input
 * DP/DM detection is done. USB Host SDP=500mA"）。
 *
 * 取 2000mA 的依据：硬件 ILIM 脚 R38=180R 给出 I_INMAX=K_ILIM/R_ILIM
 * =375/180≈2.08A（DS p.10 K_ILIM typ 375），而实际限流是 I²C 与 ILIM
 * 脚**两者的较小值**（DS p.15）。设成 2.0A 略低于硬件上限，两道闸都
 * 在起作用；再高就等于把限流全交给硬件，软件侧失去兜底。
 *
 * 电源带不动 2A 不是问题：AICL_EN（REG02[4]）与 VINDPM 都是 POR 使能
 * 的，输入塌陷时芯片自己往回退（DS p.28 Dynamic Power Management）。
 * IINLIM 是上限，不是强制取用值——5V/500mA 的电脑口照样安全。
 */
#define SY6970_IINLIM_CODE 38u    /* IINLIM[5:0]=100110               */
#define SY6970_IINLIM_MA   2000u  /* = 100mA + 50mA×38（DS p.15）     */

/* ── JEITA 高温段：为什么要关掉降压（2026-09-12 实测 + 用户口径）──────
 *
 * 症状：电池"充满"了只报 89%，停充在 4024 mV。查下来不是电量算法虚高
 * （拔线前后 BATV 都是 4024 mV，停充维持压降≈0，SY6970 有独立 BATFET
 * 与 SYS 轨，不像 ETA6098 那样被充电器抬高读数），**是真的没充满**：
 * NTC 报 Warm(010) → JEITA 把截止电压降 150 mV → 4.208-0.150 = 4.058 V。
 *
 * 为什么判定这个降压是误保护：
 *   - RT1 紧贴电池，测到的**就是**电池温度（用户 2026-09-12 确认）；
 *   - 但 43°C 在使用环境里是常态——广东夏天室温近 40°C，装进盒子、
 *     放进没有空调的座舱、300 m 起降高度，机内轻松 50°C；
 *   - 而 SY6970 的 T3(Warm) 门限固定在 45°C 附近，**不可编程**：整个
 *     寄存器表里没有一位能挪 V_T1~V_T4（REG07[0] JEITA_ISET 管的是
 *     0~10°C 低温电流，REG09[4] 只管电压高低），[DS] p.10-11/p.19-20。
 * 也就是说：照 POR 走，这台设备在它的正常工作温度下**永远充不满**。
 *
 * 置 1 后 Warm 段按 REG06 的 VREG=4.208 V（POR 010111）充满。
 * 代价与边界：
 *   - 放弃的是 Warm 段的降压保护。本板实测门限在 ~43°C（Warm）与
 *     ~57.9°C（Hot），比手册标称的 45/60 略低——分压与手册典型值
 *     5.52k/31.23k 有微差。Hot 以上仍然**停充**，那层是硬保护、寄存器
 *     关不掉，电池的高温兜底仍在。
 *   - 改分压把门限上移这条路**算过、否决了**：JEITA 四个门限是固定
 *     百分比，分压只能整体缩放映射，挪 Warm 必然把 Hot 一起挪走——
 *     R39 3.40k + R40 12.2k 可令 Warm→55°C 且 Cool 仍为 10°C，但 Hot
 *     会被推到 ~72.9°C，超出锂电充电温度规格。不值得拿安全边界换。
 *     推导与数据见 docs/internal/2026-09-12-v4.6-ic-unused-pin-audit
 *     -zh_CN.md §7.5。
 *   - 也不需要改：43~58°C 已能正常满充，座舱 50°C 在窗口内。
 */
/* 位掩码在 power_sy6970.c（SY6970_JEITA_VSET_MASK），与 init_seq 同处；
 * 这里不另立一个"看着像开关、改了却没用"的常量——初版给过一个
 * SY6970_JEITA_VSET_NO_DROP，突变测试一试便知它没有任何引用。 */

typedef struct {
    sy6970_seq_op_t op;
    uint8_t     reg;   /* 目标寄存器地址                                  */
    uint8_t     mask;  /* 参与操作的位掩码                                */
    uint8_t     val;   /* 仅 VERIFY：masked 后的期望值                    */
    const char *why;   /* 判据出处（静态字面量，页码引用）                */
} sy6970_init_step_t;

/*
 * F1 初始化序列（数据表，host 可测）。返回表头；n 非空时写入步数。
 * 取证：REG07 WATCHDOG[5:4]=00 关看门狗、REG03 WD_RST(bit6)=1 喂狗
 * （写 1 自清）、REG00 POR 位值 EN_HIZ=0|EN_ILIM=1——逐条页码引用见
 * power_sy6970.c 的表定义处。
 */
const sy6970_init_step_t *sy6970_init_seq(size_t *n);

/* ── 关机序列（BATFET_DIS / shipping mode）────────────────────────────
 *
 * 为什么需要它：V4.4 板上 /QON（U19 pad 12）**完全悬空**，J1 的 40 根
 * 脚里也没有任何电源控制线（PCB 焊盘归属已逐脚核对）。电池一旦在位，
 * SY6970 的 power path 就恒供 SYS_4V → SY7069 → VCC_5V → ME6211 →
 * 3V3_DIG，RP2040(U8) 永远掉不了电、POR 不了；而 RP2040 的 RUN 脚只接
 * 了 R5 上拉和 SW1 焊锡跳线（B 面 (51.85,77.45)），装进壳里够不着。
 * 置 BATFET_DIS 关断 Q4 是这块板上**唯一**的软件断电途径
 * （[DS] p.20 REG09[5]=1 Force BATFET Off、p.30 BATFET Disable Mode）。
 *
 * ── 调用方必须知道的两件事 ──────────────────────────────────────────
 * 1. **VBUS 在位时它不会关机**。BATFET 是电池↔SYS 之间的开关；插着
 *    USB 时 SYS 由 VBUS 供电，断开电池不影响系统供电。真正要断电必须
 *    先拔 USB。执行器会在 VBUS 在位时照写不误但打 WARN——这是调用方
 *    的语义，不是本层该替它决定的。
 * 2. **唤醒只能靠插 USB**。手册给的两条恢复路径是"插适配器"或"/QON
 *    引脚一次高→低跳变"（[DS] p.30），而 /QON 在本板悬空，只剩前者。
 *    没有电池以外供电时，关机后按什么都醒不过来。
 *
 * 表里只有一步、且**没有写后回读**：写下去芯片就断电了，读不回来。
 * 这是刻意的，不是漏了验证步。
 */
const sy6970_init_step_t *sy6970_shutdown_seq(size_t *n);

/* ── 目标端（I²C 胶水 + backend，host 单测不编译不链接）───────────────
 *
 * 线程合同（portMUX 单写者，2026-09 审计二轮定稿；教训与实现先例同
 * gps_task.c:48-53 / power_service.h 线程合同）：诊断态（st/regs/
 * reg00/ready/updated_us）捆在一份快照里，唯一写者是 power_service 的
 * 1 Hz poll 任务（经 sy6970_poll()，成功拍末尾整体提交）；读者（诊断
 * 页）经 sy6970_diag_get() 拷贝。双方都在同一把自旋锁的临界区里整体
 * 拷贝——无重试、无撕裂、无 UB，跨核正确性由构造保证（RVWMO 下
 * "计数+负载"式 seqlock 已被复审否决），临界区纳秒级。
 */

/*
 * 探测 0x6A 并在 ACK 后把 backend 注册进 power_service（v4 powered 上的
 * 权威源）。调用点在 main.c 的电源链、**先于** power_eta6098_init()——
 * 注册次序=优先级（power_service.h:15-20），v3 / 未上电的 v4 探测 NACK
 * 是**预期路径**：只打一条日志、不注册，服务自然回落 ETA6098。
 * powered / unpowered 只由 ACK 表达，与 Kconfig 板型正交
 * （pk_board.h:30-32 合同，禁止按板型门控注册）。幂等；失败不致命。
 */
void power_sy6970_init(void);

/*
 * 执行关机（sy6970_shutdown_seq）。语义与两条硬约束——「VBUS 在位时
 * 不会真断电」「唤醒只能靠插 USB」——见上面 sy6970_shutdown_seq 的注释，
 * 调用前务必读一遍。未探测到器件（v3 / 未上电 v4）返回 false。
 * 成功时函数**可能不返回**：电池供电下 BATFET 一断，MCU 随即掉电。
 */
bool power_sy6970_shutdown(void);

/*
 * F7 诊断快照：最近一次**成功**轮询拍的解码状态 + REG00 回读值（F1 写入
 * 落定证据）+ 同拍原始寄存器窗口字节——三者同拍整体提交/整体读（自旋锁
 * 临界区，见上线程合同），不存在"旧状态配新字节"的混合证据。Task 5 的
 * 诊断页据此展示 WATCHDOG_FAULT（st.wd_fault，REG0C[7]）、配置回读
 * （reg00）与原始 ADC 值。从未拿到过数据（探测 NACK / bring-up 未成功，
 * updated_us==0 哨兵）返回 false 并把 *out 清零。
 */
typedef struct {
    sy6970_status_t st;            /* 最近一次成功解码的状态               */
    uint8_t reg00;                 /* 最近一次 REG00 回读值                */
    uint8_t regs[SY6970_WIN_LEN];  /* 原始窗口：regs[0]=REG0B..regs[7]=REG12 */
    bool    ready;                 /* bring-up 已成功（在读数）            */
    int64_t updated_us;            /* 最近成功采集时刻（0=从未报数哨兵）   */
} sy6970_diag_t;

bool sy6970_diag_get(sy6970_diag_t *out);
