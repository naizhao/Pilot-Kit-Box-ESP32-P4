/* pk_tile_cache.c — 实现说明见 pk_tile_cache.h。 */
#include "pk_tile_cache.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "pk_tile_cache";
#define PK_TC_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
static uint16_t *tile_alloc(void) { return heap_caps_malloc(PK_TILE_BUF_BYTES, MALLOC_CAP_SPIRAM); }
static void      tile_free(void *p) { heap_caps_free(p); }
static size_t    psram_free(void) { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
#else
#include <stdio.h>
#include <stdlib.h>
#define PK_TC_LOGW(fmt, ...) ((void)0)
static uint16_t *tile_alloc(void) { return (uint16_t *)malloc(PK_TILE_BUF_BYTES); }
static void      tile_free(void *p) { free(p); }
/* host 侧没有 PSRAM 这个概念，返回"要多少有多少"让水位线判断恒不触发——
 * 淘汰逻辑本身仍可用 pk_tile_cache_evict_lru() 单独测。 */
static size_t    psram_free(void) { return (size_t)-1; }
#endif

static bool key_eq(pk_tile_key_t a, pk_tile_key_t b)
{
    return a.pack_id == b.pack_id && a.z == b.z && a.x == b.x && a.y == b.y;
}

static void slot_free_data(pk_tile_cache_slot_t *s)
{
    if (s->data) {
        tile_free(s->data);
        s->data = NULL;
    }
}

void pk_tile_cache_init(pk_tile_cache_t *cache)
{
    memset(cache, 0, sizeof(*cache));
}

void pk_tile_cache_deinit(pk_tile_cache_t *cache)
{
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        slot_free_data(&cache->slots[i]);
    }
    memset(cache, 0, sizeof(*cache));
}

/* 分配失败累计。模块级而非按实例：全工程只有一个 s_cache，按实例反而要把
 * 指针传进 alloc 才能记账，不值当。 */
static uint32_t s_alloc_fails;

uint32_t pk_tile_cache_alloc_fail_count(void)
{
    return s_alloc_fails;
}

void pk_tile_cache_stats(const pk_tile_cache_t *cache, int *out_used,
                         int *out_negative, size_t *out_bytes)
{
    int used = 0, neg = 0;
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        const pk_tile_cache_slot_t *s = &cache->slots[i];
        if (!s->used || s->generation != cache->generation) continue;
        if (s->negative) neg++;
        else             used++;
    }
    if (out_used)     *out_used = used;
    if (out_negative) *out_negative = neg;
    if (out_bytes)    *out_bytes = (size_t)used * (size_t)PK_TILE_BUF_BYTES;
}

uint16_t *pk_tile_cache_alloc_tile_buffer(void)
{
    uint16_t *p = tile_alloc();
    if (p == NULL) s_alloc_fails++;
    return p;
}

bool pk_tile_cache_evict_lru(pk_tile_cache_t *cache)
{
    pk_tile_cache_slot_t *victim = NULL;
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        pk_tile_cache_slot_t *s = &cache->slots[i];
        if (!s->used || s->data == NULL) continue;   /* 空槽/负缓存不占瓦片内存 */
        if (victim == NULL || s->last_used_seq < victim->last_used_seq) victim = s;
    }
    if (victim == NULL) return false;
    slot_free_data(victim);
    memset(victim, 0, sizeof(*victim));
    return true;
}

uint16_t *pk_tile_cache_acquire_buffer(pk_tile_cache_t *cache)
{
    /* 先按水位线让路。「宁可丢最久未用的瓦片，也不能把 PSRAM 压到解码路径
     * 分配不出来的地步」——后者不是丢一张，是把 SD 读/PNG 解码/目录解析
     * 一起打垮，代价大得多。 */
    while (psram_free() < PK_TILE_CACHE_PSRAM_FLOOR_BYTES + (size_t)PK_TILE_BUF_BYTES) {
        if (!pk_tile_cache_evict_lru(cache)) break;
    }
    /* 水位线之外的兜底：真分配不出来就再让一条重试，直到缓存让干净。 */
    for (int retry = 0; retry <= PK_TILE_CACHE_SLOTS; retry++) {
        uint16_t *p = tile_alloc();
        if (p != NULL) return p;
        if (!pk_tile_cache_evict_lru(cache)) break;
    }
    s_alloc_fails++;
    PK_TC_LOGW("瓦片缓冲区分配失败：缓存已全部让出，PSRAM 仍只剩 %u B",
               (unsigned)psram_free());
    return NULL;
}

void pk_tile_cache_bump_generation(pk_tile_cache_t *cache)
{
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        pk_tile_cache_slot_t *s = &cache->slots[i];
        if (s->used) {
            slot_free_data(s);
            memset(s, 0, sizeof(*s));
        }
    }
    cache->generation++;
}

/* 找一个可用槽位来放新条目：优先用空槽；没有空槽就淘汰 last_used_seq 最小
 * （最久未用）的那个，释放其数据（若有）后原地复用。同时也把 generation
 * 对不上（陈旧）的槽视为"等于空槽"，一并纳入优先淘汰——它们逻辑上已经不
 * 该继续占位了。返回槽位指针。 */
static pk_tile_cache_slot_t *find_slot_for_insert(pk_tile_cache_t *cache)
{
    /* 空槽或 generation 陈旧槽优先，谁先找到用谁。 */
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        pk_tile_cache_slot_t *s = &cache->slots[i];
        if (!s->used || s->generation != cache->generation) {
            slot_free_data(s);
            return s;
        }
    }
    /* 全满且都是当前 generation：LRU 淘汰。 */
    pk_tile_cache_slot_t *victim = &cache->slots[0];
    for (int i = 1; i < PK_TILE_CACHE_SLOTS; i++) {
        if (cache->slots[i].last_used_seq < victim->last_used_seq) {
            victim = &cache->slots[i];
        }
    }
    slot_free_data(victim);
    return victim;
}

static pk_tile_cache_slot_t *find_slot(pk_tile_cache_t *cache, pk_tile_key_t key)
{
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        pk_tile_cache_slot_t *s = &cache->slots[i];
        if (s->used && s->generation == cache->generation && key_eq(s->key, key)) {
            return s;
        }
    }
    return NULL;
}

const uint16_t *pk_tile_cache_get(pk_tile_cache_t *cache, pk_tile_key_t key,
                                   uint32_t now_ms, bool *out_is_negative)
{
    if (out_is_negative) *out_is_negative = false;

    pk_tile_cache_slot_t *s = find_slot(cache, key);
    if (!s) return NULL;

    if (s->negative || s->temp_negative) {
        uint32_t age = now_ms - s->neg_timestamp_ms; /* 无符号回绕对 TTL 量级无影响 */
        uint32_t ttl;
        if (s->temp_negative) {
            /* 临时负缓存 TTL 看失败次数：≥5 次升级到长 TTL，否则短 TTL。
             * 短(2s) < 长(5s)，短到期先返回 not-negative（允许重试）。 */
            ttl = (s->sd_fail_count >= 5) ? PK_TILE_CACHE_TEMP_NEG_LONG_TTL_MS
                                          : PK_TILE_CACHE_TEMP_NEG_TTL_MS;
        } else {
            ttl = PK_TILE_CACHE_NEGATIVE_TTL_MS;   /* 真缺失：30s */
        }
        if (age > ttl) {
            /* 过期：当作未命中，让调用方重新发起加载。
             * 真缺失负缓存(negative)：整个槽回收（memset）。
             * 临时负缓存(temp_negative)：只清标志，**保留 sd_fail_count 和槽位**——
             * 清零是 read 成功后 reset_sd_fail 的职责；过期重试若又失败，count
             * 要从原值继续往上走，退避升级路径才成立。槽位留着占住，避免下次
             * bump 又重新找位。 */
            if (s->temp_negative) {
                s->temp_negative = false;
            } else {
                memset(s, 0, sizeof(*s));
            }
            return NULL;
        }
        if (out_is_negative) *out_is_negative = true;
        return NULL;
    }

    s->last_used_seq = ++cache->seq_counter;
    return s->data;
}

void pk_tile_cache_put(pk_tile_cache_t *cache, pk_tile_key_t key, uint16_t *data)
{
    /* 同一 key 若已在场（例如 negative 过期后 loader 又拿到真数据），先当
     * 普通淘汰路径处理：find_slot_for_insert 会命中它（generation 相同、
     * used=true）走 LRU 分支，不会重复——为避免同 key 两条并存，这里显式先
     * 找一遍替换。 */
    pk_tile_cache_slot_t *existing = find_slot(cache, key);
    pk_tile_cache_slot_t *s = existing ? existing : find_slot_for_insert(cache);
    slot_free_data(s);

    s->used             = true;
    s->negative          = false;
    s->temp_negative     = false;   /* 真瓦片到了，IO 退避态作废 */
    s->key               = key;
    s->data              = data;
    s->generation        = cache->generation;
    s->last_used_seq     = ++cache->seq_counter;
    s->neg_timestamp_ms  = 0;
    s->sd_fail_count     = 0;       /* 成功 → 计数清零，一次拥塞不永久污染 */
}

void pk_tile_cache_put_negative(pk_tile_cache_t *cache, pk_tile_key_t key, uint32_t now_ms)
{
    pk_tile_cache_slot_t *existing = find_slot(cache, key);
    pk_tile_cache_slot_t *s = existing ? existing : find_slot_for_insert(cache);
    slot_free_data(s);

    s->used             = true;
    s->negative          = true;
    s->key               = key;
    s->data              = NULL;
    s->generation        = cache->generation;
    s->last_used_seq     = ++cache->seq_counter;
    s->neg_timestamp_ms  = now_ms;
}

uint8_t pk_tile_cache_bump_sd_fail(pk_tile_cache_t *cache, pk_tile_key_t key, uint32_t now_ms)
{
    pk_tile_cache_slot_t *s = find_slot(cache, key);
    if (!s) {
        /* 找不到既有条目：占一个槽位起头计数。不调 find_slot_for_insert——那条
         * 路径会 LRU 淘汰真实瓦片，而退避条目的整点就是占住槽位挡住每帧重试，
         * 不该为它牺牲一张真瓦片。优先复用过期的负缓存/临时负缓存槽。 */
        s = find_slot_for_insert(cache);
        slot_free_data(s);
        memset(s, 0, sizeof(*s));
        s->used          = true;
        s->key           = key;
        s->generation    = cache->generation;
        s->last_used_seq = ++cache->seq_counter;
        s->sd_fail_count = 0;
    }
    /* 上限保护：uint8_t 不会溢出到 0（255 后 bump 留在 255），且阈值是 3/5，
     * 远在这之下。 */
    if (s->sd_fail_count < 255) s->sd_fail_count++;
    (void)now_ms;
    return s->sd_fail_count;
}

void pk_tile_cache_put_temp_negative(pk_tile_cache_t *cache, pk_tile_key_t key, uint32_t now_ms)
{
    pk_tile_cache_slot_t *s = find_slot(cache, key);
    if (!s) return;   /* 没有计数条目 = 没 bump 过，不该插临时负缓存 */
    /* 保留 sd_fail_count（退避升级靠它），只置临时负缓存态 + 刷新时间戳。
     * slot_free_data 不需要——计数条目本来就没有瓦片 data。 */
    s->temp_negative   = true;
    s->negative        = false;
    s->data            = NULL;
    s->neg_timestamp_ms = now_ms;
    s->last_used_seq   = ++cache->seq_counter;
}

void pk_tile_cache_reset_sd_fail(pk_tile_cache_t *cache, pk_tile_key_t key)
{
    pk_tile_cache_slot_t *s = find_slot(cache, key);
    if (!s) return;
    /* 成功了：计数清零，临时负缓存态也清。槽位本身可以留着（万一马上又失败，
     * 不用重新占位），但既不再是负缓存也不再占数据。 */
    s->sd_fail_count  = 0;
    s->temp_negative  = false;
    /* 如果这个槽只用来记过失败计数、从没装过真瓦片，回收它腾给真正需要的 key。 */
    if (!s->negative && s->data == NULL) {
        memset(s, 0, sizeof(*s));
    }
}
