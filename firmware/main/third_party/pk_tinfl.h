/* pk_tinfl.h / pk_tinfl.c — 单文件 vendored 版 miniz "tinfl" 原始 DEFLATE 解压器。
 *
 * 来源: https://github.com/richgel999/miniz (miniz_tinfl.h / miniz_tinfl.c /
 *       miniz_common.h 精简合并，2026-08-01 拉取自 master 分支)。
 * 许可: The Unlicense（公共领域等价物，见文件末尾原始声明）——原作者
 *       Rich Geldreich，miniz.c 项目整体亦提供 MIT 许可备选，这里采用与
 *       ESP-IDF esp_rom/include/miniz.h 相同的 Unlicense 分支。
 *
 * 为什么要 vendor 一份，而不是直接用 ESP32-P4 ROM 自带的 tinfl_decompress：
 *   - ROM 里的 tinfl 只有链接期符号（esp32p4.rom.ld），没有随 IDF 发行 .c
 *     源码，host 侧（macOS/Linux 编译单测）根本链接不到；
 *   - 固件目标（ESP_PLATFORM 已定义）继续走 esp_rom 自带的 miniz.h 声明 +
 *     ROM 里已经烧好的实现，零 flash 成本，不链接这份 vendored 代码；
 *   - pk_pmtiles.c 用 `#if defined(ESP_PLATFORM)` 二选一 include，两侧调用
 *     的是同名 API（tinfl_decompress_mem_to_mem 等），gzip 头部解析代码本身
 *     完全共享，只有"谁提供 tinfl 实现"这一层不同。
 *
 * 精简: 只保留 tinfl_decompress + tinfl_decompress_mem_to_mem 这两个 pk_pmtiles
 *       实际用到的函数；原始文件里的 mem_to_heap/mem_to_callback/decompressor_alloc
 *       以及压缩(tdefl)、zlib 包装 API、ZIP 归档 API 全部未拉入，减少 vendor 面积。
 */
#ifndef PK_TINFL_H
#define PK_TINFL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  mz_uint8;
typedef int16_t  mz_int16;
typedef uint16_t mz_uint16;
typedef uint32_t mz_uint32;
typedef uint32_t mz_uint;
typedef uint64_t mz_uint64;

#ifdef _MSC_VER
#define MZ_MACRO_END while (0, 0)
#else
#define MZ_MACRO_END while (0)
#endif

/* CPU 能力探测——照抄上游 miniz.h 的检测逻辑，host 侧 x86_64/arm64 都是小端、
 * 支持非对齐访问、64 位寄存器高效；此文件不会被固件目标编译，无需迁就
 * ESP32-P4（那边直接用 ROM 实现）。 */
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__i386) || \
    defined(__i486__) || defined(__i486) || defined(i386) || defined(__ia64__) || defined(__x86_64__)
#define MINIZ_X86_OR_X64_CPU 1
#else
#define MINIZ_X86_OR_X64_CPU 0
#endif

#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || MINIZ_X86_OR_X64_CPU || defined(__aarch64__) || defined(__arm64__)
#define MINIZ_LITTLE_ENDIAN 1
#else
#define MINIZ_LITTLE_ENDIAN 0
#endif

/* 只在 x86/x64 上开非对齐访问快速路径：这条路径直接对 (const mz_uint32 *)
 * 做指针转型再解引用，x86/x64 的 ISA 保证非对齐访存正确（只是可能慢），但
 * 严格 C 语义上仍是未定义行为——本机 arm64 跑 UBSan 实测会在这条路径报
 * misaligned load/store（虽然 Apple Silicon 用户态实际不会崩，但别赌）。
 * aarch64 host 走下面 MZ_READ_LE16/32 的逐字节兜底实现，安全优先于那几个
 * 周期的速度（这段代码只在 host 单测里跑，不影响固件端 ROM tinfl）。 */
#if MINIZ_X86_OR_X64_CPU
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 1
#else
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#endif

#if defined(_M_X64) || defined(_WIN64) || defined(__MINGW64__) || defined(_LP64) || \
    defined(__LP64__) || defined(__ia64__) || defined(__x86_64__) || defined(__aarch64__)
#define MINIZ_HAS_64BIT_REGISTERS 1
#else
#define MINIZ_HAS_64BIT_REGISTERS 0
#endif

#define MZ_ASSERT(x) ((void)0) /* 不拉 assert.h：DEFLATE 结束时的对齐检查交给上层 CRC/长度校验 */
#define MZ_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MZ_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MZ_CLEAR_ARR(obj) memset((obj), 0, sizeof(obj))

#if MINIZ_USE_UNALIGNED_LOADS_AND_STORES && MINIZ_LITTLE_ENDIAN
#define MZ_READ_LE16(p) *((const mz_uint16 *)(p))
#define MZ_READ_LE32(p) *((const mz_uint32 *)(p))
#else
#define MZ_READ_LE16(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U))
#define MZ_READ_LE32(p)                                                       \
    ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U) | \
     ((mz_uint32)(((const mz_uint8 *)(p))[2]) << 16U) | ((mz_uint32)(((const mz_uint8 *)(p))[3]) << 24U))
#endif

/* ------------------- Low-level Decompression API (tinfl) ------------------- */

enum {
    TINFL_FLAG_PARSE_ZLIB_HEADER             = 1,
    TINFL_FLAG_HAS_MORE_INPUT                = 2,
    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4,
    TINFL_FLAG_COMPUTE_ADLER32               = 8
};

#define TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))

typedef enum {
    TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS = -4,
    TINFL_STATUS_BAD_PARAM                   = -3,
    TINFL_STATUS_ADLER32_MISMATCH            = -2,
    TINFL_STATUS_FAILED                      = -1,
    TINFL_STATUS_DONE                        = 0,
    TINFL_STATUS_NEEDS_MORE_INPUT            = 1,
    TINFL_STATUS_HAS_MORE_OUTPUT             = 2
} tinfl_status;

#define tinfl_init(r) do { (r)->m_state = 0; } MZ_MACRO_END
#define tinfl_get_adler32(r) (r)->m_check_adler32

enum {
    TINFL_MAX_HUFF_TABLES    = 3,
    TINFL_MAX_HUFF_SYMBOLS_0 = 288,
    TINFL_MAX_HUFF_SYMBOLS_1 = 32,
    TINFL_MAX_HUFF_SYMBOLS_2 = 19,
    TINFL_FAST_LOOKUP_BITS   = 10,
    TINFL_FAST_LOOKUP_SIZE   = 1 << TINFL_FAST_LOOKUP_BITS
};

#if MINIZ_HAS_64BIT_REGISTERS
#define TINFL_USE_64BIT_BITBUF 1
#else
#define TINFL_USE_64BIT_BITBUF 0
#endif

#if TINFL_USE_64BIT_BITBUF
typedef mz_uint64 tinfl_bit_buf_t;
#else
typedef mz_uint32 tinfl_bit_buf_t;
#endif

typedef struct {
    mz_uint32 m_state, m_num_bits, m_zhdr0, m_zhdr1, m_z_adler32, m_final, m_type,
        m_check_adler32, m_dist, m_counter, m_num_extra, m_table_sizes[TINFL_MAX_HUFF_TABLES];
    tinfl_bit_buf_t m_bit_buf;
    size_t m_dist_from_out_buf_start;
    mz_int16 m_look_up[TINFL_MAX_HUFF_TABLES][TINFL_FAST_LOOKUP_SIZE];
    mz_int16 m_tree_0[TINFL_MAX_HUFF_SYMBOLS_0 * 2];
    mz_int16 m_tree_1[TINFL_MAX_HUFF_SYMBOLS_1 * 2];
    mz_int16 m_tree_2[TINFL_MAX_HUFF_SYMBOLS_2 * 2];
    mz_uint8 m_code_size_0[TINFL_MAX_HUFF_SYMBOLS_0];
    mz_uint8 m_code_size_1[TINFL_MAX_HUFF_SYMBOLS_1];
    mz_uint8 m_code_size_2[TINFL_MAX_HUFF_SYMBOLS_2];
    mz_uint8 m_raw_header[4], m_len_codes[TINFL_MAX_HUFF_SYMBOLS_0 + TINFL_MAX_HUFF_SYMBOLS_1 + 137];
} tinfl_decompressor;

/* 唯一必需的底层协程：一次调用把 [pIn_buf_next, +*pIn_buf_size) 的原始
 * DEFLATE（或 decomp_flags 里要求校验的 zlib 包装）流解到输出缓冲。 */
tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *pIn_buf_next, size_t *pIn_buf_size,
                               mz_uint8 *pOut_buf_start, mz_uint8 *pOut_buf_next, size_t *pOut_buf_size,
                               const mz_uint32 decomp_flags);

/* 一把梭：整块输入解到调用方已分配好、大小刚好够用的输出缓冲。
 * 失败返回 TINFL_DECOMPRESS_MEM_TO_MEM_FAILED。 */
size_t tinfl_decompress_mem_to_mem(void *pOut_buf, size_t out_buf_len, const void *pSrc_buf, size_t src_buf_len,
                                    int flags);

#ifdef __cplusplus
}
#endif

#endif /* PK_TINFL_H */
