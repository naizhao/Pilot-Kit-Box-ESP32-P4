/*
 * pk_pmtiles.h — PMTiles v3 只读解析器（SD 离线地图一期）。
 *
 * 设计依据 docs/superpowers/specs/2026-08-01-sd-offline-map-design.md：
 *   header（127B）→ 根/叶目录（gzip 压缩，Hilbert tileid 排序）→ 目录 entry
 *   二分查找 → (offset,length) 落到 tile_data 段。
 *
 * I/O 与平台解耦：所有读盘都走调用方传入的 pk_pmtiles_read_fn 回调，本文件
 * 不直接碰 FILE* 或 VFS——固件侧后续用 /sdcard fopen 实现回调，host 单测直接
 * 喂样本文件（pk_pmtiles_open_file 是二者共用的 POSIX 便捷封装）。
 *
 * gzip 解压后端也是平台相关的一根缝：ESP_PLATFORM 编译时用 esp_rom 自带的
 * miniz.h 声明 + ROM 里已经烧好的 tinfl 实现（零 flash 成本）；host 单测/
 * 非 ESP 构建用 third_party/pk_tinfl.{h,c} 这份 vendor 版（ROM 没有源码可
 * 链接）。gzip 容器头部解析（跳过 FEXTRA/FNAME/FCOMMENT/FHCRC、读 ISIZE 预
 * 分配输出缓冲）两侧完全共享，写在 pk_pmtiles.c 里，不重复。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 随机读回调：把 [off, off+len) 读进 buf。成功返回 0；失败（含 EOF 截断）
 * 一律返回非 0，调用方不做部分读重试。 */
typedef int (*pk_pmtiles_read_fn)(void *ctx, uint64_t off, void *buf, size_t len);

#define PK_PMTILES_HEADER_LEN 127

/* internal_compression / tile_compression 字段取值，spec v3 定义。 */
typedef enum {
    PK_PMTILES_COMPRESSION_UNKNOWN = 0,
    PK_PMTILES_COMPRESSION_NONE    = 1,
    PK_PMTILES_COMPRESSION_GZIP    = 2,
    PK_PMTILES_COMPRESSION_BROTLI  = 3,
    PK_PMTILES_COMPRESSION_ZSTD    = 4,
} pk_pmtiles_compression_t;

/* tile_type 字段取值。 */
typedef enum {
    PK_PMTILES_TYPE_UNKNOWN = 0,
    PK_PMTILES_TYPE_MVT     = 1,
    PK_PMTILES_TYPE_PNG     = 2,
    PK_PMTILES_TYPE_JPEG    = 3,
    PK_PMTILES_TYPE_WEBP    = 4,
    PK_PMTILES_TYPE_AVIF    = 5,
} pk_pmtiles_tiletype_t;

typedef struct {
    uint8_t  version;                 /* spec v3 固定为 3 */
    uint64_t root_dir_offset,      root_dir_length;
    uint64_t json_metadata_offset, json_metadata_length;
    uint64_t leaf_dirs_offset,     leaf_dirs_length;
    uint64_t tile_data_offset,     tile_data_length;
    uint64_t addressed_tiles_count, tile_entries_count, tile_contents_count;
    bool     clustered;
    uint8_t  internal_compression;    /* pk_pmtiles_compression_t */
    uint8_t  tile_compression;        /* pk_pmtiles_compression_t */
    uint8_t  tile_type;               /* pk_pmtiles_tiletype_t */
    uint8_t  min_zoom, max_zoom;
    int32_t  min_lon_e7, min_lat_e7, max_lon_e7, max_lat_e7;
    uint8_t  center_zoom;
    int32_t  center_lon_e7, center_lat_e7;
} pk_pmtiles_header_t;

/* 目录 entry：内部用，二分查找与 runlength 命中判定共用。 */
typedef struct {
    uint64_t tile_id;
    uint32_t run_length;   /* 0 表示这是一条叶目录指针，不是瓦片 */
    uint32_t length;
    uint64_t offset;       /* run_length>0: 相对 tile_data_offset；
                               run_length==0: 相对 leaf_dirs_offset */
} pk_pmtiles_dir_entry_t;

typedef struct {
    pk_pmtiles_read_fn   read;
    void                 *read_ctx;
    pk_pmtiles_header_t  header;

    /* 根目录常驻内存（open 时解出一次，随实例存活）。 */
    pk_pmtiles_dir_entry_t *root_entries;
    size_t                  root_count;

    /* pk_pmtiles_open_file 内部持有的 FILE*；pk_pmtiles_open 打开的实例为
     * NULL，close 时不去动调用方自己的 read_ctx。 */
    void *owned_file;
} pk_pmtiles_t;

typedef struct {
    uint64_t offset;   /* 相对 header.tile_data_offset 的偏移 */
    uint32_t length;
} pk_pmtiles_tile_loc_t;

/* 打开一个 PMTiles 实例：读 header、拉根目录并解压驻留。read_fn 生命周期
 * 由调用方保证覆盖到 pk_pmtiles_close 为止。失败（魔数/版本不对、读盘失
 * 败、目录解压失败）返回 false，*pm 保持全零可安全 close。 */
bool pk_pmtiles_open(pk_pmtiles_t *pm, pk_pmtiles_read_fn read_fn, void *read_ctx);

/* POSIX 便捷封装：内部 fopen(path)，读回调用 fseek+fread 实现。host 单测
 * 与固件本地文件场景（若用 fopen("/sdcard/...")）都能直接用。 */
bool pk_pmtiles_open_file(pk_pmtiles_t *pm, const char *path);

/* 只关内部持有的 FILE*（拔卡预卸载用）：header/根目录驻留内存原样保留，
 * 之后的 pm->read 一律干净返回失败（posix 读回调对 NULL ctx 返回 -1）。
 * pk_pmtiles_open 打开的实例（owned_file==NULL）不动调用方的 read_ctx，
 * 本函数是 no-op。幂等，可与随后的 pk_pmtiles_close 叠加。 */
void pk_pmtiles_close_file(pk_pmtiles_t *pm);

void pk_pmtiles_close(pk_pmtiles_t *pm);

/* (z,x,y) → tileid，PMTiles spec 规定的 Hilbert 曲线编号：
 * acc(z) = sum_{i=0}^{z-1} 4^i 个更粗 zoom 的瓦片排在前面，
 * 同一 zoom 内按 Hilbert 曲线距离排序。z 最大到 26（uint64 不溢出的上限，
 * spec 原文注明）。 */
uint64_t pk_pmtiles_zxy_to_tileid(uint8_t z, uint32_t x, uint32_t y);

/* 查找 (z,x,y) 对应瓦片，命中写 *out（tile_data 段内 offset+length）并返回
 * true；越界/未命中（含目录里干脆没有这个 tile_id）返回 false。 */
bool pk_pmtiles_find_tile(pk_pmtiles_t *pm, uint8_t z, uint32_t x, uint32_t y,
                           pk_pmtiles_tile_loc_t *out);

#ifdef __cplusplus
}
#endif
