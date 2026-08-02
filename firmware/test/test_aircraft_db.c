/* test_aircraft_db.c — host proof：机型库解析器 + 合成查询 API。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_acdb \
 *      firmware/test/test_aircraft_db.c && /tmp/test_acdb
 *
 * 覆盖三件事：
 *   1) 容器头（"PKACT1" 64 B，与 pk_aero.bin 同构）解析：好头、坏 magic、
 *      坏版本、enc_algo != 0、截断；
 *   2) 一次二分取全部字段（pk_actdb_lookup）：命中/未命中、缺字段（无机型
 *      代码只有注册号）、超长串截断；
 *   3) 单条 last-lookup 缓存：连查同一架只做 1 次二分、负结果也缓存、
 *      换库（generation）后失效。缓存那份逻辑在 aircraft_db.c 里带
 *      FreeRTOS/PSA 依赖进不了 host，所以这里按同一算法复刻一份对拍——
 *      测的是"缓存策略对不对"，不是"那份实现的字节"。
 *
 * 手搓 payload 而不是喂真 8 MB bin：真 bin 不该进单测（体积 + 它随上游变），
 * 而这一层要验的只是字节布局与边界，布局的权威定义在
 * firmware/scripts/gen_aircraft_db.py 的文档字符串。
 */
#include <stdio.h>
#include <string.h>

#include "../main/aircraft_db_reader.c"

static int g_fail;

static void chk(const char *what, bool ok)
{
    if (!ok) { printf("FAIL %s\n", what); g_fail++; }
}

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
}

static void chk_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
        g_fail++;
    }
}

/* ── 手搓一份最小 PKADB1 载荷 ─────────────────────────────────────
 *   3 条记录：0x000001（type 0 + reg）、0x00ABCD（type 1，无 reg）、
 *             0xFFFFFE（无 type，有 reg）
 *   2 个类型：0 = B738/BOEING 737-800/L2J，1 = 只有 code（model/desc 缺）
 *   字符串池：offset 0 是"无"哨兵
 */
#define N_REC   3
#define N_TYP   2
#define REC_OFF 32u
#define TYP_OFF (REC_OFF + N_REC * 8u)
#define STR_OFF (TYP_OFF + N_TYP * 12u)

/* 池内偏移（第 1 字节起） */
#define S_B738   1u                       /* "B738"  5 B */
#define S_MODEL  (S_B738 + 5u)            /* "BOEING 737-800" 15 B */
#define S_L2J    (S_MODEL + 15u)          /* "L2J"   4 B */
#define S_REG1   (S_L2J + 4u)             /* "B-5797" 7 B */
#define S_LONG   (S_REG1 + 7u)            /* 60 个 'X' + NUL = 61 B（测截断） */
#define POOL_LEN (S_LONG + 61u)

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put_u24be(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 16) & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = v & 0xFF;
}

static uint8_t g_payload[STR_OFF + POOL_LEN];

static void build_payload(void)
{
    memset(g_payload, 0, sizeof(g_payload));
    memcpy(g_payload, "PKADB1", 6);
    put_u16(g_payload + 6, 2);              /* version */
    put_u32(g_payload + 8, N_REC);
    put_u32(g_payload + 12, REC_OFF);
    put_u16(g_payload + 16, N_TYP);
    put_u16(g_payload + 18, 0);             /* _reserved */
    put_u32(g_payload + 20, TYP_OFF);
    put_u32(g_payload + 24, STR_OFF);
    put_u32(g_payload + 28, POOL_LEN);

    uint8_t *r = g_payload + REC_OFF;
    put_u24be(r, 0x000001); put_u16(r + 3, 0); put_u24be(r + 5, S_REG1);
    r += 8;
    put_u24be(r, 0x00ABCD); put_u16(r + 3, 1); put_u24be(r + 5, 0);
    r += 8;
    put_u24be(r, 0xFFFFFE); put_u16(r + 3, PK_ACTDB_TYPE_NONE);
    put_u24be(r + 5, S_LONG);

    uint8_t *t = g_payload + TYP_OFF;
    put_u32(t, S_B738); put_u32(t + 4, S_MODEL); put_u32(t + 8, S_L2J);
    t += 12;
    put_u32(t, S_B738); put_u32(t + 4, 0); put_u32(t + 8, 0);

    char *pool = (char *)(g_payload + STR_OFF);
    pool[0] = '\0';
    strcpy(pool + S_B738,  "B738");
    strcpy(pool + S_MODEL, "BOEING 737-800");
    strcpy(pool + S_L2J,   "L2J");
    strcpy(pool + S_REG1,  "B-5797");
    memset(pool + S_LONG, 'X', 60);
    pool[S_LONG + 60] = '\0';
}

/* 套上 64 B 容器头。out 至少 64 + payload_len。 */
static size_t wrap_container(uint8_t *out, uint16_t version, uint8_t enc)
{
    memset(out, 0, PK_ACTDB_HEADER_SIZE);
    memcpy(out, PK_ACTDB_MAGIC, 6);
    put_u16(out + 6, version);
    memcpy(out + 8, "20260801", 8);
    put_u16(out + 16, 0);                        /* n_sections */
    put_u32(out + 18, PK_ACTDB_HEADER_SIZE);     /* sections_off */
    out[22] = enc;
    /* nonce/sha256 留 0：sha 校验是 aircraft_db.c 的事，reader 只搬运 */
    memcpy(out + PK_ACTDB_HEADER_SIZE, g_payload, sizeof(g_payload));
    return PK_ACTDB_HEADER_SIZE + sizeof(g_payload);
}

/* ── 1) 容器头解析 ─────────────────────────────────────────────── */
static void test_container(void)
{
    static uint8_t buf[PK_ACTDB_HEADER_SIZE + sizeof(g_payload)];
    pk_actdb_t db;

    size_t n = wrap_container(buf, PK_ACTDB_VERSION, PK_ACTDB_ENC_NONE);
    chk_int("container ok", pk_actdb_init(&db, buf, n, true), PK_ACTDB_OK);
    chk_str("cycle", db.cycle, "20260801");
    chk_int("payload_off", (int)(db.payload - buf), PK_ACTDB_HEADER_SIZE);
    chk_int("n_records", (int)db.n_records, N_REC);
    chk_int("n_types", (int)db.n_types, N_TYP);

    buf[0] = 'X';
    chk_int("bad magic", pk_actdb_init(&db, buf, n, true), PK_ACTDB_ERR_MAGIC);
    buf[0] = 'P';

    put_u16(buf + 6, 99);
    chk_int("bad version", pk_actdb_init(&db, buf, n, true), PK_ACTDB_ERR_VERSION);
    put_u16(buf + 6, PK_ACTDB_VERSION);

    buf[22] = 1;
    chk_int("encrypted rejected", pk_actdb_init(&db, buf, n, true),
            PK_ACTDB_ERR_ENCRYPTED);
    buf[22] = 0;

    chk_int("truncated header", pk_actdb_init(&db, buf, 32, true),
            PK_ACTDB_ERR_TRUNCATED);
    chk_int("truncated payload", pk_actdb_init(&db, buf, n - 8, true),
            PK_ACTDB_ERR_TRUNCATED);

    /* 裸载荷模式（EMBED 时代 / 单测手搓） */
    chk_int("raw payload ok",
            pk_actdb_init(&db, g_payload, sizeof(g_payload), false), PK_ACTDB_OK);
    /* 裸载荷去掉容器头后不该被当容器认出来 */
    chk_int("raw as container",
            pk_actdb_init(&db, g_payload, sizeof(g_payload), true),
            PK_ACTDB_ERR_MAGIC);
}

/* ── 2) 一次二分取全部字段 ──────────────────────────────────────── */
static void test_lookup(void)
{
    pk_actdb_t db;
    chk_int("init", pk_actdb_init(&db, g_payload, sizeof(g_payload), false),
            PK_ACTDB_OK);

    pk_aircraft_info_t info;

    chk("hit 000001", pk_actdb_lookup(&db, 0x000001, &info));
    chk_str("code", info.code, "B738");
    chk_str("model", info.model, "BOEING 737-800");
    chk_str("desc", info.desc, "L2J");
    chk_str("reg", info.reg, "B-5797");

    /* 有机型代码但 model/desc 缺、且无注册号 */
    chk("hit 00ABCD", pk_actdb_lookup(&db, 0x00ABCD, &info));
    chk_str("code2", info.code, "B738");
    chk_str("model2 empty", info.model, "");
    chk_str("desc2 empty", info.desc, "");
    chk_str("reg2 empty", info.reg, "");

    /* 无机型、有注册号，且注册号超长 → 截断到 sizeof(reg)-1 = 19 */
    chk("hit FFFFFE", pk_actdb_lookup(&db, 0xFFFFFE, &info));
    chk_str("code3 empty", info.code, "");
    chk_int("reg3 truncated", (int)strlen(info.reg),
            (int)sizeof(info.reg) - 1);

    /* 未命中：返回 false 且结构体被清空（调用方直接读字段不会读到上一架） */
    memset(&info, 'Z', sizeof(info));
    chk("miss", !pk_actdb_lookup(&db, 0x123456, &info));
    chk_str("miss cleared", info.code, "");
    chk_str("miss reg cleared", info.reg, "");

    /* 边界：首条与末条都要能查到（二分的两端最容易写错） */
    chk("first", pk_actdb_lookup(&db, 0x000000 + 1, &info));
    chk("last", pk_actdb_lookup(&db, 0xFFFFFE, &info));
    chk("below first", !pk_actdb_lookup(&db, 0x000000, &info));
    chk("above last", !pk_actdb_lookup(&db, 0xFFFFFF, &info));

    /* 高位被忽略：调用方传的 icao24 只有低 24 位有意义 */
    chk("masked", pk_actdb_lookup(&db, 0xAA000001u, &info));
    chk_str("masked code", info.code, "B738");
}

/* ── 3) 单条 last-lookup 缓存（复刻 aircraft_db.c 的策略）────────── */
static struct {
    bool               valid;
    uint32_t           icao24;
    bool               found;
    pk_aircraft_info_t info;
    uint32_t           generation;
} g_cache;

static int g_searches;   /* 实际做了几次二分 */

static const pk_aircraft_info_t *cached_lookup(const pk_actdb_t *db,
                                               uint32_t generation,
                                               uint32_t icao24)
{
    icao24 &= 0xFFFFFF;
    if (g_cache.valid && g_cache.generation == generation &&
        g_cache.icao24 == icao24)
        return g_cache.found ? &g_cache.info : NULL;

    g_searches++;
    g_cache.found      = pk_actdb_lookup(db, icao24, &g_cache.info);
    g_cache.icao24     = icao24;
    g_cache.generation = generation;
    g_cache.valid      = true;
    return g_cache.found ? &g_cache.info : NULL;
}

static void test_cache(void)
{
    pk_actdb_t db;
    pk_actdb_init(&db, g_payload, sizeof(g_payload), false);
    uint32_t gen = 1;

    memset(&g_cache, 0, sizeof(g_cache));
    g_searches = 0;

    /* 一帧内的 4 连查（adsb_list 的 reg/code/model + traffic 的 desc）= 1 次二分 */
    for (int i = 0; i < 4; i++) cached_lookup(&db, gen, 0x000001);
    chk_int("4 calls -> 1 search", g_searches, 1);

    /* 跨帧重查同一架：仍是 0 次新二分 */
    for (int f = 0; f < 30; f++)
        for (int i = 0; i < 4; i++) cached_lookup(&db, gen, 0x000001);
    chk_int("30 frames x4 -> still 1 search", g_searches, 1);

    /* 换飞机 → 1 次新二分；换回来 → 又 1 次（单条缓存的已知代价） */
    cached_lookup(&db, gen, 0x00ABCD);
    chk_int("switch aircraft", g_searches, 2);
    cached_lookup(&db, gen, 0x000001);
    chk_int("switch back", g_searches, 3);

    /* 负结果也缓存：查不到的飞机连查 10 次只做 1 次二分 */
    g_searches = 0;
    for (int i = 0; i < 10; i++)
        chk("miss cached", cached_lookup(&db, gen, 0x123456) == NULL);
    chk_int("negative cached", g_searches, 1);

    /* 换库（拔卡重插 / 重新加载）后必须失效——缓存里那份是上一份数据的拷贝 */
    g_searches = 0;
    cached_lookup(&db, gen, 0x000001);
    chk_int("warm", g_searches, 1);
    gen++;
    cached_lookup(&db, gen, 0x000001);
    chk_int("generation bump invalidates", g_searches, 2);
}

int main(void)
{
    build_payload();
    test_container();
    test_lookup();
    test_cache();
    if (g_fail == 0) printf("test_aircraft_db: ALL PASS\n");
    else             printf("test_aircraft_db: %d FAILURES\n", g_fail);
    return g_fail != 0;
}
