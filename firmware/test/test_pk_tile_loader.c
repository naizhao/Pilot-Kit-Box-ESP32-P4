/* test_pk_tile_loader.c — host proof for pk_tile_loader 的错误分类与退避 wiring
 * （2026-10-01 调查 §8.2 三个缺陷的 loader 侧行为；docs/internal/
 * 2026-10-01-sd-death-flicker-investigation-final.md）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_pk_tl firmware/test/test_pk_tile_loader.c && /tmp/test_pk_tl
 *
 * 覆盖（每条对应一个缺陷的可观察行为）：
 *   C1  资源/解码瞬时失败（输入 malloc 失败 / 输出缓冲 acquire 失败 /
 *       PNG 解码 OOM(err=83) / JPEG 解码失败）→ FETCH_TRANSIENT 且必须打
 *       短退避标记（bump_resource_fail + put_temp_negative），成功路径不打任何标记；
 *   C2  pk_tile_loader_lock_sample 把负缓存结果经 out_negative 上抛
 *       （旧实现拿到 neg 丢弃，机头朝上分支因此绕过负缓存反复 request）；
 *   C3  目录 OOM（叶目录 entry_count 巨大 → calloc 失败）→ fetch 判
 *       TRANSIENT 而非 MISSING（MISSING 会被 loader_task 钉进 30s 负缓存），
 *       且同样走 C1 短退避。
 *
 * 白盒：本文件把 pk_tinfl.c / pk_pmtiles.c / pk_map_store.c / pk_tile_loader.c
 * 全部拉进同一翻译单元（test_pk_pmtiles.c 的既有惯例），直接调 static 的
 * fetch_and_decode / lock_sample，直接读写模块静态 s_store / s_cache / s_lock。
 *
 * 桩与注入点（只替换平台缝，不替换被测判定）：
 *   - esp_timer_get_time：假时钟（host_stubs/esp_timer.h 的声明-测试实现约定）；
 *   - pk_sdcard_*：固定"已挂载、无介质错误"；
 *   - pk_jpeg_decode_tile / lodepng_decode32：可注入失败（83=上游 OOM 语义）；
 *   - pk_tile_cache_*：**记录式假体**。真缓存的 TTL/计数语义由
 *     test_pk_tile_cache.c 用真代码把关（本文件不做重复实现），这里验证的是
 *     loader 是否在正确的失败路径上、用正确的 key 打了正确的标记，以及
 *     TRANSIENT/MISSING 的分类本身。acquire 失败在 host 上无法让真 malloc
 *     "按尺寸"失败（macOS 无 RLIMIT_AS），只能经假体注入；
 *   - malloc/free：拉入 pk_tile_loader.c 前用宏换成可注入失败的
 *     pk_test_malloc/pk_test_free——宏只作用于 loader 这一个 TU（pmtiles/
 *     store 在宏之前已编完，目录分配仍走真 malloc），专门打 fetch_and_decode
 *     里输入 scratch malloc 失败那一条路径。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* tile_cache 记录式假体：见文件头说明。 */
static struct {
    int get_calls, acquire_calls, put_calls, put_negative_calls;
    int bump_calls, temp_calls, sd_bump_calls;
    bool get_neg;
    const uint16_t *get_data;
    bool acquire_fail;
    pk_tile_key_t last_bump_key, last_temp_key;
    uint8_t bump_return;
} g_c;

void pk_tile_cache_init(pk_tile_cache_t *c) { memset(c, 0, sizeof(*c)); }
void pk_tile_cache_deinit(pk_tile_cache_t *c) { (void)c; }
uint16_t *pk_tile_cache_alloc_tile_buffer(void) { return malloc(PK_TILE_BUF_BYTES); }
uint16_t *pk_tile_cache_acquire_buffer(pk_tile_cache_t *c)
{ (void)c; g_c.acquire_calls++; return g_c.acquire_fail ? NULL : malloc(PK_TILE_BUF_BYTES); }
bool pk_tile_cache_evict_lru(pk_tile_cache_t *c) { (void)c; return false; }
void pk_tile_cache_bump_generation(pk_tile_cache_t *c) { (void)c; }
const uint16_t *pk_tile_cache_get(pk_tile_cache_t *c, pk_tile_key_t k,
                                  uint32_t now_ms, bool *out_is_negative)
{ (void)c; (void)k; (void)now_ms; g_c.get_calls++; *out_is_negative = g_c.get_neg; return g_c.get_data; }
void pk_tile_cache_put(pk_tile_cache_t *c, pk_tile_key_t k, uint16_t *data)
{ (void)c; (void)k; g_c.put_calls++; free(data); }
void pk_tile_cache_put_negative(pk_tile_cache_t *c, pk_tile_key_t k, uint32_t now_ms)
{ (void)c; (void)k; (void)now_ms; g_c.put_negative_calls++; }
uint8_t pk_tile_cache_bump_sd_fail(pk_tile_cache_t *c, pk_tile_key_t k, uint32_t now_ms)
{ (void)c; (void)k; (void)now_ms; g_c.sd_bump_calls++; return (uint8_t)g_c.sd_bump_calls; }
uint8_t pk_tile_cache_bump_resource_fail(pk_tile_cache_t *c, pk_tile_key_t k, uint32_t now_ms)
{ (void)c; (void)now_ms; g_c.bump_calls++; g_c.last_bump_key = k; return ++g_c.bump_return; }
void pk_tile_cache_put_temp_negative(pk_tile_cache_t *c, pk_tile_key_t k, uint32_t now_ms)
{ (void)c; (void)now_ms; g_c.temp_calls++; g_c.last_temp_key = k; }
void pk_tile_cache_reset_sd_fail(pk_tile_cache_t *c, pk_tile_key_t k) { (void)c; (void)k; }
void pk_tile_cache_stats(const pk_tile_cache_t *c, int *a, int *b, size_t *d)
{ (void)c; if (a) *a = 0; if (b) *b = 0; if (d) *d = 0; }
void pk_tile_cache_diag(const pk_tile_cache_t *c, pk_tile_cache_diag_t *out)
{ (void)c; memset(out,0,sizeof(*out)); }
uint32_t pk_tile_cache_alloc_fail_count(void) { return 0; }

/* malloc 注入：只作用于 pk_tile_loader.c 这一个 TU（见文件头）。 */
static size_t g_malloc_fail_ge = 0;   /* >0：尺寸 >= 它的分配失败 */
void *pk_test_malloc(size_t n)
{ if (g_malloc_fail_ge && n >= g_malloc_fail_ge) return NULL; return malloc(n); }
void pk_test_free(void *p) { free(p); }

/* calloc 注入（只作用于 pk_pmtiles.c TU）：macOS 的 malloc/calloc 对超大请求
 * 走 overcommit mmap——entry_count=2^40 的 calloc 在 host 上**不会失败**
 * （先成功、再被 2^40 次解析循环拖死），而真机 PSRAM 吃紧时这恰是最先失败
 * 的分配。注入点只把"条数超出真机可行域"的 calloc 变成失败，其余走真
 * calloc；分类逻辑（OOM ≠ 未找到）仍是被测的真代码。 */
static uint64_t g_calloc_fail_nmemb = 0;   /* >0：nmemb >= 它的 calloc 失败 */
void *pk_test_calloc(size_t nmemb, size_t size)
{
    if (g_calloc_fail_nmemb && (uint64_t)nmemb >= g_calloc_fail_nmemb) return NULL;
    return calloc(nmemb, size);
}

/* ------------------------------------------------------ 拉进被测实现 */

#include "../main/third_party/pk_tinfl.c"
#define calloc pk_test_calloc
#include "../main/pk_pmtiles.c"
#undef calloc
#include "../main/pk_map_store.c"
#define malloc pk_test_malloc
#define free   pk_test_free
#include "../main/pk_tile_loader.c"
#undef malloc
#undef free

/* --------------------------------------------------------- 测试基建 */

static int g_fail = 0;

static void chk_true(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

typedef struct { const uint8_t *buf; size_t len; } mem_ctx_t;
static mem_ctx_t g_img_ctx;

static int mem_read(void *ctx, uint64_t off, void *out, size_t len)
{
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

/* 手搓最小 PMTiles 内存镜像（internal_compression=none，不依赖样本包）：
 *   variant 0（GOOD）：root 一条 run_length=1 的真瓦片（tile_id 0 = z0(0,0)），
 *                      tile_data 是 TEST_TILE_LEN 字节伪数据（解码已打桩）；
 *   variant 1（OOM_LEAF，C3）：root 一条 run_length=0 的叶目录指针，叶目录
 *                      blob 的 varint entry_count = 2^40——配合上面的 calloc
 *                      注入复现"目录 entries 分配失败"；且读盘本身成功
 *                      （io_error 不置位），精确对上"目录 OOM 被误判缺失"
 *                      的原始条件。 */
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
        put_v(buf, &pos, 12);                     /* 叶目录长 12B */
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
        wr_u64(buf, 48, 12);
        size_t leaf = pos;
        put_v(buf, &pos, (uint64_t)1 << 40);      /* entry_count = 2^40 → calloc 必败 */
        while (pos < leaf + 12) buf[pos++] = 0;   /* 补齐声明的 12B */
        wr_u64(buf, 56, pos);                     /* tile_data_offset（空） */
        wr_u64(buf, 64, 0);
    }
    return pos;
}

/* 每个用例统一从这里起步：清模块静态状态 + 装一个包（meta 覆盖 z0 全球）。 */
static uint8_t g_img[512];

static void loader_setup(int variant, uint8_t tile_type)
{
    memset(&g_c, 0, sizeof(g_c));
    g_png_err = 0;
    g_jpeg_fail = false;
    g_malloc_fail_ge = 0;
    g_sd_mounted = true;
    g_now_us = 1000000;

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

/* 退避标记断言：打了标记、bump/temp 成对、key 就是请求键（pack 0, z0(0,0)）。 */
static void chk_backoff_marked(const char *what)
{
    chk_true(what, g_c.bump_calls == 1 && g_c.sd_bump_calls == 0 && g_c.temp_calls == 1 &&
                g_c.last_bump_key.pack_id == 0 && g_c.last_bump_key.z == 0 &&
                g_c.last_bump_key.x == 0 && g_c.last_bump_key.y == 0 &&
                g_c.last_temp_key.pack_id == 0);
}

static void chk_no_marks(const char *what)
{
    chk_true(what, g_c.bump_calls == 0 && g_c.sd_bump_calls == 0 && g_c.temp_calls == 0 &&
                g_c.put_negative_calls == 0);
}

/* ------------------------------------------------------------- C1 用例 */

static void test_c1_png_decode_oom_marks_backoff(void)
{
    printf("-- C1: PNG 解码 OOM(err=83) → 短退避标记 --\n");
    loader_setup(0, PK_PMTILES_TYPE_PNG);
    g_png_err = 83;
    fetch_result_t r = fetch_and_decode(0, 0, 0, 0);
    chk_true("分类仍是 TRANSIENT（不进 30s 负缓存）", r == FETCH_TRANSIENT);
    chk_backoff_marked("打了短退避标记（bump+temp，key=pack0 z0(0,0)）");
}

static void test_c1_acquire_fail_marks_backoff(void)
{
    printf("-- C1: RGB565 输出缓冲 acquire 失败（SD 已全读）→ 短退避标记 --\n");
    loader_setup(0, PK_PMTILES_TYPE_PNG);
    g_c.acquire_fail = true;
    fetch_result_t r = fetch_and_decode(0, 0, 0, 0);
    chk_true("分类仍是 TRANSIENT", r == FETCH_TRANSIENT);
    chk_backoff_marked("打了短退避标记（下一帧不再全量重读）");
}

static void test_c1_input_malloc_fail_marks_backoff(void)
{
    printf("-- C1: 输入 scratch malloc 失败（主体读取前）→ 短退避标记 --\n");
    loader_setup(0, PK_PMTILES_TYPE_PNG);
    g_malloc_fail_ge = TEST_TILE_LEN;   /* 恰好打掉 png=malloc(loc.length) 这一笔 */
    fetch_result_t r = fetch_and_decode(0, 0, 0, 0);
    chk_true("分类仍是 TRANSIENT", r == FETCH_TRANSIENT);
    chk_backoff_marked("打了短退避标记（目录查找也不必每帧重做）");
}

static void test_c1_jpeg_decode_fail_marks_backoff(void)
{
    printf("-- C1: JPEG 解码失败（TRANSIENT 分类）→ 短退避标记 --\n");
    loader_setup(0, PK_PMTILES_TYPE_JPEG);
    g_jpeg_fail = true;
    fetch_result_t r = fetch_and_decode(0, 0, 0, 0);
    chk_true("分类仍是 TRANSIENT", r == FETCH_TRANSIENT);
    chk_backoff_marked("打了短退避标记");
}

static void test_success_path_no_marks(void)
{
    printf("-- C1 守卫: 成功路径不新增任何标记/分类 --\n");
    loader_setup(0, PK_PMTILES_TYPE_PNG);
    fetch_result_t r = fetch_and_decode(0, 0, 0, 0);
    chk_true("成功 → FETCH_OK", r == FETCH_OK);
    chk_true("照常 pk_tile_cache_put", g_c.put_calls == 1);
    chk_no_marks("无 bump/temp/negative 标记");
}

static void test_clean_miss_still_missing(void)
{
    printf("-- 回归守卫: 目录干净未命中仍判 MISSING --\n");
    loader_setup(0, PK_PMTILES_TYPE_PNG);
    /* z1(0,0)=tile_id 1，root 只覆盖 tile_id 0 → 干净 miss（非 OOM） */
    fetch_result_t r = fetch_and_decode(0, 1, 0, 0);
    chk_true("干净未命中 → MISSING（维持 30s 负缓存语义）", r == FETCH_MISSING);
    chk_no_marks("不掺短退避标记");
}

/* ------------------------------------------------------------- C3 用例 */

static void test_c3_dir_oom_transient_not_missing(void)
{
    printf("-- C3: 叶目录 calloc OOM → TRANSIENT + 短退避（不再 MISSING）--\n");
    loader_setup(1, PK_PMTILES_TYPE_PNG);
    g_calloc_fail_nmemb = 1;   /* 叶目录 entries calloc 必失败（root 已在 open 时装好） */
    fetch_result_t r = fetch_and_decode(0, 0, 0, 0);
    g_calloc_fail_nmemb = 0;
    chk_true("目录 OOM → TRANSIENT（旧实现误判 MISSING 进 30s 负缓存）",
             r == FETCH_TRANSIENT);
    chk_backoff_marked("走了 C1 短退避（bump+temp）");
}

/* ------------------------------------------------------------- C2 用例 */

static void test_c2_lock_sample_reports_negative(void)
{
    printf("-- C2: lock_sample 上抛负缓存结果 --\n");
    loader_setup(0, PK_PMTILES_TYPE_PNG);
    static uint16_t tile[PK_TILE_BUF_PIXELS];
    const uint16_t *data = NULL;
    uint32_t shift = 0, cx = 0, cy = 0;
    bool neg = false;

    /* 1) 负缓存命中（30s 真缺失或 2-5s 退避）：调用方据此**不再 request** */
    g_c.get_neg = true; g_c.get_data = NULL;
    neg = true; /* 预置脏值，验证实现会先清零 */
    bool locked = pk_tile_loader_lock_sample(0, 0, 0, 1000, &data, &shift, &cx, &cy, &neg);
    chk_true("负缓存 → 不锁定", !locked);
    chk_true("out_negative=true（旧实现丢弃该信息）", neg);

    /* 2) 干净未命中：允许调用方 request */
    g_c.get_neg = false; g_c.get_data = NULL;
    neg = true;
    locked = pk_tile_loader_lock_sample(0, 0, 0, 1000, &data, &shift, &cx, &cy, &neg);
    chk_true("干净未命中 → 不锁定", !locked);
    chk_true("out_negative=false", !neg);

    /* 3) 数据命中：正常锁定采样 */
    g_c.get_neg = false; g_c.get_data = tile;
    neg = true;
    locked = pk_tile_loader_lock_sample(0, 0, 0, 1000, &data, &shift, &cx, &cy, &neg);
    chk_true("数据命中 → 锁定", locked && data == tile && shift == 0);
    chk_true("锁定时 out_negative=false", !neg);
    if (locked) pk_tile_loader_unlock_sample();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_c1_png_decode_oom_marks_backoff();
    test_c1_acquire_fail_marks_backoff();
    test_c1_input_malloc_fail_marks_backoff();
    test_c1_jpeg_decode_fail_marks_backoff();
    test_success_path_no_marks();
    test_clean_miss_still_missing();
    test_c3_dir_oom_transient_not_missing();
    test_c2_lock_sample_reports_negative();

    printf("\n%s（%d 处失败）\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
