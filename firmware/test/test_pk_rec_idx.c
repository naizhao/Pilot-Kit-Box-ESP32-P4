/* test_pk_rec_idx.c — host proof for pk_rec_idx（traffic.idx 按机摘要：
 * 在线构建 + 从 traffic.trk 全扫掉电重建）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_recidx \
 *      firmware/test/test_pk_rec_idx.c && /tmp/test_recidx
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_recidx_asan \
 *      firmware/test/test_pk_rec_idx.c && /tmp/test_recidx_asan
 *
 * 同 test_pk_rec_format.c 的翻译单元惯例：把被测 .c 直接 #include 进同一
 * TU。pk_rec_idx.c 内部调用 pk_trk_pos_decode / pk_trk_id_decode /
 * pk_idx_rec_encode 等 pk_rec_format 的编解码函数，所以这里连带 #include
 * pk_rec_format.c（它自己的单测在 test_pk_rec_format.c，这里只是拿它的
 * 实现来搭建输入数据，不重复断言它的字节布局）。
 */
#include <stdio.h>
#include <string.h>

#include "../main/pk_rec_format.c"
#include "../main/pk_rec_idx.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

static const uint8_t ICAO_A[3] = {0xAB, 0xCD, 0xEF};
static const uint8_t ICAO_B[3] = {0x12, 0x34, 0x56};
static const uint8_t ICAO_C[3] = {0x00, 0x00, 0x01};

/* ================================================================ reset / find */

static void test_reset_and_find_empty(void)
{
    pk_rec_idx_table_t t;
    pk_rec_idx_reset(&t);
    CHECK(t.count == 0);
    CHECK(pk_rec_idx_find(&t, ICAO_A) == NULL);
}

static void test_find_or_add_dedup(void)
{
    pk_rec_idx_table_t t;
    pk_rec_idx_reset(&t);

    pk_idx_rec_t *e1 = pk_rec_idx_find_or_add(&t, ICAO_A);
    CHECK(e1 != NULL);
    CHECK(t.count == 1);
    CHECK(memcmp(e1->icao24, ICAO_A, 3) == 0);

    pk_idx_rec_t *e2 = pk_rec_idx_find_or_add(&t, ICAO_A);
    CHECK(e2 == e1);            /* 同一个 ICAO 不新增条目 */
    CHECK(t.count == 1);

    pk_idx_rec_t *e3 = pk_rec_idx_find_or_add(&t, ICAO_B);
    CHECK(e3 != NULL && e3 != e1);
    CHECK(t.count == 2);

    CHECK(pk_rec_idx_find(&t, ICAO_A) == e1);
    CHECK(pk_rec_idx_find(&t, ICAO_B) == e3);
    CHECK(pk_rec_idx_find(&t, ICAO_C) == NULL);
}

static void test_capacity_full(void)
{
    pk_rec_idx_table_t t;
    pk_rec_idx_reset(&t);

    for (unsigned i = 0; i < PK_REC_IDX_MAX_AIRCRAFT; i++) {
        uint8_t icao[3] = {
            (uint8_t)((i >> 16) & 0xFF), (uint8_t)((i >> 8) & 0xFF), (uint8_t)(i & 0xFF)
        };
        CHECK(pk_rec_idx_find_or_add(&t, icao) != NULL);
    }
    CHECK(t.count == PK_REC_IDX_MAX_AIRCRAFT);

    /* 表满：一个全新的 ICAO 加不进去，返回 NULL，count 不变。 */
    uint8_t overflow_icao[3] = {0xFF, 0xFF, 0xFF};
    CHECK(pk_rec_idx_find_or_add(&t, overflow_icao) == NULL);
    CHECK(t.count == PK_REC_IDX_MAX_AIRCRAFT);

    /* 但查找/更新已有条目仍然正常。 */
    uint8_t icao0[3] = {0, 0, 0};
    CHECK(pk_rec_idx_find_or_add(&t, icao0) != NULL);
    CHECK(t.count == PK_REC_IDX_MAX_AIRCRAFT);
}

/* ================================================================ ingest_pos / ingest_id */

static void test_ingest_pos_basic(void)
{
    pk_rec_idx_table_t t;
    pk_rec_idx_reset(&t);

    pk_trk_pos_t p1 = {0};
    memcpy(p1.icao24, ICAO_A, 3);
    p1.ts_ms = 1000;
    p1.flags = 0;   /* 不在地面 */

    pk_rec_idx_ingest_pos(&t, &p1, /*is_own=*/false);

    const pk_idx_rec_t *e = pk_rec_idx_find(&t, ICAO_A);
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(e->point_count == 1);
        CHECK(e->identity_count == 0);
        CHECK((e->flags & PK_IDX_FLAG_HAD_POSITION) != 0);
        CHECK((e->flags & PK_IDX_FLAG_HAD_GROUND) == 0);
        CHECK((e->flags & PK_IDX_FLAG_IS_OWN) == 0);
        CHECK(e->first_ts_ms == 1000);
        CHECK(e->last_ts_ms == 1000);
    }

    /* 第二条：更早的时间戳应该把 first_ts_ms 往前拉，更晚的把 last_ts_ms
     * 往后推——不是"先到先得"。 */
    pk_trk_pos_t p2 = {0};
    memcpy(p2.icao24, ICAO_A, 3);
    p2.ts_ms = 500;
    p2.flags = PK_TRK_FLAG_ON_GROUND;
    pk_rec_idx_ingest_pos(&t, &p2, /*is_own=*/true);

    pk_trk_pos_t p3 = {0};
    memcpy(p3.icao24, ICAO_A, 3);
    p3.ts_ms = 2000;
    pk_rec_idx_ingest_pos(&t, &p3, /*is_own=*/false);

    e = pk_rec_idx_find(&t, ICAO_A);
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(e->point_count == 3);
        CHECK(e->first_ts_ms == 500);
        CHECK(e->last_ts_ms == 2000);
        CHECK((e->flags & PK_IDX_FLAG_HAD_GROUND) != 0);  /* p2 的 on_ground 位留下的印记 */
        CHECK((e->flags & PK_IDX_FLAG_IS_OWN) != 0);       /* p2 的 is_own=true 留下的印记 */
    }
    CHECK(t.count == 1);   /* 全是同一架飞机，不应该多出条目 */
}

static void test_ingest_id_basic(void)
{
    pk_rec_idx_table_t t;
    pk_rec_idx_reset(&t);

    pk_trk_id_t id = {0};
    memcpy(id.icao24, ICAO_B, 3);
    id.ts_ms = 42;
    memcpy(id.callsign, "CCA1234\0", 9);
    id.emitter_category = 5;

    pk_rec_idx_ingest_id(&t, &id, /*is_own=*/false);

    const pk_idx_rec_t *e = pk_rec_idx_find(&t, ICAO_B);
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK(e->identity_count == 1);
        CHECK(e->point_count == 0);
        CHECK((e->flags & PK_IDX_FLAG_HAD_CALLSIGN) != 0);
        CHECK(memcmp(e->callsign, "CCA1234\0", 9) == 0);
        CHECK(e->first_ts_ms == 42);
        CHECK(e->last_ts_ms == 42);
    }
}

/* ================================================================ rebuild_from_records */

static void append_record(uint8_t *buf, size_t *n, const uint8_t rec[PK_TRK_RECORD_LEN])
{
    memcpy(buf + (*n) * PK_TRK_RECORD_LEN, rec, PK_TRK_RECORD_LEN);
    (*n)++;
}

static void test_rebuild_from_records(void)
{
    uint8_t stream[8 * PK_TRK_RECORD_LEN];
    size_t n = 0;
    uint8_t rec[PK_TRK_RECORD_LEN];

    /* A：两条位置（一条 on_ground）+ 一条身份，A 是"本机"。 */
    pk_trk_pos_t pa1 = {0};
    memcpy(pa1.icao24, ICAO_A, 3);
    pa1.ts_ms = 1000;
    pk_trk_pos_encode(&pa1, rec);
    append_record(stream, &n, rec);

    pk_trk_pos_t pa2 = {0};
    memcpy(pa2.icao24, ICAO_A, 3);
    pa2.ts_ms = 3000;
    pa2.flags = PK_TRK_FLAG_ON_GROUND;
    pk_trk_pos_encode(&pa2, rec);
    append_record(stream, &n, rec);

    pk_trk_id_t ida = {0};
    memcpy(ida.icao24, ICAO_A, 3);
    ida.ts_ms = 2000;
    memcpy(ida.callsign, "OWNSHIP\0", 9);
    pk_trk_id_encode(&ida, rec);
    append_record(stream, &n, rec);

    /* B：一条地面位置，不是本机。 */
    pk_trk_pos_t pb1 = {0};
    memcpy(pb1.icao24, ICAO_B, 3);
    pb1.ts_ms = 1500;
    pb1.flags = PK_TRK_FLAG_ON_GROUND;
    pk_trk_pos_encode(&pb1, rec);
    append_record(stream, &n, rec);

    pk_rec_idx_table_t t;
    pk_rec_idx_rebuild_from_records(&t, stream, n, ICAO_A);

    CHECK(t.count == 2);

    const pk_idx_rec_t *ea = pk_rec_idx_find(&t, ICAO_A);
    CHECK(ea != NULL);
    if (ea != NULL) {
        CHECK(ea->point_count == 2);
        CHECK(ea->identity_count == 1);
        CHECK(ea->first_ts_ms == 1000);
        CHECK(ea->last_ts_ms == 3000);
        CHECK((ea->flags & PK_IDX_FLAG_HAD_POSITION) != 0);
        CHECK((ea->flags & PK_IDX_FLAG_HAD_CALLSIGN) != 0);
        CHECK((ea->flags & PK_IDX_FLAG_HAD_GROUND) != 0);
        CHECK((ea->flags & PK_IDX_FLAG_IS_OWN) != 0);
        CHECK(memcmp(ea->callsign, "OWNSHIP\0", 9) == 0);
    }

    const pk_idx_rec_t *eb = pk_rec_idx_find(&t, ICAO_B);
    CHECK(eb != NULL);
    if (eb != NULL) {
        CHECK(eb->point_count == 1);
        CHECK(eb->identity_count == 0);
        CHECK((eb->flags & PK_IDX_FLAG_HAD_GROUND) != 0);
        CHECK((eb->flags & PK_IDX_FLAG_IS_OWN) == 0);   /* own_icao 传的是 A，B 不该带这位 */
    }

    /* own_icao=NULL：不应该有任何条目被标 IS_OWN。 */
    pk_rec_idx_table_t t2;
    pk_rec_idx_rebuild_from_records(&t2, stream, n, NULL);
    const pk_idx_rec_t *ea2 = pk_rec_idx_find(&t2, ICAO_A);
    CHECK(ea2 != NULL && (ea2->flags & PK_IDX_FLAG_IS_OWN) == 0);
}

/* ================================================================ encode_all */

static void test_encode_all(void)
{
    pk_rec_idx_table_t t;
    pk_rec_idx_reset(&t);
    pk_idx_rec_t *a = pk_rec_idx_find_or_add(&t, ICAO_A);
    a->point_count = 7;
    pk_idx_rec_t *b = pk_rec_idx_find_or_add(&t, ICAO_B);
    b->point_count = 9;

    uint8_t buf[4 * PK_IDX_RECORD_LEN];

    CHECK(pk_rec_idx_encode_all(&t, buf, 1) == 1);   /* 容量截断 */
    pk_idx_rec_t back;
    pk_idx_rec_decode(buf, &back);
    CHECK(memcmp(back.icao24, ICAO_A, 3) == 0);
    CHECK(back.point_count == 7);

    CHECK(pk_rec_idx_encode_all(&t, buf, 4) == 2);   /* 容量够，只写实际条数 */
    pk_idx_rec_decode(buf + PK_IDX_RECORD_LEN, &back);
    CHECK(memcmp(back.icao24, ICAO_B, 3) == 0);
    CHECK(back.point_count == 9);
}

int main(void)
{
    test_reset_and_find_empty();
    test_find_or_add_dedup();
    test_capacity_full();
    test_ingest_pos_basic();
    test_ingest_id_basic();
    test_rebuild_from_records();
    test_encode_all();

    if (g_fail == 0) {
        printf("ALL PASS (test_pk_rec_idx)\n");
        return 0;
    }
    fprintf(stderr, "%d CHECK(s) FAILED (test_pk_rec_idx)\n", g_fail);
    return 1;
}
