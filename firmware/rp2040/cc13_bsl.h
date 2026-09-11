/*
 * cc13_bsl.h — 经 CC1312R 的 **ROM 串行 bootloader**（SSI0）烧录，不走 cJTAG。
 *
 * 这是本板 CC1312R 固件升级的**正式通道**。cjtag.c 那条位脉冲 cJTAG 只为空片
 * 的第一次烧录而写，实测没打通，那一次改由外部仿真器完成
 * （见 docs/internal/firmware-v3v4/HANDOVER-CC13-CJTAG.md）。
 *
 * ⚠ 更正（2026-09-11）
 * -------------------
 * 本文件早先写着「交接文档说 cJTAG 是唯一恢复通道，这个前提是错的，因为
 * §10.1 说空片会自动进 bootloader」。**那个更正本身是错的，现予撤回。**
 *
 * TRM 表 11-15（BL_CONFIG）写死：BOOTLOADER_ENABLE **只有 0xC5 是 enabled，
 * 其余任何值一律 disabled**。空片的 CCFG 是擦除态全 0xFF ≠ 0xC5 → ROM
 * bootloader 是**关**的。§10.1「no valid image 就自动执行 bootloader」要和
 * §10.1.1「disabled 时不执行任何命令」合起来读：空片确实会跳进 bootloader，
 * 但它拒绝执行命令。实测也如此——16 个变体（mode0/3 × backdoor 高低 × 片选
 * 粒度 × 首字节延时）读回的全是 0xFF。
 *
 * 结论：**串行 bootloader 是现场升级机制，不是首次烧录机制**，原交接文档说的
 * 「cJTAG 是首次烧录的唯一通道」是对的。这也是每块 TI LaunchPad 都焊着
 * XDS110 的原因。
 *
 * 所以本模块的前提是：目标 flash 里**已有**一版镜像，且其 CCFG 打开了
 * bootloader 与 backdoor。firmware/cc1312r/ccfg.c 保证这一点，并由
 * check_ccfg.py 在构建时卡死（BL_CONFIG 必须是 0xC5FF0DC5）。
 *
 * ── 接线：为什么必须位脉冲 ────────────────────────────────────────
 * ROM bootloader 的 SSI0 引脚是**固定的**（TRM 表 10-2），应用固件可以用 IOC
 * 自由 mux，ROM 不行：
 *
 *   SSI0_CLK = DIO10   SSI0_FSS = DIO11   SSI0_RX = DIO9   SSI0_TX = DIO8
 *
 * 本板（sheet_subghz.py / board_pins.h，网表焊盘已核）：
 *
 *   DIO10 = SUBG_SCK  ← RP2040 GPIO10   ✔ 正好是 SSI0_CLK
 *   DIO11 = SUBG_CSN  ← RP2040 GPIO13   ✔ 正好是 SSI0_FSS
 *   DIO9  = SUBG_MISO ↔ RP2040 GPIO12   ✘ 板上叫"主机输入"，ROM 当**器件输入**
 *   DIO8  = SUBG_MOSI ↔ RP2040 GPIO11   ✘ 板上叫"主机输出"，ROM 当**器件输出**
 *
 * 时钟和片选**天生就对**——这两根要是错了，软件再怎么写都救不回来。只有数据
 * 两根的方向与 ROM 的固定角色相反（板子是按应用固件的 mux 画的）。
 *
 * 而方向恰恰是软件能改的，所以这里**不用硬件 SPI**（外设的引脚角色定死了，
 * 换不过来），全部位脉冲，把两根数据线对调：
 *   往器件送 → GPIO12（经 SUBG_MISO 到 DIO9 = SSI0_RX）
 *   从器件收 ← GPIO11（经 SUBG_MOSI 到 DIO8 = SSI0_TX）
 * 名字对不上但方向是对的，**不要"顺手把名字改回来"**。
 *
 * backdoor：复位那一刻 DIO13（SUBG_SYNC ← GPIO15）为**高**即强制进 bootloader。
 * 高有效在 ccfg.c 里定，理由见那里：GPIO15 复位后是输入+下拉，若设成低有效，
 * CC1312R 每次上电都会进 bootloader 而不跑应用固件。
 *
 * 与 SPI master 互斥：本模块独占 GPIO10~13 与 GPIO18，调用期间 spim 必须暂停
 * （它在 RECOVERY 里会拉低 GPIO18 复位 CC1312R，正好打断烧录）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 烧录结果。**故意把失败分这么细**：CRC 不匹配与 flash 写失败必须分开报。
 * TRM §10.2.3.8 没写 CRC32 用的多项式（我们按标准 IEEE 802.3 实现，见
 * cc13_bsl_proto.h），所以"校验不过"的第一嫌疑是那个假设错了，而不是 flash
 * 坏了。报成同一种错会让人跑去反复重擦，方向全反——这类"说得通但方向反了"
 * 的排查，本项目这轮已经踩过两次。
 */
typedef enum {
    CC13_BSL_OK = 0,
    CC13_BSL_ERR_SIZE,        /* 镜像长度非法 */
    CC13_BSL_ERR_LINK,        /* 无 ACK：没进 bootloader，或 SPI 模式/接线不对 */
    CC13_BSL_ERR_PROTOCOL,    /* 收到的包长度或校验和不自洽 */
    CC13_BSL_ERR_ERASE,       /* 擦除被拒 */
    CC13_BSL_ERR_PROGRAM,     /* 某片写完后 GET_STATUS 非 SUCCESS */
    CC13_BSL_ERR_CRC,         /* 数据全写完了但 CRC32 对不上 —— 先查多项式假设 */
    CC13_BSL_ERR_STATE,       /* 调用顺序不对（没 begin 就 feed 之类） */
} cc13_bsl_result_t;

const char *cc13_bsl_result_str(cc13_bsl_result_t r);

/* ── 流式烧录 ────────────────────────────────────────────────────
 * 镜像从 USB CDC 流进来，352 KB 在 RP2040 上不可能全驻留，所以是
 * begin → feed×N → finish 三段式，内部按 252 字节一片发 SEND_DATA。 */

/* 独占引脚 → 拉高 backdoor → 复位 → PING → 整片擦除 → DOWNLOAD。 */
cc13_bsl_result_t cc13_bsl_begin(uint32_t total_len);

/* 喂数据。内部分片、逐片 GET_STATUS、并增量算 CRC32。 */
cc13_bsl_result_t cc13_bsl_feed(const uint8_t *data, size_t n);

/* 冲尾片 → CRC32 校验 → 复位让新固件跑 → 交还引脚。 */
cc13_bsl_result_t cc13_bsl_finish(void);

/* 中止：复位目标并交还引脚。 */
void cc13_bsl_abort(void);

/* 是否正在烧录中（core0 调度器据此暂停 spim）。 */
bool cc13_bsl_active(void);

/* ── CDC 流式接口（adsb1090.c 调用）────────────────────────────── */

/* 'U'：进入烧录模式。随后先收 4 字节小端长度，再收镜像。 */
void cc13_bsl_cdc_start(void);

/* 烧录模式中的每个字节都喂这里（进入后不再按命令解析——镜像里必然含 'B'，
 * 当成命令会让 RP2040 进 BOOTSEL）。 */
void cc13_bsl_cdc_byte(uint8_t b);

/* 是否处于 CDC 烧录模式。 */
bool cc13_bsl_cdc_busy(void);

/* ── 诊断 ───────────────────────────────────────────────────────── */

/* 跑一遍 SPI 模式 / backdoor 电平 / 片选粒度 / 首字节延时的变体矩阵并打印
 * 回读字节。手册说 mode 3，而 ADSBee 实机用的是 mode 0，所以这个矩阵要留着
 * ——第一次接上真正能响应的芯片时，它能一次问清到底是哪一套。 */
void cc13_bsl_diag(void);
