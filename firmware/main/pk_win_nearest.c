/*
 * pk_win_nearest.c — 见 pk_win_nearest.h。
 *
 * 与 pk_aero_reader.c 的 nearest_generic 同属"3×3 格内找最近"算法，但数据
 * 来源不同（全量库 payload vs 窗口驻留格），且**省掉 approx 粗排阶段**：
 * nearest_generic 用 approx 是因为全量库单格可能上千条、Haversine 量大；
 * 窗口驻留的是视口内几个格、每格几十条，直接全精算 Haversine 更快更简单，
 * 也不需要"粗排存 idx → 精算重读记录"那条窗口没有的随机访问路径。
 *
 * 与全量 nearest_generic 的结果一致性靠这个保证：两者扫的格集合相同时，
 * 每条候选都算精确 Haversine、同一个 insert_near 有序表 → 输出逐条一致。
 * （approx 只是 nearest_generic 的性能优化，APPROX 模式最终也会精算重排，
 * 结果与全精算一致——所以对拍时全量侧用 EXACT 模式即可。）
 *
 * 故意保留两份算法而不是抽公共函数：nearest_generic 在全量库热路径上，动它
 * 的签名会牵动所有 nearest_* 调用，W1.4 先隔离验证，去重留到 W 系列收尾。
 */
#include "pk_win_nearest.h"
#include "geo.h"

#include <math.h>

/* insert_near 与 pk_aero_reader.c 逐字相同（有序插入，键 (dist,idx) 保确定性）。
 * 加 wn_ 前缀避免与 reader.c 的同名 static 在对拍测试（同时 include 两份 .c）里
 * 冲突——逻辑完全一致，去重留到 W 系列收尾。 */
static void wn_insert_near(pk_aero_near_t *arr, int *n, int cap,
                        uint32_t idx, double dist_nm, double brg_deg)
{
    if (*n == cap) {
        const pk_aero_near_t *last = &arr[cap - 1];
        if (dist_nm > last->dist_nm ||
            (dist_nm == last->dist_nm && idx >= last->idx))
            return;
        (*n)--;
    }
    int i = *n;
    while (i > 0 && (arr[i - 1].dist_nm > dist_nm ||
                     (arr[i - 1].dist_nm == dist_nm && arr[i - 1].idx > idx))) {
        arr[i] = arr[i - 1];
        i--;
    }
    arr[i].idx     = idx;
    arr[i].dist_nm = dist_nm;
    arr[i].brg_deg = brg_deg;
    (*n)++;
}

static inline int32_t wn_rd_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

int pk_win_nearest_compute(const uint16_t *cells, int n_cells, uint16_t rec_size,
                   double lat, double lon,
                   pk_win_rec_fn rec_fn, void *ctx,
                   pk_aero_near_t *out, int max)
{
    if (max <= 0 || out == NULL || rec_fn == NULL) return 0;
    if (max > PK_AERO_NEAR_MAX) max = PK_AERO_NEAR_MAX;

    int n_sel = 0;
    for (int ci = 0; ci < n_cells; ci++) {
        const uint8_t *recs = NULL;
        uint32_t n = 0, first = 0;
        if (!rec_fn(cells[ci], &recs, &n, &first, ctx) || n == 0) continue;

        for (uint32_t k = 0; k < n; k++) {
            const uint8_t *r = recs + (size_t)k * rec_size;
            int32_t lat_e7 = wn_rd_i32(r);
            int32_t lon_e7 = wn_rd_i32(r + 4);
            if (lat_e7 == (int32_t)PK_AERO_COORD_NONE) continue;
            double rlat = lat_e7 / 1e7, rlon = lon_e7 / 1e7;
            double dist_nm, brg_deg;
            geo_dist_brg(lat, lon, rlat, rlon, &dist_nm, &brg_deg);
            wn_insert_near(out, &n_sel, max, first + k, dist_nm, brg_deg);
        }
    }
    return n_sel;
}
