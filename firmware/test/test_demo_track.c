/* test_demo_track.c — host proof for demo_track（演示模式的真实轨迹回放）。
 *
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_demo_track \
 *      firmware/test/test_demo_track.c firmware/main/demo_track.c \
 *      firmware/main/demo_track_data.c -lm && /tmp/test_demo_track
 *
 * 本模块是演示模式"本机会动"的唯一来源，同时也是窗口机制（pk_win.c）能被
 * 验证的前提。这里钉住四件真机上不好复现、出错却只表现为"演示看起来怪怪的"
 * 的性质：
 *   1. **纯函数**：同一个 t_us 任意顺序问多少次都得到同一结果（模拟器
 *      `--shot <秒>` 的定格靠它；一旦有人加了"上次查到的段"缓存就会崩）；
 *   2. **往返播放**（循环点）：走到终点折返而不是瞬移回起点——瞬移会让窗口
 *      48 个槽一次性全淘汰全重载，那是本任务要避免的头号现象；
 *   3. **跨格能力**：一趟单程必须真的跨过若干个 1°×1° 网格，否则 pk_win 的
 *      让路分支照样压不到，改这一版就白改了；
 *   4. **读数合理**：地速/高度/坡度/俯仰不出物理范围，且相邻时刻不跳变
 *      （角度过北点的短弧插值写错就会在这里露馅）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/demo_track.h"

static int g_fail = 0;

static void chk(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static void chk_near(const char *what, double got, double want, double tol)
{
    const bool ok = fabs(got - want) <= tol;
    printf("  [%s] %-52s got=%.4f want=%.4f tol=%.4f\n",
           ok ? "PASS" : "FAIL", what, got, want, tol);
    if (!ok) g_fail++;
}

/* 回放 t 秒（墙钟）对应的轨迹时间 = t * 倍速。 */
static int64_t wall_us_for_track_s(double track_s)
{
    return (int64_t)(track_s / (double)PK_DEMO_TRACK_SPEED * 1e6);
}

static double dist_nm(double la1, double lo1, double la2, double lo2)
{
    const double dlat = (la2 - la1) * 60.0;
    const double dlon = (lo2 - lo1) * 60.0 * cos((la1 + la2) * 0.5 * M_PI / 180.0);
    return sqrt(dlat * dlat + dlon * dlon);
}

static double wrap180d(double d)
{
    d = fmod(d + 180.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}

/* ── 1. 表本身 ─────────────────────────────────────────────────────── */
static void test_table(void)
{
    printf("table\n");
    chk("表非空", pk_demo_track_n >= 2);
    chk("时长非零", pk_demo_track_dur_s > 0);
    chk("首点 t_s == 0", pk_demo_track[0].t_s == 0);
    chk("末点 t_s == dur",
        pk_demo_track[pk_demo_track_n - 1].t_s == pk_demo_track_dur_s);

    bool mono = true, trk_ok = true, roll_ok = true, gs_ok = true;
    for (uint32_t i = 0; i < pk_demo_track_n; ++i) {
        if (i && pk_demo_track[i].t_s <= pk_demo_track[i - 1].t_s) mono = false;
        if (pk_demo_track[i].trk_ddeg > 3599) trk_ok = false;
        if (abs(pk_demo_track[i].roll_ddeg) > 300) roll_ok = false;
        if (pk_demo_track[i].gs_kt < 0 || pk_demo_track[i].gs_kt > 700) gs_ok = false;
    }
    /* 严格递增是二分查找的前提，不是"最好有"。 */
    chk("t_s 严格递增", mono);
    chk("trk_ddeg ∈ [0,3599]", trk_ok);
    chk("roll_ddeg 已钳 ±30.0°", roll_ok);
    chk("gs_kt ∈ [0,700]", gs_ok);
    printf("  (n=%u, dur=%u s, 表体 %u B)\n", (unsigned)pk_demo_track_n,
           (unsigned)pk_demo_track_dur_s,
           (unsigned)(pk_demo_track_n * sizeof(pk_demo_track_pt_t)));
}

/* ── 2. 纯函数 ─────────────────────────────────────────────────────── */
static void test_pure(void)
{
    printf("purity\n");
    static const double probe[] = { 0.0, 137.5, 4001.25, 9999.0, 12345.0 };
    const size_t n = sizeof(probe) / sizeof(probe[0]);

    pk_demo_track_state_t first[5];
    for (size_t i = 0; i < n; ++i)
        chk("采样成功", pk_demo_track_at(wall_us_for_track_s(probe[i]), &first[i]));

    /* 倒序再问一遍：有任何"上次查到的段"式缓存都会在这里出不同结果。 */
    bool same = true;
    for (size_t k = n; k-- > 0;) {
        pk_demo_track_state_t s;
        pk_demo_track_at(wall_us_for_track_s(probe[k]), &s);
        if (memcmp(&s, &first[k], sizeof(s)) != 0) same = false;
    }
    chk("乱序重采样结果逐字节相同（无隐藏状态）", same);

    pk_demo_track_state_t bad;
    memset(&bad, 0x5A, sizeof(bad));
    chk("out=NULL 不崩且返回 false", !pk_demo_track_at(0, NULL));
}

/* ── 3. 端点与往返 ─────────────────────────────────────────────────── */
static void test_pingpong(void)
{
    printf("ping-pong\n");
    const double dur = (double)pk_demo_track_dur_s;
    pk_demo_track_state_t s0, sEnd, fwd, rev, wrap;

    pk_demo_track_at(0, &s0);
    chk_near("t=0 纬度 == 首点", s0.lat, pk_demo_track[0].lat_e7 * 1e-7, 1e-6);
    chk_near("t=0 经度 == 首点", s0.lon, pk_demo_track[0].lon_e7 * 1e-7, 1e-6);
    chk("t=0 不在回程", !s0.reverse);

    pk_demo_track_at(wall_us_for_track_s(dur), &sEnd);
    chk_near("t=dur 纬度 == 末点", sEnd.lat,
             pk_demo_track[pk_demo_track_n - 1].lat_e7 * 1e-7, 1e-6);

    /* 折返对称：dur-x 与 dur+x 必须落在同一个地理点上。
     * 这一条如果挂了，说明循环点又变回了"瞬移回起点"。 */
    const double x = 600.0;
    pk_demo_track_at(wall_us_for_track_s(dur - x), &fwd);
    pk_demo_track_at(wall_us_for_track_s(dur + x), &rev);
    chk_near("折返对称 · 纬度", rev.lat, fwd.lat, 1e-6);
    chk_near("折返对称 · 经度", rev.lon, fwd.lon, 1e-6);
    chk_near("折返对称 · 高度", rev.alt_ft, fwd.alt_ft, 1.0);
    chk("回程标记置位", rev.reverse && !fwd.reverse);
    chk_near("回程航迹 = 去程 +180°",
             fabs(wrap180d(rev.track_deg - fwd.track_deg)), 180.0, 0.5);
    chk_near("回程坡度取反", rev.roll_deg, -fwd.roll_deg, 0.01);
    chk_near("回程升降率取反", rev.vs_fpm, -fwd.vs_fpm, 1.0);

    /* 一个完整周期后回到原点。 */
    pk_demo_track_at(wall_us_for_track_s(2.0 * dur + 250.0), &wrap);
    pk_demo_track_state_t at250;
    pk_demo_track_at(wall_us_for_track_s(250.0), &at250);
    chk_near("周期 = 2×dur · 纬度", wrap.lat, at250.lat, 1e-6);
    chk_near("周期 = 2×dur · 经度", wrap.lon, at250.lon, 1e-6);

    /* 循环点没有瞬移：折返前后各 1 秒（墙钟）的位移必须是正常巡航量级。 */
    pk_demo_track_state_t a, b;
    pk_demo_track_at(wall_us_for_track_s(dur) - 1000000, &a);
    pk_demo_track_at(wall_us_for_track_s(dur) + 1000000, &b);
    chk("折返处无瞬移（2 s 内位移 < 5 NM）", dist_nm(a.lat, a.lon, b.lat, b.lon) < 5.0);
}

/* ── 4. 单程扫描：跨格、读数范围、无跳变 ───────────────────────────── */
static void test_sweep(void)
{
    printf("sweep（单程 1 Hz 墙钟采样）\n");
    const double dur = (double)pk_demo_track_dur_s;
    const int wall_s = (int)(dur / (double)PK_DEMO_TRACK_SPEED);

    /* 1°×1° 格：pk_win 的加载/淘汰就以它为粒度。 */
    long cells[4096];
    int  n_cells = 0;
    double max_jump_nm = 0.0, max_trk_rate = 0.0;
    double alt_min = 1e9, alt_max = -1e9, gs_max = 0.0, roll_max = 0.0;
    double pitch_max = 0.0;
    int    vs_min = 1 << 30, vs_max = -(1 << 30);
    bool   finite_ok = true;
    pk_demo_track_state_t prev;
    bool have_prev = false;

    for (int t = 0; t <= wall_s; ++t) {
        pk_demo_track_state_t s;
        if (!pk_demo_track_at((int64_t)t * 1000000, &s)) { finite_ok = false; break; }
        if (!isfinite(s.lat) || !isfinite(s.lon) || !isfinite(s.alt_ft) ||
            !isfinite(s.gs_kt) || !isfinite(s.track_deg) ||
            !isfinite(s.roll_deg) || !isfinite(s.pitch_deg)) finite_ok = false;

        const long cell = (long)floor(s.lat) * 1000L + (long)floor(s.lon);
        int seen = 0;
        for (int i = 0; i < n_cells; ++i) if (cells[i] == cell) { seen = 1; break; }
        if (!seen && n_cells < (int)(sizeof(cells) / sizeof(cells[0])))
            cells[n_cells++] = cell;

        if (s.alt_ft < alt_min) alt_min = s.alt_ft;
        if (s.alt_ft > alt_max) alt_max = s.alt_ft;
        if (s.gs_kt > gs_max) gs_max = s.gs_kt;
        if (fabs(s.roll_deg) > roll_max) roll_max = fabs(s.roll_deg);
        if (fabs(s.pitch_deg) > pitch_max) pitch_max = fabs(s.pitch_deg);
        if (s.vs_fpm < vs_min) vs_min = s.vs_fpm;
        if (s.vs_fpm > vs_max) vs_max = s.vs_fpm;

        if (have_prev) {
            const double d = dist_nm(prev.lat, prev.lon, s.lat, s.lon);
            if (d > max_jump_nm) max_jump_nm = d;
            const double dtrk = fabs(wrap180d(s.track_deg - prev.track_deg));
            if (dtrk > max_trk_rate) max_trk_rate = dtrk;
        }
        prev = s;
        have_prev = true;
    }

    chk("全程无 NaN/Inf", finite_ok);
    /* 让路分支要被压到，前提就是这一条：单程真的跨了很多格。 */
    chk("单程跨过的 1° 格数 ≥ 12", n_cells >= 12);
    /* 每墙钟秒推进 = 倍速 × 真实秒。480 kt × 10 s ≈ 1.33 NM，
     * 留一倍余量当上限；超过它说明表里混进了瞬移。 */
    chk("相邻墙钟秒位移 < 3 NM（无瞬移）", max_jump_nm < 3.0);
    chk("相邻墙钟秒航迹变化 < 60°（短弧插值正确）", max_trk_rate < 60.0);
    chk("高度覆盖地面到巡航（< 500 ft 且 > 30000 ft）",
        alt_min < 500.0 && alt_max > 30000.0);
    chk("最大地速 ∈ (300, 700) kt", gs_max > 300.0 && gs_max < 700.0);
    chk("最大坡度 ≤ 30°", roll_max <= 30.0 + 1e-3);
    chk("最大俯仰 < 25°（航迹倾角不该是姿态角）", pitch_max < 25.0);
    chk("升降率覆盖爬升与下降", vs_max > 500 && vs_min < -500);
    chk("升降率量级合理（|VS| < 6000 fpm）",
        vs_max < 6000 && vs_min > -6000);
    printf("  (格 %d 个, 最大步进 %.2f NM, alt %.0f..%.0f ft, gs_max %.0f kt, "
           "roll_max %.1f°, pitch_max %.1f°, vs %d..%d fpm, 单程墙钟 %d s)\n",
           n_cells, max_jump_nm, alt_min, alt_max, gs_max, roll_max, pitch_max,
           vs_min, vs_max, wall_s);
}

int main(void)
{
    printf("== demo_track ==\n");
    test_table();
    test_pure();
    test_pingpong();
    test_sweep();
    printf(g_fail ? "\nFAILED (%d)\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
