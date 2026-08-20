/* test_pk_win_geom.c — host proof for pk_win_geom（窗口几何 + 格集合差分）。
 *
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_win \
 *      firmware/test/test_pk_win_geom.c firmware/main/pk_win_geom.c \
 *      firmware/main/pk_aero_reader.c firmware/main/geo.c -lm && /tmp/test_win
 *
 * （geo.c 不能省：pk_aero_reader.c 的 nearest_generic 要 geo_dist_brg。
 *   原来的命令行少了它，照抄会在链接期报 undefined symbol。）
 *
 * 覆盖窗口化数据架构设计（内部文档）里
 * W1.1 点名要测的四件事 + 风险 R8：
 *   1. 椭圆判据本身（前 100 / 后 40 / 侧 50，以及"沿航迹拉长"确实生效）；
 *   2. 格求交：边界格不漏、格数分布落在文档 §1.2 的实测区间里；
 *   3. **跨 ±180 经线**（R8）与**极区钳位**；
 *   4. 增量差集：进/出的格集合，以及集合运算的升序/去重不变式。
 * 外加淘汰顺序里用到的"点到格最短距离"（让路规则 R1/R2 的分界）。
 *
 * 参考实现在注释里写死成期望值——文档 §1.1/§1.2 的数字就是"Python 对拍"
 * 的那一份，这里逐条落成断言。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../main/pk_win_geom.h"

static int g_fail = 0;

static void chk(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static void chk_int(const char *what, long got, long want)
{
    bool ok = got == want;
    printf("  [%s] %-46s got=%ld want=%ld\n", ok ? "PASS" : "FAIL", what,
           got, want);
    if (!ok) g_fail++;
}

static void chk_range(const char *what, long got, long lo, long hi)
{
    bool ok = got >= lo && got <= hi;
    printf("  [%s] %-46s got=%ld want=[%ld,%ld]\n", ok ? "PASS" : "FAIL",
           what, got, lo, hi);
    if (!ok) g_fail++;
}

/* 从 (lat,lon) 沿真方位角 brg 走 d 海里后的点（球面小距离用平面近似足够：
 * 这里只用来造测试点，误差远小于"在不在椭圆里"的判据尺度）。 */
static void offset_point(double lat, double lon, double brg, double d_nm,
                         double *out_lat, double *out_lon)
{
    const double r = brg * M_PI / 180.0;
    const double dy = d_nm * cos(r);
    const double dx = d_nm * sin(r);
    *out_lat = lat + dy / 60.0;
    *out_lon = lon + dx / (60.0 * cos(lat * M_PI / 180.0));
}

/* ---- 1. 椭圆判据 ---- */
static void test_shape(void)
{
    printf("\n== 1. 偏心椭圆判据（前100/后40/侧50 NM）==\n");
    pk_win_shape_t s;
    /* 中心 40N 116E，航迹正东 (090) */
    pk_win_shape_ellipse(&s, 40.0, 116.0, 90.0);

    double la, lo;
    /* 正前方 99 NM（东）→ 在内；101 NM → 在外 */
    offset_point(40.0, 116.0, 90.0, 99.0, &la, &lo);
    chk("前 99 NM 在窗口内", pk_win_shape_contains(&s, la, lo));
    offset_point(40.0, 116.0, 90.0, 101.0, &la, &lo);
    chk("前 101 NM 在窗口外", !pk_win_shape_contains(&s, la, lo));

    /* 正后方（西）39 / 41 NM */
    offset_point(40.0, 116.0, 270.0, 39.0, &la, &lo);
    chk("后 39 NM 在窗口内", pk_win_shape_contains(&s, la, lo));
    offset_point(40.0, 116.0, 270.0, 41.0, &la, &lo);
    chk("后 41 NM 在窗口外", !pk_win_shape_contains(&s, la, lo));

    /* 正侧向（北/南）49 / 51 NM */
    offset_point(40.0, 116.0, 0.0, 49.0, &la, &lo);
    chk("左 49 NM 在窗口内", pk_win_shape_contains(&s, la, lo));
    offset_point(40.0, 116.0, 0.0, 51.0, &la, &lo);
    chk("左 51 NM 在窗口外", !pk_win_shape_contains(&s, la, lo));
    offset_point(40.0, 116.0, 180.0, 49.0, &la, &lo);
    chk("右 49 NM 在窗口内", pk_win_shape_contains(&s, la, lo));

    /* 航迹转到正北（000）后，"前"应当变成北 */
    pk_win_shape_ellipse(&s, 40.0, 116.0, 0.0);
    offset_point(40.0, 116.0, 0.0, 90.0, &la, &lo);
    chk("航迹 000 时正北 90 NM 在内", pk_win_shape_contains(&s, la, lo));
    offset_point(40.0, 116.0, 90.0, 90.0, &la, &lo);
    chk("航迹 000 时正东 90 NM 在外", !pk_win_shape_contains(&s, la, lo));

    /* 退化圆：各向同性 60 NM */
    pk_win_shape_circle(&s, 40.0, 116.0, 60.0);
    for (int b = 0; b < 360; b += 45) {
        offset_point(40.0, 116.0, (double)b, 59.0, &la, &lo);
        if (!pk_win_shape_contains(&s, la, lo)) { g_fail++; }
        offset_point(40.0, 116.0, (double)b, 61.0, &la, &lo);
        if (pk_win_shape_contains(&s, la, lo)) { g_fail++; }
    }
    chk("退化圆 60 NM 八向 59/61 判据一致", true);

    /* 1.3× 卸载环：原本 101 NM 的点应当进来 */
    pk_win_shape_ellipse(&s, 40.0, 116.0, 90.0);
    pk_win_shape_grow(&s, PK_WIN_KEEP_SCALE);
    offset_point(40.0, 116.0, 90.0, 101.0, &la, &lo);
    chk("1.3× 卸载环把前 101 NM 收进来", pk_win_shape_contains(&s, la, lo));
    offset_point(40.0, 116.0, 90.0, 131.0, &la, &lo);
    chk("1.3× 卸载环外（前 131 NM）仍在外",
        !pk_win_shape_contains(&s, la, lo));
}

/* ---- 2. 格求交 ---- */

/* 暴力参考实现：对全球 64,800 格逐个取 5×5 采样判相交，与 pk_win_cells
 * 的"外接盒剪枝"结果比对。剪枝错了（漏格）这里立刻现形。 */
static int brute_cells(const pk_win_shape_t *s, uint16_t *out, int max)
{
    static const double f[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    int n = 0;
    for (int r = -90; r <= 89; r++) {
        for (int c = -180; c <= 179; c++) {
            bool hit = false;
            for (int a = 0; a < 5 && !hit; a++)
                for (int b = 0; b < 5 && !hit; b++)
                    if (pk_win_shape_contains(s, (double)r + f[a],
                                              (double)c + f[b]))
                        hit = true;
            if (hit && n < max) out[n++] = (uint16_t)((r + 90) * 360 + (c + 180));
        }
    }
    return n;
}

static void test_cells(void)
{
    printf("\n== 2. 椭圆 × 1° 网格求交 ==\n");

    struct { const char *name; double lat, lon; } pts[] = {
        { "赤道 0N 0E",        0.0,   0.0   },
        { "10N",              10.0,  20.0   },
        { "23N 珠三角",       23.2, 113.5   },
        { "35N 东京",         35.65,139.8   },
        { "40N 北京",         40.1, 116.6   },
        { "44N",              44.0,  -70.0  },
        { "55N",              55.0,   10.0  },
        { "43.9N 内蒙",       43.9, 115.9   },
    };
    int lo_all = 99, hi_all = 0;
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
        for (int trk = 0; trk < 360; trk += 45) {
            pk_win_shape_t s;
            pk_win_shape_ellipse(&s, pts[i].lat, pts[i].lon, (double)trk);
            pk_win_cellset_t set;
            int n = pk_win_cells(&s, &set);
            if (n < lo_all) lo_all = n;
            if (n > hi_all) hi_all = n;

            /* 升序 + 互异 */
            for (int k = 1; k < n; k++)
                if (set.cell[k] <= set.cell[k - 1]) g_fail++;

            /* 与暴力参考逐个对拍（不漏、不多） */
            uint16_t ref[512];
            int rn = brute_cells(&s, ref, 512);
            if (rn != n || memcmp(ref, set.cell, (size_t)n * 2) != 0) {
                printf("  [FAIL] %s trk=%d: 剪枝结果 %d 条 vs 暴力 %d 条\n",
                       pts[i].name, trk, n, rn);
                g_fail++;
            }
        }
    }
    printf("  8 个采样点 × 8 个航向：格数 min=%d max=%d\n", lo_all, hi_all);
    /* 文档 §1.2 实测：10°–55° 纬度带 min 6 / max 15 */
    chk_range("格数落在文档 §1.2 的实测区间", hi_all, 6, 15);
    chk_range("格数下界不小于文档 min", lo_all, 6, 15);

    /* 退化圆 60 NM 的账在文档里"全部 ≤ 椭圆"——格数上也该更少或相等 */
    pk_win_shape_t e, c;
    pk_win_shape_ellipse(&e, 40.1, 116.6, 90.0);
    pk_win_shape_circle(&c, 40.1, 116.6, PK_WIN_CIRCLE_NM);
    pk_win_cellset_t se, sc;
    int ne = pk_win_cells(&e, &se);
    int nc = pk_win_cells(&c, &sc);
    printf("  北京：椭圆 %d 格 / 60 NM 圆 %d 格\n", ne, nc);
    chk("退化圆的格数 ≤ 椭圆", nc <= ne);
}

/* ---- 3. 跨 ±180 与极区（风险 R8）---- */
static void test_edges(void)
{
    printf("\n== 3. 跨 ±180 经线与极区钳位（风险 R8）==\n");

    /* 179.5E，航迹正东 → 窗口必然骑在日期变更线上 */
    pk_win_shape_t s;
    pk_win_shape_ellipse(&s, 51.0, 179.5, 90.0);
    pk_win_cellset_t set;
    int n = pk_win_cells(&s, &set);
    bool has_east = false, has_west = false;
    for (int i = 0; i < n; i++) {
        double la, lo;
        pk_win_cell_sw_corner(set.cell[i], &la, &lo);
        if (lo >= 175.0)   has_east = true;
        if (lo <= -175.0)  has_west = true;
    }
    printf("  179.5E 航迹 090：%d 格\n", n);
    chk("跨 ±180 时东西两侧的格都在", has_east && has_west);
    for (int k = 1; k < n; k++)
        if (set.cell[k] <= set.cell[k - 1]) g_fail++;
    chk("跨 ±180 输出仍是升序互异", true);

    /* -179.5（西侧）同样 */
    pk_win_shape_ellipse(&s, 51.0, -179.5, 270.0);
    n = pk_win_cells(&s, &set);
    has_east = has_west = false;
    for (int i = 0; i < n; i++) {
        double la, lo;
        pk_win_cell_sw_corner(set.cell[i], &la, &lo);
        if (lo >= 175.0)  has_east = true;
        if (lo <= -175.0) has_west = true;
    }
    chk("-179.5 侧同样两边都有", has_east && has_west);

    /* 极区：89.5N。1° 格在经度方向被压扁，100 NM 会横跨几十个格，
     * 这里只要求"不崩、不越界、升序、n 不超容量"。 */
    pk_win_shape_ellipse(&s, 89.5, 30.0, 0.0);
    n = pk_win_cells(&s, &set);
    printf("  89.5N：%d 格（truncated=%d）\n", n, (int)set.truncated);
    chk("极区不超过槽表容量", n <= PK_WIN_MAX_CELLS);
    bool in_range = true;
    for (int i = 0; i < n; i++) if (set.cell[i] >= 64800) in_range = false;
    chk("极区格号全部在 [0, 64800)", in_range);
    /* row 钳位：89.5 + 100NM/60 = 91.2°，不得越出 row 179（lat 89） */
    bool row_ok = true;
    for (int i = 0; i < n; i++) if (set.cell[i] / 360 > 179) row_ok = false;
    chk("北极越界被钳位在 row 179", row_ok);

    pk_win_shape_ellipse(&s, -89.5, 30.0, 180.0);
    n = pk_win_cells(&s, &set);
    bool row_ok2 = true;
    for (int i = 0; i < n; i++) if (set.cell[i] / 360 > 179) row_ok2 = false;
    chk("南极同样不越界", row_ok2 && n <= PK_WIN_MAX_CELLS);

    /* 视口 bbox 跨 ±180（min_lon > max_lon 语义） */
    pk_win_cellset_t vp;
    int vn = pk_win_cells_bbox(30.0, 179.0, 31.0, -179.0, &vp);
    printf("  bbox 30..31N / 179E..-179E：%d 格\n", vn);
    chk("跨线 bbox 展开出 3 个经度列", vn == 3 * 2 || vn == 3);
}

/* ---- 4. 增量差集 ---- */
static void test_diff(void)
{
    printf("\n== 4. 增量差集（进 / 出的格集合）==\n");

    pk_win_cellset_t a, b, in, out;
    pk_win_cellset_clear(&a);
    pk_win_cellset_clear(&b);
    /* 乱序插入，验证 add 维持升序 */
    uint16_t ain[]  = { 500, 100, 300, 200, 400 };
    uint16_t bin[]  = { 300, 400, 600, 700 };
    for (size_t i = 0; i < sizeof(ain) / sizeof(ain[0]); i++)
        pk_win_cellset_add(&a, ain[i]);
    for (size_t i = 0; i < sizeof(bin) / sizeof(bin[0]); i++)
        pk_win_cellset_add(&b, bin[i]);

    chk_int("a 升序插入后条数", a.n, 5);
    bool asc = true;
    for (int i = 1; i < (int)a.n; i++) if (a.cell[i] <= a.cell[i - 1]) asc = false;
    chk("a 保持升序", asc);
    pk_win_cellset_add(&a, 300);
    chk_int("重复插入不增长", a.n, 5);

    /* 进入集 = 新窗口 − 已驻留 */
    pk_win_cellset_diff(&b, &a, &in);
    chk_int("进入集条数（b−a）", in.n, 2);
    chk("进入集内容 = {600,700}", in.cell[0] == 600 && in.cell[1] == 700);

    /* 移出集 = 已驻留 − 新保留环 */
    pk_win_cellset_diff(&a, &b, &out);
    chk_int("移出集条数（a−b）", out.n, 3);
    chk("移出集内容 = {100,200,500}",
        out.cell[0] == 100 && out.cell[1] == 200 && out.cell[2] == 500);

    /* out 与入参同一块内存（advance() 里会这么用） */
    pk_win_cellset_t c = a;
    pk_win_cellset_diff(&c, &b, &c);
    chk_int("原地 diff 结果条数一致", c.n, 3);
    chk("原地 diff 内容一致",
        c.cell[0] == 100 && c.cell[1] == 200 && c.cell[2] == 500);

    /* 并集（W_keep ∪= 视口） */
    pk_win_cellset_t u = a;
    pk_win_cellset_union(&u, &b);
    chk_int("并集条数", u.n, 7);
    asc = true;
    for (int i = 1; i < (int)u.n; i++) if (u.cell[i] <= u.cell[i - 1]) asc = false;
    chk("并集保持升序互异", asc);

    /* 容量边界：塞满 48 之后再塞不进 */
    pk_win_cellset_t full;
    pk_win_cellset_clear(&full);
    bool fill_ok = true;
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++)
        if (!pk_win_cellset_add(&full, (uint16_t)(i * 7))) fill_ok = false;
    chk("48 个格全部插得进", fill_ok);
    chk_int("填满后条数", full.n, PK_WIN_MAX_CELLS);
    chk("满了之后新格插不进", !pk_win_cellset_add(&full, 60000));
    chk("满了之后已有格仍返回 true", pk_win_cellset_add(&full, 7));

    /* 真实推进：北京起飞往东飞 2°，看进/出格数是不是"一列" */
    pk_win_shape_t s0, s1;
    pk_win_cellset_t c0, c1, din, dout;
    pk_win_shape_ellipse(&s0, 40.0, 116.0, 90.0);
    pk_win_shape_ellipse(&s1, 40.0, 118.0, 90.0);
    pk_win_cells(&s0, &c0);
    pk_win_cells(&s1, &c1);
    pk_win_cellset_diff(&c1, &c0, &din);
    pk_win_cellset_diff(&c0, &c1, &dout);
    printf("  东移 2°：驻留 %u → %u，进 %u 出 %u\n",
           c0.n, c1.n, din.n, dout.n);
    chk_range("东移一格进的格数是一列（2–6）", din.n, 2, 6);
    chk_range("东移一格出的格数是一列（2–6）", dout.n, 2, 6);
}

/* ---- 5. 点到格最短距离（让路规则 R1/R2 的分界）---- */
static void test_dist(void)
{
    printf("\n== 5. 点到格最短距离（R1/R2 分界）==\n");
    /* 北京 40.1N 116.6E 落在 cell(row=130, col=296) */
    const uint16_t here = (uint16_t)((40 + 90) * 360 + (116 + 180));
    chk_int("点落在格内距离为 0",
            (long)(pk_win_cell_dist_nm(here, 40.1, 116.6) + 0.5), 0);

    /* 正北隔一格：40.1 → 格 41..42 的下边界 41，距离 0.9° = 54 NM */
    const uint16_t north2 = (uint16_t)((41 + 90) * 360 + (116 + 180));
    double d = pk_win_cell_dist_nm(north2, 40.1, 116.6);
    printf("  到北邻格距离 %.1f NM（期望 ~54）\n", d);
    chk("到北邻格距离在 50–58 NM", d > 50.0 && d < 58.0);

    /* 跨 ±180：179E 的点到 -180..-179 那一格应当很近，而不是绕地球一圈 */
    const uint16_t across = (uint16_t)((51 + 90) * 360 + (-180 + 180));
    d = pk_win_cell_dist_nm(across, 51.5, 179.5);
    printf("  跨 ±180 到邻格距离 %.1f NM（期望 ~19）\n", d);
    chk("跨 ±180 距离没有绕地球", d < 40.0);
}

int main(void)
{
    printf("== pk_win_geom host test ==\n");
    test_shape();
    test_cells();
    test_edges();
    test_diff();
    test_dist();
    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
           g_fail);
    return g_fail == 0 ? 0 : 1;
}
