/*
 * pk_tile_loader_sim.c — sim 侧对 firmware/main/pk_tile_loader.h 的同步实现。
 *
 * 真机版（firmware/main/pk_tile_loader.c）靠独立 FreeRTOS 任务 + 队列做异步
 * 解码，专门解决"渲染线程不能被磁盘 I/O 卡住"这件事。sim 是单线程、跑一次
 * 截图就退出的工具，没有 FreeRTOS，也没有那个问题要解决——这里改用最简单的
 * 同步实现：pk_tile_loader_try_blit() 未命中时就地走 pk_map_store（fw 源码，
 * host 可编译）→ pk_pmtiles → lodepng 解码 → 转 RGB565 → 存进本文件的内存
 * 缓存 → blit。pk_tile_loader_request()/_bump_view() 因此是空实现：同步模型
 * 里没有"排队等 loader 任务处理"或"视图已翻页、丢弃旧请求"这两件事，但函数
 * 必须留着——map_page.c（不可改）在这两条路径上无条件调用它们。
 *
 * PNG 解码复用 sim 已经链接的 LVGL lodepng 模块（sim/lv_conf.h 打开了
 * LV_USE_LODEPNG=1），不用另外 vendor 一份：与固件是同一份 LVGL v9.5.0
 * 源码同一个模块，解码行为逐字节一致。
 */
#include "pk_tile_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "libs/lodepng/lodepng.h"

#include "display.h"       /* pk_rgb565 / PK_DISPLAY_W / PK_DISPLAY_H */
#include "pk_map_store.h"
#include "pk_pmtiles.h"
#include "pk_tile_cache.h"  /* PK_TILE_PIXELS / PK_TILE_BUF_PIXELS / PK_TILE_BUF_BYTES */

#ifndef PK_SIM_MAPS_DIR_DEFAULT
#define PK_SIM_MAPS_DIR_DEFAULT "datafiles/maps"
#endif

/* 一次性截图跑批，一次进程里碰到的不同瓦片数量远够不到这个上限（可见范围
 * 800×480 逻辑像素 ÷ 256 一格，满打满算几十格）；给够余量就不用做真 LRU。 */
#define SIM_TILE_CACHE_MAX 512
/* 原始 PNG 上限，同真机 pk_tile_loader.c 的 LOADER_PNG_MAX_BYTES 那条注释：
 * 超限直接当失败处理，不去读一个明显不是瓦片的文件。 */
#define SIM_PNG_MAX_BYTES (1u * 1024u * 1024u)

typedef struct {
    bool      used;
    bool      negative;   /* true：路由命中的包，但目录里没有这块瓦片/解码失败 */
    size_t    pack_index;
    uint8_t   z;
    uint32_t  x, y;
    uint16_t *data;        /* used&&!negative 时非 NULL，PK_TILE_BUF_PIXELS 个像素 */
} sim_tile_slot_t;

static pk_map_store_t  s_store;
static bool             s_inited;
static sim_tile_slot_t  s_cache[SIM_TILE_CACHE_MAX];
static int              s_cache_n;

static sim_tile_slot_t *cache_find(size_t pack_index, uint8_t z, uint32_t x, uint32_t y)
{
    for (int i = 0; i < s_cache_n; i++) {
        sim_tile_slot_t *s = &s_cache[i];
        if (s->used && s->pack_index == pack_index && s->z == z && s->x == x && s->y == y)
            return s;
    }
    return NULL;
}

static sim_tile_slot_t *cache_new_slot(void)
{
    if (s_cache_n < SIM_TILE_CACHE_MAX) return &s_cache[s_cache_n++];
    /* 撞上限：复用最后一格。截图场景够不到这里，真撞到了也只是最新瓦片
     * 顶掉一张旧的，不影响正确性，只是那张旧瓦片这次要重新解码。 */
    sim_tile_slot_t *s = &s_cache[SIM_TILE_CACHE_MAX - 1];
    free(s->data);
    memset(s, 0, sizeof(*s));
    return s;
}

/* 同 firmware/main/pk_tile_loader.c 的 fetch_and_decode：读 PMTiles 目录项
 * 指向的那段字节 → lodepng_decode32 → RGBA8 逐像素转 pk_rgb565（大端，
 * ST7789 线序，与真机约定一致）。成功返回 malloc 的 PK_TILE_BUF_BYTES
 * 缓冲区，失败返回 NULL。 */
static uint16_t *decode_tile(pk_map_pack_t *pack, const pk_pmtiles_tile_loc_t *loc)
{
    if (loc->length == 0 || loc->length > SIM_PNG_MAX_BYTES) {
        fprintf(stderr, "W pk_tile_loader_sim: 瓦片长度异常 %u，当失败处理\n",
                (unsigned)loc->length);
        return NULL;
    }

    uint8_t *png = (uint8_t *)malloc(loc->length);
    if (png == NULL) return NULL;

    bool ok = pack->pm.read(pack->pm.read_ctx,
                            pack->pm.header.tile_data_offset + loc->offset,
                            png, loc->length) == 0;
    if (!ok) {
        free(png);
        fprintf(stderr, "W pk_tile_loader_sim: 读瓦片字节失败\n");
        return NULL;
    }

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    unsigned err = lodepng_decode32(&rgba, &pw, &ph, png, loc->length);
    free(png);
    if (err != 0 || rgba == NULL) {
        if (rgba) lv_free(rgba);
        fprintf(stderr, "W pk_tile_loader_sim: PNG 解码失败 lodepng err=%u\n", err);
        return NULL;
    }

    /* LVGL lodepng 返回的 rgba 实际上是 lv_draw_buf_t* 的转换。
     * 需要转换回来才能正确访问像素数据。详见 LVGL
     * src/libs/lodepng/lodepng.c 第 5800 行的 *out = (unsigned char*)decoded。 */
    lv_draw_buf_t *decoded_buf = (lv_draw_buf_t *)rgba;
    unsigned char *pixel_data = (unsigned char *)decoded_buf->data;

    if (pw != PK_TILE_PIXELS || ph != PK_TILE_PIXELS) {
        lv_draw_buf_destroy(decoded_buf);
        fprintf(stderr, "W pk_tile_loader_sim: 瓦片尺寸异常 %ux%u（期望 %dx%d）\n",
                pw, ph, PK_TILE_PIXELS, PK_TILE_PIXELS);
        return NULL;
    }

    uint16_t *buf = (uint16_t *)malloc(PK_TILE_BUF_BYTES);
    if (buf == NULL) { lv_draw_buf_destroy(decoded_buf); return NULL; }
    for (int i = 0; i < PK_TILE_BUF_PIXELS; i++) {
        const unsigned char *p = &pixel_data[(size_t)i * 4];
        buf[i] = pk_rgb565(p[0], p[1], p[2]);
    }
    lv_draw_buf_destroy(decoded_buf);
    return buf;
}

void pk_tile_loader_init(void)
{
    if (s_inited) return;
    memset(&s_store, 0, sizeof(s_store));

    /* PK_SIM_MAPS_DIR：测试数据目录（4 个真实 pmtiles 包，见
     * datafiles/README.md）。指向空目录/不存在的目录时
     * pk_map_store_scan 直接返回 0，走"无有效包"降级态——这正是
     * ui-4.3-map-no-pack 场景要的效果，不用另外分支处理。 */
    const char *dir = getenv("PK_SIM_MAPS_DIR");
    if (dir == NULL || dir[0] == '\0') dir = PK_SIM_MAPS_DIR_DEFAULT;
    size_t n = pk_map_store_scan(&s_store, dir);
    fprintf(stderr, "I pk_tile_loader_sim: 扫描 %s：%u 个有效包\n", dir, (unsigned)n);
    s_inited = true;
}

size_t pk_tile_loader_pack_count(void)
{
    pk_tile_loader_init();
    return s_store.count;
}

bool pk_tile_loader_route(uint8_t z, uint32_t x, uint32_t y, pk_map_route_result_t *out)
{
    pk_tile_loader_init();
    if (s_store.count == 0) { memset(out, 0, sizeof(*out)); return false; }

    pk_map_pack_meta_t meta[PK_MAP_STORE_MAX_PACKS];
    for (size_t i = 0; i < s_store.count; i++) meta[i] = s_store.packs[i].meta;
    *out = pk_map_route_find(meta, s_store.count, z, x, y, /*min_zoom_floor=*/0);
    return out->found;
}

/* 同真机 pk_tile_loader.c 的 blit_tile_scaled：从 src（route.actual 瓦片的
 * 256×256 RGB565）裁出 (crop,crop) 子块并最近邻放大填满 dst 的 256×256。 */
static void blit_tile_scaled(const uint16_t *src, uint32_t scale,
                             uint32_t crop_x0, uint32_t crop_y0,
                             uint16_t *fb, int dst_x0, int dst_y0)
{
    for (int dy = 0; dy < PK_TILE_PIXELS; dy++) {
        int py = dst_y0 + dy;
        if (py < 0 || py >= PK_DISPLAY_H) continue;
        uint32_t sy = crop_y0 + (uint32_t)dy / scale;
        for (int dx = 0; dx < PK_TILE_PIXELS; dx++) {
            int px = dst_x0 + dx;
            if (px < 0 || px >= PK_DISPLAY_W) continue;
            uint32_t sx = crop_x0 + (uint32_t)dx / scale;
            fb[py * PK_DISPLAY_W + px] = src[sy * PK_TILE_PIXELS + sx];
        }
    }
}

bool pk_tile_loader_try_blit(const pk_map_route_result_t *route,
                             uint32_t req_x, uint32_t req_y,
                             uint16_t *fb, int dst_x0, int dst_y0,
                             uint32_t now_ms, bool *out_negative)
{
    (void)now_ms;   /* 同步实现没有负缓存 TTL：失败态就是失败态，不需要过期重试 */
    *out_negative = false;
    if (!route->found) return false;

    sim_tile_slot_t *slot = cache_find(route->pack_index, route->actual_z,
                                       route->actual_x, route->actual_y);
    if (slot == NULL) {
        /* 未命中：这就是任务要的"同步走 store→pmtiles→PNG 解码→cache put"。
         * route 已经带了 actual_z/actual_x/actual_y（overzoom 时是父瓦片
         * 坐标），直接对选中的包查目录项，不用重新过一遍 pk_map_route_find。 */
        pk_map_pack_t *pack = &s_store.packs[route->pack_index];
        pk_pmtiles_tile_loc_t loc;
        bool found = pk_pmtiles_find_tile(&pack->pm, route->actual_z,
                                          route->actual_x, route->actual_y, &loc);

        slot = cache_new_slot();
        slot->used       = true;
        slot->pack_index = route->pack_index;
        slot->z = route->actual_z;
        slot->x = route->actual_x;
        slot->y = route->actual_y;
        slot->data     = found ? decode_tile(pack, &loc) : NULL;
        slot->negative = (slot->data == NULL);
    }

    if (slot->negative) { *out_negative = true; return false; }

    /* 请求瓦片在 actual（更粗 zoom）瓦片里的子格位置，公式同真机版：
     * local = req - (actual << log2(scale))。 */
    uint32_t scale = route->scale;
    if (scale > PK_TILE_PIXELS) scale = PK_TILE_PIXELS;
    uint32_t crop = PK_TILE_PIXELS / scale;
    uint32_t shift = 0;
    for (uint32_t s = scale; s > 1; s >>= 1) shift++;
    uint32_t local_x = req_x - (route->actual_x << shift);
    uint32_t local_y = req_y - (route->actual_y << shift);

    blit_tile_scaled(slot->data, scale, local_x * crop, local_y * crop, fb, dst_x0, dst_y0);
    return true;
}

/* 同真机 pk_tile_loader.c 的同名函数：heading-up 旋转扫描线的逐像素采样入口。
 * sim 是单线程同步模型，没有真机那把 s_lock，"锁定"在这里只是走一遍与
 * try_blit 相同的"未命中就同步解码"路径，再把裸指针递给调用方——语义上与
 * 真机版一致（返回的数据在下一次可能重新分配缓存槽之前有效），配对的
 * unlock_sample() 是空操作，但仍然要提供，因为 map_page.c 无条件调用它。 */
bool pk_tile_loader_lock_sample(uint8_t z, uint32_t tx, uint32_t ty, uint32_t now_ms,
                                const uint16_t **out_data, uint32_t *out_shift,
                                uint32_t *out_crop_x0, uint32_t *out_crop_y0)
{
    (void)now_ms;
    pk_map_route_result_t route;
    if (!pk_tile_loader_route(z, tx, ty, &route)) return false;

    sim_tile_slot_t *slot = cache_find(route.pack_index, route.actual_z,
                                       route.actual_x, route.actual_y);
    if (slot == NULL) {
        pk_map_pack_t *pack = &s_store.packs[route.pack_index];
        pk_pmtiles_tile_loc_t loc;
        bool found = pk_pmtiles_find_tile(&pack->pm, route.actual_z,
                                          route.actual_x, route.actual_y, &loc);
        slot = cache_new_slot();
        slot->used       = true;
        slot->pack_index = route.pack_index;
        slot->z = route.actual_z;
        slot->x = route.actual_x;
        slot->y = route.actual_y;
        slot->data     = found ? decode_tile(pack, &loc) : NULL;
        slot->negative = (slot->data == NULL);
    }
    if (slot->negative) return false;

    uint32_t scale = route.scale;
    if (scale > PK_TILE_PIXELS) scale = PK_TILE_PIXELS;
    uint32_t crop = PK_TILE_PIXELS / scale;
    uint32_t shift = 0;
    for (uint32_t s = scale; s > 1; s >>= 1) shift++;
    uint32_t local_x = tx - (route.actual_x << shift);
    uint32_t local_y = ty - (route.actual_y << shift);

    *out_data     = slot->data;
    *out_shift    = shift;
    *out_crop_x0  = local_x * crop;
    *out_crop_y0  = local_y * crop;
    return true;
}

void pk_tile_loader_unlock_sample(void)
{
    /* 空实现，见函数组注释。 */
}

/* 同真机 pk_tile_loader.c 的同名函数：未命中时从**已解码的上级瓦片**里裁子块
 * 放大顶上，让放大一级时画面保持连续。
 *
 * 与真机一样只认已在缓存里的祖先，不在这里触发解码：它是"缓存里恰好还有"
 * 的兜底，顺手把没画过的祖先也解出来，等于把 overzoom 变成一条正常加载路径，
 * 与真机行为就对不上了（真机那侧缓存没有就返回 false，由调用方画占位）。 */
bool pk_tile_loader_try_blit_ancestor(uint8_t z, uint32_t x, uint32_t y,
                                      uint16_t *fb, int dst_x0, int dst_y0,
                                      uint32_t now_ms, int max_levels_up)
{
    (void)now_ms;   /* 同步实现没有负缓存 TTL，同 try_blit */

    for (int k = 1; k <= max_levels_up && k <= (int)z; k++) {
        const uint8_t  az = (uint8_t)(z - k);
        const uint32_t ax = x >> k, ay = y >> k;

        pk_map_route_result_t route;
        if (!pk_tile_loader_route(az, ax, ay, &route)) continue;
        /* 只认精确命中的祖先：route 自己还在 overzoom 说明那一级也没数据，
           继续往上找更粗的，别在这里叠两层放大。（同真机版） */
        if (route.scale != 1) continue;

        uint32_t scale = 1u << k;
        if (scale > PK_TILE_PIXELS) break;   /* 再往上子块不足 1 像素，没意义 */

        const sim_tile_slot_t *slot = cache_find(route.pack_index, az, ax, ay);
        if (slot == NULL || slot->negative || slot->data == NULL) continue;

        const uint32_t crop = PK_TILE_PIXELS / scale;
        blit_tile_scaled(slot->data, scale,
                         (x & (scale - 1)) * crop, (y & (scale - 1)) * crop,
                         fb, dst_x0, dst_y0);
        return true;
    }
    return false;
}

void pk_tile_loader_request(const pk_map_route_result_t *route)
{
    /* 空实现：见文件头——try_blit 已经在未命中时同步把瓦片加载进缓存，
     * 不存在"发起请求、下一帧再来看结果"这一步。 */
    (void)route;
}

void pk_tile_loader_bump_view(void)
{
    /* 空实现：同步模型没有"视图已翻页、丢弃旧请求"的概念，见文件头。 */
}
