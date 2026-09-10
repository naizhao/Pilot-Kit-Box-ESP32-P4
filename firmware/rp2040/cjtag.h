/*
 * cjtag.h — RP2040 侧 CC1312R cJTAG 位脉冲烧录引擎。
 *
 * 硬件（pinmap_978.md，PCB 网表验证）：
 *   GPIO16 → SUBG_TMSC → CC1312R pin 24（JTAG_TMSC，双向）
 *   GPIO17 → SUBG_TCKC → CC1312R pin 25（JTAG_TCKC，本侧输出）
 *   GPIO18 → SUBG_RESET → CC1312R pin 35（RESET_N，低有效）
 *
 * 协议（IEEE 1149.7 cJTAG compact 格式，简化子集）：
 * CC13x2 的 ICEPICK-D3 默认处于 cJTAG 2 线模式。在本实现使用的
 * 非高级模式（non-advanced mode）下，每个 TCKC 时钟周期 TMSC 携带
 * 一个位：
 *   - 非 Shift 状态：TMSC = TMS（本侧驱动，目标在 TCKC↑采样）
 *   - Shift-IR/Shift-DR 写入：TMSC = TDI（本侧驱动）
 *   - Shift-IR/Shift-DR 读取：目标在 TCKC↓驱动 TMSC = TDO（本侧
 *     切输入在 TCKC↓采样后在 TCKC↑读取）
 *
 * 烧录路径：cJTAG → ICEPICK-D3 → AHB-AP → Flash Controller
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

/* ── TAP 指令寄存器（CC13x2 ICEPICK-D3）────────────────────────── */
/* ── ICEPick Type C TAP 指令（TI SPRUH35 Table 2-1）─────────────────
 * IR 是 **6 位**，不是 4 位。指令值：
 *   ROUTER=000010b=0x02  IDCODE=000100b=0x04  ICEPICKCODE=000101b=0x05
 *   BYPASS=111111b=0x3F
 * ⚠ 旧代码把 IDCODE 写成 0x2 且只移 4 位：0x2 其实是 ROUTER，IR 宽度也不对，
 *   所以既没选中 IDCODE、也读不回来（恒 0）。 */
#define ICEPICK_IR_BITS      6
#define JTAG_IR_ROUTER       0x02
#define JTAG_IR_IDCODE       0x04
#define JTAG_IR_ICEPICKCODE  0x05
#define JTAG_IR_BYPASS       0x3F

/* 二级 TAP（Cortex-M DAP）的 4 位 IR —— 必须先用 ROUTER+SDTAP 路由后才可见。 */
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

/* ── CC13x2 Flash Controller（基地址 0x40030000）────────────────── */
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
#define CC13_SECTOR_SIZE   4096            /* 4 KB per sector */
#define CC13_IDCODE        0x4CC13E2F     /* Cortex-M4 + TI 制造商 */

/* ── 初始化 / 退出 ───────────────────────────────────────────────── */

/* 进入代刷模式：接管 GPIO16/17/18、发 RESET 脉冲让 CC1312R 进
 * cJTAG 模式（复位后 ICEPICK 默认 cJTAG）。返回前 TAP 处于
 * Test-Logic-Reset。调用者须先暂停 SPI master。 */
void cjtag_enter(void);

/* 退出代刷模式：释放 GPIO、发 RESET 脉冲让 CC1312R 重启。 */
void cjtag_exit(void);

/* ── JTAG 原语（host 可测的纯逻辑 + 目标胶水）─────────────────── */

/* 读取 32-bit IDCODE（IR=0x2 → DR 返回）。用于通信证明。 */
uint32_t cjtag_read_idcode(void);

/* AHB-AP 内存读写（经 ICEPICK → DAP → MEM-AP）。 */
uint32_t cjtag_ahb_read32(uint32_t addr);
void     cjtag_ahb_write32(uint32_t addr, uint32_t val);

/* ── Flash 操作 ─────────────────────────────────────────────────── */

/* 擦除指定 sector（4 KB 对齐）。阻塞至完成或超时。 */
bool cjtag_flash_erase_sector(uint32_t addr);

/* 写入一个 32-bit word 到 flash。阻塞至完成。 */
bool cjtag_flash_write_word(uint32_t addr, uint32_t val);

/* 校验一块 flash（逐 word 比对）。 */
bool cjtag_flash_verify(uint32_t addr, const uint8_t *data, size_t len);

/* 完整烧录流程：擦除 → 写入 → 校验 → 打印进度。
 * 返回 true = 全部成功。 */
bool cjtag_flash_program(uint32_t addr, const uint8_t *data, size_t len);

/* ── CDC 命令接口（adsb1090.c 调用）───────────────────────────── */

/* 处理 CDC 字符 'F'（进入代刷模式）。返回 true = 进入成功。 */
bool cjtag_cdc_enter(void);

/* 处理代刷模式中的数据（镜像流）。返回 true = 继续接收。 */
bool cjtag_cdc_data(uint8_t byte);

/* 处理 CDC 字符 'Q'（退出代刷模式）。 */
void cjtag_cdc_quit(void);

/* 查询：是否处于代刷模式。 */
bool cjtag_cdc_active(void);
