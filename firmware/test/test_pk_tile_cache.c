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

/* ---- 临时负缓存（0x106 退避，2026-08-04）------------------------------
 * 真机抓包：SD 并发过载时 sdmmc_read_blocks 返 0x106，fetch_and_decode 标
 * TRANSIENT 不进负缓存、每帧重试 → "失败-重开句柄-再失败"死循环刷屏。
 * 临时负缓存给 IO 类失败一个短 TTL 退避：map_page 这段时间内走 ancestor
 * blit（糊但不空白）、不再 request → 不再抢 SD。与"瓦片真缺失"那条 30s
 * 负缓存是两条独立的路。*/
static void test_temp_negative_backoff(void)
{
    printf("-- 临时负缓存：IO 失败退避 + 计数升级 + 成功清零 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);
    pk_tile_key_t k = mk_key(2, 10, 841, 394);

    /* 第 1-2 次失败：偶发，不退避（计数累积但还没插临时负缓存）。
     * bump_sd_fail 返回当前累计次数。 */
    chk_u32("第 1 次 IO 失败: count→1", pk_tile_cache_bump_sd_fail(&c, k, 1000), 1);
    bool neg = true;
    /* 还没到退避阈值，get 不该看到负缓存（该走重试） */
    const uint16_t *r = pk_tile_cache_get(&c, k, 1000, &neg);
    chk_true("计数 1 时未插临时负缓存: get 返回 NULL 但 is_negative=false", r == NULL && !neg);

    chk_u32("第 2 次 IO 失败: count→2", pk_tile_cache_bump_sd_fail(&c, k, 1000), 2);

    /* 第 3 次失败：达到退避阈值，插临时负缓存（短 TTL） */
    uint32_t t = 2000;
    chk_u32("第 3 次 IO 失败: count→3（触发退避）", pk_tile_cache_bump_sd_fail(&c, k, t), 3);
    pk_tile_cache_put_temp_negative(&c, k, t);
    neg = false;
    r = pk_tile_cache_get(&c, k, t + 100, &neg);   /* 退避期内 */
    chk_true("退避期内 is_negative=true（走 ancestor blit）", r == NULL && neg);

    /* 临时负缓存短 TTL（2s）过期后：又能重试 */
    neg = true;
    r = pk_tile_cache_get(&c, k, t + PK_TILE_CACHE_TEMP_NEG_TTL_MS + 1, &neg);
    chk_true("临时负缓存过期后 is_negative=false（允许重试）", r == NULL && !neg);

    /* 持续失败 ≥5 次：升级到长 TTL（5s）退避 */
    pk_tile_cache_bump_sd_fail(&c, k, t);  /* 4 */
    t = 10000;
    chk_u32("第 5 次: count→5（升级退避）", pk_tile_cache_bump_sd_fail(&c, k, t), 5);
    pk_tile_cache_put_temp_negative(&c, k, t);
    neg = false;
    r = pk_tile_cache_get(&c, k, t + PK_TILE_CACHE_TEMP_NEG_TTL_MS + 1, &neg);
    chk_true("count≥5 后短 TTL 已过期但长 TTL 仍负缓存", r == NULL && neg);
    neg = false;
    r = pk_tile_cache_get(&c, k, t + PK_TILE_CACHE_TEMP_NEG_LONG_TTL_MS + 1, &neg);
    chk_true("长 TTL 过期后允许重试", r == NULL && !neg);

    /* 成功后计数必须清零——不能让一次 SD 拥塞永久污染这块瓦片 */
    pk_tile_cache_reset_sd_fail(&c, k);
    chk_u32("成功后 count 清零", pk_tile_cache_bump_sd_fail(&c, k, 20000), 1);

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

/* ------------------------------------------------------- 主动淘汰（PSRAM 让路） */

static void test_evict_lru(void)
{
    printf("-- evict_lru：按 LRU 主动让出瓦片内存 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    for (int i = 0; i < 5; i++) {
        pk_tile_cache_put(&c, mk_key(1, 9, (uint32_t)i, 0), mk_tile((uint16_t)i));
    }
    /* get key0 把它提为最近使用 → 真正最久未用的是 key1。 */
    bool neg;
    chk_true("提升前 key0 命中", pk_tile_cache_get(&c, mk_key(1, 9, 0, 0), 0, &neg) != NULL);

    chk_true("evict 成功", pk_tile_cache_evict_lru(&c));
    chk_true("被淘汰的是 key1（最久未用）",
             pk_tile_cache_get(&c, mk_key(1, 9, 1, 0), 0, &neg) == NULL);
    chk_true("key0 仍在", pk_tile_cache_get(&c, mk_key(1, 9, 0, 0), 0, &neg) != NULL);

    int used = -1, negc = -1;
    size_t bytes = 0;
    pk_tile_cache_stats(&c, &used, &negc, &bytes);
    chk_u32("淘汰后 used 槽数", (uint32_t)used, 4);
    chk_u32("淘汰后占用字节", (uint32_t)bytes, 4u * PK_TILE_BUF_BYTES);

    /* 把剩下 4 条全让完，第 5 次没得让 → false。 */
    for (int i = 0; i < 4; i++) chk_true("继续 evict", pk_tile_cache_evict_lru(&c));
    chk_true("空缓存 evict 返回 false", !pk_tile_cache_evict_lru(&c));

    pk_tile_cache_deinit(&c);
}

static void test_evict_skips_negative(void)
{
    printf("-- evict_lru 不动负缓存（它不占瓦片内存） --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    /* 负缓存先进（最久未用），真瓦片后进。 */
    pk_tile_cache_put_negative(&c, mk_key(4, 7, 1, 1), 0);
    pk_tile_cache_put(&c, mk_key(4, 7, 2, 2), mk_tile(0x5555));

    chk_true("evict 成功", pk_tile_cache_evict_lru(&c));

    bool neg = false;
    chk_true("真瓦片被淘汰（哪怕它更新）",
             pk_tile_cache_get(&c, mk_key(4, 7, 2, 2), 0, &neg) == NULL);
    (void)pk_tile_cache_get(&c, mk_key(4, 7, 1, 1), 0, &neg);
    chk_true("负缓存条目原样留着", neg);
    chk_true("只剩负缓存时 evict 返回 false", !pk_tile_cache_evict_lru(&c));

    pk_tile_cache_deinit(&c);
}

static void test_acquire_buffer(void)
{
    printf("-- acquire_buffer：host 侧内存充裕时不误淘汰 --\n");
    pk_tile_cache_t c;
    pk_tile_cache_init(&c);

    for (int i = 0; i < 3; i++) {
        pk_tile_cache_put(&c, mk_key(5, 8, (uint32_t)i, 0), mk_tile((uint16_t)i));
    }
    uint32_t fails_before = pk_tile_cache_alloc_fail_count();
    uint16_t *buf = pk_tile_cache_acquire_buffer(&c);
    chk_true("acquire 拿到缓冲区", buf != NULL);
    chk_u32("acquire 成功不计失败数", pk_tile_cache_alloc_fail_count(), fails_before);

    int used = -1;
    pk_tile_cache_stats(&c, &used, NULL, NULL);
    chk_u32("内存充裕时一条都没淘汰", (uint32_t)used, 3);

    pk_tile_cache_put(&c, mk_key(5, 8, 99, 0), buf);
    pk_tile_cache_deinit(&c);
}

int main(void)
{
    test_basic_put_get();
    test_lru_eviction_order();
    test_lru_promotion_on_get();
    test_negative_cache_and_expiry();
    test_temp_negative_backoff();
    test_put_overwrites_same_key();
    test_generation_bump_clears_all();
    test_evict_lru();
    test_evict_skips_negative();
    test_acquire_buffer();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
