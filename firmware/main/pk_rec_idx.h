/*
 * pk_rec_idx.h — traffic.idx 按机摘要：内存表维护 + 从 traffic.trk 重建。
 *
 * 设计依据 ADS-B 数据持久化设计（内部文档）
 * 「traffic.idx」节 + 文末「实现记录」第 9 条（3a 只搭骨架，3b 补构建/重建）。
 *
 * 本文件**不依赖任何 IDF 头**——固件（pk_rec_store_fs.c，运行期在线维护 +
 * 掉电场景下扫 traffic.trk 重建）与 host 单测共用同一份源码
 * （firmware/test/test_pk_rec_idx.c 直接 #include 本 .c 进同一 TU）。
 *
 * 「重建」是纯逻辑：给一段已经在内存里的 traffic.trk 记录（真机侧负责把
 * 文件分块读进这样的缓冲，见 pk_rec_store_fs.c 的 pk_rec_store_rebuild_index），
 * 按 rec_type 分派到 ingest_pos / ingest_id，身份记录用来恢复呼号——这正是
 * spec 「rec_type 区分两种记录…这样 idx 可从本文件完整重建」那句话说的机制。
 */
#pragma once

#include "pk_rec_format.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一个 session 里出现的飞机数上限。繁忙空域一次飞行几百个 ICAO（spec
 * 「为什么不是物理一机一文件」节的说法），512 留了一倍余量。超过上限的
 * 飞机不会再被 find_or_add 出新条目——早期先到的飞机摘要仍然完整，只是
 * 超限之后的新面孔不再单独统计，不影响 traffic.trk 本身的完整性（原始
 * 记录仍然全部落盘，只有"按机摘要"这一层退化）。 */
#define PK_REC_IDX_MAX_AIRCRAFT 512

typedef struct {
    pk_idx_rec_t entries[PK_REC_IDX_MAX_AIRCRAFT];
    size_t       count;
} pk_rec_idx_table_t;

void pk_rec_idx_reset(pk_rec_idx_table_t *t);

/* 只读查找，不存在返回 NULL（不会新增条目）。 */
const pk_idx_rec_t *pk_rec_idx_find(const pk_rec_idx_table_t *t,
                                     const uint8_t icao24[3]);

/* 查找或新增一条空摘要（icao24 已填，其余置零）。表满且 icao24 不存在时
 * 返回 NULL——调用方（ingest_pos/ingest_id）在这种情况下直接放弃这条更新，
 * 不覆盖任何已有条目。 */
pk_idx_rec_t *pk_rec_idx_find_or_add(pk_rec_idx_table_t *t,
                                     const uint8_t icao24[3]);

/* 用一条位置/身份记录更新按机摘要。is_own 由调用方判定（运行期：比对
 * 当前绑定的 ICAO；重建：比对传入的 own_icao 参数）。 */
void pk_rec_idx_ingest_pos(pk_rec_idx_table_t *t, const pk_trk_pos_t *pos, bool is_own);
void pk_rec_idx_ingest_id(pk_rec_idx_table_t *t, const pk_trk_id_t *id, bool is_own);

/* 掉电重建：给一段连续的 traffic.trk 记录（record_count 条，每条
 * PK_TRK_RECORD_LEN 字节，已经在内存里——不含 32B 文件头），按 rec_type
 * 分派重建整张索引表（先 reset）。own_icao 为 NULL 表示没有绑定机 /
 * 不关心 IS_OWN 标记。 */
void pk_rec_idx_rebuild_from_records(pk_rec_idx_table_t *t, const uint8_t *buf,
                                      size_t record_count, const uint8_t own_icao[3]);

/* 把表里的条目编码进 out（调用方保证容量 >= cap_records 条 ×
 * PK_IDX_RECORD_LEN 字节）。返回实际写入的条目数（= min(count, cap_records)）。 */
size_t pk_rec_idx_encode_all(const pk_rec_idx_table_t *t, uint8_t *out, size_t cap_records);

#ifdef __cplusplus
}
#endif
