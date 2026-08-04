/*
 * pk_tile_loader.c — 实现说明见 pk_tile_loader.h。
 */
#include "pk_tile_loader.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

/* 解码器用 vendored 上游 lodepng（纯 malloc、线程安全）而非 LVGL 自带那份：
 * LVGL 的 lodepng 走 lv_malloc(TLSF,非线程安全)且返回 lv_draw_buf_t*——本任务
 * 与 LVGL 任务并发进其分配器会撞（真机实测:进地图页卡死）。本文件从此不碰
 * 任何 LVGL API。 */
#include "third_party/pk_lodepng.h"

/* lodepng 的错误码表里 83 = "memory allocation failed"（pk_lodepng.c:6991，
 * 上游没给这些码起名字，这里补一个，别在业务代码里写裸 83）。 */
#define LODEPNG_ERR_OUT_OF_MEMORY 83u

#include "esp_attr.h"
#include "pk_sdcard.h"
#include "pk_tile_cache.h"
#include "display.h"               /* pk_rgb565 */
#include "ui_state.h"               /* pk_ui_toast_show */
#include "i18n_catalog.h"

static const char *TAG = "tile_loader";

#define MAP_DIR              "/sdcard/maps"
#define LOADER_TASK_STACK    6144
#define LOADER_QUEUE_DEPTH   32
#define LOADER_INFLIGHT_MAX  32
/* 原始 PNG 上限——spec §盒子端架构:内存预算,压缩输入 ≤1MB。超限直接当失败处理。 */
#define LOADER_PNG_MAX_BYTES (1u * 1024u * 1024u)
/* SD 状态探测周期:队列空闲时的 xQueueReceive 超时,顺带充当"卡插回来了"轮询。 */
#define LOADER_POLL_MS       300

typedef struct {
    size_t   pack_index;
    uint8_t  z;
    uint32_t x, y;
    uint32_t view_gen;
} loader_req_t;

/* 本模块的大静态一律放 PSRAM .bss，不占内部 RAM——这不是优化，是硬约束。
 *
 * ESP32-P4 rev<v3 的内存布局（heap/port/esp32p4/memory_layout.c）里，高位那
 * 两段（日志中的 18KiB + 224KiB）标着 startup_stack，**调度器启动之前不可
 * 分配**；那段窗口里系统能用的只有 [_heap_start_low, 0x4ff3afc0) 这一段，
 * 本工程实测仅 65KB 且已被 dsp_task 等吃到只剩约 1.5KB 余量。往内部 .bss
 * 里再加 2KB，vTaskStartScheduler 里 2KB 的定时器任务栈就分配不出来，直接
 * assert boot loop（2026-08-01：加 2KB 无意义填充同样必崩，与改动语义无关）。
 *
 * s_store 是 16 包 × (pk_pmtiles_t + path[]) 的冷结构，s_cache/s_inflight 同理
 * ——都只在 loader 任务与渲染线程里访问，不做 DMA，PSRAM 完全够快。
 * 三者合计曾占 dram0 约 11KB，正是压垮那 65KB 窗口的主因。 */
EXT_RAM_BSS_ATTR static pk_map_store_t s_store;
EXT_RAM_BSS_ATTR static pk_tile_cache_t s_cache;
static SemaphoreHandle_t s_lock;          /* 保护 s_store 的 meta 快照 + s_cache。
                                             绝不横跨 SD I/O 持有——渲染线程在
                                             try_blit/route 里等它,I/O 挪进 s_io_lock */
static SemaphoreHandle_t s_io_lock;       /* 串行化 pmtiles 文件 I/O(fetch/scan) 与
                                             pre-unmount 关句柄回调;锁序恒为
                                             io_lock → s_lock,反向禁止 */
static QueueHandle_t     s_queue;
static volatile uint32_t s_view_gen;
static bool               s_sd_was_mounted;
/* 上一轮看到的 pk_sdcard_media_error()，「卡在位但读不出」的边沿判定用。
 * 两个 worker 共享，读改写走 claim_edge() 的 CAS。 */
static bool               s_sd_media_err_seen;

/* 去重表:记录已入队/正在处理、尚未有结果的瓦片,避免 map_page 每帧重复请求
 * 同一块缺失瓦片把队列灌满。 */
typedef struct { bool used; size_t pack_index; uint8_t z; uint32_t x, y; } inflight_t;
EXT_RAM_BSS_ATTR static inflight_t s_inflight[LOADER_INFLIGHT_MAX];

static bool key_eq(size_t pi, uint8_t z, uint32_t x, uint32_t y,
                   size_t pi2, uint8_t z2, uint32_t x2, uint32_t y2)
{
    return pi == pi2 && z == z2 && x == x2 && y == y2;
}

static bool inflight_mark(size_t pack_index, uint8_t z, uint32_t x, uint32_t y)
{
    int free_slot = -1;
    for (int i = 0; i < LOADER_INFLIGHT_MAX; i++) {
        if (!s_inflight[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (key_eq(s_inflight[i].pack_index, s_inflight[i].z, s_inflight[i].x, s_inflight[i].y,
                  pack_index, z, x, y))
            return false;   /* 已经在跑 */
    }
    if (free_slot < 0) return false;   /* 去重表满,静默丢弃这次请求 */
    s_inflight[free_slot] = (inflight_t){ .used = true, .pack_index = pack_index,
                                          .z = z, .x = x, .y = y };
    return true;
}

static void inflight_clear(size_t pack_index, uint8_t z, uint32_t x, uint32_t y)
{
    for (int i = 0; i < LOADER_INFLIGHT_MAX; i++) {
        if (s_inflight[i].used &&
            key_eq(s_inflight[i].pack_index, s_inflight[i].z, s_inflight[i].x, s_inflight[i].y,
                  pack_index, z, x, y)) {
            s_inflight[i].used = false;
            return;
        }
    }
}

/* ── 请求/路由/blit：渲染线程调用 ─────────────────────────────────── */

bool pk_tile_loader_route(uint8_t z, uint32_t x, uint32_t y, pk_map_route_result_t *out)
{
    pk_map_pack_meta_t meta[PK_MAP_STORE_MAX_PACKS];
    size_t count;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    count = s_store.count;
    for (size_t i = 0; i < count; i++) meta[i] = s_store.packs[i].meta;
    xSemaphoreGive(s_lock);

    if (count == 0) { memset(out, 0, sizeof(*out)); return false; }
    *out = pk_map_route_find(meta, count, z, x, y, /*min_zoom_floor=*/0);
    return out->found;
}

size_t pk_tile_loader_pack_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_store.count;
    xSemaphoreGive(s_lock);
    return n;
}

/* 最近邻裁剪放大 blit：src 是 route.actual 瓦片的 256×256 RGB565，裁出
 * (crop, crop) 大小的子块(左上角 crop_x0,crop_y0),放大填满 dst 的 256×256
 * 区域(scale = 256/crop)。scale==1 时退化成整块直拷。 */
static void blit_tile_scaled(const uint16_t *src, uint32_t scale,
                             uint32_t crop_x0, uint32_t crop_y0, uint32_t crop,
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
    (void)crop;
}

bool pk_tile_loader_try_blit(const pk_map_route_result_t *route,
                             uint32_t req_x, uint32_t req_y,
                             uint16_t *fb, int dst_x0, int dst_y0,
                             uint32_t now_ms, bool *out_negative)
{
    *out_negative = false;
    if (!route->found) return false;

    /* scale 上限钳到 256(2^8)：crop 尺寸最小 1 像素,再大只是更深的马赛克,
     * 不再有视觉意义,也避免整数除法出现 crop=0。 */
    uint32_t scale = route->scale;
    if (scale > PK_TILE_PIXELS) scale = PK_TILE_PIXELS;
    uint32_t crop = PK_TILE_PIXELS / scale;

    /* 请求瓦片在 actual（更粗 zoom）瓦片里的子格位置：actual 瓦片覆盖
     * scale×scale 个当前 zoom 的瓦片格，req_x/req_y 落在其中第几格由
     * `req - (actual << log2(scale))` 给出（route.scale==1 时该式恒为 0，
     * 与"精确命中、不裁剪"的语义一致）。 */
    uint32_t shift = 0;
    for (uint32_t s = scale; s > 1; s >>= 1) shift++;
    uint32_t local_x = req_x - (route->actual_x << shift);
    uint32_t local_y = req_y - (route->actual_y << shift);
    uint32_t crop_x0 = local_x * crop;
    uint32_t crop_y0 = local_y * crop;

    pk_tile_key_t key = {
        .pack_id = (uint32_t)route->pack_index,
        .z = route->actual_z, .x = route->actual_x, .y = route->actual_y,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool neg = false;
    const uint16_t *data = pk_tile_cache_get(&s_cache, key, now_ms, &neg);
    if (data == NULL) {
        xSemaphoreGive(s_lock);
        *out_negative = neg;
        return false;
    }
    blit_tile_scaled(data, scale, crop_x0, crop_y0, crop, fb, dst_x0, dst_y0);
    xSemaphoreGive(s_lock);
    return true;
}

bool pk_tile_loader_lock_sample(uint8_t z, uint32_t tx, uint32_t ty, uint32_t now_ms,
                                const uint16_t **out_data, uint32_t *out_shift,
                                uint32_t *out_crop_x0, uint32_t *out_crop_y0)
{
    pk_map_route_result_t route;
    if (!pk_tile_loader_route(z, tx, ty, &route)) return false;

    uint32_t scale = route.scale;
    if (scale > PK_TILE_PIXELS) scale = PK_TILE_PIXELS;
    uint32_t crop = PK_TILE_PIXELS / scale;
    uint32_t shift = 0;
    for (uint32_t s = scale; s > 1; s >>= 1) shift++;
    uint32_t local_x = tx - (route.actual_x << shift);
    uint32_t local_y = ty - (route.actual_y << shift);

    pk_tile_key_t key = {
        .pack_id = (uint32_t)route.pack_index,
        .z = route.actual_z, .x = route.actual_x, .y = route.actual_y,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool neg = false;
    const uint16_t *data = pk_tile_cache_get(&s_cache, key, now_ms, &neg);
    if (data == NULL) {
        xSemaphoreGive(s_lock);
        return false;
    }
    *out_data = data;
    *out_shift = shift;
    *out_crop_x0 = local_x * crop;
    *out_crop_y0 = local_y * crop;
    return true;   /* 锁仍持有,调用方用完必须调 pk_tile_loader_unlock_sample() */
}

void pk_tile_loader_unlock_sample(void)
{
    xSemaphoreGive(s_lock);
}

bool pk_tile_loader_try_blit_ancestor(uint8_t z, uint32_t x, uint32_t y,
                                      uint16_t *fb, int dst_x0, int dst_y0,
                                      uint32_t now_ms, int max_levels_up)
{
    for (int k = 1; k <= max_levels_up && k <= z; k++) {
        uint8_t  az = (uint8_t)(z - k);
        uint32_t ax = x >> k, ay = y >> k;

        pk_map_route_result_t route;
        if (!pk_tile_loader_route(az, ax, ay, &route)) continue;
        /* 只认精确命中的祖先：route 自己还在 overzoom 说明那一级也没数据，
           继续往上找更粗的，别在这里叠两层放大。 */
        if (route.scale != 1) continue;

        pk_tile_key_t key = { .pack_id = (uint32_t)route.pack_index,
                              .z = az, .x = ax, .y = ay };
        uint32_t scale = 1u << k;
        if (scale > PK_TILE_PIXELS) break;      /* 再往上子块不足 1 像素，没意义 */
        uint32_t crop = PK_TILE_PIXELS / scale;
        uint32_t crop_x0 = (x & (scale - 1)) * crop;
        uint32_t crop_y0 = (y & (scale - 1)) * crop;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool neg = false;
        const uint16_t *data = pk_tile_cache_get(&s_cache, key, now_ms, &neg);
        if (data != NULL) {
            blit_tile_scaled(data, scale, crop_x0, crop_y0, crop, fb, dst_x0, dst_y0);
            xSemaphoreGive(s_lock);
            return true;
        }
        xSemaphoreGive(s_lock);
    }
    return false;
}

void pk_tile_loader_request(const pk_map_route_result_t *route)
{
    if (!route->found || s_queue == NULL) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool need = inflight_mark(route->pack_index, route->actual_z, route->actual_x, route->actual_y);
    xSemaphoreGive(s_lock);
    if (!need) return;

    loader_req_t req = {
        .pack_index = route->pack_index, .z = route->actual_z,
        .x = route->actual_x, .y = route->actual_y,
        .view_gen = s_view_gen,
    };
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        /* 队满：撤回去重标记，下一帧还会再试一次。 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        inflight_clear(req.pack_index, req.z, req.x, req.y);
        xSemaphoreGive(s_lock);
    }
}

void pk_tile_loader_bump_view(void)
{
    s_view_gen++;
}

/* ── loader 任务：唯一碰 SD 卡的地方 ──────────────────────────────── */

/* 前 N 张瓦片打 INFO 级分段耗时(find/read/decode),之后降 DEBUG——
 * 优化对比要数据不要体感,又不能让稳态刷屏。 */
static volatile uint32_t s_timing_logged;
#define TIMING_LOG_FIRST_N 20

/* 把所有打了 io_error 的包句柄重开一遍，返回是否至少有一个包出过 I/O 故障。
 * 必须在持 s_io_lock 时调用（要碰 FILE*，与 sd_close_files_cb 互斥）。
 *
 * 为什么要整包重开而不是重试一次读：FatFs 的 FIL 在 disk_read 失败后把
 * fp->err 钉死，同一个句柄再读多少次都直接返回错误，不会真去碰卡。
 * 卡已经拔掉时 fopen 会失败，io_error 保持置位，下一轮插卡重扫会整体重来。 */
static bool reopen_faulted_packs(size_t pack_count)
{
    bool any = false;
    for (size_t i = 0; i < pack_count && i < PK_MAP_STORE_MAX_PACKS; i++) {
        pk_map_pack_t *p = &s_store.packs[i];
        if (!p->pm.io_error) continue;
        any = true;
        bool ok = pk_pmtiles_reopen_file(&p->pm, p->path);
        ESP_LOGW(TAG, "包 %s 读盘出错（FatFs 句柄已钉死），重开句柄：%s",
                 p->path, ok ? "成功" : "失败");
    }
    return any;
}

/* fetch_and_decode 的三态结果。为什么不能只用 bool（2026-08-02 真机自检）：
 * 老代码"失败即 put_negative"，把「这块瓦片确实不存在」和「这次内存/IO 不
 * 凑手」混为一谈。PSRAM 被吃穿时后者成片出现，负缓存 30 s TTL 把整屏钉成
 * 占位网格，且负缓存条目还会把正常瓦片从 LRU 里挤掉——一次瞬时抖动放大成
 * 半分钟的全屏缺图。瞬时失败必须原样退回，让下一帧重试。 */
typedef enum {
    FETCH_OK = 0,
    FETCH_MISSING,      /* 数据本身不存在/不可用 → 该进负缓存 */
    FETCH_TRANSIENT,    /* 内存不足、SD 读失败等 → 不进负缓存，下一帧重试 */
} fetch_result_t;

static fetch_result_t fetch_and_decode(size_t pack_index, uint8_t z, uint32_t x, uint32_t y)
{
    int64_t t0 = esp_timer_get_time();
    pk_map_pack_t *pack;
    pk_pmtiles_tile_loc_t loc;
    pk_map_route_result_t route_unused;

    /* 这里直接按 (pack_index,z,x,y) 找目录项，而不是重新走
     * pk_map_store_get_tile(z,x,y=请求坐标)——那个签名找的是"请求 zoom"的
     * 瓦片并自己做 overzoom 路由；我们已经知道要的就是 actual 瓦片本身
     * （z/x/y 在这里就是 actual_z/actual_x/actual_y），所以直接对选中的包
     * 调 pk_pmtiles_find_tile。 */
    /* io_lock 覆盖 find→fread 全程：中途放锁会给 sd_close_files_cb 一个把
     * FILE* 关掉的窗口，回来 fread 就是已关句柄。渲染线程不等这把锁，长持无害。 */
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t pack_count = s_store.count;
    bool idx_ok = pack_index < pack_count;
    pack = idx_ok ? &s_store.packs[pack_index] : NULL;
    xSemaphoreGive(s_lock);          /* meta 校验完立刻放,I/O 只握 io_lock */
    (void)route_unused;

    bool found = idx_ok && pk_pmtiles_find_tile(&pack->pm, z, x, y, &loc);
    if (!found) {
        /* 路由只看包的 bounds/zoom 矩形范围，命中的包不一定真有这块瓦片：
         * 边界那一列瓦片"贴着"包边界时会被判为覆盖，而出包渲染的是严格落在
         * bounds 内的瓦片，于是那一列就是洞（罩哥 2026-08-01 实测：z9 x=415
         * 整列空白——该列右边缘正好压在珠三角试点包的西边界 112.5°E 上）。
         *
         * 这种"声称覆盖、实际没有"的洞，降级到其它同样覆盖该瓦片的包去找，
         * 通常就落回全球底图包。缓存 key 仍用路由选中的 pack_index（渲染线程
         * 按同一套路由算 key），只是数据来源换了一个包，两边保持一致。 */
        for (size_t i = 0; i < pack_count && !found; i++) {
            if (i == pack_index) continue;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            pk_map_pack_t *alt = (i < s_store.count) ? &s_store.packs[i] : NULL;
            bool z_ok = alt && z >= alt->meta.min_zoom && z <= alt->meta.max_zoom;
            xSemaphoreGive(s_lock);
            if (!z_ok) continue;
            if (pk_pmtiles_find_tile(&alt->pm, z, x, y, &loc)) {
                pack = alt;
                found = true;
            }
        }
    }
    if (!found) {
        /* 先分清"目录里确实没有"和"目录压根读不出来"。后者会被 io_error 标
         * 出来：FatFs 一次 disk_read 失败就把句柄钉死，之后整个包每次读都失败
         * ——这时候把整片区域记成"确认缺失"是彻头彻尾的误判（2026-08-02 实测：
         * 一次 sdmmc 0x106 之后，剩下 120 s 里该包一张瓦片都没能读出来）。 */
        bool io_fault = reopen_faulted_packs(pack_count);
        xSemaphoreGive(s_io_lock);
        if (io_fault) return FETCH_TRANSIENT;
        /* 这条曾经是整条链路上唯一一个「静默失败」：目录没命中就直接进负缓存，
         * 屏上表现为网格占位，但串口里一个字都没有——排查时分不清瓦片到底是
         * 「没请求」「读失败」还是「解码失败」，三者修法完全不同。补一条 W。 */
        ESP_LOGW(TAG, "瓦片 z%u(%u,%u) 所有包目录均未命中（稀疏空洞），pack=%u/%u",
                 z, x, y, (unsigned)pack_index, (unsigned)pack_count);
        return FETCH_MISSING;
    }
    if (loc.length == 0 || loc.length > LOADER_PNG_MAX_BYTES) {
        xSemaphoreGive(s_io_lock);
        ESP_LOGW(TAG, "tile z%u(%u,%u) 长度异常 %u,当失败处理", z, x, y,
                (unsigned)loc.length);
        return FETCH_MISSING;
    }

    uint8_t *png = malloc(loc.length);
    if (png == NULL) {
        xSemaphoreGive(s_io_lock);
        ESP_LOGW(TAG, "png scratch alloc 失败 (%u B)", (unsigned)loc.length);
        return FETCH_TRANSIENT;
    }

    int64_t t_find = esp_timer_get_time();
    bool ok = pack->pm.read(pack->pm.read_ctx,
                            pack->pm.header.tile_data_offset + loc.offset,
                            png, loc.length) == 0;
    xSemaphoreGive(s_io_lock);
    int64_t t_read = esp_timer_get_time();
    if (!ok) {
        free(png);
        /* SD 读失败（sdmmc 0x106 超时 / NO_MEM 等）：环境问题，不是"瓦片不存在"。
         *
         * 退避（2026-08-04）：真机抓包发现 0x106 在 SD 并发过载时密集重试——
         * 旧逻辑每次失败都 reopen（reopen 本身要读 FAT 表，火上浇油），且
         * TRANSIENT 不进负缓存、每帧重试 → "失败-重开-再失败"死循环刷屏。
         * 现在改为：
         *   - 只在首次失败 reopen（清 FatFs 粘滞），后续退避期间不重复 reopen；
         *   - 计数到阈值(3)插临时负缓存：这段时间内 map_page 走 ancestor blit
         *     （糊但不空白）、不再 request → 不再抢 SD，给卡喘息；
         *   - 持续失败(≥5)升级到长 TTL。read 成功后 reset 清零（见 FETCH_OK 路径）。
         * 仍 return TRANSIENT：临时负缓存 ≠ "确认缺失"，只是"这段时间别再 request"。*/
        pk_tile_key_t fkey = { .pack_id = (uint32_t)pack_index, .z = z, .x = x, .y = y };
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t fails;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        fails = pk_tile_cache_bump_sd_fail(&s_cache, fkey, now_ms);
        if (fails >= PK_TILE_CACHE_SD_FAIL_BACKOFF) {
            pk_tile_cache_put_temp_negative(&s_cache, fkey, now_ms);
        }
        xSemaphoreGive(s_lock);

        /* reopen 与 cache 操作用不同的锁，不能嵌套（锁序约定 io→s）。
         * 首次失败才 reopen：清 FatFs 粘滞；后续退避期不重复——reopen 要读
         * SD（FAT 表），过载期反复重开只会加剧争抢。 */
        if (fails == 1) {
            xSemaphoreTake(s_io_lock, portMAX_DELAY);
            pack->pm.io_error = true;
            reopen_faulted_packs(pack_count);
            xSemaphoreGive(s_io_lock);
        }

        if (fails >= PK_TILE_CACHE_SD_FAIL_BACKOFF) {
            ESP_LOGW(TAG, "读瓦片 z%u(%u,%u) 失败（第 %u 次，退避 %lus）",
                     z, x, y, fails,
                     (unsigned long)(fails >= 5 ? PK_TILE_CACHE_TEMP_NEG_LONG_TTL_MS
                                                : PK_TILE_CACHE_TEMP_NEG_TTL_MS) / 1000);
        } else {
            ESP_LOGW(TAG, "读瓦片 z%u(%u,%u) 失败（第 %u 次）", z, x, y, fails);
        }
        return FETCH_TRANSIENT;
    }

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    unsigned err = lodepng_decode32(&rgba, &pw, &ph, png, loc.length);
    free(png);
    if (err != 0 || rgba == NULL) {
        ESP_LOGW(TAG, "PNG 解码失败 z%u(%u,%u): lodepng err=%u", z, x, y, err);
        free(rgba);
        /* lodepng 的 83 = "memory allocation failed"（它自己的错误码表）。
         * 这条是内存紧张的信号，不是 PNG 坏了，绝不能记成"确认缺失"。 */
        return (err == LODEPNG_ERR_OUT_OF_MEMORY) ? FETCH_TRANSIENT : FETCH_MISSING;
    }
    if (pw != PK_TILE_PIXELS || ph != PK_TILE_PIXELS) {
        ESP_LOGW(TAG, "瓦片尺寸异常 z%u(%u,%u): %ux%u（期望 %dx%d）",
                z, x, y, pw, ph, PK_TILE_PIXELS, PK_TILE_PIXELS);
        free(rgba);
        return FETCH_MISSING;
    }

    /* acquire 会在 PSRAM 触及水位线时先淘汰最久未用的瓦片——缓存自己把
     * 内存让出来，而不是把内存耗光再让别人失败。持锁调用是它的契约。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t *buf = pk_tile_cache_acquire_buffer(&s_cache);
    xSemaphoreGive(s_lock);
    if (buf == NULL) {
        free(rgba);
        return FETCH_TRANSIENT;
    }
    for (int i = 0; i < PK_TILE_BUF_PIXELS; i++) {
        const unsigned char *p = &rgba[(size_t)i * 4];
        buf[i] = pk_rgb565(p[0], p[1], p[2]);
    }
    free(rgba);

    pk_tile_key_t key = { .pack_id = (uint32_t)pack_index, .z = z, .x = x, .y = y };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pk_tile_cache_put(&s_cache, key, buf);
    xSemaphoreGive(s_lock);

    int64_t t_done = esp_timer_get_time();
    uint32_t n = s_timing_logged++;
    if (n < TIMING_LOG_FIRST_N) {
        ESP_LOGI(TAG, "tile z%u(%u,%u) %uB: find=%dms read=%dms decode=%dms total=%dms",
                 z, x, y, (unsigned)loc.length,
                 (int)((t_find - t0) / 1000), (int)((t_read - t_find) / 1000),
                 (int)((t_done - t_read) / 1000), (int)((t_done - t0) / 1000));
    } else {
        ESP_LOGD(TAG, "tile z%u(%u,%u) total=%dms", z, x, y, (int)((t_done - t0) / 1000));
    }
    return FETCH_OK;
}

/* 认领一次布尔跳变：*flag（上一轮观测值）与 now（本轮观测值）不同时，把 *flag
 * 更新为 now 并返回 true；相同则返回 false。跳变的方向由调用方看 now 判断。
 *
 * 为什么要 CAS：handle_sd_transition() 同时跑在 tile_ld0/tile_ld1 两个 worker
 * 上，两者的轮询周期相同（LOADER_POLL_MS），"读 flag → 判断 → 写 flag"这段
 * 若不原子，两个任务可能都看到同一次跳变，于是重扫跑两遍、toast 弹两次。
 * 单条 CAS 保证一次跳变只被一个 worker 认领。 */
static bool claim_edge(bool *flag, bool now)
{
    bool expected = !now;
    return __atomic_compare_exchange_n(flag, &expected, now, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void handle_sd_transition(void)
{
    pk_sd_state_t st = pk_sdcard_state();

    /* 格式化期间 is_mounted() 为假，但那是用户在设置页显式发起的操作、不是
     * 插拔事件。不跳过的话每次格式化都会误弹一对"已拔出 / 已挂载"。 */
    if (st == PK_SD_FORMATTING) return;

    bool mounted = (st == PK_SD_MOUNTED);

    /* 挂载态跳变。开机就插着卡的情况**不会**在这里响：pk_tile_loader_init()
     * 已按真实状态给 s_sd_was_mounted 播种，首轮轮询看到的是"没变化"。
     * 常态不是事件，不该弹提示。 */
    if (claim_edge(&s_sd_was_mounted, mounted)) {
        if (mounted) {
            xSemaphoreTake(s_io_lock, portMAX_DELAY);   /* 锁序 io→s */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            pk_map_store_invalidate(&s_store);
            size_t n = pk_map_store_scan(&s_store, MAP_DIR);
            pk_tile_cache_bump_generation(&s_cache);
            xSemaphoreGive(s_lock);
            xSemaphoreGive(s_io_lock);
            ESP_LOGI(TAG, "SD (重新)挂载，重扫 %s：%u 个有效包", MAP_DIR, (unsigned)n);
            /* 报挂载**结果**而不是"卡插进来了"——后者用户自己看得见。扫出包
             * 才是绿的好消息；卡认了但 /maps 下没有有效包，对地图页来说和没卡
             * 一样用不了，按错误态提示，省得用户以为插上就好了。 */
            pk_ui_toast_show(n > 0 ? PK_TR_MAP_SD_MOUNTED : PK_TR_MAP_SD_NO_PACKS,
                             n == 0);
        } else {
            /* 文件句柄已由 pre-unmount 回调（sd_close_files_cb，在 pk_sdcard
             * 卸载序列里同步跑）关掉，这里只负责 toast/日志。包清单 meta 与
             * 缓存原样保留，已缓存瓦片继续显示（spec 错误态「运行中拔卡」）
             * 的行为不变——只是不会再有新瓦片进来，直到卡回来触发上面那支
             * 重扫。 */
            ESP_LOGW(TAG, "SD 卡被拔出——地图降级，已缓存瓦片继续显示");
            pk_ui_toast_show(PK_TR_MAP_SD_REMOVED, true);
        }
    }

    /* 「插了张读不出的卡」是独立于挂载状态的一条线：这种卡永远挂不上，
     * mounted 一直是 false，上面两支边沿都不会响。同样只在跳变时提示一次，
     * 否则 3 s 一轮的重试会把 toast 一直顶在屏上。 */
    bool media_err = pk_sdcard_media_error();
    if (claim_edge(&s_sd_media_err_seen, media_err) && media_err) {
        ESP_LOGW(TAG, "SD 卡在位但文件系统挂不上");
        pk_ui_toast_show(PK_TR_MAP_SD_UNREADABLE, true);
    }
}

int pk_tile_loader_pending(void)
{
    if (s_lock == NULL) return 0;
    /* 数的是**去重表**而不是队列长度：inflight 标记在 pk_tile_loader_request
     * 里 xQueueSend **之前**就打上、在 worker 处理完之后才清掉，所以它同时
     * 覆盖"排队中"与"正在解码中"两种状态。只看 uxQueueMessagesWaiting 会漏
     * 掉"队列已空但两个 worker 都在解 PNG"——那正是最该让路的时刻；两者相加
     * 则会把排队中的请求数两遍。 */
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LOADER_INFLIGHT_MAX; i++)
        if (s_inflight[i].used) n++;
    xSemaphoreGive(s_lock);
    return n;
}

static void loader_task(void *arg)
{
    (void)arg;
    while (1) {
        handle_sd_transition();

        loader_req_t req;
        if (xQueueReceive(s_queue, &req, pdMS_TO_TICKS(LOADER_POLL_MS)) != pdTRUE)
            continue;

        bool stale = (req.view_gen != s_view_gen);
        bool mounted = pk_sdcard_is_mounted();

        if (!stale && mounted) {
            fetch_result_t r = fetch_and_decode(req.pack_index, req.z, req.x, req.y);
            if (r == FETCH_MISSING) {
                pk_tile_key_t key = { .pack_id = (uint32_t)req.pack_index,
                                      .z = req.z, .x = req.x, .y = req.y };
                xSemaphoreTake(s_lock, portMAX_DELAY);
                pk_tile_cache_put_negative(&s_cache, key, (uint32_t)(esp_timer_get_time() / 1000));
                xSemaphoreGive(s_lock);
            }
            /* FETCH_TRANSIENT：不进负缓存。inflight 标记在下面清掉后，下一帧
             * map_page 还会再请求一次——瞬时失败该重试，不该被钉 30 s。 */
        }
        /* stale（视图已经翻页）或未挂载：直接丢弃，不入负缓存——那不是
         * "这块瓦片确认缺失"，只是这次请求已经过时/暂时拿不到卡。 */

        xSemaphoreTake(s_lock, portMAX_DELAY);
        inflight_clear(req.pack_index, req.z, req.x, req.y);
        xSemaphoreGive(s_lock);
    }
}

/* pk_sdcard 卸载前回调：关掉所有 pmtiles 文件句柄。fetch_and_decode 的
 * fread 全程持 s_lock，这里 take 本身就是「等在途读退出」的栅栏——回调
 * 返回时本模块保证无打开的 SD fd、无在途 SD I/O（pk_sdcard.h 契约）。
 * meta/缓存不动，「拔卡后已缓存瓦片继续显示」由 handle_sd_transition
 * 那条注释接管。 */
static void sd_close_files_cb(void)
{
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    pk_map_store_close_files(&s_store);
    xSemaphoreGive(s_io_lock);
}

/* ── PSRAM 自检（默认关闭）────────────────────────────────────────────
 * 「Z7 连续一片瓦片缺失」的怀疑对象之一是 PSRAM 被机型库/航空库吃光，
 * pk_tile_cache_alloc_tile_buffer() 分配不出来 → 进负缓存 → 屏上一片占位。
 * 手滑屏幕复现慢且不可量化，这里用程序化请求把缓存灌满，每 N 张打一次
 * PSRAM 余量 / 最大连续块 / 缓存槽数 / 分配失败数，直接给出曲线。
 *
 * 只在排查时临时改成 1 编译，不进常规固件——它会持续占满瓦片缓存。 */
#define PK_TILE_LOADER_SELFTEST 0

#if PK_TILE_LOADER_SELFTEST
#include "esp_heap_caps.h"

/* 华北（北京 39.9°N,116.4°E）在 z7 的瓦片坐标，其余 zoom 由位移得到。 */
#define SELFTEST_BASE_Z 7
#define SELFTEST_BASE_X 105
#define SELFTEST_BASE_Y 48
#define SELFTEST_RADIUS 6      /* 每级扫 (2r+1)^2 = 169 张，远超一屏 15 张 */

static void selftest_log(const char *phase, unsigned requested)
{
    int used = 0, neg = 0;
    size_t bytes = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pk_tile_cache_stats(&s_cache, &used, &neg, &bytes);
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "SELFTEST %s req=%u cache=%d槽/%uKB neg=%d allocfail=%u "
                  "psram_free=%uKB psram_largest=%uKB",
             phase, requested, used, (unsigned)(bytes / 1024), neg,
             (unsigned)pk_tile_cache_alloc_fail_count(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

static void selftest_task(void *arg)
{
    (void)arg;
    /* 等航空库/机型库在后台把 PSRAM 吃到稳态——自检要量的是稳态余量。 */
    vTaskDelay(pdMS_TO_TICKS(45000));
    selftest_log("baseline", 0);

    unsigned requested = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (uint8_t z = 5; z <= 10; z++) {
            int32_t cx = SELFTEST_BASE_X, cy = SELFTEST_BASE_Y;
            if (z >= SELFTEST_BASE_Z) { cx <<= (z - SELFTEST_BASE_Z); cy <<= (z - SELFTEST_BASE_Z); }
            else                      { cx >>= (SELFTEST_BASE_Z - z); cy >>= (SELFTEST_BASE_Z - z); }
            for (int dy = -SELFTEST_RADIUS; dy <= SELFTEST_RADIUS; dy++) {
                for (int dx = -SELFTEST_RADIUS; dx <= SELFTEST_RADIUS; dx++) {
                    int32_t tx = cx + dx, ty = cy + dy;
                    int32_t n = (int32_t)1 << z;
                    if (tx < 0 || ty < 0 || tx >= n || ty >= n) continue;

                    pk_map_route_result_t route;
                    if (pk_tile_loader_route(z, (uint32_t)tx, (uint32_t)ty, &route))
                        pk_tile_loader_request(&route);
                    requested++;
                    /* 给两个 worker 留出解码时间；太快只会把队列打满被丢弃，
                     * 量不出真实的缓存增长速度。 */
                    vTaskDelay(pdMS_TO_TICKS(40));
                    if (requested % 16 == 0) selftest_log("scan", requested);
                }
            }
        }
        selftest_log("pass-done", requested);
    }
    selftest_log("final", requested);
    vTaskDelete(NULL);
}
#endif /* PK_TILE_LOADER_SELFTEST */

void pk_tile_loader_init(void)
{
    if (s_lock != NULL) return;   /* 幂等 */

    s_lock    = xSemaphoreCreateMutex();
    s_io_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(LOADER_QUEUE_DEPTH, sizeof(loader_req_t));
    pk_tile_cache_init(&s_cache);
    memset(&s_store, 0, sizeof(s_store));
    pk_sdcard_register_pre_unmount_cb(sd_close_files_cb);

    if (pk_sdcard_is_mounted()) {
        size_t n = pk_map_store_scan(&s_store, MAP_DIR);
        ESP_LOGI(TAG, "启动扫描 %s：%u 个有效包", MAP_DIR, (unsigned)n);
    }
    /* 按真实状态播种，两条边沿都从"当前即常态"起步：开机就插着卡不弹
     * "已挂载"，开机就插着一张坏卡也不弹"读不出"——那是启动状态，不是
     * 用户刚做的动作，日志里已经有记录。 */
    s_sd_was_mounted     = pk_sdcard_is_mounted();
    s_sd_media_err_seen  = pk_sdcard_media_error();

    /* 双 worker 各钉一核:SD I/O 被 s_io_lock 串行化(SDMMC 本就串行),
       但 PNG 解码是纯 CPU——两核并行解码,首屏 12 张的解码墙钟近乎减半。
       handle_sd_transition 在两个任务里都会跑,幂等(状态翻转有 s_sd_was_mounted
       守卫,重扫在 io+s 双锁内)。 */
    BaseType_t ok0 = xTaskCreatePinnedToCore(loader_task, "tile_ld0",
                                             LOADER_TASK_STACK, NULL, 3, NULL, 0);
    BaseType_t ok1 = xTaskCreatePinnedToCore(loader_task, "tile_ld1",
                                             LOADER_TASK_STACK, NULL, 3, NULL, 1);
    if (ok0 != pdTRUE || ok1 != pdTRUE) ESP_LOGE(TAG, "loader task create failed");

#if PK_TILE_LOADER_SELFTEST
    xTaskCreate(selftest_task, "tile_st", 4096, NULL, 2, NULL);
#endif
}
