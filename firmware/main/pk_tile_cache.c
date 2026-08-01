/* pk_tile_cache.c — 实现说明见 pk_tile_cache.h。 */
#include "pk_tile_cache.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "pk_tile_cache";
#define PK_TC_LOGD(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
static uint16_t *tile_alloc(void) { return heap_caps_malloc(PK_TILE_BUF_BYTES, MALLOC_CAP_SPIRAM); }
static void      tile_free(void *p) { heap_caps_free(p); }
#else
#include <stdio.h>
#include <stdlib.h>
#define PK_TC_LOGD(fmt, ...) ((void)0)
static uint16_t *tile_alloc(void) { return (uint16_t *)malloc(PK_TILE_BUF_BYTES); }
static void      tile_free(void *p) { free(p); }
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

uint16_t *pk_tile_cache_alloc_tile_buffer(void)
{
    return tile_alloc();
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

    if (s->negative) {
        uint32_t age = now_ms - s->neg_timestamp_ms; /* 无符号回绕对 30s 量级 TTL 无影响 */
        if (age > PK_TILE_CACHE_NEGATIVE_TTL_MS) {
            /* 过期：腾出槽位，当作未命中处理，让调用方重新发起加载。 */
            memset(s, 0, sizeof(*s));
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
    s->key               = key;
    s->data              = data;
    s->generation        = cache->generation;
    s->last_used_seq     = ++cache->seq_counter;
    s->neg_timestamp_ms  = 0;
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
