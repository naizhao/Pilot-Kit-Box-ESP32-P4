/* pk_map_store.c — 实现说明见 pk_map_store.h。 */
#include "pk_map_store.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>      /* snprintf() —— 两个分支都要用，之前只在非 ESP_PLATFORM
                          * 分支里 include 了，固件端隐式声明触发 -Werror */
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
static const char *TAG = "pk_map_store";
#define PK_MS_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define PK_MS_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define PK_MS_LOGI(fmt, ...) fprintf(stderr, "I pk_map_store: " fmt "\n", ##__VA_ARGS__)
#define PK_MS_LOGW(fmt, ...) fprintf(stderr, "W pk_map_store: " fmt "\n", ##__VA_ARGS__)
#endif

#define PK_MAP_STORE_SUFFIX ".pmtiles"

/* ------------------------------------------------------------ 纯路由逻辑 */

/* Web Mercator slippy-map 瓦片 (z,x,y) 的经纬度包围盒（度）。y 向南递增，
 * 所以 lat_max 对应 y、lat_min 对应 y+1。标准公式，PMTiles/OSM 生态通用。 */
static void tile_bbox_deg(uint8_t z, uint32_t x, uint32_t y,
                           double *min_lon, double *min_lat, double *max_lon, double *max_lat)
{
    double n = (double)(1u << z);
    *min_lon = (double)x / n * 360.0 - 180.0;
    *max_lon = (double)(x + 1) / n * 360.0 - 180.0;

    double lat_top    = atan(sinh(M_PI * (1.0 - 2.0 * (double)y / n)))       * 180.0 / M_PI;
    double lat_bottom = atan(sinh(M_PI * (1.0 - 2.0 * (double)(y + 1) / n))) * 180.0 / M_PI;
    *max_lat = lat_top;
    *min_lat = lat_bottom;
}

static bool bbox_intersects(double a_min_lon, double a_min_lat, double a_max_lon, double a_max_lat,
                             double b_min_lon, double b_min_lat, double b_max_lon, double b_max_lat)
{
    if (a_max_lon < b_min_lon || a_min_lon > b_max_lon) return false;
    if (a_max_lat < b_min_lat || a_min_lat > b_max_lat) return false;
    return true;
}

pk_map_route_result_t pk_map_route_find(const pk_map_pack_meta_t *packs, size_t count,
                                         uint8_t z, uint32_t x, uint32_t y,
                                         uint8_t min_zoom_floor)
{
    pk_map_route_result_t result;
    memset(&result, 0, sizeof(result));

    for (int zz = (int)z; zz >= (int)min_zoom_floor; zz--) {
        uint32_t shift = (uint32_t)z - (uint32_t)zz;
        uint32_t xx = x >> shift;
        uint32_t yy = y >> shift;

        double tmin_lon, tmin_lat, tmax_lon, tmax_lat;
        tile_bbox_deg((uint8_t)zz, xx, yy, &tmin_lon, &tmin_lat, &tmax_lon, &tmax_lat);

        long best = -1;
        int  best_max_zoom = -1;
        for (size_t i = 0; i < count; i++) {
            const pk_map_pack_meta_t *p = &packs[i];
            if (!p->valid) continue;
            if (zz < p->min_zoom || zz > p->max_zoom) continue;
            if (!bbox_intersects(tmin_lon, tmin_lat, tmax_lon, tmax_lat,
                                  p->min_lon, p->min_lat, p->max_lon, p->max_lat)) continue;
            if ((int)p->max_zoom > best_max_zoom) {
                best_max_zoom = p->max_zoom;
                best = (long)i;
            }
        }

        if (best >= 0) {
            result.found      = true;
            result.pack_index = (size_t)best;
            result.actual_z   = (uint8_t)zz;
            result.actual_x   = xx;
            result.actual_y   = yy;
            result.scale      = 1u << shift;
            return result;
        }

        if (zz == 0) break; /* 防 zz-- 在 int 下继续跑没问题，这里只是提前退出 */
    }
    return result; /* found=false */
}

/* ------------------------------------------------------------------ 扫描 */

static bool has_suffix(const char *name, const char *suffix)
{
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    if (nlen < slen) return false;
    return strcmp(name + nlen - slen, suffix) == 0;
}

static void fill_meta_from_header(pk_map_pack_meta_t *meta, const pk_pmtiles_header_t *h)
{
    meta->valid    = true;
    meta->min_lon  = (double)h->min_lon_e7 / 1e7;
    meta->min_lat  = (double)h->min_lat_e7 / 1e7;
    meta->max_lon  = (double)h->max_lon_e7 / 1e7;
    meta->max_lat  = (double)h->max_lat_e7 / 1e7;
    meta->min_zoom = h->min_zoom;
    meta->max_zoom = h->max_zoom;
}

size_t pk_map_store_scan(pk_map_store_t *store, const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        PK_MS_LOGW("opendir(%s) failed: %s", dir_path, strerror(errno));
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_suffix(ent->d_name, PK_MAP_STORE_SUFFIX)) continue;
        if (store->count >= PK_MAP_STORE_MAX_PACKS) {
            PK_MS_LOGW("scan: 达到包上限 %d，忽略 %s", PK_MAP_STORE_MAX_PACKS, ent->d_name);
            break;
        }

        char path[PK_MAP_STORE_MAX_PATH];
        int n = snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            PK_MS_LOGW("scan: 路径过长跳过 %s/%s", dir_path, ent->d_name);
            continue;
        }

        pk_map_pack_t *slot = &store->packs[store->count];
        memset(slot, 0, sizeof(*slot));
        if (!pk_pmtiles_open_file(&slot->pm, path)) {
            PK_MS_LOGW("scan: 坏包跳过 %s", path);
            continue; /* pk_pmtiles_open_file 失败时 slot->pm 已是全零，无需 close */
        }

        fill_meta_from_header(&slot->meta, &slot->pm.header);
        /* snprintf 而非 strncpy：path 长度已在上面按 sizeof(path) 校验过，
         * 这里只是保证目标结尾一定有 NUL——strncpy 在 src 长度≥n 时不补 NUL，
         * 触发 -Werror=stringop-truncation（slot 已 memset 过其实安全，但
         * gcc 的静态分析看不到跨函数的这层保证）。 */
        snprintf(slot->path, sizeof(slot->path), "%s", path);
        store->count++;
        PK_MS_LOGI("scan: 加载 %s (z%u-%u, bounds %.4f,%.4f,%.4f,%.4f)",
                   path, slot->meta.min_zoom, slot->meta.max_zoom,
                   slot->meta.min_lon, slot->meta.min_lat, slot->meta.max_lon, slot->meta.max_lat);
    }
    closedir(d);
    return store->count;
}

void pk_map_store_close_files(pk_map_store_t *store)
{
    /* 只关句柄不动 meta/count：拔卡后路由（pk_map_route_find 只看 meta）与
     * PSRAM 里的已缓存瓦片都要继续工作，「拔卡后已缓存瓦片继续显示」这条
     * 产品行为全靠清单活着。彻底清空走 pk_map_store_invalidate（重挂前）。 */
    for (size_t i = 0; i < store->count; i++) {
        pk_pmtiles_close_file(&store->packs[i].pm);
    }
}

void pk_map_store_invalidate(pk_map_store_t *store)
{
    for (size_t i = 0; i < store->count; i++) {
        pk_pmtiles_close(&store->packs[i].pm);
    }
    memset(store, 0, sizeof(*store));
}

bool pk_map_store_get_tile(pk_map_store_t *store, uint8_t z, uint32_t x, uint32_t y,
                            pk_map_pack_t **out_pack, pk_pmtiles_tile_loc_t *out_loc,
                            pk_map_route_result_t *out_route)
{
    if (store->count == 0) return false;

    pk_map_pack_meta_t meta[PK_MAP_STORE_MAX_PACKS];
    for (size_t i = 0; i < store->count; i++) meta[i] = store->packs[i].meta;

    pk_map_route_result_t route = pk_map_route_find(meta, store->count, z, x, y, /*min_zoom_floor=*/0);
    if (!route.found) return false;

    pk_map_pack_t *pack = &store->packs[route.pack_index];
    pk_pmtiles_tile_loc_t loc;
    if (!pk_pmtiles_find_tile(&pack->pm, route.actual_z, route.actual_x, route.actual_y, &loc)) {
        /* bounds/zoom 元数据说这个包覆盖，但实际目录里这块瓦片是空洞（稀疏
         * 覆盖）——一期不在这里继续降级重试（设计文档路由那一节没有覆盖这个
         * 子情形），直接报未命中，跟"无包覆盖"同一种表现。 */
        PK_MS_LOGW("get_tile: 包 %s 声称覆盖 z%u(%u,%u) 但目录未命中（稀疏空洞）",
                   pack->path, route.actual_z, route.actual_x, route.actual_y);
        return false;
    }

    if (out_pack)  *out_pack  = pack;
    if (out_loc)   *out_loc   = loc;
    if (out_route) *out_route = route;
    return true;
}
