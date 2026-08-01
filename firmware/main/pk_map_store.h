/*
 * pk_map_store.h — SD 卡离线地图包清单：扫描 /sdcard/maps、按 (z,x,y) 路由到
 * 覆盖该瓦片且 maxzoom 最深的包，无包覆盖时逐级向低 zoom 回退取父瓦片
 * （overzoom）。设计依据
 * docs/superpowers/specs/2026-08-01-sd-offline-map-design.md「SD 卡布局」节。
 *
 * 分层同 pk_pmtiles：路由判定（pk_map_route_find）只碰 header 里的
 * bounds/zoom 元数据，不碰磁盘，host 单测直接喂合成的 pk_map_pack_meta_t
 * 数组；pk_map_store_scan/get_tile 才真正 opendir + pk_pmtiles_open_file。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pk_pmtiles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一个包最多扫这么多个 —— SD 卡上按设计文档的分包粒度（global/cn/us/测试包），
 * 个位数量级，给到两位数上限足够留余量，同时给 pk_map_store_t 一个固定大小
 * （不用堆分配包数组，热插拔重扫更简单）。 */
#define PK_MAP_STORE_MAX_PACKS 16
#define PK_MAP_STORE_MAX_PATH  256

/* 一个包的纯元数据：路由只看这些字段，不碰实际瓦片目录/数据。
 * bounds 用 double 经纬度（由 header 的 e7 定点转来，一次性转换，避免路由热
 * 路径里反复整数转浮点）。 */
typedef struct {
    bool    valid;      /* false = 扫描时这个槽位打开失败/未使用，路由要跳过 */
    double  min_lon, min_lat, max_lon, max_lat;
    uint8_t min_zoom, max_zoom;
} pk_map_pack_meta_t;

typedef struct {
    bool     found;
    size_t   pack_index;   /* 命中 packs[] 里的下标 */
    uint8_t  actual_z;     /* 实际取到的 zoom（overzoom 时 < 请求的 z） */
    uint32_t actual_x, actual_y;
    uint32_t scale;        /* 1 = 精确命中；2^n = overzoom 放大倍数 */
} pk_map_route_result_t;

/* 纯函数：在 packs[0..count) 里找覆盖 (z,x,y) 的包。
 *   1) 从 zz=z 开始，若某个 valid 包满足 min_zoom<=zz<=max_zoom 且瓦片
 *      (zz, x>>(z-zz), y>>(z-zz)) 的经纬度包围盒与包 bounds 相交，则它是候选；
 *      多个候选取 max_zoom 最深的那个。
 *   2) zz 没有候选就 zz-- 重试（overzoom 回退），直到 zz < min_zoom_floor
 *      仍无候选则 found=false。
 * 不做任何 I/O —— 这是本文件里唯一为 host 单测设计、不依赖真实 pmtiles 文件
 * 的入口。 */
pk_map_route_result_t pk_map_route_find(const pk_map_pack_meta_t *packs, size_t count,
                                         uint8_t z, uint32_t x, uint32_t y,
                                         uint8_t min_zoom_floor);

/* --------------------------------------------------------------- 带 I/O 的包清单 */

typedef struct {
    pk_pmtiles_t        pm;
    pk_map_pack_meta_t  meta;
    char                path[PK_MAP_STORE_MAX_PATH];
} pk_map_pack_t;

typedef struct {
    pk_map_pack_t packs[PK_MAP_STORE_MAX_PACKS];
    size_t        count;
} pk_map_store_t;

/* 扫描 dir_path 下所有 *.pmtiles，逐包 pk_pmtiles_open_file；坏包（打不开/
 * 魔数或版本不对/目录解压失败）跳过并记日志，不影响其余包。调用前
 * store 须已 memset 为 0 或刚 pk_map_store_invalidate 过。返回成功打开的
 * 包数（0 也算成功调用，只是没有可用包）。superset 目录里非 .pmtiles 文件
 * 一律忽略。 */
size_t pk_map_store_scan(pk_map_store_t *store, const char *dir_path);

/* 拔卡预卸载：只对每个包关文件句柄（pk_pmtiles_close_file），meta 与 count
 * 原样保留——路由继续命中、PSRAM 里已缓存瓦片继续显示，只是任何读盘从此
 * 干净失败。与 invalidate 的分工：close_files = 拔卡时关句柄保清单；
 * invalidate = 重挂前彻底清空再 scan。幂等。 */
void pk_map_store_close_files(pk_map_store_t *store);

/* 拔卡/整体失效：关闭所有已打开的包（pk_pmtiles_close），count 归零。
 * 之后可以再次 pk_map_store_scan 重新加载（例如插回卡）。对已被
 * close_files 关过文件的包同样安全（close 内部按 owned_file 判空）。 */
void pk_map_store_invalidate(pk_map_store_t *store);

/* 查瓦片：先用 pk_map_route_find 在 store->packs 的 meta 上选包（含 overzoom
 * 回退），再对选中包实际调 pk_pmtiles_find_tile 拿 (offset,length)。
 * 成功写 *out_pack（供调用方 fseek 到 tile_data_offset+loc.offset 读)、
 * *out_loc、*out_route（actual_z/x/y/scale），返回 true。
 * 路由选中的包如果实际目录里没有这块瓦片（bounds/zoom 元数据覆盖但目录是
 * 稀疏空洞——一期未出现过，暂不处理），按未命中处理，不再降级重试。 */
bool pk_map_store_get_tile(pk_map_store_t *store, uint8_t z, uint32_t x, uint32_t y,
                            pk_map_pack_t **out_pack, pk_pmtiles_tile_loc_t *out_loc,
                            pk_map_route_result_t *out_route);

#ifdef __cplusplus
}
#endif
