/*
 * pk_win_nearest.h — 窗口 nearest 查询的纯算法核心（不依赖任何 IDF 头）。
 *
 * 与 pk_aero_reader.c 的 nearest_generic 同属"3×3 格内找最近"，但**省掉
 * approx 粗排阶段**：窗口驻留格记录少（视口内几个格、每格几十条），直接全
 * 精算 Haversine + 有序插入，不需要 nearest_generic 那套"粗排选 N → 精算重排"
 * （那是因为全量库单格可能上千条）。区别只在数据来源：
 *   nearest_generic 读 db->payload（全量库，段内全局下标 idx）；
 *   本文件读调用方传入的「按格记录回调」（窗口驻留格，idx = 格首记录 + 段内偏移）。
 *
 * 抽出来单独成文件、不 include IDF 头，是为了 host 单测能直接编它、与全量
 * nearest_generic 逐条对拍（合成同一份数据，两边各查一遍，断言完全一致）。
 * 固件侧（pk_win.c）用一层薄胶水把 pk_win_cell_records 包成回调喂进来。
 *
 * 记录格式约定（与 airport/navaid/fix 段一致）：每条记录前 8 字节是
 * lat_e7(i32@0) / lon_e7(i32@4)，之后是段特有字段。本算法只读前 8 字节算
 * 距离，不碰段特有字段——那由调用方拿到 idx 后自行解码。
 *
 * 详见窗口化数据架构设计（内部文档） W1.4。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pk_aero_reader.h"   /* pk_aero_near_t / PK_AERO_NEAR_MAX / *_SLACK */

#ifdef __cplusplus
extern "C" {
#endif

/* 按格取记录的回调。对 cell 返回：记录基地址 recs、条数 n、首记录的段内
 * 全局下标 first（窗口里记录是局部的，idx 对齐靠它）。cell 无数据返回 false。
 * ctx 透传给算法，供调用方携带自己的状态（如 pk_win 的锁/格集合）。
 * recs 指向的内存必须在算法返回前有效（调用方持锁保证）。 */
typedef bool (*pk_win_rec_fn)(uint16_t cell, const uint8_t **recs,
                              uint32_t *n, uint32_t *first, void *ctx);

/* 窗口 nearest：在 query 点 (lat,lon) 周围 3×3 格里找最近的 max 条记录。
 *
 *   cells       要扫的格集合（通常是 query 点的 3×3 邻域，共 ≤9 格）。
 *   n_cells     cells 的条数。
 *   rec_size    单条记录字节数（同 pk_aero_section_t.rec_size）。
 *   rec_fn/ctx  按格取记录的回调 + 透传上下文。
 *   out/max     输出（升序，按距离）。max 上限 PK_AERO_NEAR_MAX。
 *
 * 与 nearest_generic 的语义差异：只扫 cells 给的那些格（nearest_generic 扫
 * 的是 query 点的 3×3）；若 cells 里有格回调返回 false（窗口没加载到），
 * 该格静默跳过——返回值可能少于全量查询。调用方据此判断要不要 fallback。
 *
 * 返回写入 out 的条数。纯函数，无全局状态，host 可直接编。
 * （固件侧包装 pk_win_nearest 在 pk_win.c，内部调本函数。） */
int pk_win_nearest_compute(const uint16_t *cells, int n_cells, uint16_t rec_size,
                   double lat, double lon,
                   pk_win_rec_fn rec_fn, void *ctx,
                   pk_aero_near_t *out, int max);

#ifdef __cplusplus
}
#endif
