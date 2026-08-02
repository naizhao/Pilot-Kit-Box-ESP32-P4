/*
 * aircraft_db_reader.h — pk_actdb.bin 的纯 C 解析器（无 OS / 无 IDF 依赖）
 *
 * 与 pk_aero_reader.h 同构、同分工：本文件只管「一块已经整段载入内存的
 * buffer → 结构体 + 查询」，文件 IO / 状态机 / 并发全部由 aircraft_db.c 负责。
 * 这样拆的直接收益是这一层能进 host 单测（firmware/test/test_aircraft_db.c），
 * 而 aircraft_db.c 那层拖着 FreeRTOS/PSA 进不去。
 *
 * ── 两层格式 ────────────────────────────────────────────────────────
 * 1) 容器头（64 B，与 pk_aero.bin 逐字段同构，见 pk_aero_reader.h/.c：
 *    固件侧因此能复用同一套「头解析 + payload 定位 + SHA-256 校验」的写法）：
 *      off 0   char   magic[6]      "PKACT1"
 *      off 6   uint16 version       == 1
 *      off 8   char   cycle[8]      生成日期串 "20260801"（不足补 NUL）
 *      off 16  uint16 n_sections    == 0（机型库是单块载荷，没有段表）
 *      off 18  uint32 sections_off  == 64
 *      off 22  uint8  enc_algo      == 0（PK_ACTDB_ENC_NONE，机型库是公开数据）
 *      off 23  uint8  _reserved
 *      off 24  uint8  nonce[8]      未加密，恒 0
 *      off 32  uint8  sha256[32]    payload 的 SHA-256
 *      off 64  payload              ← align16(sections_off + n_sections*32)
 *
 * 2) payload = 原有的 PKADB1 载荷，一个字节都没动。布局的权威定义在
 *    firmware/scripts/gen_aircraft_db.py 的文档字符串里：
 *      32 B 头 + records[]（8 B/条，按 icao24 升序）+ types[]（12 B/条）
 *      + 字符串池。查找 = records[] 上的一次二分。
 *
 * 约束（照抄 pk_aero_reader 的两条）：
 *   - 零堆分配；除 init 外不写入任何全局态；
 *   - 多字节字段逐字节组装，不做未对齐强转解引用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aircraft_db.h"   /* pk_aircraft_info_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 错误码（编号照 pk_aero_reader 的风格）---- */
enum {
    PK_ACTDB_OK            =  0,
    PK_ACTDB_ERR_TRUNCATED = -1,   /* buffer 太短 / 段越界 */
    PK_ACTDB_ERR_MAGIC     = -2,   /* 容器 magic 不是 "PKACT1"，或内层不是 "PKADB1" */
    PK_ACTDB_ERR_VERSION   = -3,   /* 版本不支持 */
    PK_ACTDB_ERR_ENCRYPTED = -4,   /* enc_algo != 0（本库不加密，见文件头） */
    PK_ACTDB_ERR_ARG       = -6,
};

#define PK_ACTDB_MAGIC          "PKACT1"
#define PK_ACTDB_VERSION        1
#define PK_ACTDB_HEADER_SIZE    64
#define PK_ACTDB_SECTION_SIZE   32
#define PK_ACTDB_ENC_NONE       0

/* 内层载荷 */
#define PK_ACTDB_PAYLOAD_MAGIC  "PKADB1"
#define PK_ACTDB_PAYLOAD_VER    2
#define PK_ACTDB_RECORD_SIZE    8
#define PK_ACTDB_TYPE_SIZE      12
#define PK_ACTDB_TYPE_NONE      0xFFFFu   /* type_idx：该机无机型代码 */
#define PK_ACTDB_STR_NONE       0u        /* 字符串池偏移 0 = 无 */

typedef struct {
    /* 容器层（with_container == false 时全部保持 0/空） */
    uint16_t container_version;
    char     cycle[9];
    uint8_t  enc_algo;
    uint8_t  sha256[32];
    const uint8_t *payload;      /* 指向内层 PKADB1 头 */
    uint32_t       payload_len;

    /* 内层 PKADB1 */
    uint16_t       version;
    const uint8_t *records;      /* n_records × 8 B，按 icao24 升序 */
    const uint8_t *types;        /* n_types  × 12 B */
    const char    *strings;
    uint32_t       n_records;
    uint16_t       n_types;
    uint32_t       strings_size;
} pk_actdb_t;

/*
 * 定位容器内 payload 的起点。与 pk_aero_payload_off 同算法（段表尾 16 B
 * 对齐）。buffer 太短返回 0。
 */
uint32_t pk_actdb_payload_off(const uint8_t *buf, size_t len);

/*
 * 校验并填 *db。with_container = true 走「容器头 + PKADB1」两层；false 则
 * 把 buf 直接当 PKADB1 载荷（EMBED_FILES 时代的裸 blob、以及单测手搓载荷）。
 * 只做边界校验，不算 SHA——SHA 要分块让渡，是调用方（aircraft_db.c）的事。
 */
int pk_actdb_init(pk_actdb_t *db, const uint8_t *buf, size_t len,
                  bool with_container);

/*
 * 一次二分取全部字段。命中返回 true 并填满 *out（缺的字段是空串），
 * 未命中返回 false 并把 *out 清空。
 *
 * 这是替代原先四个入口各做一次二分的核心改动：查同一架飞机的
 * code/model/desc/registration 从 4 次二分（~76 次随机读）降到 1 次。
 */
bool pk_actdb_lookup(const pk_actdb_t *db, uint32_t icao24,
                     pk_aircraft_info_t *out);

#ifdef __cplusplus
}
#endif
