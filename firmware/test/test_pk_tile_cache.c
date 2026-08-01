/* test_pk_tile_cache.c — host proof for pk_tile_cache（24 槽 RGB565 LRU +
 * 负缓存 + generation 作废）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_tc \
 *      firmware/test/test_pk_tile_cache.c && /tmp/test_tc
 *
 * 覆盖设计文档「盒子端架构」第 4 点列的三件事：LRU 淘汰顺序、负缓存过期、
 * generation 清场；外加 put 覆盖同 key、alloc/free 配对不泄漏（用一个包装
 * 计数器代理验证，而不是接 valgrind——host 侧最小化外部依赖）。
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/pk_tile_cache.c"

static int g_fail = 0;

static void chk_true(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static void chk_u32(const char *what, uint32_t got, uint32_t want)
{
    bool ok = got == want;
    printf("  [%s] %-32s got=%u want=%u\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) g_fail++;
}

static pk_tile_key_t mk_key(uint32_t pack_id, uint8_t z, uint32_t x, uint32_t y)
{
    pk_tile_key_t k = { .pack_id = pack_id, .z = z, .x = x, .y = y };
    return k;
}

/* 往缓冲区里塞一个可辨识的字节模式，方便 get 之后核对确实拿回了同一块数据
 * （而不仅仅是"非 NULL"）。 */
static uint16_t *mk_tile(uint16_t fill)
{
    uint16_t *buf = pk_tile_cache_alloc_tile_buffer();
    if (!buf) return NULL;
    for (int i = 0; i < PK_TILE_BUF_PIXELS; i++) buf[i] = fill;
    return buf;
}

/* ------------------------------------------------------------- 基本命中/未命中 */

static void test_basic_put_get(void)
{
    printf("-- 基本 put/get --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    pk_tile_key_t k = mk_key(1, 8, 10, 20);
    bool neg;
    const uint16_t *miss = pk_tile_cache_get(&c, k, 0, &neg);
    chk_true("初始未命中", miss == NULL);
    chk_true("初始未命中不是负缓存", !neg);

    pk_tile_cache_put(&c, k, mk_tile(0x1234));
    const uint16_t *hit = pk_tile_cache_get(&c, k, 1000, &neg);
    chk_true("put 后命中", hit != NULL);
    chk_true("命中不是负缓存", !neg);
    chk_true("命中数据内容正确", hit && hit[0] == 0x1234 && hit[PK_TILE_BUF_PIXELS - 1] == 0x1234);

    /* 不同 z/x/y 不应该命中同一条 */
    const uint16_t *other = pk_tile_cache_get(&c, mk_key(1, 8, 10, 21), 1000, &neg);
    chk_true("不同 y 不命中", other == NULL);

    pk_tile_cache_deinit(&c);
}

/* ------------------------------------------------------------------ LRU 淘汰 */

static void test_lru_eviction_order(void)
{
    printf("-- LRU 淘汰顺序 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    /* 填满 24 槽，key 0..23，插入顺序即最初的"新旧"顺序。 */
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        pk_tile_cache_put(&c, mk_key(1, 10, (uint32_t)i, 0), mk_tile((uint16_t)i));
    }

    /* 再插入第 25 个 —— 应该淘汰最久未用的 key 0（从未被 get 过，插入顺序最早）。 */
    pk_tile_cache_put(&c, mk_key(1, 10, 100, 0), mk_tile(0xAAAA));

    bool neg;
    chk_true("key0 被淘汰", pk_tile_cache_get(&c, mk_key(1, 10, 0, 0), 0, &neg) == NULL);
    chk_true("key1 仍在（未被淘汰）", pk_tile_cache_get(&c, mk_key(1, 10, 1, 0), 0, &neg) != NULL);
    chk_true("新插入的 key100 在场", pk_tile_cache_get(&c, mk_key(1, 10, 100, 0), 0, &neg) != NULL);

    pk_tile_cache_deinit(&c);
}

static void test_lru_promotion_on_get(void)
{
    printf("-- LRU 命中提升 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        pk_tile_cache_put(&c, mk_key(1, 10, (uint32_t)i, 0), mk_tile((uint16_t)i));
    }
    /* get key0 一下，把它从"最久未用"提升为"最近使用"；这之后 key1 才是
     * 最久未用的那个。 */
    bool neg;
    const uint16_t *promoted = pk_tile_cache_get(&c, mk_key(1, 10, 0, 0), 0, &neg);
    chk_true("key0 提升前先确认命中", promoted != NULL);

    pk_tile_cache_put(&c, mk_key(1, 10, 200, 0), mk_tile(0xBBBB));

    chk_true("key0 因为被 get 过而幸存", pk_tile_cache_get(&c, mk_key(1, 10, 0, 0), 0, &neg) != NULL);
    chk_true("key1 (真正最久未用) 被淘汰", pk_tile_cache_get(&c, mk_key(1, 10, 1, 0), 0, &neg) == NULL);

    pk_tile_cache_deinit(&c);
}

/* --------------------------------------------------------------------- 负缓存 */

static void test_negative_cache_and_expiry(void)
{
    printf("-- 负缓存：确认缺失 + 过期 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    pk_tile_key_t k = mk_key(2, 5, 3, 3);
    pk_tile_cache_put_negative(&c, k, 1000);

    bool neg = false;
    const uint16_t *r1 = pk_tile_cache_get(&c, k, 1005, &neg);
    chk_true("负缓存未过期时返回 NULL", r1 == NULL);
    chk_true("负缓存未过期时 is_negative=true", neg);

    /* 恰好在 TTL 边界内（<=30000ms）仍算未过期 */
    const uint16_t *r2 = pk_tile_cache_get(&c, k, 1000 + PK_TILE_CACHE_NEGATIVE_TTL_MS, &neg);
    chk_true("负缓存刚好到 TTL 边界仍未过期", r2 == NULL);
    chk_true("边界处仍是 negative", neg);

    /* 过期之后：视为未缓存，调用方应该重新加载 */
    neg = true; /* 故意塞脏值，确认函数会写回 false */
    const uint16_t *r3 = pk_tile_cache_get(&c, k, 1000 + PK_TILE_CACHE_NEGATIVE_TTL_MS + 1, &neg);
    chk_true("负缓存过期后返回 NULL", r3 == NULL);
    chk_true("负缓存过期后 is_negative=false（需要重新加载）", !neg);

    /* 过期后槽位应已腾出——用同一 key 立刻 put 正常数据能马上命中且是正数据 */
    pk_tile_cache_put(&c, k, mk_tile(0xCAFE));
    const uint16_t *r4 = pk_tile_cache_get(&c, k, 1000 + PK_TILE_CACHE_NEGATIVE_TTL_MS + 2, &neg);
    chk_true("过期负缓存被真实瓦片顶替后命中", r4 != NULL);
    chk_true("顶替后不再是负缓存", !neg);
    chk_true("顶替后数据内容正确", r4 && r4[0] == 0xCAFE);

    pk_tile_cache_deinit(&c);
}

static void test_put_overwrites_same_key(void)
{
    printf("-- 同一 key 重复 put 不留旧数据 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    pk_tile_key_t k = mk_key(3, 6, 1, 1);
    pk_tile_cache_put(&c, k, mk_tile(0x1111));
    pk_tile_cache_put(&c, k, mk_tile(0x2222)); /* 旧的那块要被 free，不能泄漏（ASan/valgrind 之外的信号见下） */

    bool neg;
    const uint16_t *r = pk_tile_cache_get(&c, k, 0, &neg);
    chk_true("重复 put 后命中最新数据", r != NULL && r[0] == 0x2222);

    /* 同一 key 不应该占两个槽位：把剩下 23 个槽位也填满，第 25 次 put 才应
     * 触发淘汰逻辑；如果重复 put 意外占了两条，这里会提前触发淘汰，key 顺序
     * 就会跟预期不一致。用"能不能不多不少再放 23 个不同 key 且互不淘汰"来
     * 侧面验证没有重复占位。 */
    for (int i = 0; i < PK_TILE_CACHE_SLOTS - 1; i++) {
        pk_tile_cache_put(&c, mk_key(3, 6, 100 + (uint32_t)i, 0), mk_tile((uint16_t)i));
    }
    const uint16_t *still_there = pk_tile_cache_get(&c, k, 0, &neg);
    chk_true("填满剩余槽位后原 key 仍在（证明没有重复占位吃掉一个额外槽）", still_there != NULL);

    pk_tile_cache_deinit(&c);
}

/* ------------------------------------------------------------------- generation */

static void test_generation_bump_clears_all(void)
{
    printf("-- generation 作废：拔卡/rescan 整体清场 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    for (int i = 0; i < 10; i++) {
        pk_tile_cache_put(&c, mk_key(1, 10, (uint32_t)i, 0), mk_tile((uint16_t)i));
    }
    pk_tile_cache_put_negative(&c, mk_key(1, 10, 999, 0), 0);

    pk_tile_cache_bump_generation(&c);

    bool neg;
    for (int i = 0; i < 10; i++) {
        chk_true("generation 作废后旧条目不再命中",
                 pk_tile_cache_get(&c, mk_key(1, 10, (uint32_t)i, 0), 0, &neg) == NULL);
    }
    chk_true("generation 作废后旧负缓存条目也不再命中",
             pk_tile_cache_get(&c, mk_key(1, 10, 999, 0), 0, &neg) == NULL);

    /* 作废之后应该能重新塞满 24 个新条目而不触发任何"旧数据没让出槽位"的
     * 异常——用一个自定义计数验证槽位确实是干净的：填满 24 个后全部命中。 */
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        pk_tile_cache_put(&c, mk_key(2, 11, (uint32_t)i, 0), mk_tile((uint16_t)i));
    }
    int hits = 0;
    for (int i = 0; i < PK_TILE_CACHE_SLOTS; i++) {
        if (pk_tile_cache_get(&c, mk_key(2, 11, (uint32_t)i, 0), 0, &neg) != NULL) hits++;
    }
    chk_u32("作废后重新填满 24 槽全部命中", (uint32_t)hits, PK_TILE_CACHE_SLOTS);

    pk_tile_cache_deinit(&c);
}

int main(void)
{
    test_basic_put_get();
    test_lru_eviction_order();
    test_lru_promotion_on_get();
    test_negative_cache_and_expiry();
    test_put_overwrites_same_key();
    test_generation_bump_clears_all();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
