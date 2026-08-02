/*
 * pk_win_geom.c — 实现说明见 pk_win_geom.h。
 */
#include "pk_win_geom.h"

#include <math.h>
#include <string.h>

#include "pk_aero_reader.h"   /* pk_aero_grid_cell()：唯一一套格编码 */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD (M_PI / 180.0)

/* ------------------------------------------------------------------ */
/* 形状                                                                */
/* ------------------------------------------------------------------ */

void pk_win_shape_ellipse(pk_win_shape_t *s, double lat, double lon,
                          double track_deg)
{
    if (s == NULL) return;
    s->lat       = lat;
    s->lon       = lon;
    s->track_deg = track_deg;
    s->a_fwd     = PK_WIN_A_FWD_NM;
    s->a_aft     = PK_WIN_A_AFT_NM;
    s->b         = PK_WIN_B_NM;
    s->circle    = false;
}

void pk_win_shape_circle(pk_win_shape_t *s, double lat, double lon,
                         double radius_nm)
{
    if (s == NULL) return;
    if (!(radius_nm > 0.0)) radius_nm = PK_WIN_CIRCLE_NM;
    s->lat       = lat;
    s->lon       = lon;
    s->track_deg = 0.0;
    s->a_fwd     = radius_nm;
    s->a_aft     = radius_nm;
    s->b         = radius_nm;
    s->circle    = true;
}

void pk_win_shape_grow(pk_win_shape_t *s, double k)
{
    if (s == NULL || !(k > 0.0)) return;
    s->a_fwd *= k;
    s->a_aft *= k;
    s->b     *= k;
}

/* 中心 → 目标点的**局部平面位移**（海里）。
 * 等距柱状近似：经度差乘中心纬度的 cos。窗口最大 130 NM（含 1.3× 卸载环），
 * 这个尺度上等距柱状与真球面的误差是千分之几海里量级，而判据本身是
 * "格相不相交"这种粗粒度问题——用 Haversine 只是把 1 Hz 的成本抬上去。
 * 经度差先归一到 [-180, 180]，跨 ±180 自然成立。 */
static void local_offset_nm(double lat0, double lon0, double lat, double lon,
                            double *dx_nm, double *dy_nm)
{
    double dlon = lon - lon0;
    while (dlon > 180.0)  dlon -= 360.0;
    while (dlon < -180.0) dlon += 360.0;
    double cosf_ = cos(lat0 * DEG2RAD);
    if (cosf_ < 1e-6) cosf_ = 1e-6;
    *dx_nm = dlon * 60.0 * cosf_;   /* 东为正 */
    *dy_nm = (lat - lat0) * 60.0;   /* 北为正 */
}

bool pk_win_shape_contains(const pk_win_shape_t *s, double lat, double lon)
{
    if (s == NULL) return false;
    double dx, dy;
    local_offset_nm(s->lat, s->lon, lat, lon, &dx, &dy);

    double x, y;
    if (s->circle) {
        x = dy;
        y = dx;
    } else {
        /* 航迹方向单位矢量 = (sin trk, cos trk)（东, 北）；右侧向 = (cos, -sin） */
        const double t = s->track_deg * DEG2RAD;
        const double st = sin(t), ct = cos(t);
        x = dx * st + dy * ct;    /* 沿航迹（前为正） */
        y = dx * ct - dy * st;    /* 右侧向 */
    }

    const double a = (x >= 0.0) ? s->a_fwd : s->a_aft;
    if (a <= 0.0 || s->b <= 0.0) return false;
    const double u = x / a, v = y / s->b;
    return (u * u + v * v) <= 1.0;
}

/* ------------------------------------------------------------------ */
/* 集合                                                                */
/* ------------------------------------------------------------------ */

void pk_win_cellset_clear(pk_win_cellset_t *s)
{
    if (s == NULL) return;
    s->n = 0;
    s->truncated = false;
}

/* 升序数组的 lower_bound */
static int cellset_lower_bound(const pk_win_cellset_t *s, uint16_t cell)
{
    int lo = 0, hi = (int)s->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->cell[mid] < cell) lo = mid + 1;
        else                     hi = mid;
    }
    return lo;
}

bool pk_win_cellset_has(const pk_win_cellset_t *s, uint16_t cell)
{
    if (s == NULL || s->n == 0) return false;
    int i = cellset_lower_bound(s, cell);
    return i < (int)s->n && s->cell[i] == cell;
}

bool pk_win_cellset_add(pk_win_cellset_t *s, uint16_t cell)
{
    if (s == NULL) return false;
    int i = cellset_lower_bound(s, cell);
    if (i < (int)s->n && s->cell[i] == cell) return true;   /* 已在集合内 */
    if (s->n >= PK_WIN_MAX_CELLS) return false;
    memmove(&s->cell[i + 1], &s->cell[i],
            (size_t)((int)s->n - i) * sizeof(s->cell[0]));
    s->cell[i] = cell;
    s->n++;
    return true;
}

int pk_win_cellset_diff(const pk_win_cellset_t *a, const pk_win_cellset_t *b,
                        pk_win_cellset_t *out)
{
    if (a == NULL || b == NULL || out == NULL) return 0;
    /* out 可能与 a 同一块内存，所以先攒到局部再写回 */
    uint16_t tmp[PK_WIN_MAX_CELLS];
    int n = 0;
    int i = 0, j = 0;
    while (i < (int)a->n) {
        while (j < (int)b->n && b->cell[j] < a->cell[i]) j++;
        if (j < (int)b->n && b->cell[j] == a->cell[i]) { i++; continue; }
        tmp[n++] = a->cell[i++];
    }
    memcpy(out->cell, tmp, (size_t)n * sizeof(tmp[0]));
    out->n = (uint8_t)n;
    out->truncated = false;
    return n;
}

int pk_win_cellset_union(pk_win_cellset_t *dst, const pk_win_cellset_t *src)
{
    if (dst == NULL || src == NULL) return dst ? (int)dst->n : 0;
    for (int i = 0; i < (int)src->n; i++) {
        if (!pk_win_cellset_add(dst, src->cell[i])) dst->truncated = true;
    }
    return (int)dst->n;
}

void pk_win_cell_sw_corner(uint16_t cell, double *out_lat, double *out_lon)
{
    double la = 0.0, lo = 0.0;
    if (cell < 64800u) {
        la = (double)(cell / 360u) - 90.0;
        lo = (double)(cell % 360u) - 180.0;
    }
    if (out_lat) *out_lat = la;
    if (out_lon) *out_lon = lo;
}

double pk_win_cell_dist_nm(uint16_t cell, double lat, double lon)
{
    double clat, clon;
    pk_win_cell_sw_corner(cell, &clat, &clon);

    /* 纬度：点到 [clat, clat+1] 的距离 */
    double dlat = 0.0;
    if (lat < clat)            dlat = clat - lat;
    else if (lat > clat + 1.0) dlat = lat - (clat + 1.0);

    /* 经度：先把 lon 归一到格附近再算，跨 ±180 自然成立 */
    double nlon = lon;
    while (nlon - clon >= 180.0)  nlon -= 360.0;
    while (nlon - clon <  -180.0) nlon += 360.0;
    double dlon = 0.0;
    if (nlon < clon)            dlon = clon - nlon;
    else if (nlon > clon + 1.0) dlon = nlon - (clon + 1.0);

    /* 经度差乘 cos：用格与点里**纬度绝对值大的那个**，得到偏保守（偏小）
     * 的距离——R1/R2 的分界宁可早一点判成"紧急"，也不要晚。 */
    double phi = fabs(lat);
    double cphi = fabs(clat) > fabs(clat + 1.0) ? fabs(clat) : fabs(clat + 1.0);
    if (cphi > phi) phi = cphi;
    if (phi > 89.5) phi = 89.5;

    const double x = dlon * 60.0 * cos(phi * DEG2RAD);
    const double y = dlat * 60.0;
    return sqrt(x * x + y * y);
}

/* ------------------------------------------------------------------ */
/* 求交                                                                */
/* ------------------------------------------------------------------ */

/* 候选表：按"到窗口中心的最近采样点距离"排序保留最近的 PK_WIN_MAX_CELLS 个。
 * 为什么不是简单地取升序前 48 个：cell 升序 = 纬度优先，高纬截断会把北边
 * 一整片留下、把脚底下的格丢掉，正好丢反。 */
typedef struct {
    uint16_t cell;
    double   dist2;   /* 平方距离（海里²），只用于排序，不开方 */
} cand_t;

static void cand_offer(cand_t *c, int *n, uint16_t cell, double dist2)
{
    for (int i = 0; i < *n; i++) {
        if (c[i].cell == cell) {
            if (dist2 < c[i].dist2) c[i].dist2 = dist2;
            return;
        }
    }
    if (*n < PK_WIN_MAX_CELLS) {
        c[*n].cell = cell;
        c[*n].dist2 = dist2;
        (*n)++;
        return;
    }
    /* 满了：挤掉当前最远的那个（若新来的更近） */
    int worst = 0;
    for (int i = 1; i < *n; i++) if (c[i].dist2 > c[worst].dist2) worst = i;
    if (dist2 < c[worst].dist2) {
        c[worst].cell  = cell;
        c[worst].dist2 = dist2;
    }
}

/* 1° 格 [lat_i, lat_i+1] × [lon_i, lon_i+1] 与窗口是否相交（5×5 采样 +
 * 中心落格兜底），相交时同时给出最近采样点到中心的平方距离。 */
static bool cell_hits(const pk_win_shape_t *s, int lat_i, int lon_i,
                      double *out_dist2)
{
    static const double f[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    bool hit = false;
    double best = 1e30;

    for (int a = 0; a < 5; a++) {
        const double la = (double)lat_i + f[a];
        for (int b = 0; b < 5; b++) {
            const double lo = (double)lon_i + f[b];
            double dx, dy;
            local_offset_nm(s->lat, s->lon, la, lo, &dx, &dy);
            const double d2 = dx * dx + dy * dy;
            if (d2 < best) best = d2;
            if (!hit && pk_win_shape_contains(s, la, lo)) hit = true;
        }
    }
    /* 防御性兜底：椭圆完全被格包住时 25 个采样点全在外面。本设计里
     * 1° 格 ≥ 43 NM 而短半轴 50 NM，理论上不会发生；但窗口一旦被调小
     * （或将来支持更小的圆）就会——所以这一步留着。 */
    if (!hit) {
        double clat = s->lat, clon = s->lon;
        double nlon = clon;
        while (nlon - (double)lon_i >= 360.0) nlon -= 360.0;
        while (nlon - (double)lon_i < 0.0)    nlon += 360.0;
        if (clat >= (double)lat_i && clat < (double)lat_i + 1.0 &&
            nlon >= (double)lon_i && nlon < (double)lon_i + 1.0) {
            hit = true;
            best = 0.0;
        }
    }
    if (hit && out_dist2) *out_dist2 = best;
    return hit;
}

int pk_win_cells(const pk_win_shape_t *s, pk_win_cellset_t *out)
{
    if (out == NULL) return 0;
    pk_win_cellset_clear(out);
    if (s == NULL) return 0;

    double R = s->a_fwd;
    if (s->a_aft > R) R = s->a_aft;
    if (s->b     > R) R = s->b;
    if (!(R > 0.0)) return 0;

    /* 1. 外接盒。纬度直接钳位到 [-90, 90]；经度用 cos(|lat|+1°) 保守放大
     *    （文档 §1.2 的"高纬用 cos(|lat|+1°) 保守放大，避免漏格"）。 */
    const double dlat = R / 60.0;
    double lat_lo = s->lat - dlat, lat_hi = s->lat + dlat;
    if (lat_lo < -90.0) lat_lo = -90.0;
    if (lat_hi >  90.0) lat_hi =  90.0;

    double phi = fabs(s->lat) + 1.0;
    if (phi > 89.5) phi = 89.5;
    double dlon = R / (60.0 * cos(phi * DEG2RAD));
    if (!(dlon < 180.0)) dlon = 180.0;   /* NaN/极区 → 全经度圈 */

    int row_lo = (int)floor(lat_lo);
    int row_hi = (int)floor(lat_hi);
    if (row_lo < -90) row_lo = -90;
    if (row_hi >  89) row_hi =  89;

    int col_lo = (int)floor(s->lon - dlon);
    int col_hi = (int)floor(s->lon + dlon);
    if (col_hi - col_lo > 359) { col_lo = -180; col_hi = 179; }

    /* 2. 逐格精筛 */
    cand_t cand[PK_WIN_MAX_CELLS];
    int ncand = 0;
    for (int r = row_lo; r <= row_hi; r++) {
        for (int c = col_lo; c <= col_hi; c++) {
            double d2 = 0.0;
            if (!cell_hits(s, r, c, &d2)) continue;
            /* 格编码走 pk_aero_grid_cell：行钳位 + 列环绕，与生成端同一套 */
            const uint16_t cell = pk_aero_grid_cell((double)r + 0.5,
                                                    (double)c + 0.5);
            cand_offer(cand, &ncand, cell, d2);
            if (ncand == PK_WIN_MAX_CELLS) out->truncated = true;
        }
    }

    /* 3. 升序输出 */
    for (int i = 0; i < ncand; i++) pk_win_cellset_add(out, cand[i].cell);
    /* truncated 只在真的挤掉过东西时才有意义：候选恰好等于容量不算截断。
     * cand_offer 满了之后仍可能只是"每次都比最远的远"从而无变化，这里
     * 用最终条数是否触顶来表述，语义是"可能不完整"，够诊断用。 */
    out->truncated = (out->n >= PK_WIN_MAX_CELLS);
    return (int)out->n;
}

int pk_win_cells_bbox(double min_lat, double min_lon,
                      double max_lat, double max_lon,
                      pk_win_cellset_t *out)
{
    if (out == NULL) return 0;
    pk_win_cellset_clear(out);
    if (min_lat > max_lat) return 0;

    if (min_lat < -90.0) min_lat = -90.0;
    if (max_lat >  90.0) max_lat =  90.0;

    int row_lo = (int)floor(min_lat);
    int row_hi = (int)floor(max_lat);
    if (row_lo < -90) row_lo = -90;
    if (row_hi >  89) row_hi =  89;

    /* min_lon > max_lon = 跨 ±180：把上界 +360 展开成连续区间，
     * 格编码那一步的取模会把它绕回来。 */
    if (max_lon < min_lon) max_lon += 360.0;
    int col_lo = (int)floor(min_lon);
    int col_hi = (int)floor(max_lon);
    if (col_hi - col_lo > 359) { col_lo = -180; col_hi = 179; }

    for (int r = row_lo; r <= row_hi; r++) {
        for (int c = col_lo; c <= col_hi; c++) {
            const uint16_t cell = pk_aero_grid_cell((double)r + 0.5,
                                                    (double)c + 0.5);
            if (!pk_win_cellset_add(out, cell)) out->truncated = true;
        }
    }
    return (int)out->n;
}
