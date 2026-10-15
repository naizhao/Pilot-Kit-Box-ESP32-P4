/* test_pk_tile_backoff_integration.c — 真实 cache/loader/PMTiles 组合回归。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_pk_tile_backoff_integration firmware/test/test_pk_tile_backoff_integration.c && /tmp/test_pk_tile_backoff_integration
 *
 * 故障注入只替换时钟、分配和解码/文件读取平台缝；cache TTL、计数、路由、
 * PMTiles 目录查找和真实 POSIX 重开均使用生产代码。资源失败后 SD 第一次失败
 * 必须换出被钉死的读取回调，并从真实临时文件读取、成功解码到真实缓存。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include "freertos/semphr.h"

#include "esp_timer.h"
#include "i18n_catalog.h"
#include "pk_map_store.h"
#include "pk_sdcard.h"
#include "pk_tile_cache.h"
#include "ui_state.h"

/* ---------------------------------------------------------- 注入桩 */

static int64_t g_now_us = 1000000;
int64_t esp_timer_get_time(void) { return g_now_us; }

static bool g_sd_mounted = true;
bool pk_sdcard_is_mounted(void) { return g_sd_mounted; }
pk_sd_state_t pk_sdcard_state(void)
{ return g_sd_mounted ? PK_SD_MOUNTED : PK_SD_NO_CARD; }
bool pk_sdcard_media_error(void) { return false; }
bool pk_sdcard_register_pre_unmount_cb(void (*cb)(void)) { (void)cb; return true; }

static bool g_jpeg_fail = false;
bool pk_jpeg_dec_init(void) { return true; }
bool pk_jpeg_dec_inited(void) { return true; }
bool pk_jpeg_decode_tile(const uint8_t *d, size_t n, uint16_t *out)
{ (void)d; (void)n; (void)out; return !g_jpeg_fail; }

void pk_ui_toast_show(pk_tr_id_t id, bool is_error) { (void)id; (void)is_error; }

static unsigned g_png_err = 0;   /* 0=成功；83=lodepng 的 OOM 码 */
unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                          const unsigned char *in, size_t insize)
{
    (void)in; (void)insize;
    if (g_png_err != 0) { *out = NULL; *w = *h = 0; return g_png_err; }
    *out = malloc((size_t)PK_TILE_PIXELS * PK_TILE_PIXELS * 4);
    if (*out == NULL) { *w = *h = 0; return 83u; }
    memset(*out, 0xA5, (size_t)PK_TILE_PIXELS * PK_TILE_PIXELS * 4);
    *w = *h = PK_TILE_PIXELS;
    return 0;
}

/* 真 cache，仅在平台分配缝注入缓冲区 OOM。 */
static bool g_tile_alloc_fail;
static void *test_tile_malloc(size_t n)
{ return g_tile_alloc_fail ? NULL : malloc(n); }
#ifndef PK_TEST_CACHE_SOURCE
#define PK_TEST_CACHE_SOURCE "../main/pk_tile_cache.c"
#endif
#define malloc test_tile_malloc
#define key_eq pk_tc_key_eq
#include PK_TEST_CACHE_SOURCE
#undef key_eq
#undef malloc

/* malloc 注入：只作用于 pk_tile_loader.c 这一个 TU（见文件头）。 */
static size_t g_malloc_fail_ge = 0;   /* >0：尺寸 >= 它的分配失败 */
void *pk_test_malloc(size_t n)
{ if (g_malloc_fail_ge && n >= g_malloc_fail_ge) return NULL; return malloc(n); }
void pk_test_free(void *p) { free(p); }

/* 只在PMTiles目录分配缝注入OOM：正常1-entry叶目录可失败，
 * 不依赖畸形/巨大目录或主机overcommit行为。 */
static uint64_t g_calloc_fail_nmemb = 0;   /* >0：nmemb >= 它的 calloc 失败 */
void *pk_test_calloc(size_t nmemb, size_t size)
{
    if (g_calloc_fail_nmemb && (uint64_t)nmemb >= g_calloc_fail_nmemb) return NULL;
    return calloc(nmemb, size);
}

/* 真实双 worker 抢日志槽：仅将平台互斥缝切到 pthread，诊断代码本身不替换。 */
static bool g_native_mutex;
static pthread_mutex_t g_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static int test_mutex_take(SemaphoreHandle_t lock, int delay);
static int test_mutex_give(SemaphoreHandle_t lock);

/* ------------------------------------------------------ 拉进被测实现 */

#include "../main/third_party/pk_tinfl.c"
#define calloc pk_test_calloc
#include "../main/pk_pmtiles.c"
#undef calloc
#include "../main/pk_map_store.c"
#define malloc pk_test_malloc
#define free   pk_test_free
#ifndef PK_TEST_LOADER_SOURCE
#define PK_TEST_LOADER_SOURCE "../main/pk_tile_loader.c"
#endif
#define xSemaphoreTake test_mutex_take
#define xSemaphoreGive test_mutex_give
#include PK_TEST_LOADER_SOURCE
#undef xSemaphoreTake
#undef xSemaphoreGive
#undef malloc
#undef free

static int test_mutex_take(SemaphoreHandle_t lock, int delay)
{
    if (g_native_mutex) return pthread_mutex_lock(&g_diag_mutex) == 0;
    return xSemaphoreTake(lock, delay);
}
static int test_mutex_give(SemaphoreHandle_t lock)
{
    if (g_native_mutex) return pthread_mutex_unlock(&g_diag_mutex) == 0;
    return xSemaphoreGive(lock);
}

/* --------------------------------------------------------- 测试基建 */

static int g_fail = 0;

typedef struct { const uint8_t *buf; size_t len; } mem_ctx_t;
static mem_ctx_t g_img_ctx;

static int g_tile_reads;
static int mem_read(void *ctx, uint64_t off, void *out, size_t len)
{
    if (off == 132) { g_tile_reads++; }
    mem_ctx_t *m = (mem_ctx_t *)ctx;
    if (off + len > m->len) return -1;
    memcpy(out, m->buf + off, len);
    return 0;
}

static void put_v(uint8_t *buf, size_t *pos, uint64_t v)
{
    while (v >= 0x80) { buf[(*pos)++] = (uint8_t)(v | 0x80); v >>= 7; }
    buf[(*pos)++] = (uint8_t)v;
}
static void wr_u64(uint8_t *b, size_t off, uint64_t v)
{ for (int i = 0; i < 8; i++) b[off + i] = (uint8_t)(v >> (8 * i)); }

#define TEST_TILE_LEN 64

/* 合法最小PMTiles：variant0为根目录瓦片；variant1为根→1-entry叶目录
 * →相同瓦片。平台calloc注入在open完成后开启，仅阻断合法叶目录分配。 */
static size_t build_image(uint8_t *buf, size_t cap, int variant, uint8_t tile_type)
{
    memset(buf, 0, cap);
    memcpy(buf, "PMTiles", 7);
    buf[7] = 3;                                   /* version */
    buf[97] = PK_PMTILES_COMPRESSION_NONE;        /* internal_compression */
    buf[98] = PK_PMTILES_COMPRESSION_NONE;        /* tile_compression */
    buf[99] = tile_type;
    buf[100] = 0; buf[101] = 0;                   /* min/max zoom */

    size_t pos = 127;                             /* root dir @127 */
    size_t root_start = pos;
    put_v(buf, &pos, 1);                          /* entry_count */
    put_v(buf, &pos, 0);                          /* tile_id delta → 0 */
    if (variant == 0) {
        put_v(buf, &pos, 1);                      /* run_length=1：真瓦片 */
        put_v(buf, &pos, TEST_TILE_LEN);
        put_v(buf, &pos, 1);                      /* offset 编码 = 0+1 */
    } else {
        put_v(buf, &pos, 0);                      /* run_length=0：叶目录指针 */
        put_v(buf, &pos, 5);                      /* 叶目录长 5B */
        put_v(buf, &pos, 1);                      /* 相对 leaf_dirs 偏移 0 */
    }
    size_t root_len = pos - root_start;
    wr_u64(buf, 8, 127);                          /* root_dir_offset */
    wr_u64(buf, 16, root_len);                    /* root_dir_length */

    if (variant == 0) {
        wr_u64(buf, 40, pos);                     /* leaf_dirs_offset（无叶目录） */
        wr_u64(buf, 48, 0);
        for (int i = 0; i < TEST_TILE_LEN; i++) buf[pos + i] = 0xEE;
        wr_u64(buf, 56, pos);                     /* tile_data_offset */
        wr_u64(buf, 64, TEST_TILE_LEN);
        pos += TEST_TILE_LEN;
    } else {
        wr_u64(buf, 40, pos);                     /* leaf_dirs_offset = root 尾 */
        wr_u64(buf, 48, 5);
        put_v(buf, &pos, 1);                     /* 1-entry合法叶目录 */
        put_v(buf, &pos, 0);                     /* tile_id=0 */
        put_v(buf, &pos, 1);                     /* run_length=1 */
        put_v(buf, &pos, TEST_TILE_LEN);
        put_v(buf, &pos, 1);                     /* offset=0 */
        wr_u64(buf, 56, pos);
        wr_u64(buf, 64, TEST_TILE_LEN);
        for (int i = 0; i < TEST_TILE_LEN; i++) buf[pos + i] = 0xEE;
        pos += TEST_TILE_LEN;
    }
    return pos;
}

/* 每个用例统一从这里起步：清模块静态状态 + 装一个包（meta 覆盖 z0 全球）。 */
static uint8_t g_img[512];

static void loader_setup(int variant, uint8_t tile_type)
{
    g_tile_alloc_fail = false;
    g_calloc_fail_nmemb = 0;
    g_tile_reads = 0;
    g_png_err = 0;
    g_jpeg_fail = false;
    g_malloc_fail_ge = 0;
    g_sd_mounted = true;
    g_now_us = 1000000;

    memset(&s_diag, 0, sizeof(s_diag));
    memset(&s_store, 0, sizeof(s_store));
    memset(&s_cache, 0, sizeof(s_cache));
    s_view_gen = 0;
    s_lock    = xSemaphoreCreateMutex();
    s_io_lock = xSemaphoreCreateMutex();
    s_queue   = NULL;

    size_t len = build_image(g_img, sizeof(g_img), variant, tile_type);
    g_img_ctx.buf = g_img;
    g_img_ctx.len = len;

    pk_map_pack_t *p = &s_store.packs[0];
    if (!pk_pmtiles_open(&p->pm, mem_read, &g_img_ctx)) {
        printf("  [FATAL] 测试镜像 pk_pmtiles_open 失败（variant=%d）\n", variant);
        g_fail++;
        return;
    }
    p->meta = (pk_map_pack_meta_t){ .valid = true,
        .min_lon = -180.0, .min_lat = -85.0, .max_lon = 180.0, .max_lat = 85.0,
        .min_zoom = 0, .max_zoom = 0 };
    snprintf(p->path, sizeof(p->path), "/mem/test.pmtiles");
    s_store.count = 1;
}


static void check(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}
static void cleanup(void)
{
    pk_tile_cache_deinit(&s_cache);
    pk_pmtiles_close(&s_store.packs[0].pm);
    vSemaphoreDelete(s_lock); vSemaphoreDelete(s_io_lock);
}
static void inject_resource(int kind)
{
    g_png_err = kind == 0 ? 83 : 0;
    g_malloc_fail_ge = kind == 1 ? TEST_TILE_LEN : 0;
    g_tile_alloc_fail = kind == 2;
    g_jpeg_fail = kind == 3;
    g_calloc_fail_nmemb = kind == 4 ? 1 : 0;
}
static void test_same_frame(void)
{
    for (int kind = 0; kind < 5; kind++) {
        loader_setup(kind == 4, kind == 3 ? PK_PMTILES_TYPE_JPEG : PK_PMTILES_TYPE_PNG);
        if (kind == 4) {
            pk_pmtiles_tile_loc_t loc;
            check("正常1-entry叶目录可定位合法瓦片",pk_pmtiles_find_tile_status(&s_store.packs[0].pm,0,0,0,&loc)==PK_PMTILES_FIND_FOUND && loc.length==TEST_TILE_LEN);
            cleanup(); loader_setup(1,PK_PMTILES_TYPE_PNG);
        }
        inject_resource(kind);
        unsigned requests = 0;
        for (int i = 0; i < 6; i++) {
            g_now_us = 1000000 + i * 1000;  /* worker 时间晚于整帧的 999ms */
            const uint16_t *data = NULL; uint32_t shift, cx, cy; bool neg = false;
            bool locked = pk_tile_loader_lock_sample(0, 0, 0, 999, &data, &shift, &cx, &cy, &neg);
            if (locked) pk_tile_loader_unlock_sample();
            if (!locked && !neg) { requests++; fetch_and_decode(0, 0, 0, 0); }
        }
        char name[120];
        snprintf(name, sizeof(name), "资源路径%d 同帧6次采样仅请求一次 (requests=%u reads=%d)", kind, requests, g_tile_reads);
        check(name, requests == 1 && g_tile_reads == (kind == 1 ? 0 : 1));
        pk_tile_key_t key = {.pack_id=0, .z=0, .x=0, .y=0};
        check("资源退避不得计入SD失败次数", find_slot(&s_cache, key)->sd_fail_count == 0);
        cleanup();
    }
}
/* 模拟 FatFs 粘滞读取失败：重试同一回调永远失败，真实 reopen_file 才会
 * 换回生产 posix_file_read。保留实际 owned_file，使重开/关文件路径也真实执行。 */
static int pinned_read(void *ctx, uint64_t off, void *out, size_t len)
{
    if (off == 132) { g_tile_reads++; return -1; }
    return posix_file_read(ctx, off, out, len);
}
static void test_resource_then_io(void)
{
    for (int kind = 0; kind < 4; kind++) {
        loader_setup(0, kind == 3 ? PK_PMTILES_TYPE_JPEG : PK_PMTILES_TYPE_PNG);
        char path[] = "/tmp/pk_tile_reopen_XXXXXX";
        int fd = mkstemp(path); check("创建真实PMTiles临时文件", fd >= 0);
        if (fd < 0) { cleanup(); continue; }
        check("完整写入测试包", write(fd, g_img, g_img_ctx.len) == (ssize_t)g_img_ctx.len);
        close(fd);
        pk_map_pack_t *pack = &s_store.packs[0];
        pk_pmtiles_close(&pack->pm);
        check("真实POSIX打开PMTiles", pk_pmtiles_open_file(&pack->pm, path));
        snprintf(pack->path, sizeof(pack->path), "%s", path);
        inject_resource(kind);
        check("资源故障判瞬时失败", fetch_and_decode(0,0,0,0) == FETCH_TRANSIENT);
        inject_resource(-1); g_now_us = 3002000;
        bool neg = true; pk_tile_key_t key = {.pack_id=0,.z=0,.x=0,.y=0};
        pk_tile_cache_get(&s_cache, key, 3002, &neg);
        check("2秒资源退避到期", !neg);
        pack->pm.read = pinned_read;
        check("第一次SD失败判瞬时失败", fetch_and_decode(0,0,0,0) == FETCH_TRANSIENT);
        check("资源失败后第一次SD失败重开实际文件", pack->pm.read == posix_file_read && pack->pm.owned_file && !pack->pm.io_error);
        check("重开后下一次读取解码成功", fetch_and_decode(0,0,0,0) == FETCH_OK);
        const uint16_t *tile = pk_tile_cache_get(&s_cache, key, 3003, &neg);
        check("成功结果进入真实cache并清SD失败", tile && !neg && find_slot(&s_cache,key)->sd_fail_count == 0);
        cleanup(); unlink(path); /* 仅删除本用例创建的临时夹具。 */
    }
}
static void test_ttl_wrap_and_missing(void)
{
    pk_tile_cache_t cache; pk_tile_cache_init(&cache);
    pk_tile_key_t key = {.pack_id=2, .z=0, .x=0, .y=0}; bool neg;
    for (int temporary = 0; temporary < 2; temporary++) {
        uint32_t stamp = UINT32_MAX - 1000u;
        if (temporary) { pk_tile_cache_bump_sd_fail(&cache,key,stamp); pk_tile_cache_put_temp_negative(&cache,key,stamp); }
        else pk_tile_cache_put_negative(&cache,key,stamp);
        uint32_t ttl = temporary ? PK_TILE_CACHE_TEMP_NEG_TTL_MS : PK_TILE_CACHE_NEGATIVE_TTL_MS;
        pk_tile_cache_get(&cache,key,stamp-1,&neg); check("较晚marker不应被旧帧时间过期",neg);
        pk_tile_cache_get(&cache,key,stamp+ttl,&neg); check("uint32回绕后TTL边界仍退避",neg);
        pk_tile_cache_get(&cache,key,stamp+ttl+1,&neg); check("uint32回绕后TTL边界加1允许重试",!neg);
        pk_tile_cache_bump_generation(&cache);
    }
    pk_tile_cache_put(&cache,key,pk_tile_cache_alloc_tile_buffer());
    for (int i = 0; i < 5; i++) {
        pk_tile_cache_bump_resource_fail(&cache,key,5000);
        pk_tile_cache_bump_sd_fail(&cache,key,5000);
    }
    pk_tile_cache_reset_sd_fail(&cache,key);
    check("显式成功清账重置资源失败",pk_tile_cache_bump_resource_fail(&cache,key,5000)==1);
    check("显式成功清账重置SD失败",pk_tile_cache_bump_sd_fail(&cache,key,5000)==1);
    pk_tile_cache_put_temp_negative(&cache,key,5000);
    pk_tile_cache_get(&cache,key,7001,&neg); check("显式清账后退避回到2秒",!neg);
    pk_tile_cache_deinit(&cache);
}
static void test_resource_long_backoff_and_reset(void)
{
    loader_setup(0,PK_PMTILES_TYPE_PNG); g_png_err=83;
    pk_tile_key_t key={.pack_id=0,.z=0,.x=0,.y=0}; bool neg;
    for (int i=0;i<5;i++) {
        g_now_us=(1000+i*2001)*1000LL;
        pk_tile_cache_get(&s_cache,key,(uint32_t)(g_now_us/1000),&neg);
        check("持续资源故障维持瞬时分类",fetch_and_decode(0,0,0,0)==FETCH_TRANSIENT);
    }
    uint32_t stamp=(uint32_t)(g_now_us/1000);
    pk_tile_cache_get(&s_cache,key,stamp+2001,&neg); check("持续5次资源失败升级5秒退避",neg);
    pk_tile_cache_get(&s_cache,key,stamp+5001,&neg); check("5秒退避到期允许重试",!neg);
    g_png_err=0;
    check("资源恢复后成功解码",fetch_and_decode(0,0,0,0)==FETCH_OK);
    g_png_err=83; fetch_and_decode(0,0,0,0);
    stamp=(uint32_t)(g_now_us/1000);
    pk_tile_cache_get(&s_cache,key,stamp+2001,&neg); check("成功清账后再次资源失败回到2秒退避",!neg);
    cleanup();
}
static atomic_int g_diag_ready, g_diag_go;
static void *diagnostic_worker(void *arg)
{
    atomic_fetch_add(&g_diag_ready, 1);
    while (!atomic_load(&g_diag_go)) { }
    *(bool *)arg = diagnostic_log_maybe();
    return NULL;
}
static void test_live_diagnostic_concurrent(void)
{
    loader_setup(0, PK_PMTILES_TYPE_PNG);
    fetch_and_decode(0,0,0,0);
    g_native_mutex = true;
    for (int round=0;round<32;round++) {
        g_now_us = 1000000LL + round*2000000LL;
        bool printed[2] = {false,false};
        pthread_t threads[2];
        atomic_store(&g_diag_ready,0); atomic_store(&g_diag_go,0);
        int a=pthread_create(&threads[0],NULL,diagnostic_worker,&printed[0]);
        int b=pthread_create(&threads[1],NULL,diagnostic_worker,&printed[1]);
        if (a || b) { fprintf(stderr,"pthread_create failed: %d/%d\n",a,b); abort(); }
        while (atomic_load(&g_diag_ready)!=2) { }
        atomic_store(&g_diag_go,1);
        pthread_join(threads[0],NULL); pthread_join(threads[1],NULL);
        check("real concurrent workers claim exactly one shared log slot",printed[0] != printed[1]);
    }
    g_native_mutex = false;
    cleanup();
}

static void test_live_diagnostic(void)
{
    for (int kind=0;kind<5;kind++) {
        loader_setup(kind==4, kind==3?PK_PMTILES_TYPE_JPEG:PK_PMTILES_TYPE_PNG);
        inject_resource(kind);
        fetch_and_decode(0,0,0,0);
        loader_diag_event_t expected[] = {DIAG_PNG_OOM, DIAG_INPUT_OOM,
            DIAG_BUFFER_OOM, DIAG_JPEG_BAD, DIAG_DIR_OOM};
        check("resource diagnostic counts actual failure category",s_diag.events[expected[kind]]==1);
        check("resource diagnostic excludes IO failures",s_diag.events[DIAG_IO]==0);
        check("resource diagnostic aggregate fetch/transient",s_diag.calls==1 && s_diag.transient==1 && s_diag.ok==0 && s_diag.missing==0);
        check("read attempts agree with actual injected IO",s_diag.events[DIAG_READ]==(kind==4 ? 0u : (uint32_t)g_tile_reads));
        check("successful read bytes agree with actual IO",s_diag.events[DIAG_READ_BYTES]==(kind==4 ? 0u : (uint32_t)g_tile_reads*TEST_TILE_LEN));
        pk_tile_cache_diag_t d; pk_tile_cache_diag(&s_cache,&d);
        check("resource marker not reported as resident",d.tiles==0 && d.temporary==1 && d.retry==0);
        check("first worker claims diagnostic slot",diagnostic_log_maybe());
        check("second worker cannot print at same time",!diagnostic_log_maybe());
        g_now_us+=1999999;
        check("second worker waits full two seconds",!diagnostic_log_maybe());
        g_now_us++;
        check("either worker may print at two-second boundary",diagnostic_log_maybe());
        check("first worker cannot duplicate second worker slot",!diagnostic_log_maybe());
        check("diagnostic itself does not add tile IO",s_diag.events[DIAG_READ]==(kind==4 ? 0u : (uint32_t)g_tile_reads));
        cleanup();
    }
    loader_setup(0,PK_PMTILES_TYPE_PNG);
    check("no fetch means no diagnostic traffic",!diagnostic_log_maybe());
    check("successful load with live diagnostics",fetch_and_decode(0,0,0,0)==FETCH_OK);
    check("success counts actual fetch and read bytes",s_diag.calls==1 && s_diag.ok==1 && s_diag.events[DIAG_READ]==1 && s_diag.events[DIAG_READ_BYTES]==TEST_TILE_LEN);
    g_png_err=31;
    check("bad PNG retains missing classification",fetch_and_decode(0,0,0,0)==FETCH_MISSING);
    check("bad PNG counted separately from PNG83",s_diag.events[DIAG_PNG_BAD]==1 && s_diag.events[DIAG_PNG_OOM]==0 && s_diag.missing==1);
    s_store.packs[0].pm.read=pinned_read;
    check("IO diagnostic retains transient classification",fetch_and_decode(0,0,0,0)==FETCH_TRANSIENT);
    check("IO counted once and failed bytes excluded",s_diag.events[DIAG_IO]==1 && s_diag.events[DIAG_READ]==3 && s_diag.events[DIAG_READ_BYTES]==2*TEST_TILE_LEN);
    cleanup();
}

int main(void)
{
    test_live_diagnostic();
    test_live_diagnostic_concurrent();
    test_same_frame(); test_resource_then_io();
    test_ttl_wrap_and_missing(); test_resource_long_backoff_and_reset();
    check("测试未泄漏互斥量",pk_host_mutex_live==0);
    printf("%s (%d fail)\n",g_fail ? "FAILED" : "PASSED",g_fail);
    return g_fail ? 1 : 0;
}
