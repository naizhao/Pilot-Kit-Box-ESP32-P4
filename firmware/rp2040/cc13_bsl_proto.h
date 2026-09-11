/*
 * cc13_bsl_proto.h — CC13x2 ROM 串行 bootloader 的**纯协议层**。
 *
 * 为什么单独一个文件：这一层没有任何平台依赖，可以在 host 上百分之百验证。
 * 而 I/O 那一层（位脉冲 SSI）要等仿真器把第一版镜像烧进去、bootloader 被
 * CCFG 打开之后才跑得起来。把两者分开，"上不了板"就只挡住 I/O，挡不住
 * 协议本身的正确性——cjtag 那次把时序和逻辑混在一起、host 测试全绿却毫无
 * 意义的教训，不该再犯第二次。
 *
 * 判据来源：CC13x2/CC26x2 TRM SWCU185G §10.2
 *   §10.2.1  包格式：[size][checksum][data...]
 *            size = **整包字节数**（含 size 与 checksum 这两个头字节）
 *            checksum = data 各字节之和（mod 256），**不含**两个头字节
 *            手册图 10-2 的例子：size=0x06 checksum=0x84
 *            data=48 6F 6C 61 00 → 和 = 0x184 → 0x84 ✔
 *   §10.2.1.1 ACK = 0xCC，NACK = 0x33（前面还有一个 0x00）
 *   §10.2.3  命令码与各自的参数布局；地址/长度一律 **MSB first**
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── 协议常量（TRM §10.2.1.1 / 表 10-3）─────────────────────────── */
#define CC13_BSL_ACK            0xCCu
#define CC13_BSL_NACK           0x33u

#define CC13_BSL_CMD_PING          0x20u
#define CC13_BSL_CMD_DOWNLOAD      0x21u
#define CC13_BSL_CMD_GET_STATUS    0x23u
#define CC13_BSL_CMD_SEND_DATA     0x24u
#define CC13_BSL_CMD_RESET         0x25u
#define CC13_BSL_CMD_SECTOR_ERASE  0x26u
#define CC13_BSL_CMD_CRC32         0x27u
#define CC13_BSL_CMD_BANK_ERASE    0x2Cu
#define CC13_BSL_CMD_SET_CCFG      0x2Du

/* 状态码（TRM §10.2.3.5）*/
#define CC13_BSL_RET_SUCCESS       0x40u
#define CC13_BSL_RET_UNKNOWN_CMD   0x41u
#define CC13_BSL_RET_INVALID_CMD   0x42u
#define CC13_BSL_RET_INVALID_ADR   0x43u
#define CC13_BSL_RET_FLASH_FAIL    0x44u

/* size 是**单字节**，所以整包最多 255 字节；除去 size/checksum/命令码三个
 * 字节，一条 SEND_DATA 最多带 252 字节数据。 */
#define CC13_BSL_MAX_PACKET     255u
#define CC13_BSL_MAX_DATA       (CC13_BSL_MAX_PACKET - 3u)

/* CC1312R1F3 的 flash 容量。烧录长度上界，也用来挡住明显离谱的长度头。 */
#define CC13_FLASH_BYTES        (352u * 1024u)

/* ── 纯函数 ─────────────────────────────────────────────────────── */

/* 组一个协议包到 out。返回整包长度；cap 不足或参数过长返回 0 且不写输出
 * （失败不改写输出，与本仓库其它 codec 同款单元契约）。 */
size_t cc13_bsl_pkt_build(uint8_t *out, size_t cap, uint8_t cmd,
                          const uint8_t *params, size_t nparams);

/* 校验收到的包：长度自洽 + checksum 对得上。 */
bool cc13_bsl_pkt_valid(const uint8_t *pkt, size_t len);

/* 把 32 位值按 MSB first 写进 out[0..3]（TRM §10.2.3.2 等）。 */
void cc13_bsl_put_be32(uint8_t *out, uint32_t v);

/* 一条 SEND_DATA 该带多少字节：min(remaining, 252)。 */
size_t cc13_bsl_chunk_len(size_t remaining);

/*
 * CRC32，用于和 COMMAND_CRC32 的返回值比对。
 *
 * ⚠ TRM §10.2.3.8 只说"用 CRC32 校验一段 flash"，**没有写多项式**。这里用的
 * 是标准 IEEE 802.3 CRC-32（反射多项式 0xEDB88320，初值 0xFFFFFFFF，末尾取
 * 反）——社区工具（cc2538-bsl 等）用的都是它。如果上板第一次校验就不匹配，
 * **先怀疑这个假设**，而不是怀疑 flash 写坏了：所以 cc13_bsl.c 里把"CRC 不匹配"
 * 和"flash 操作失败"报成两种不同的错。
 */
uint32_t cc13_bsl_crc32(const uint8_t *data, size_t len);

/* CRC32 的流式版本：init 用 0xFFFFFFFFu，最后调 cc13_bsl_crc32_final。
 * 352 KB 镜像是流式喂进来的，不可能在 RP2040 上一次性驻留后再算。 */
uint32_t cc13_bsl_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t cc13_bsl_crc32_final(uint32_t crc);
