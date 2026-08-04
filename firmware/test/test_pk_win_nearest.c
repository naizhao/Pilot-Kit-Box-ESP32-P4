/* test_pk_win_nearest.c — host 对拍：窗口 nearest vs 全量 nearest_generic。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_wn \
 *      firmware/test/test_pk_win_nearest.c \
 *      firmware/main/pk_win_nearest.c firmware/main/geo.c && /tmp/test_wn
 *
 *   ASan：
 *   cc -std=c11 -Wall -Wextra -O0 -g -fsanitize=address -I firmware/main \
 *      -o /tmp/test_wn_asan firmware/test/test_pk_win_nearest.c \
 *      firmware/main/pk_win_nearest.c firmware/main/geo.c && /tmp/test_wn_asan
 *
 * 对拍原理：合成一份机场记录铺到多个 1° 格，每个格有自己的 (first,count)。
 *   - 全量 pk_aero_nearest_airports(EXACT)：扫 query 的 3×3 格，从 payload 取记录；
 *   - 窗口 pk_win_nearest：扫同样的 3×3 格，从回调取记录（回调按 cell 返回该格的
 *     recs/first/count，数据指向同一份 payload）。
 * 两者扫的格相同、读的记录字节相同、都用 Haversine → 输出必须逐条一致
 * (idx / dist_nm / brg_deg)。这就是 W1.4 "窗口 nearest 与全量等价" 的验证基准。
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../main/pk_aero_reader.c"
#include "pk_win_nearest.h"   /* 声明；实现由命令行单独编译链接 pk_win_nearest.c */

static int g_fail = 0;

static void chk_true(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static void chk_int(const char *what, int got, int want)
{
    bool ok = got == want;
    printf("  [%s] %-42s got=%d want=%d\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) g_fail++;
}

/* ---- 合成数据：rec_size=12，前 8 字节 lat_e7/lon_e7 ---- */
#define REC_SIZE 12
#define MAX_RECS 128
#define MAX_CELLS 16

/* payload 布局：[记录区 MAX_RECS*REC_SIZE][grid 表区 MAX_CELLS*8]。
 * grid_off = MAX_RECS*REC_SIZE，让全量 grid_lookup 的 db->payload+grid_off 落在
 * 同一个缓冲区内的 grid 表上（grid_lookup 要求 grid 表在 payload 内）。 */
static uint8_t s_payload[MAX_RECS * REC_SIZE + MAX_CELLS * 8];
#define GRID_OFF  (MAX_RECS * REC_SIZE)
static uint8_t *s_grid = s_payload + GRID_OFF;
static uint32_t s_n_rec;

static void put_rec(uint32_t k, double lat, double lon)
{
    int32_t le = (int32_t)(lat * 1e7);
    int32_t ln = (int32_t)(lon * 1e7);
    uint8_t *r = s_payload + (size_t)k * REC_SIZE;
    r[0]=le&0xFF; r[1]=(le>>8)&0xFF; r[2]=(le>>16)&0xFF; r[3]=(le>>24)&0xFF;
    r[4]=ln&0xFF; r[5]=(ln>>8)&0xFF; r[6]=(ln>>16)&0xFF; r[7]=(ln>>24)&0xFF;
}

/* 格→(first,count) 映射，供窗口回调和全量 grid 表共用 */
typedef struct { uint16_t cell; uint32_t first; uint16_t count; } cell_map_t;
static cell_map_t s_map[MAX_CELLS];
static int s_n_map;

static const cell_map_t *map_find(uint16_t cell)
{
    for (int i = 0; i < s_n_map; i++)
        if (s_map[i].cell == cell) return &s_map[i];
    return NULL;
}

/* 窗口回调：按 cell 从 map 查 (first,count)，recs 指向 payload */
static bool test_rec_fn(uint16_t cell, const uint8_t **recs,
                        uint32_t *n, uint32_t *first, void *ctx)
{
    (void)ctx;
    const cell_map_t *m = map_find(cell);
    if (!m || m->count == 0) return false;
    if (recs)  *recs  = s_payload + (size_t)m->first * REC_SIZE;
    if (n)     *n     = m->count;
    if (first) *first = m->first;
    return true;
}

/* grid 表 8 字节项 [cell u16 @0][first u32 @2][count u16 @6]，按 cell 升序 */
static void build_grid(void)
{
    for (int i = 0; i < s_n_map; i++) {
        uint8_t *e = s_grid + (size_t)i * 8;
        e[0]=s_map[i].cell&0xFF; e[1]=(s_map[i].cell>>8)&0xFF;
        e[2]=s_map[i].first&0xFF; e[3]=(s_map[i].first>>8)&0xFF;
        e[4]=(s_map[i].first>>16)&0xFF; e[5]=(s_map[i].first>>24)&0xFF;
        e[6]=s_map[i].count&0xFF; e[7]=(s_map[i].count>>8)&0xFF;
    }
}

/* 算 query 的 3×3 格集合 */
static int make_3x3(double lat, double lon, uint16_t *out)
{
    int row0 = (int)floor(lat) + 90;
    if (row0 < 0) row0 = 0;
    if (row0 > 179) row0 = 179;
    int col0 = ((int)floor(lon) + 180) % 360;
    if (col0 < 0) col0 += 360;
    int nc = 0;
    for (int dr = -1; dr <= 1; dr++) {
        int row = row0 + dr;
        if (row < 0 || row > 179) continue;
        for (int dc = -1; dc <= 1; dc++) {
            int col = (col0 + dc + 360) % 360;
            out[nc++] = (uint16_t)(row * 360 + col);
        }
    }
    return nc;
}

static void compare_one(const char *label, double qlat, double qlon,
                        pk_aero_db_t *db)
{
    pk_aero_near_t ref[PK_AERO_NEAR_MAX];
    pk_aero_near_t win[PK_AERO_NEAR_MAX];
    int n_ref = pk_aero_nearest_airports(db, qlat, qlon, ref, PK_AERO_NEAR_MAX,
                                         PK_AERO_DIST_HAVERSINE);
    uint16_t cells[9];
    int nc = make_3x3(qlat, qlon, cells);
    int n_win = pk_win_nearest_compute(cells, nc, REC_SIZE, qlat, qlon,
                               test_rec_fn, NULL, win, PK_AERO_NEAR_MAX);

    printf("-- %s (lat=%.2f lon=%.2f): 全量=%d 窗口=%d --\n",
           label, qlat, qlon, n_ref, n_win);

    /* 核心：两边扫同样的 3×3 格、读同样的记录 → 条数与逐条内容必须一致 */
    chk_int("全量 == 窗口 条数", n_ref, n_win);
    int n = n_ref < n_win ? n_ref : n_win;
    bool all_match = true;
    for (int i = 0; i < n; i++) {
        if (ref[i].idx != win[i].idx) { all_match = false; break; }
        if (fabs(ref[i].dist_nm - win[i].dist_nm) > 1e-9) { all_match = false; break; }
        if (fabs(ref[i].brg_deg - win[i].brg_deg) > 1e-6) { all_match = false; break; }
    }
    chk_true("逐条一致 (idx/dist/brg)", all_match);
}

int main(void)
{
    /* 铺记录到 3 个相邻格。s_map 必须按 cell 升序（grid_lookup 是二分）。
     * 40°N 116°E → row=130 col=296；格号 = row*360+col。
     * 升序：46736(129,296) < 47096(130,296) < 47097(130,297) */
    /* 格 (129,296)=46736（南邻）：2 条 */
    s_map[0] = (cell_map_t){46736, 3, 2};
    put_rec(3, 39.11, 117.18);   /* 天津 */
    put_rec(4, 39.50, 116.38);
    /* 格 (130,296)=47096：3 条 */
    s_map[1] = (cell_map_t){47096, 0, 3};
    put_rec(0, 40.07, 116.58);   /* 首都 */
    put_rec(1, 39.91, 116.60);   /* 南苑 */
    put_rec(2, 40.10, 116.30);   /* 八达岭 */
    /* 格 (130,297)=47097（东邻）：1 条 */
    s_map[2] = (cell_map_t){47097, 5, 1};
    put_rec(5, 40.05, 117.10);
    s_n_map = 3;
    s_n_rec = 6;
    build_grid();

    pk_aero_db_t db;
    memset(&db, 0, sizeof(db));
    db.payload = s_payload;
    db.sec_airport.type = PK_AERO_SEC_AIRPORTS;
    db.sec_airport.rec_size = REC_SIZE;
    db.sec_airport.n = s_n_rec;
    db.sec_airport.data_off = 0;
    db.apt_idx.n_grid = s_n_map;
    db.apt_idx.grid_off = GRID_OFF;

    compare_one("北京区域 query=40.05,116.60", 40.05, 116.60, &db);
    compare_one("query 偏南 39.60,116.80",     39.60, 116.80, &db);
    compare_one("query 压格边界 40.00,117.00",  40.00, 117.00, &db);

    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
