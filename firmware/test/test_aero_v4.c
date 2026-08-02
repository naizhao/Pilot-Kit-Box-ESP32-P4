/* test_aero_v4.c — host proof：v4 空域/航路读取 + **v2/v3/v4 三版兼容**。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_av4 \
 *      firmware/test/test_aero_v4.c firmware/main/geo.c -lm && /tmp/test_av4
 *
 * 为什么必须有这条：
 *   1. **三版兼容是硬要求**——用户卡上现在还是 v3，v4 只是躺在电脑里。
 *      "老卡上新段缺席 → 查询返回 0/false 而不是报错/野指针"这件事，
 *      靠真机只能证明手边那一版，三版一起证只能在 host 上做（手搓三个
 *      版本的容器，逐版跑同一组断言）。
 *   2. **经度跨 ±180 的短弧口径**是管线那边修过的一个真 bug：朴素 min/max
 *      会把阿留申的短航段算成横跨半球的假区间。读端与绘制端必须同口径，
 *      所以这里把 pk_aero_airway_lon_span 的数值钉死。
 *
 * 手搓容器而不是喂真 bin：真库 16.63 MB 且加密，进不了 host 单测；而这一层
 * 要验的只是段表解析 + 记录字段偏移 + 格展开索引的字节布局，那些都是常量。
 * 布局的权威定义在 pk_aero_reader.h 的文件头与 export_box_bin.py。
 */
#include <stdio.h>
#include <string.h>

#include "../main/pk_aero_reader.c"

static int g_fail;

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
}

static void chk_dbl(const char *what, double got, double want)
{
    double d = got - want;
    if (d < 0) d = -d;
    if (d > 1e-9) { printf("FAIL %s: got %.12f want %.12f\n", what, got, want); g_fail++; }
}

static void chk_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
        g_fail++;
    }
}

static void chk_true(const char *what, bool got)
{
    if (!got) { printf("FAIL %s: got false\n", what); g_fail++; }
}

/* ── 字节写入小工具（与 reader 的 rd_* 对偶）───────────────────────── */

static void wr_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr_i32(uint8_t *p, int32_t v) { wr_u32(p, (uint32_t)v); }
static void wr_i16(uint8_t *p, int16_t v) { wr_u16(p, (uint16_t)v); }

static void wr_u24be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

/* 定长 char[n]，不足补 NUL（同管线 _fix_bytes）*/
static void wr_fix(uint8_t *p, const char *s, size_t n)
{
    memset(p, 0, n);
    size_t l = strlen(s);
    memcpy(p, s, l < n ? l : n);
}

/* 稀疏格表一项：{u16 cell; u32 first; u16 count}，first 故意不对齐 */
static void wr_grid(uint8_t *p, uint32_t cell, uint32_t first, uint32_t count)
{
    wr_u16(p, cell); wr_u32(p + 2, first); wr_u16(p + 6, count);
}

/* ── payload 布局（偏移都取整到 0x40，留足富余）────────────────────── */

#define APT_DATA   0x000u   /* 1 条机场（40 B）*/
#define APT_IDX    0x040u   /* 8 + 1×8 + 1×4 */
#define RWY_DATA   0x080u   /* 0 条 */
#define FRQ_DATA   0x0C0u   /* 0 条 */
#define NAV_DATA   0x100u   /* 1 条导航台（32 B）*/
#define NAV_IDX    0x140u
#define FIX_DATA   0x180u   /* 0 条 */
#define FIX_IDX    0x1C0u
#define ASP_DATA   0x200u   /* 2 条空域（48 B）*/
#define ASP_POOL   0x280u   /* 空域段字符串池 */
#define VTX_DATA   0x300u   /* 7 个顶点（4 B）*/
#define AWY_DATA   0x400u   /* 3 条航段（48 B）*/
#define ASPC_DATA  0x600u   /* 空域格展开表（u32）*/
#define ASPC_IDX   0x640u
#define AWYC_DATA  0x680u
#define AWYC_IDX   0x6C0u
#define PAYLOAD_SZ 0x700u

/* 空域段字符串池（rel 0 是"无"哨兵的单个 NUL）*/
#define S_NONE      0u
#define S_ALPHA     1u    /* "ALPHA CTR" */
#define S_ZZZZ      11u   /* "ZZZZ" */
#define ASP_POOL_SZ 16u

/* 两条空域的 1° 格（= bbox 中心代表点，与生成端 grid_cell_u16 同式）：
 *   0 号 bbox lat[10,11] lon[100,101] → (10+90)*360 + (100+180) = 36280
 *   1 号 bbox lat[20,21] lon[120,121] → (20+90)*360 + (120+180) = 39900 */
#define ASP0_CELL 36280u
#define ASP1_CELL 39900u

/* 航段 0（阿留申短弧）落在的三个格，行 = 51+90 = 141：
 *   lon -180 → 列 0、lon -179 → 列 1、lon 179 → 列 359 */
#define AWY_CELL_A (141u * 360u + 0u)     /* 50760 */
#define AWY_CELL_B (141u * 360u + 1u)     /* 50761 */
#define AWY_CELL_C (141u * 360u + 359u)   /* 51119 */

/* ── 段表（按版本截断：前 5 个 = v2，前 9 个 = v3，全 14 个 = v4）──── */

typedef struct {
    uint16_t type, rec_size;
    uint32_t n, data_off, data_size, index_off, index_size;
    uint32_t strings_off, strings_size;
} sec_desc_t;

static const sec_desc_t SECTIONS[14] = {
    /* ---- v2 的 5 个记录段 ---- */
    { PK_AERO_SEC_AIRPORTS,      PK_AERO_AIRPORT_SIZE, 1, APT_DATA, 0x40,
      APT_IDX, 20, 0, 0 },
    { PK_AERO_SEC_RUNWAY_DIRS,   PK_AERO_RWY_DIR_SIZE, 0, RWY_DATA, 0x40,
      0, 0, 0, 0 },
    { PK_AERO_SEC_FREQUENCIES,   PK_AERO_FREQ_SIZE,    0, FRQ_DATA, 0x40,
      0, 0, 0, 0 },
    { PK_AERO_SEC_NAVAIDS,       PK_AERO_NAVAID_SIZE,  1, NAV_DATA, 0x40,
      NAV_IDX, 20, 0, 0 },
    { PK_AERO_SEC_WAYPOINTS_FIX, PK_AERO_FIX_SIZE,     0, FIX_DATA, 0x40,
      FIX_IDX, 16, 0, 0 },
    /* ---- v3 追加的 4 个纯索引段（本测试给空表，只验"能解析、不报错"）---- */
    { PK_AERO_SEC_IDX_AIRPORT_KEY,  PK_AERO_IDX_ENTRY_SIZE, 0, ASPC_DATA, 0,
      0, 0, 0, 0 },
    { PK_AERO_SEC_IDX_AIRPORT_NAME, PK_AERO_IDX_ENTRY_SIZE, 0, ASPC_DATA, 0,
      0, 0, 0, 0 },
    { PK_AERO_SEC_IDX_NAVAID_NAME,  PK_AERO_IDX_ENTRY_SIZE, 0, ASPC_DATA, 0,
      0, 0, 0, 0 },
    { PK_AERO_SEC_IDX_FIX_NAME,     PK_AERO_IDX_ENTRY_SIZE, 0, ASPC_DATA, 0,
      0, 0, 0, 0 },
    /* ---- v4 追加的 5 个段 ---- */
    { PK_AERO_SEC_AIRSPACES,      PK_AERO_AIRSPACE_SIZE,     2, ASP_DATA, 0x80,
      0, 0, ASP_POOL, ASP_POOL_SZ },
    { PK_AERO_SEC_AIRSPACE_VERTS, PK_AERO_AIRSPACE_VTX_SIZE, 7, VTX_DATA, 0x40,
      0, 0, 0, 0 },
    { PK_AERO_SEC_AIRWAYS,        PK_AERO_AIRWAY_SIZE,       3, AWY_DATA, 0x100,
      0, 0, 0, 0 },
    { PK_AERO_SEC_IDX_AIRSPACE_CELL, PK_AERO_IDX_ENTRY_SIZE, 2, ASPC_DATA, 0x40,
      ASPC_IDX, 8 + 2 * 8, 0, 0 },
    { PK_AERO_SEC_IDX_AIRWAY_CELL,   PK_AERO_IDX_ENTRY_SIZE, 3, AWYC_DATA, 0x40,
      AWYC_IDX, 8 + 3 * 8, 0, 0 },
};

#define SEC_COUNT_V2 5
#define SEC_COUNT_V3 9
#define SEC_COUNT_V4 14

static uint8_t g_file[8192];

/* 造一份完整的 bin（header + 段表 + payload）。n_sections 决定"这是第几版"。 */
static size_t build_file(uint16_t version, uint16_t n_sections)
{
    memset(g_file, 0, sizeof(g_file));

    const uint32_t sections_off = PK_AERO_HEADER_SIZE;
    memcpy(g_file, PK_AERO_MAGIC, 6);
    wr_u16(g_file + 6, version);
    memcpy(g_file + 8, "2026-02", 7);
    wr_u16(g_file + 16, n_sections);
    wr_u32(g_file + 18, sections_off);
    g_file[22] = PK_AERO_ENC_NONE;

    for (uint16_t i = 0; i < n_sections; i++) {
        const sec_desc_t *s = &SECTIONS[i];
        uint8_t *e = g_file + sections_off + (size_t)i * PK_AERO_SECTION_SIZE;
        wr_u16(e,      s->type);
        wr_u16(e + 2,  s->rec_size);
        wr_u32(e + 4,  s->n);
        wr_u32(e + 8,  s->data_off);
        wr_u32(e + 12, s->data_size);
        wr_u32(e + 16, s->index_off);
        wr_u32(e + 20, s->index_size);
        wr_u32(e + 24, s->strings_off);
        /* v2 的最后一个 u32 是 _reserved。这里**故意写脏**：读端必须按版本
         * 把它清零，否则顺扫字符串池会拿这个数当池长度，一路读出段外。 */
        wr_u32(e + 28, version >= 3 ? s->strings_size : 0xDEADBEEFu);
    }

    const uint32_t poff = pk_aero_payload_off(g_file, sizeof(g_file));
    uint8_t *p = g_file + poff;

    /* 机场：1 条（字段值无所谓，只要网格索引在）*/
    wr_grid(p + APT_IDX + 8, 36280, 0, 1);
    wr_u32(p + APT_IDX,     1);   /* n_grid */
    wr_u32(p + APT_IDX + 4, 1);   /* n_second */
    wr_u32(p + APT_IDX + 16, 0);  /* 第二索引：记录 0 */

    wr_grid(p + NAV_IDX + 8, 36280, 0, 1);
    wr_u32(p + NAV_IDX,     1);
    wr_u32(p + NAV_IDX + 4, 1);
    wr_u32(p + NAV_IDX + 16, 0);

    wr_u32(p + FIX_IDX,     1);   /* FIX 段空，但索引头要在 */
    wr_u32(p + FIX_IDX + 4, 0);
    wr_grid(p + FIX_IDX + 8, 0, 0, 0);

    if (n_sections < SEC_COUNT_V4) return poff + PAYLOAD_SZ;

    /* ---- 空域段字符串池 ---- */
    memcpy(p + ASP_POOL + S_ALPHA, "ALPHA CTR", 10);
    memcpy(p + ASP_POOL + S_ZZZZ,  "ZZZZ",      5);

    /* ---- 空域 0：bbox lat[10,11] lon[100,101] ---- */
    uint8_t *a0 = p + ASP_DATA;
    wr_i32(a0,      100000000);   /* min_lat 10.0 */
    wr_i32(a0 + 4, 1000000000);   /* min_lon 100.0 */
    wr_i32(a0 + 8,  110000000);   /* max_lat 11.0 */
    wr_i32(a0 + 12, 1010000000);  /* max_lon 101.0 */
    a0[16] = PK_AERO_ASP_TYPE_CTR;
    a0[17] = PK_AERO_ASP_CLASS_D;
    a0[18] = PK_AERO_ALT_REF_GND;
    a0[19] = PK_AERO_ALT_REF_MSL;
    wr_i16(a0 + 20, 0);           /* 下限 0（GND）*/
    wr_i16(a0 + 22, 25);          /* 上限 2500 ft */
    wr_u24be(a0 + 24, S_ALPHA);
    wr_u24be(a0 + 27, S_ZZZZ);
    wr_u24be(a0 + 30, 0);         /* fine  从顶点 0 起 */
    wr_u24be(a0 + 33, 4);         /* coarse 从顶点 4 起 */
    wr_u16(a0 + 36, 4);           /* fine   4 点 */
    wr_u16(a0 + 38, 3);           /* coarse 3 点 */
    wr_u16(a0 + 40, ASP0_CELL);

    /* ---- 空域 1：bbox lat[20,21] lon[120,121]，无名、高度哨兵 ---- */
    uint8_t *a1 = p + ASP_DATA + PK_AERO_AIRSPACE_SIZE;
    wr_i32(a1,      200000000);
    wr_i32(a1 + 4, 1200000000);
    wr_i32(a1 + 8,  210000000);
    wr_i32(a1 + 12, 1210000000);
    a1[16] = PK_AERO_ASP_TYPE_PROHIBITED;
    a1[17] = PK_AERO_ASP_CLASS_NONE;
    wr_i16(a1 + 20, PK_AERO_ALT_NONE);        /* 无下限 */
    wr_i16(a1 + 22, PK_AERO_ALT_UNLIMITED);   /* UNL */
    wr_u24be(a1 + 24, S_NONE);
    wr_u24be(a1 + 27, S_NONE);
    wr_u16(a1 + 40, ASP1_CELL);

    /* ---- 顶点池：0–3 是 0 号的 fine（四角），4–6 是 coarse ---- */
    const uint16_t vx[7][2] = {
        {0, 0}, {65535, 0}, {65535, 65535}, {0, 65535},
        {0, 0}, {32768, 65535}, {65535, 0},
    };
    for (int i = 0; i < 7; i++) {
        wr_u16(p + VTX_DATA + i * 4,     vx[i][0]);
        wr_u16(p + VTX_DATA + i * 4 + 2, vx[i][1]);
    }

    /* ---- 空域格展开索引：cell → 记录下标（表内按 (cell, 下标) 排序）---- */
    wr_u32(p + ASPC_DATA,     0);
    wr_u32(p + ASPC_DATA + 4, 1);
    wr_u32(p + ASPC_IDX,     2);   /* n_grid */
    wr_u32(p + ASPC_IDX + 4, 0);   /* n_second（展开索引不用）*/
    wr_grid(p + ASPC_IDX + 8,     ASP0_CELL, 0, 1);
    wr_grid(p + ASPC_IDX + 8 + 8, ASP1_CELL, 1, 1);

    /* ---- 航段：记录**已按 (designator, seq) 排序**（读端据此直接二分）----
     * 0/1 是 Q188 的两段（阿留申短弧跨 ±180），2 是 W12。 */
    struct { const char *dsg, *si, *ei;
             double slat, slon, elat, elon; uint16_t seq; } awy[3] = {
        { "Q188", "SYA", "ADK",  52.0,  174.06,  51.88, -176.67, 1 },
        { "Q188", "ADK", "AKN",  51.88, -176.67, 58.68, -156.65, 2 },
        { "W12",  "AAA", "BBB",  10.0,  100.0,   11.0,  101.0,   1 },
    };
    for (int i = 0; i < 3; i++) {
        uint8_t *w = p + AWY_DATA + (size_t)i * PK_AERO_AIRWAY_SIZE;
        /* round 而不是截断：管线用的是 Python round()，截断会在负经度上
         * 少一个最低位（-176.67 → -1766699999），断言里就要写一串 9。 */
        wr_i32(w,      (int32_t)lround(awy[i].slat * 1e7));
        wr_i32(w + 4,  (int32_t)lround(awy[i].slon * 1e7));
        wr_i32(w + 8,  (int32_t)lround(awy[i].elat * 1e7));
        wr_i32(w + 12, (int32_t)lround(awy[i].elon * 1e7));
        wr_fix(w + 16, awy[i].dsg, 6);
        wr_fix(w + 22, awy[i].si,  6);
        wr_fix(w + 28, awy[i].ei,  6);
        w[34] = PK_AERO_AWY_TYPE_Q;
        w[35] = PK_AERO_AWY_LEVEL_HIGH;
        wr_u16(w + 36, i == 0 ? 1234 : PK_AERO_TRACK_NONE);   /* 123.4° */
        wr_u16(w + 38, i == 0 ? 1500 : PK_AERO_DIST_NONE);    /* 150.0 nm */
        wr_i16(w + 40, i == 0 ? 180 : PK_AERO_ALT_NONE);      /* FL180 */
        wr_i16(w + 42, PK_AERO_ALT_NONE);
        wr_u16(w + 44, awy[i].seq);
        w[46] = PK_AERO_AWY_DIR_BOTH;
    }

    /* ---- 航路格展开索引：只把航段 0 放进三个格 ---- */
    for (int i = 0; i < 3; i++) wr_u32(p + AWYC_DATA + i * 4, 0);
    wr_u32(p + AWYC_IDX,     3);
    wr_u32(p + AWYC_IDX + 4, 0);
    wr_grid(p + AWYC_IDX + 8,      AWY_CELL_A, 0, 1);
    wr_grid(p + AWYC_IDX + 8 + 8,  AWY_CELL_B, 1, 1);
    wr_grid(p + AWYC_IDX + 8 + 16, AWY_CELL_C, 2, 1);

    return poff + PAYLOAD_SZ;
}

/* ── 用例 ─────────────────────────────────────────────────────────── */

/* 1. 三版都能 init，且各自的 db->version 对；区间外的版本被拒。 */
static void test_version_range(void)
{
    pk_aero_db_t db;
    size_t len;

    len = build_file(2, SEC_COUNT_V2);
    chk_int("v2 init 成功", pk_aero_init(&db, g_file, len, true), PK_AERO_OK);
    chk_int("v2 db->version", db.version, 2);
    /* v2 的 _reserved 被写脏（0xDEADBEEF），读端必须按版本清零 */
    chk_int("v2 strings_size 被清零", (int)db.sec_airport.strings_size, 0);

    len = build_file(3, SEC_COUNT_V3);
    chk_int("v3 init 成功", pk_aero_init(&db, g_file, len, true), PK_AERO_OK);
    chk_int("v3 db->version", db.version, 3);

    len = build_file(4, SEC_COUNT_V4);
    chk_int("v4 init 成功", pk_aero_init(&db, g_file, len, true), PK_AERO_OK);
    chk_int("v4 db->version", db.version, 4);
    chk_int("v4 空域条数", (int)db.sec_airspace.n, 2);
    chk_int("v4 顶点条数", (int)db.sec_asp_vtx.n, 7);
    chk_int("v4 航段条数", (int)db.sec_airway.n, 3);

    len = build_file(1, SEC_COUNT_V2);
    chk_int("v1 被拒", pk_aero_init(&db, g_file, len, true),
            PK_AERO_ERR_VERSION);
    len = build_file(5, SEC_COUNT_V4);
    chk_int("v5 被拒", pk_aero_init(&db, g_file, len, true),
            PK_AERO_ERR_VERSION);
}

/* 2. 老卡（v2/v3）上空域/航路 API 一律优雅退化，不报错、不读野指针。 */
static void test_old_bin_degrades(void)
{
    const uint16_t vers[2]  = { 2, 3 };
    const uint16_t nsecs[2] = { SEC_COUNT_V2, SEC_COUNT_V3 };

    for (int k = 0; k < 2; k++) {
        pk_aero_db_t db;
        char tag[64];
        size_t len = build_file(vers[k], nsecs[k]);
        chk_int("老卡 init 成功", pk_aero_init(&db, g_file, len, true),
                PK_AERO_OK);

        pk_aero_airspace_t a;
        pk_aero_airway_t w;
        pk_aero_lonlat_t ring[8];
        uint32_t ids[8];

        snprintf(tag, sizeof(tag), "v%u airspace_get 返回 false", vers[k]);
        chk_int(tag, pk_aero_airspace_get(&db, 0, &a) ? 1 : 0, 0);
        snprintf(tag, sizeof(tag), "v%u airway_get 返回 false", vers[k]);
        chk_int(tag, pk_aero_airway_get(&db, 0, &w) ? 1 : 0, 0);
        /* ring 的入口是 airspace_get，段缺席 → 参数错（调用方按 <=0 处理）*/
        snprintf(tag, sizeof(tag), "v%u airspace_ring 不返回正数", vers[k]);
        chk_int(tag, pk_aero_airspace_ring(&db, 0, false, ring, 8) > 0 ? 1 : 0, 0);

        snprintf(tag, sizeof(tag), "v%u airspaces_in_cell=0", vers[k]);
        chk_int(tag, pk_aero_airspaces_in_cell(&db, ASP0_CELL, ids, 8), 0);
        snprintf(tag, sizeof(tag), "v%u airways_in_cell=0", vers[k]);
        chk_int(tag, pk_aero_airways_in_cell(&db, AWY_CELL_A, ids, 8), 0);
        snprintf(tag, sizeof(tag), "v%u airspaces_in_bbox=0", vers[k]);
        chk_int(tag, pk_aero_airspaces_in_bbox(&db, 10.2, 100.2, 10.8, 100.8,
                                               ids, 8), 0);
        snprintf(tag, sizeof(tag), "v%u airways_in_bbox=0", vers[k]);
        chk_int(tag, pk_aero_airways_in_bbox(&db, 51.5, -179.5, 52.5, -179.0,
                                             ids, 8), 0);
        snprintf(tag, sizeof(tag), "v%u by_designator=0", vers[k]);
        chk_int(tag, pk_aero_find_airways_by_designator(&db, "Q188", ids, 8), 0);
        snprintf(tag, sizeof(tag), "v%u by_prefix=0", vers[k]);
        chk_int(tag, pk_aero_find_airways_by_prefix(&db, "Q", ids, 8), 0);
    }
}

/* 3. 空域记录字段 + 哨兵。 */
static void test_airspace_record(void)
{
    pk_aero_db_t db;
    size_t len = build_file(4, SEC_COUNT_V4);
    chk_int("init", pk_aero_init(&db, g_file, len, true), PK_AERO_OK);

    pk_aero_airspace_t a;
    chk_true("airspace_get(0)", pk_aero_airspace_get(&db, 0, &a));
    chk_dbl("min_lat", a.min_lat, 10.0);
    chk_dbl("max_lon", a.max_lon, 101.0);
    chk_int("type=CTR", a.type, PK_AERO_ASP_TYPE_CTR);
    chk_int("class=D",  a.cls,  PK_AERO_ASP_CLASS_D);
    chk_int("lower_ref=GND", a.lower_ref, PK_AERO_ALT_REF_GND);
    chk_int("upper_ref=MSL", a.upper_ref, PK_AERO_ALT_REF_MSL);
    chk_int("has_lower", a.has_lower ? 1 : 0, 1);
    chk_int("upper=2500ft", a.upper_100ft, 25);
    chk_str("name", a.name, "ALPHA CTR");
    chk_str("designator", a.designator, "ZZZZ");
    chk_int("grid_cell", (int)a.grid_cell, (int)ASP0_CELL);
    chk_int("fine 顶点数", (int)a.vtx_count_fine, 4);
    chk_int("coarse 顶点数", (int)a.vtx_count_coarse, 3);

    chk_true("airspace_get(1)", pk_aero_airspace_get(&db, 1, &a));
    chk_int("1 号无下限", a.has_lower ? 1 : 0, 0);
    chk_int("1 号有上限（UNL 是个真值，不是哨兵）", a.has_upper ? 1 : 0, 1);
    chk_int("1 号上限 = UNL 编码", a.upper_100ft, PK_AERO_ALT_UNLIMITED);
    chk_str("1 号无名 → 空串", a.name, "");

    chk_int("越界 idx 返回 false", pk_aero_airspace_get(&db, 2, &a) ? 1 : 0, 0);
}

/* 4. 顶点定点还原：lon = min + span * q / 65535。 */
static void test_airspace_ring(void)
{
    pk_aero_db_t db;
    size_t len = build_file(4, SEC_COUNT_V4);
    chk_int("init", pk_aero_init(&db, g_file, len, true), PK_AERO_OK);

    pk_aero_lonlat_t r[8];
    chk_int("fine 点数", pk_aero_airspace_ring(&db, 0, false, r, 8), 4);
    chk_dbl("fine[0].lon", r[0].lon, 100.0);
    chk_dbl("fine[0].lat", r[0].lat, 10.0);
    chk_dbl("fine[1].lon", r[1].lon, 101.0);   /* q=65535 → 满量程 */
    chk_dbl("fine[2].lat", r[2].lat, 11.0);
    chk_dbl("fine[3].lon", r[3].lon, 100.0);

    chk_int("coarse 点数", pk_aero_airspace_ring(&db, 0, true, r, 8), 3);
    chk_dbl("coarse[1].lon 半量程",
            r[1].lon, 100.0 + 1.0 * 32768.0 / PK_AERO_VTX_SCALE_MAX);

    /* max 小于顶点数 → 只写 max 个，不越界写 */
    chk_int("max 截断", pk_aero_airspace_ring(&db, 0, false, r, 2), 2);
    /* 1 号空域没有顶点（count=0）→ 0 点，不是错误 */
    chk_int("无顶点 → 0", pk_aero_airspace_ring(&db, 1, false, r, 8), 0);
}

/* 5. 经度短弧跨度：管线修过的那个真 bug 的回归钉。 */
static void test_airway_lon_span(void)
{
    double lo, hi;

    /* 阿留申 Q188：SYA 174.06°E → ADK 176.67°W。短弧只有一两百海里，
     * 从 174.06 往东跨过 180° 就到；朴素 min/max 会给 [-176.67, 174.06]
     * 这个横跨大半个地球的假区间。返回值**不夹回** ±180。 */
    pk_aero_airway_lon_span(174.06, -176.67, &lo, &hi);
    chk_dbl("跨线段 lo", lo, 174.06);
    chk_dbl("跨线段 hi", hi, 183.33);

    /* 反向同一条段，区间必须一样（两端点顺序不影响几何）*/
    pk_aero_airway_lon_span(-176.67, 174.06, &lo, &hi);
    chk_dbl("反向 lo", lo, -185.94);
    chk_dbl("反向 hi", hi, -176.67);

    /* 普通段：短弧就是 min/max 本身 */
    pk_aero_airway_lon_span(100.0, 101.0, &lo, &hi);
    chk_dbl("普通段 lo", lo, 100.0);
    chk_dbl("普通段 hi", hi, 101.0);
    pk_aero_airway_lon_span(101.0, 100.0, &lo, &hi);
    chk_dbl("普通段反向 lo", lo, 100.0);
    chk_dbl("普通段反向 hi", hi, 101.0);

    /* 正好 180° 的边界：dlon == 180 不做平移（> 才平移），保持 min/max */
    pk_aero_airway_lon_span(-90.0, 90.0, &lo, &hi);
    chk_dbl("正好 180 lo", lo, -90.0);
    chk_dbl("正好 180 hi", hi, 90.0);
}

/* 6. 航段记录 + designator 二分 + 前缀枚举。 */
static void test_airway_record(void)
{
    pk_aero_db_t db;
    size_t len = build_file(4, SEC_COUNT_V4);
    chk_int("init", pk_aero_init(&db, g_file, len, true), PK_AERO_OK);

    pk_aero_airway_t w;
    chk_true("airway_get(0)", pk_aero_airway_get(&db, 0, &w));
    chk_str("designator", w.designator, "Q188");
    chk_str("start_ident", w.start_ident, "SYA");
    chk_str("end_ident",   w.end_ident,   "ADK");
    chk_dbl("start_lon", w.start_lon, 174.06);
    chk_dbl("end_lon",   w.end_lon,  -176.67);
    chk_int("type=Q",  w.type,  PK_AERO_AWY_TYPE_Q);
    chk_int("level=H", w.level, PK_AERO_AWY_LEVEL_HIGH);
    chk_int("dir=BOTH", w.direction, PK_AERO_AWY_DIR_BOTH);
    chk_int("has_mag_track", w.has_mag_track ? 1 : 0, 1);
    chk_int("mag_track 0.1deg", (int)w.mag_track_dd, 1234);
    chk_int("has_distance", w.has_distance ? 1 : 0, 1);
    chk_int("min_alt FL180", w.min_alt_100ft, 180);
    chk_int("无上限高度", w.has_max_alt ? 1 : 0, 0);
    chk_int("seq", (int)w.seq, 1);

    chk_true("airway_get(2)", pk_aero_airway_get(&db, 2, &w));
    chk_int("2 号无磁航迹", w.has_mag_track ? 1 : 0, 0);
    chk_int("2 号无距离",   w.has_distance  ? 1 : 0, 0);
    chk_int("越界返回 false", pk_aero_airway_get(&db, 3, &w) ? 1 : 0, 0);

    uint32_t ids[8];
    chk_int("Q188 两段", pk_aero_find_airways_by_designator(&db, "Q188", ids, 8), 2);
    chk_int("Q188[0]", (int)ids[0], 0);
    chk_int("Q188[1]", (int)ids[1], 1);
    chk_int("小写同样命中（reader 自己 toupper）",
            pk_aero_find_airways_by_designator(&db, "q188", ids, 8), 2);
    chk_int("W12 一段", pk_aero_find_airways_by_designator(&db, "W12", ids, 8), 1);
    chk_int("W12[0]", (int)ids[0], 2);
    chk_int("不存在的航路", pk_aero_find_airways_by_designator(&db, "ZZ9", ids, 8), 0);
    /* max 小于同名条数：只写前 max 个，但返回**总数**（同 fix_by_ident）*/
    chk_int("max=1 时仍返回总数", pk_aero_find_airways_by_designator(&db, "Q188", ids, 1), 2);

    chk_int("前缀 Q", pk_aero_find_airways_by_prefix(&db, "Q", ids, 8), 2);
    chk_int("前缀 W", pk_aero_find_airways_by_prefix(&db, "W", ids, 8), 1);
    chk_int("前缀 Q188", pk_aero_find_airways_by_prefix(&db, "Q188", ids, 8), 2);
    chk_int("前缀无命中", pk_aero_find_airways_by_prefix(&db, "X", ids, 8), 0);
}

/* 7. 格 / bbox 空间查询，含跨 ±180 的约定。 */
static void test_spatial_queries(void)
{
    pk_aero_db_t db;
    size_t len = build_file(4, SEC_COUNT_V4);
    chk_int("init", pk_aero_init(&db, g_file, len, true), PK_AERO_OK);

    uint32_t ids[8];

    /* 单格 */
    chk_int("空域格 0", pk_aero_airspaces_in_cell(&db, ASP0_CELL, ids, 8), 1);
    chk_int("空域格 0 内容", (int)ids[0], 0);
    chk_int("空域格 1", pk_aero_airspaces_in_cell(&db, ASP1_CELL, ids, 8), 1);
    chk_int("空域格 1 内容", (int)ids[0], 1);
    chk_int("空格无结果", pk_aero_airspaces_in_cell(&db, 1, ids, 8), 0);

    /* bbox：只该命中 0 号（1 号在两千公里外）*/
    chk_int("bbox 命中 1 条",
            pk_aero_airspaces_in_bbox(&db, 10.2, 100.2, 10.8, 100.8, ids, 8), 1);
    chk_int("bbox 命中的是 0 号", (int)ids[0], 0);

    /* 视野落在格里但与记录 bbox 不相交 → 格展开取到候选、精筛把它筛掉。
     * 空域 1 的 bbox 是 lat[20,21]，这里查 lat[20,21] lon[121.5,121.8]
     * ——同一格（floor(121.5)=121 → 与 max_lon 121.0 不相交）。 */
    chk_int("精筛把不相交的候选筛掉",
            pk_aero_airspaces_in_bbox(&db, 20.2, 121.5, 20.8, 121.8, ids, 8), 0);

    /* 跨 ±180 的视野：约定要求 min_lon <= max_lon，违约是**错误**而不是空结果
     * ——静默返回 0 会让绘制端以为"那边真没东西"。 */
    chk_int("min_lon > max_lon → ERR_ARG",
            pk_aero_airspaces_in_bbox(&db, 10.0, 179.0, 11.0, -179.0, ids, 8),
            PK_AERO_ERR_ARG);
    chk_int("航路同一约定",
            pk_aero_airways_in_bbox(&db, 10.0, 179.0, 11.0, -179.0, ids, 8),
            PK_AERO_ERR_ARG);

    /* 纬度反了不是错误，是空结果（同 Python 的空 range）*/
    chk_int("纬度反了 → 0",
            pk_aero_airspaces_in_bbox(&db, 11.0, 100.2, 10.0, 100.8, ids, 8), 0);

    /* 航段 0 的**真实位置**（180° 东侧一点点）：短弧口径下必须查得到。
     * 朴素 min/max 口径会在这里漏掉——这正是管线修过的那个 bug 的表现面。 */
    chk_int("阿留申段：真实位置查得到",
            pk_aero_airways_in_bbox(&db, 51.5, -179.5, 52.5, -179.0, ids, 8), 1);
    chk_int("命中的是航段 0", (int)ids[0], 0);
    /* 同一条段被三个格同时命中，结果必须去重（bbox 展开会重复取到）*/
    chk_int("多格命中同一条只算一次",
            pk_aero_airways_in_bbox(&db, 51.5, -180.0, 52.5, -179.0, ids, 8), 1);
}

int main(void)
{
    test_version_range();
    test_old_bin_degrades();
    test_airspace_record();
    test_airspace_ring();
    test_airway_lon_span();
    test_airway_record();
    test_spatial_queries();

    if (g_fail) { printf("test_aero_v4: %d 项失败\n", g_fail); return 1; }
    printf("test_aero_v4: 全部通过\n");
    return 0;
}
