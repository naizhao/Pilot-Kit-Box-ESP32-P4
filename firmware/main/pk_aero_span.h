/*
 * pk_aero_span.h — pk_aero.bin 的**区间顺序读**（常驻句柄 + AES-CTR 随机解密）。
 *
 * 设计依据窗口化数据架构设计（内部文档）
 * 「增量加载：按格一次连续读，不是随机点查」一节 + W1.2 / W1.3 两步。
 *
 * 这个模块存在的唯一理由：窗口机制要"加载一个格"，而一个格在每个段里都是
 * **一段连续的记录区间**（文档 §1.5 的实测：rwy_first / freq_first 零逆序、
 * 空域记录按 grid_cell 零逆序、顶点单调）。所以正确的原语是
 * `fseek + fread(区间)`，不是"随机 4 KB 点查"——两者的 IO 模型差 6 倍以上
 * （顺序 6.45 MB/s vs 随机 1.04 ms/4KB）。
 *
 * 与 pk_aero_db 的关系（**并存，不替换**）：
 *   - pk_aero_db 仍然把整份 bin 读进 PSRAM，老路径一个字节都不动；
 *   - 本模块**另开一个只读句柄**按需读区间，两者互不知道对方存在。
 *   W1 阶段这样做是有意的：窗口读出来的字节可以直接和 pk_aero_db 的
 *   查询结果对拍（见 pk_win.c 的自检），这是"窗口读对了没有"最强的证据。
 *   替换发生在 W1.4/W1.5 把 nearest/快照切过来之后，不在本轮。
 *
 * 头与段表是**明文**（payload_off = 对齐后的段表末尾，见
 * pk_aero_payload_off），只有 payload 走 AES-128-CTR。所以本模块可以
 * 只读前 512 B 就把段表全解出来，不必碰密钥。
 *
 * 并发 / 热插拔：
 *   - 所有 IO 串在模块内的 s_io_lock 后面；
 *   - 注册 pk_sdcard 的 pre-unmount 回调，卸载前关句柄（IDF 的
 *     esp_vfs_fat_sdcard_unmount() 会无条件 free 掉含 FIL 数组的 fat_ctx，
 *     开着文件卸载就是 use-after-free —— 见 pk_sdcard.h 的契约）；
 *   - generation 每次 open 成功 +1，调用方靠它识别"手里这个偏移是不是
 *     上一张卡的"。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pk_aero_reader.h"   /* pk_aero_section_t / pk_aero_index_t / 段类型常量 */

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 pk_sdcard 的 pre-unmount 回调（关句柄 + IO 栅栏）。幂等，须在
 * pk_sdcard_init() 之后调用一次（pk_win_init() 会替你调）。
 * 回调槽固定 8 个（pk_sdcard.c:264），现有占用 4 个，有余量。 */
void pk_aero_span_register_unmount_cb(void);

/* 幂等。SD 未挂载 / 无文件 / 头非法时返回 false（不是错误，是"现在读不了"）。 */
bool pk_aero_span_open(void);
void pk_aero_span_close(void);
bool pk_aero_span_is_open(void);

/* bin 版本（2/3/4）；未打开为 0。 */
uint16_t    pk_aero_span_version(void);
const char *pk_aero_span_cycle(void);
/* 每成功 open 一次 +1，只增不减。 */
uint32_t    pk_aero_span_generation(void);

/* 段描述（PK_AERO_SEC_*）。段缺席（老卡）返回 NULL。 */
const pk_aero_section_t *pk_aero_span_section(uint16_t type);

/* 某段的格索引定位。索引区在 payload 里（加密），所以这一步**要读盘**。
 * 段缺席 / 无索引 / 读失败返回 false。 */
bool pk_aero_span_index(uint16_t type, pk_aero_index_t *out);

/* payload 内 [off, off+len) → dst。已解密。越界 / 未打开 / 读失败返回 false。
 * len == 0 视为成功（不动 dst）。 */
bool pk_aero_span_read(uint32_t off, void *dst, uint32_t len);

/* 诊断计数（自 open 起累计）。 */
uint64_t pk_aero_span_bytes_read(void);
uint32_t pk_aero_span_read_calls(void);
void     pk_aero_span_stats_reset(void);

/*
 * AES-CTR 的 IV：高 64 位 = header 里的 nonce，低 64 位 = payload 内偏移 / 16
 * 的**大端**块号。口径必须与 pk_aero_db.c 的整读路径一字不差——那边是
 * iv[0..7]=nonce、iv[8..15]=0 从 payload 起点开始流，走到偏移 off 时块计数
 * 正好是 off/16。
 *
 * 写成 static inline 是为了能在 host 上直接验这条算术（W1.3 的验收项）：
 * 它是整条"随机解密"链路里最容易写错、错了又不会崩（只会读出乱字节）的
 * 一处。真正的字节级证据在真机自检的 verify_against_full_db。
 *
 * ⚠ off 必须 16 B 对齐，否则块号会被截断（调用方负责对齐到块边界）。
 */
static inline void pk_aero_span_ctr_iv(const uint8_t nonce[8], uint32_t off,
                                       uint8_t iv[16])
{
    for (int i = 0; i < 8; i++) iv[i] = nonce[i];
    const uint64_t block = (uint64_t)off / 16u;
    for (int i = 0; i < 8; i++)
        iv[8 + i] = (uint8_t)(block >> (56 - 8 * i));
}

#ifdef __cplusplus
}
#endif
