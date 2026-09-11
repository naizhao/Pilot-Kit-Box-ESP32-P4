/*
 * cjtag.h — RP2040 侧 CC1312R cJTAG（IEEE 1149.7 OScan1）烧录引擎。
 *
 * 硬件（pinmap_978.md，PCB 网表验证）：
 *   GPIO16 → SUBG_TMSC → CC1312R pin 24（JTAG_TMSC，双向）
 *   GPIO17 → SUBG_TCKC → CC1312R pin 25（JTAG_TCKC，本侧输出）
 *   GPIO18 → SUBG_RESET → CC1312R pin 35（RESET_N，低有效）
 *
 * 只有 2 根线 → 只能走 cJTAG OScan1：TDI/TDO 在 CC13x2 上是复用到别的 DIO
 * 的，本板没接。所以**绝对不能**跑 OpenOCD tcl/target/ti/cjtag.cfg 里的
 * ti_cjtag_to_4pin_jtag——那段是把器件从 2 线切到 4 线 JTAG 的，跑完链路就没了。
 *
 * 流程（依据 CC13x2 TRM SWCU185G §6.2/§6.4，逐条出处见 cjtag.c 文件头）：
 *   RESET 脉冲 → ICEMelter 唤醒 JTAG 电源域（16 个 TCK + 等 ≥200µs）
 *   → 上电默认的 2 线 JScan 下开 cJTAG 命令窗 → STFMT 命令切到 OScan1
 *   → 此后每个 JTAG 位 = 3 个 TCKC 周期（nTDI / TMS / TDO），TDO 才有通路。
 *
 * 与 SPI master 互斥：代刷期间独占 GPIO16/17/18。调用者负责在
 * 进入/退出代刷模式时暂停/恢复 SPI master（adsb1090.c 的 CDC
 * 命令 'F' 处理）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── 引脚（pinmap_978.md）────────────────────────────────────────── */
#define CJTAG_PIN_TMSC   16
#define CJTAG_PIN_TCKC   17
#define CJTAG_PIN_RESET  18

/* ── cJTAG 时序（保守值）────────────────────────────────────────── */
#define CJTAG_TCK_DELAY_NS  2000   /* ~250 kHz TCKC；正确性优先 */

/* ── cJTAG 命令（TRM SWCU185G Table 6-4）────────────────────────────
 * 命令的「值」由 DR 扫描在 Shift-DR 里停留的时钟数承载（0..31），
 * CP0 = opcode，CP1 = operand。TI 的 cJTAG **不用** IEEE 1149.7 的 TMSC
 * escape 激活序列——整本 TRM 里 "escape" 一次都没出现。 */
#define CJTAG_CMD_STMC        0   /* Store Miscellaneous Control */
#define CJTAG_CMD_STC1        1   /* Store Conditional 1 bit（含 SEDGE 采样沿） */
#define CJTAG_CMD_STC2        2   /* Store Conditional 2 bit（APFC → 切 4 线） */
#define CJTAG_CMD_STFMT       3   /* Store Scan Format */

#define CJTAG_FMT_CODE_OSCAN1 9   /* STFMT operand：9 = OSCAN1（Table 6-4） */

/* ── TAP 指令 ────────────────────────────────────────────────────── */
/* ICEPick（JTAG Route Controller）——OpenOCD tcl/target/ti/cc26x0.cfg：
 *   `jtag newtap $_CHIPNAME jrc -irlen 6 -ircapture 0x1 -irmask 0x3f`
 * 指令码见 tcl/target/ti/icepick.cfg 与 cjtag.cfg。 */
#define ICEPICK_IR_BITS      6
#define JTAG_IR_ICEPICK_BYPASS  0x00
#define JTAG_IR_ROUTER          0x02
#define JTAG_IR_CONNECT         0x07
#define JTAG_IR_IDCODE          0x04
#define JTAG_IR_BYPASS          0x3F

/* 二级 TAP（Cortex-M DAP）——cc26x0.cfg：`-irlen 4`。
 * ⚠ 该 TAP 在 cc26x0.cfg 里是 `-disable` 的，必须先经 ICEPick
 * CONNECT + router 写把它挂上链才可见。 */
#define DAP_IR_BITS          4
#define JTAG_IR_DPACC        0xA    /* Debug Port Access */
#define JTAG_IR_APACC        0xB    /* Access Port Access */

/* ── ARM ADIv5 常量 ──────────────────────────────────────────────── */
#define DP_CTRLSTAT        0x4   /* DP register: Control/Status */
#define DP_SELECT          0x8   /* DP register: AP Select */
#define DP_RDBUFF          0xC   /* DP register: Read Buffer */
#define AP_CSW             0x00  /* AP register: Control Status Word */
#define AP_TAR             0x04  /* AP register: Transfer Address */
#define AP_DRW             0x0C  /* AP register: Data Read/Write */
#define AHB_AP_IDR         0xED  /* 期望 AHB-AP IDR */

#define DP_CTRL_CSYSPWRUP  (1u << 30)
#define DP_CTRL_CDBGPWRUP  (1u << 28)

/* ── CC13x2 Flash Controller（基地址 0x40030000）──────────────────
 * ⚠ 这套寄存器直写模型来自 Stellaris/CC2538，尚未对 CC13x2 核实。
 * OpenOCD 给 CC13x2 用的是 `flash bank ... cc26xx`（SRAM flash loader）。
 * 见 cjtag.c 「Flash 操作」上方的未决说明。 */
#define FLASH_BASE         0x40030000
#define FLASH_FADDR        (FLASH_BASE + 0x04)
#define FLASH_FMC          (FLASH_BASE + 0x08)
#define FLASH_FDATA0       (FLASH_BASE + 0x10)
#define FLASH_FSTAT        (FLASH_BASE + 0x00)

#define FMC_WRD           0x2     /* 写入 32-bit word */
#define FMC_ERASE_SECTOR  0x4     /* 擦除 4KB sector */
#define FMC_ERASE_ALL     0x5     /* 全片擦除 */
#define FSTAT_BUSY        (1u << 0)

/* CC1312R 参数 */
#define CC13_FLASH_SIZE    (352 * 1024)  /* 352 KB flash */
#define CC13_SECTOR_SIZE   4096          /* 4 KB per sector */

/* TLR 之后从 DR 读到的是 **ICEPick JRC** 的 IDCODE，不是 Cortex-M 的。
 * OpenOCD tcl/target/ti/cc13x2.cfg：`set JRC_TAPID 0x0BB4102F`。
 * bit[31:28] 是版本号，不同批次会变 —— cc26x0.cfg 对这个 TAP 用了
 * `-ignore-version`，所以比对必须屏蔽掉。 */
#define CC13_JRC_IDCODE      0x0BB4102Fu
#define CC13_IDCODE_MASK     0x0FFFFFFFu
/* 路由到二级 TAP 之后才看得到（cc26x0.cfg：DAP_TAPID）。 */
#define CC13_DAP_IDCODE      0x4BA00477u

/* ── 初始化 / 退出 ───────────────────────────────────────────────── */

/* 进入代刷模式：接管 GPIO16/17/18 → RESET 脉冲 → cJTAG 激活到 OScan1
 * → TLR。调用者须先暂停 SPI master。 */
void cjtag_enter(void);

/* 退出代刷模式：回 TLR、发 RESET 脉冲让 CC1312R 重启。 */
void cjtag_exit(void);

/* ── JTAG 原语 ──────────────────────────────────────────────────── */

/* 读取 32-bit IDCODE：TLR 会自动把 IDCODE 装进 IR，所以这条路径不依赖
 * IR 宽度/指令码，是最短的存活证明。期望 CC13_JRC_IDCODE（屏蔽版本号）。 */
uint32_t cjtag_read_idcode(void);

/* 诊断：一次上板跑完激活参数矩阵并打印每个变体读到的原始 DR 值。 */
void cjtag_diag(void);

/* 把 GPIO16/17/18 全部置高阻并暂停 SPI master，交给外部仿真器
 * （J-Link/XDS110）接管。本板没给这三根网络留测试点，但它们与 RP2040 共用，
 * 焊 RP2040 侧引脚即可。恢复靠复位 RP2040。 */
void cjtag_release_bus(void);

/* ⚠ flash 编程接口已删除（cjtag_ahb_read32/write32、cjtag_flash_*）。
 * 那套寄存器直写来自 Stellaris/CC2538、对 CC13x2 从未核实，且依赖的 DAP TAP
 * 挂链步骤也不存在；加上本模块的链路实测没打通，它从未被执行过。理由详见
 * cjtag.c 里「Flash 编程：已删除」一节。正式烧录通道是 cc13_bsl.c。 */

/* ── CDC 命令接口（adsb1090.c 调用）───────────────────────────── */

/*
 * ⚠ 本模块**不提供烧录**，只有探测与诊断。原来的流式烧录接口
 * （cjtag_cdc_enter/data/quit）已随 flash 寄存器直写一并删除——理由见 cjtag.c
 * 「Flash 编程：已删除」。正式烧录通道是 cc13_bsl.c（CDC 'U'）。
 */

/* 'F'：走一遍完整上电流程并读 IDCODE。返回 true = 读到的值与 ICEPick JRC
 * 相符（屏蔽版本号）。*idcode_out 无论成败都写入，便于打印原值。 */
bool cjtag_probe(uint32_t *idcode_out);

/* 'Z' 之后为 true：三根线已交给外部仿真器，core0 必须一直跳过 spim_poll
 * （spim 在 RECOVERY 里会拉低 GPIO18 复位 CC1312R，正好打断仿真器会话）。 */
bool cjtag_bus_released(void);
