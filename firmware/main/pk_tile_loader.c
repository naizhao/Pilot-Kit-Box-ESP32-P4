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

static pk_map_store_t  s_store;
static pk_tile_cache_t s_cache;
static SemaphoreHandle_t s_lock;          /* 保护 s_store 的 meta 快照 + s_cache。
                                             绝不横跨 SD I/O 持有——渲染线程在
                                             try_blit/route 里等它,I/O 挪进 s_io_lock */
static SemaphoreHandle_t s_io_lock;       /* 串行化 pmtiles 文件 I/O(fetch/scan) 与
                                             pre-unmount 关句柄回调;锁序恒为
                                             io_lock → s_lock,反向禁止 */
static QueueHandle_t     s_queue;
static volatile uint32_t s_view_gen;
static bool               s_sd_was_mounted;

/* 去重表:记录已入队/正在处理、尚未有结果的瓦片,避免 map_page 每帧重复请求
 * 同一块缺失瓦片把队列灌满。 */
typedef struct { bool used; size_t pack_index; uint8_t z; uint32_t x, y; } inflight_t;
static inflight_t s_inflight[LOADER_INFLIGHT_MAX];

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

static bool fetch_and_decode(size_t pack_index, uint8_t z, uint32_t x, uint32_t y)
{
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
    bool idx_ok = pack_index < s_store.count;
    pack = idx_ok ? &s_store.packs[pack_index] : NULL;
    xSemaphoreGive(s_lock);          /* meta 校验完立刻放,I/O 只握 io_lock */
    (void)route_unused;

    bool found = idx_ok && pk_pmtiles_find_tile(&pack->pm, z, x, y, &loc);
    if (!found) {
        xSemaphoreGive(s_io_lock);
        return false;
    }
    if (loc.length == 0 || loc.length > LOADER_PNG_MAX_BYTES) {
        xSemaphoreGive(s_io_lock);
        ESP_LOGW(TAG, "tile z%u(%u,%u) 长度异常 %u,当失败处理", z, x, y,
                (unsigned)loc.length);
        return false;
    }

    uint8_t *png = malloc(loc.length);
    if (png == NULL) {
        xSemaphoreGive(s_io_lock);
        ESP_LOGW(TAG, "png scratch alloc 失败 (%u B)", (unsigned)loc.length);
        return false;
    }

    bool ok = pack->pm.read(pack->pm.read_ctx,
                            pack->pm.header.tile_data_offset + loc.offset,
                            png, loc.length) == 0;
    xSemaphoreGive(s_io_lock);
    if (!ok) {
        free(png);
        ESP_LOGW(TAG, "读瓦片 z%u(%u,%u) 失败", z, x, y);
        return false;
    }

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    unsigned err = lodepng_decode32(&rgba, &pw, &ph, png, loc.length);
    free(png);
    if (err != 0 || rgba == NULL) {
        ESP_LOGW(TAG, "PNG 解码失败 z%u(%u,%u): lodepng err=%u", z, x, y, err);
        free(rgba);
        return false;
    }
    if (pw != PK_TILE_PIXELS || ph != PK_TILE_PIXELS) {
        ESP_LOGW(TAG, "瓦片尺寸异常 z%u(%u,%u): %ux%u（期望 %dx%d）",
                z, x, y, pw, ph, PK_TILE_PIXELS, PK_TILE_PIXELS);
        free(rgba);
        return false;
    }

    uint16_t *buf = pk_tile_cache_alloc_tile_buffer();
    if (buf == NULL) {
        free(rgba);
        ESP_LOGW(TAG, "瓦片缓冲区分配失败（PSRAM 不足?）");
        return false;
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
    return true;
}

static void handle_sd_transition(void)
{
    bool mounted = pk_sdcard_is_mounted();
    if (mounted && !s_sd_was_mounted) {
        xSemaphoreTake(s_io_lock, portMAX_DELAY);   /* 锁序 io→s */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        pk_map_store_invalidate(&s_store);
        size_t n = pk_map_store_scan(&s_store, MAP_DIR);
        pk_tile_cache_bump_generation(&s_cache);
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_io_lock);
        ESP_LOGI(TAG, "SD (重新)挂载，重扫 %s：%u 个有效包", MAP_DIR, (unsigned)n);
    } else if (!mounted && s_sd_was_mounted) {
        /* 文件句柄已由 pre-unmount 回调（sd_close_files_cb，在 pk_sdcard
         * 卸载序列里同步跑）关掉，这里只负责 toast/日志。包清单 meta 与
         * 缓存原样保留，已缓存瓦片继续显示（spec 错误态「运行中拔卡」）
         * 的行为不变——只是不会再有新瓦片进来，直到卡回来触发上面那支
         * 重扫。 */
        ESP_LOGW(TAG, "SD 卡被拔出——地图降级，已缓存瓦片继续显示");
        pk_ui_toast_show(PK_TR_MAP_SD_REMOVED, true);
    }
    s_sd_was_mounted = mounted;
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
            bool ok = fetch_and_decode(req.pack_index, req.z, req.x, req.y);
            if (!ok) {
                pk_tile_key_t key = { .pack_id = (uint32_t)req.pack_index,
                                      .z = req.z, .x = req.x, .y = req.y };
                xSemaphoreTake(s_lock, portMAX_DELAY);
                pk_tile_cache_put_negative(&s_cache, key, (uint32_t)(esp_timer_get_time() / 1000));
                xSemaphoreGive(s_lock);
            }
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
    s_sd_was_mounted = pk_sdcard_is_mounted();

    BaseType_t ok = xTaskCreatePinnedToCore(loader_task, "tile_loader",
                                            LOADER_TASK_STACK, NULL, 3, NULL, 0);
    if (ok != pdTRUE) ESP_LOGE(TAG, "loader task create failed");
}
