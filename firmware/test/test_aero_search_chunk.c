/* test_aero_search_chunk.c — host proof：子串搜索的**分段版与一口气版结果
 * 逐条相同**，且分段版真的按预算中断。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_asc \
 *      firmware/test/test_aero_search_chunk.c firmware/main/geo.c -lm && /tmp/test_asc
 * （geo.c 是被 pk_aero_reader.c 的 nearest 路径引用的，与本测试无关，链上即可）
 *
 * 为什么必须有这条：pk_aero_db_search_substring 是全项目唯一一个**中途放锁**
 * 的查询。放锁本身好写，难的是"放开再回来还接得上"——游标里存的是字符串池
 * 内偏移与段号，接错一个字节，命中就会漂到别的记录上，而屏上只会显示一个
 * 看着挺正常的机场。所以这里把两条路径逐条对拍。
 *
 * 手搓 payload 而不是喂真 bin：真库 9.88 MB 且加密，进不了 host 单测；而
 * 这一层要验的只是"顺扫 + 反向索引二分"的续扫正确性，那只依赖字符串池与
 * 一张 u32 索引表的字节布局。布局的权威定义在 pk_aero_reader.h 的文件头。
 */
#include <stdio.h>
#include <string.h>

#include "../main/pk_aero_reader.c"

static int g_fail;

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
}

/* ── 手搓一份只含"机场段 + 名称反向索引"的最小 payload ─────────────
 *
 *   off 8   : 字符串池（rel 0 是"无"哨兵的单个 NUL）
 *             rel 1 "ALPHA"  rel 7 "BRAVO"  rel 13 "ALPHABET"
 *   off 64  : 3 条机场记录（40 B/条），name_off 在记录内偏移 31（u24be），
 *             city_off 在 34（这里全填 0 = 无）
 *   off 256 : 名称反向索引，3 项 u32 = 记录下标，按各自 name_off 升序
 */
#define POOL_OFF    8u
#define REC_OFF     64u
#define IDX_OFF     256u
#define POOL_SIZE   22u          /* 1 + 6 + 6 + 9 */

static uint8_t g_payload[512];

static void wr_u24be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static void wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void build_db(pk_aero_db_t *db)
{
    memset(g_payload, 0, sizeof(g_payload));
    memcpy(g_payload + POOL_OFF + 1,  "ALPHA",    6);
    memcpy(g_payload + POOL_OFF + 7,  "BRAVO",    6);
    memcpy(g_payload + POOL_OFF + 13, "ALPHABET", 9);

    const uint32_t name_off[3] = { 1, 7, 13 };
    for (uint32_t i = 0; i < 3; ++i) {
        uint8_t *r = g_payload + REC_OFF + i * PK_AERO_AIRPORT_SIZE;
        wr_u24be(r + 31, name_off[i]);
        wr_u24be(r + 34, 0);
        for (uint32_t k = 0; k < 3; ++k) wr_u32le(g_payload + IDX_OFF + k * 4, k);
    }

    memset(db, 0, sizeof(*db));
    db->payload     = g_payload;
    db->payload_len = sizeof(g_payload);
    db->version     = 3;
    db->sec_airport.data_off     = REC_OFF;
    db->sec_airport.n            = 3;
    db->sec_airport.rec_size     = PK_AERO_AIRPORT_SIZE;
    db->sec_airport.strings_off  = POOL_OFF;
    db->sec_airport.strings_size = POOL_SIZE;
    db->sec_idx_apt_name.data_off = IDX_OFF;
    db->sec_idx_apt_name.n        = 3;
    /* 导航台 / FIX 两段留空（strings_off==0）→ 扫描直接跳过，正是 v2 卡上
     * 索引缺席时该走的那条优雅退化路径。 */
}

/* 分段跑到底，返回条数与调用次数。 */
static int run_chunked(pk_aero_db_t *db, const char *q, pk_aero_hit_t *out,
                       int max, uint32_t budget, int *calls)
{
    pk_aero_search_cursor_t cur = { 0, 0, 0 };
    *calls = 0;
    while (1) {
        (*calls)++;
        if (pk_aero_search_substring_step(db, q, out, max, &cur, budget)) break;
        if (*calls > 1000) { printf("FAIL 分段版不收敛\n"); g_fail++; break; }
    }
    return cur.n;
}

static void test_equivalence(void)
{
    pk_aero_db_t db;
    pk_aero_hit_t a[8], b[8];

    build_db(&db);
    const int n1 = pk_aero_search_substring(&db, "ALPHA", a, 8);
    chk_int("一口气版命中数", n1, 2);
    chk_int("一口气版[0] 是机场段", a[0].type, PK_AERO_SEC_AIRPORTS);
    chk_int("一口气版[0] 记录 0（\"ALPHA\"）", (int)a[0].idx, 0);
    chk_int("一口气版[1] 记录 2（\"ALPHABET\" 也含 ALPHA）", (int)a[1].idx, 2);

    /* 预算 4 字节：池里最短的串也有 6 字节，必然每条串都要中断一次。 */
    build_db(&db);
    int calls = 0;
    const int n2 = run_chunked(&db, "ALPHA", b, 8, 4, &calls);
    chk_int("分段版命中数与一口气版相同", n2, n1);
    for (int i = 0; i < n2; ++i) {
        chk_int("分段版 type 逐条相同", b[i].type, a[i].type);
        chk_int("分段版 idx 逐条相同", (int)b[i].idx, (int)a[i].idx);
    }
    if (calls < 3) { printf("FAIL 分段版没有真的中断（calls=%d）\n", calls); g_fail++; }

    /* 换一个只命中中间那条的查询，验证续扫没有把游标推过头 */
    build_db(&db);
    const int n3 = run_chunked(&db, "BRAVO", b, 8, 4, &calls);
    chk_int("BRAVO 命中数", n3, 1);
    chk_int("BRAVO 命中记录 1", (int)b[0].idx, 1);

    /* 大小写不敏感（norm_query 会把查询转大写，池内比较也归一） */
    build_db(&db);
    chk_int("小写查询同样命中",
            run_chunked(&db, "alpha", b, 8, 4, &calls), 2);

    /* max 截断：写满就停，两条路径都该只给 1 条 */
    build_db(&db);
    chk_int("一口气版 max=1", pk_aero_search_substring(&db, "ALPHA", a, 1), 1);
    build_db(&db);
    chk_int("分段版 max=1", run_chunked(&db, "ALPHA", b, 1, 4, &calls), 1);

    /* 无命中 */
    build_db(&db);
    chk_int("无命中", run_chunked(&db, "ZULU", b, 8, 4, &calls), 0);
}

/* 预算 0 = 不限，必须一次调用就跑完（pk_aero_search_substring 正是这么用的）*/
static void test_unlimited_is_one_shot(void)
{
    pk_aero_db_t db;
    pk_aero_hit_t out[8];
    build_db(&db);
    int calls = 0;
    const int n = run_chunked(&db, "ALPHA", out, 8, 0, &calls);
    chk_int("不限预算 命中数", n, 2);
    chk_int("不限预算 只需一次调用", calls, 1);
}

int main(void)
{
    test_equivalence();
    test_unlimited_is_one_shot();
    if (g_fail == 0) printf("test_aero_search_chunk: 全部通过\n");
    else             printf("test_aero_search_chunk: %d 处失败\n", g_fail);
    return g_fail ? 1 : 0;
}
