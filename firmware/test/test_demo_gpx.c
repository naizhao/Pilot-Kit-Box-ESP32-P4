/* test_demo_gpx.c — host proof for demo_gpx（SD 卡 GPX → 演示轨迹表）。
 *
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_demo_gpx \
 *      firmware/test/test_demo_gpx.c firmware/main/demo_gpx.c -lm \
 *      && /tmp/test_demo_gpx
 *
 * 这里钉的是**盒子上现算**这条新路径。它与 gen_demo_track.py 的逐字段对拍
 * 由 firmware/scripts/check_demo_track_parity.py 负责（那才是防两份实现漂移的
 * 主证据）；本文件补的是对拍覆盖不到的东西：
 *
 *   1. 目录扫描：只认 .gpx、跳隐藏文件、**按名字排序**（readdir 在 FAT 上的
 *      返回序取决于目录项物理顺序，不排序的话"删一个再拷一个"就换人了）；
 *   2. 畸形输入不崩：截断的 XML、缺 lat/lon、缺 time、缺 ele、空文件、
 *      自闭合 trkpt、非 GPX 文件、不存在的文件；
 *   3. 五条清洗逻辑各自的边界（每条都构造一份会让它露馅的最小 GPX）；
 *   4. 大文件截断上限确实生效，且截断之后产出的表仍然可用；
 *   5. 跨 4 KB 读块边界的标签不会被吞掉——这是流式扫描器最容易写错、又最难
 *      在真机上复现的一处（只在文件尺寸恰好让 <trkpt 骑在块边界上时才发作）。
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../main/demo_gpx.h"

static int g_fail = 0;

static void chk(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static void chk_near(const char *what, double got, double want, double tol)
{
    const bool ok = fabs(got - want) <= tol;
    printf("  [%s] %-50s got=%.3f want=%.3f tol=%.3f\n",
           ok ? "PASS" : "FAIL", what, got, want, tol);
    if (!ok) g_fail++;
}

/* ── 临时文件 / 目录 ────────────────────────────────────────────────── */
static char g_dir[256];

static void tmpdir_make(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/pk_demo_gpx_test_%d", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) { printf("  [FAIL] 建临时目录失败\n"); g_fail++; }
}

static void tmpdir_drop(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    (void)!system(cmd);
}

static const char *write_file(const char *name, const char *text)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  [FAIL] 写 %s 失败\n", path); g_fail++; return path; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return path;
}

/* ── GPX 构造 ───────────────────────────────────────────────────────── */
#define GPX_HEAD \
    "<?xml version=\"1.0\"?>\n<gpx version=\"1.1\"><trk><trkseg>\n"
#define GPX_TAIL "</trkseg></trk></gpx>\n"

/* 一个可变长的字符串拼接器，够构造几 MB 的 GPX。 */
typedef struct { char *buf; size_t len, cap; } sb_t;

static void sb_init(sb_t *s) { s->cap = 1 << 16; s->buf = malloc(s->cap); s->len = 0; s->buf[0] = 0; }
static void sb_free(sb_t *s) { free(s->buf); s->buf = NULL; }

static void sb_add(sb_t *s, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        const int need = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
        va_end(ap);
        if (need >= 0 && (size_t)need < s->cap - s->len) { s->len += (size_t)need; return; }
        s->cap *= 2;
        s->buf = realloc(s->buf, s->cap);
    }
}

/* 秒 → ISO8601（2025-01-01T00:00:00Z 起算），只在同一天内用。 */
static void iso(char *out, size_t n, double t)
{
    const int h = (int)(t / 3600.0);
    const int m = (int)((t - h * 3600.0) / 60.0);
    const double s = t - h * 3600.0 - m * 60.0;
    snprintf(out, n, "2025-01-01T%02d:%02d:%06.3fZ", h, m, s);
}

static void sb_pt(sb_t *s, double t, double lat, double lon, double ele)
{
    char ts[64];
    iso(ts, sizeof(ts), t);
    sb_add(s, "<trkpt lat=\"%.8f\" lon=\"%.8f\"><time>%s</time>"
              "<ele>%.4f</ele></trkpt>\n", lat, lon, ts, ele);
}

static bool load(const char *path, pk_demo_gpx_result_t *r, const char **err)
{
    memset(r, 0, sizeof(*r));
    return pk_demo_gpx_load_file(path, 0, NULL, r, err);
}

/* 造一条「向东匀速直飞」的直线轨迹，供各清洗用例做底板。
 * 250 kt ≈ 128.6 m/s，纬度 30° 上每秒 ≈ 0.001336° 经度。 */
#define BASE_LAT   30.0
#define BASE_LON  120.0
#define DLON_PER_S 0.00133600

static void sb_straight(sb_t *s, int n, double alt)
{
    for (int i = 0; i < n; ++i)
        sb_pt(s, i, BASE_LAT, BASE_LON + DLON_PER_S * i, alt);
}

/* ── 1. 目录扫描 ────────────────────────────────────────────────────── */
static void test_list_dir(void)
{
    printf("list_dir\n");
    char names[PK_DEMO_GPX_LIST_MAX][PK_DEMO_GPX_NAME_MAX];

    chk("目录不存在返回 0",
        pk_demo_gpx_list_dir("/tmp/definitely-not-here-1234", names,
                             PK_DEMO_GPX_LIST_MAX) == 0);

    /* 刻意乱序写入，且掺进不该被认的文件。 */
    write_file("zulu.gpx", GPX_HEAD GPX_TAIL);
    write_file("alpha.gpx", GPX_HEAD GPX_TAIL);
    write_file("Mike.GPX", GPX_HEAD GPX_TAIL);
    write_file("notes.txt", "x");
    write_file("track.gpx.bak", "x");
    write_file(".hidden.gpx", GPX_HEAD GPX_TAIL);   /* macOS 拷卡会撒 ._xxx */
    write_file(".gpx", GPX_HEAD GPX_TAIL);          /* 只有扩展名，不算 */

    const int n = pk_demo_gpx_list_dir(g_dir, names, PK_DEMO_GPX_LIST_MAX);
    chk("只认 .gpx 且跳过隐藏文件（3 个）", n == 3);
    if (n == 3) {
        /* ASCII 升序：大写 M(0x4D) < 小写 a(0x61) < z(0x7A)。 */
        chk("按名字 ASCII 升序", strcmp(names[0], "Mike.GPX") == 0 &&
                                 strcmp(names[1], "alpha.gpx") == 0 &&
                                 strcmp(names[2], "zulu.gpx") == 0);
        printf("    -> %s, %s, %s\n", names[0], names[1], names[2]);
    }
    /* 大小写扩展名也要认：Windows 上另存为常给 .GPX。 */
    chk(".GPX 大写扩展名也认", n == 3);

    const int n1 = pk_demo_gpx_list_dir(g_dir, names, 1);
    chk("max_names 生效且取的是排序最小的那个",
        n1 == 1 && strcmp(names[0], "Mike.GPX") == 0);

    tmpdir_drop();
    tmpdir_make();
}

/* ── 2. 畸形输入 ────────────────────────────────────────────────────── */
static void test_malformed(void)
{
    printf("malformed\n");
    pk_demo_gpx_result_t r;
    const char *err;

    chk("不存在的文件 → false", !load("/tmp/no-such-file-9876.gpx", &r, &err));
    chk("空文件 → false", !load(write_file("empty.gpx", ""), &r, &err));
    chk("非 GPX（纯文本）→ false",
        !load(write_file("plain.gpx", "hello world, not xml at all"), &r, &err));
    chk("只有 1 个点 → false（不足以定义一段）",
        !load(write_file("one.gpx", GPX_HEAD
              "<trkpt lat=\"30\" lon=\"120\"><time>2025-01-01T00:00:00Z</time>"
              "<ele>10</ele></trkpt>" GPX_TAIL), &r, &err));

    /* 缺 lat / 缺 time 的点被**静默跳过**，剩下的照用。 */
    chk("缺 lat/lon 或缺 time 的点被跳过，其余可用",
        load(write_file("partial.gpx", GPX_HEAD
             "<trkpt lon=\"120\"><time>2025-01-01T00:00:00Z</time></trkpt>\n"
             "<trkpt lat=\"30\"><time>2025-01-01T00:00:01Z</time></trkpt>\n"
             "<trkpt lat=\"30\" lon=\"120\"><ele>5</ele></trkpt>\n"
             "<trkpt lat=\"30.000\" lon=\"120.000\">"
                 "<time>2025-01-01T00:00:10Z</time><ele>10</ele></trkpt>\n"
             "<trkpt lat=\"30.001\" lon=\"120.001\">"
                 "<time>2025-01-01T00:00:20Z</time><ele>20</ele></trkpt>\n"
             GPX_TAIL), &r, &err));
    if (r.pts) { chk("  剩 2 个点", r.raw_n == 2); free(r.pts); }

    /* 缺 <ele> → 高度按 0 处理，不能因此判失败（很多手机导出的 GPX 没有 ele）。 */
    chk("缺 <ele> 仍可用，高度按 0",
        load(write_file("noele.gpx", GPX_HEAD
             "<trkpt lat=\"30\" lon=\"120\"><time>2025-01-01T00:00:00Z</time></trkpt>\n"
             "<trkpt lat=\"30.01\" lon=\"120\"><time>2025-01-01T00:00:10Z</time></trkpt>\n"
             GPX_TAIL), &r, &err));
    if (r.pts) { chk("  高度 == 0", r.pts[0].alt_m == 0); free(r.pts); }

    /* 自闭合 <trkpt .../> 没有 time，全部丢弃。 */
    chk("自闭合 trkpt（无 time）全丢 → false",
        !load(write_file("selfclose.gpx", GPX_HEAD
              "<trkpt lat=\"30\" lon=\"120\"/>\n"
              "<trkpt lat=\"30.1\" lon=\"120.1\"/>\n" GPX_TAIL), &r, &err));

    /* 文件在半路被截断（拷卡拷了一半）——不许崩，也不许把半个点当整点。 */
    chk("XML 截断在 trkpt 中间 → 不崩，尾点丢弃",
        load(write_file("trunc.gpx", GPX_HEAD
             "<trkpt lat=\"30.000\" lon=\"120.000\">"
                 "<time>2025-01-01T00:00:00Z</time><ele>0</ele></trkpt>\n"
             "<trkpt lat=\"30.001\" lon=\"120.001\">"
                 "<time>2025-01-01T00:00:10Z</time><ele>0</ele></trkpt>\n"
             "<trkpt lat=\"30.002\" lon=\"120.0"), &r, &err));
    if (r.pts) { chk("  只收下完整的 2 点", r.raw_n == 2); free(r.pts); }

    /* 时间回跳（记录器跨段拼接）：必须被单调过滤掉，否则回放的二分查找失效。 */
    chk("时间回跳的点被丢弃",
        load(write_file("backjump.gpx", GPX_HEAD
             "<trkpt lat=\"30.000\" lon=\"120.000\">"
                 "<time>2025-01-01T00:00:00Z</time><ele>0</ele></trkpt>\n"
             "<trkpt lat=\"30.001\" lon=\"120.010\">"
                 "<time>2025-01-01T00:00:20Z</time><ele>0</ele></trkpt>\n"
             "<trkpt lat=\"30.002\" lon=\"120.020\">"
                 "<time>2025-01-01T00:00:05Z</time><ele>0</ele></trkpt>\n"
             "<trkpt lat=\"30.003\" lon=\"120.030\">"
                 "<time>2025-01-01T00:00:40Z</time><ele>0</ele></trkpt>\n"
             GPX_TAIL), &r, &err));
    if (r.pts) {
        chk("  4 点里丢掉回跳的那个", r.raw_n == 3);
        bool mono = true;
        for (uint32_t i = 1; i < r.n; ++i)
            if (r.pts[i].t_s <= r.pts[i - 1].t_s) mono = false;
        chk("  输出 t_s 严格递增", mono);
        free(r.pts);
    }

    /* 命名空间前缀（GPX 1.0 / 某些导出器）。写死 xmlns 会静默返回 0 个点。 */
    chk("带命名空间前缀的 <gpx:ele>/<gpx:time> 也认",
        load(write_file("ns.gpx",
             "<?xml version=\"1.0\"?><gpx:gpx xmlns:gpx=\"http://x\">"
             "<gpx:trk><gpx:trkseg>\n"
             "<gpx:trkpt lat=\"30.000\" lon=\"120.000\">"
                 "<gpx:time>2025-01-01T00:00:00Z</gpx:time>"
                 "<gpx:ele>111</gpx:ele></gpx:trkpt>\n"
             "<gpx:trkpt lat=\"30.010\" lon=\"120.000\">"
                 "<gpx:time>2025-01-01T00:00:20Z</gpx:time>"
                 "<gpx:ele>111</gpx:ele></gpx:trkpt>\n"
             "</gpx:trkseg></gpx:trk></gpx:gpx>\n"), &r, &err));
    if (r.pts) { chk("  高度读到 111 m", r.pts[0].alt_m == 111); free(r.pts); }

    /* 时间格式：Z / +08:00 / +0000（flyGarmin 无冒号）三种都要吃。 */
    chk("三种时区写法混排都能解析",
        load(write_file("tz.gpx", GPX_HEAD
             "<trkpt lat=\"30.000\" lon=\"120.000\">"
                 "<time>2025-01-01T08:00:00+08:00</time><ele>0</ele></trkpt>\n"
             "<trkpt lat=\"30.010\" lon=\"120.000\">"
                 "<time>2025-01-01T00:00:10Z</time><ele>0</ele></trkpt>\n"
             "<trkpt lat=\"30.020\" lon=\"120.000\">"
                 "<time>2025-01-01T00:00:20+0000</time><ele>0</ele></trkpt>\n"
             GPX_TAIL), &r, &err));
    if (r.pts) {
        chk("  +08:00 与 Z 归一到同一时基（首点 t=0，末点 t=20）",
            r.raw_n == 3 && r.dur_s == 20);
        free(r.pts);
    }
}

/* ── 3. 清洗 ①：高度台阶用限速跟随摊开 ─────────────────────────────── */
static void test_alt_step(void)
{
    printf("cleanup ① 高度台阶（限速跟随）\n");
    sb_t s; sb_init(&s);
    sb_add(&s, GPX_HEAD);
    /* 前 40 s 平飞 1000 m，第 40→42 s 之间 +439 m 的台阶（记录换基准），
     * 之后一直停在 1439 m。这是源轨迹 t≈925 s 处真实发生过的事。 */
    for (int i = 0; i < 120; ++i) {
        const double alt = (i < 41) ? 1000.0 : 1439.0;
        sb_pt(&s, i, BASE_LAT, BASE_LON + DLON_PER_S * i, alt);
    }
    sb_add(&s, GPX_TAIL);

    pk_demo_gpx_result_t r; const char *err;
    chk("加载成功", load(write_file("step.gpx", s.buf), &r, &err));
    sb_free(&s);
    if (!r.pts) return;

    /* 限速之后任意相邻两点的爬升率都不许超过 4000 fpm（20.32 m/s）。
     * 不限速的话这里是 439 m / 2 s = 219 m/s ≈ 43000 fpm。 */
    double max_rate = 0.0;
    for (uint32_t i = 1; i < r.n; ++i) {
        const double dt = (double)r.pts[i].t_s - (double)r.pts[i - 1].t_s;
        if (dt <= 0.0) continue;
        const double rate = fabs((double)r.pts[i].alt_m - (double)r.pts[i - 1].alt_m) / dt;
        if (rate > max_rate) max_rate = rate;
    }
    chk_near("最大高度变化率 ≤ 20.32 m/s（4000 fpm）", max_rate, 0.0, 20.4);
    /* 台阶之后的基准必须被追平，不能整段减 439 m —— 那是"用正确的开头换错误
     * 的结尾"（终点会落在比停机坪低 439 m 的地方）。 */
    chk_near("末点高度追平到台阶后的基准 1439 m",
             r.pts[r.n - 1].alt_m, 1439.0, 1.0);
    free(r.pts);
}

/* ── 4. 清洗 ②：停机坪 gs≈0 时保持上一航迹 ─────────────────────────── */
static void test_gate_heading(void)
{
    printf("cleanup ② 停机坪航迹保持 + 首个有效航迹回填\n");
    sb_t s; sb_init(&s);
    sb_add(&s, GPX_HEAD);
    /* 前 60 s 停在廊桥上，位置只有 ±2.8 m 的 GPS 噪声。这个量级刻意压在 3 kt
     * 门槛**以下**（4 s 跨度窗口上约 1.8 kt）——噪声大到越过门槛就不叫"停着"
     * 了，那是另一个场景。但方位角照样在整圈里乱转：不做门槛保持的话，
     * 这 60 s 的 trk 是一串垃圾，屏上就是开场十几秒 HSI 疯转。 */
    for (int i = 0; i < 60; ++i) {
        const double jitter = 2.5e-5 * sin(i * 2.3);           /* ≈ ±2.8 m */
        sb_pt(&s, i, BASE_LAT + jitter, BASE_LON - jitter, 10.0);
    }
    for (int i = 60; i < 120; ++i)
        sb_pt(&s, i, BASE_LAT, BASE_LON + DLON_PER_S * (i - 60), 10.0);
    sb_add(&s, GPX_TAIL);

    pk_demo_gpx_result_t r; const char *err;
    chk("加载成功", load(write_file("gate.gpx", s.buf), &r, &err));
    sb_free(&s);
    if (!r.pts) return;

    /* 停机坪段（t < 55 s）的航迹必须全部相等 —— 被回填成了起飞后的 090°，
     * 而不是一堆噪声方位角。不做这条，开场十几秒 HSI 会疯转。 */
    bool gate_const = true;
    uint16_t first_trk = r.pts[0].trk_ddeg;
    uint32_t gate_pts = 0;
    for (uint32_t i = 0; i < r.n && r.pts[i].t_s < 55; ++i) {
        if (r.pts[i].trk_ddeg != first_trk) gate_const = false;
        ++gate_pts;
    }
    chk("停机坪段航迹恒定（无噪声乱转）", gate_const && gate_pts >= 1);
    chk_near("回填值 == 起飞航迹 090°", first_trk * 0.1, 90.0, 1.5);

    /* 相邻点航迹变化率：整段不许出现 >45°/s 的跳变。 */
    double max_dtrk = 0.0;
    for (uint32_t i = 1; i < r.n; ++i) {
        const double dt = (double)r.pts[i].t_s - (double)r.pts[i - 1].t_s;
        double d = fabs((double)r.pts[i].trk_ddeg - (double)r.pts[i - 1].trk_ddeg) * 0.1;
        if (d > 180.0) d = 360.0 - d;
        if (dt > 0.0 && d / dt > max_dtrk) max_dtrk = d / dt;
    }
    chk("全程航迹变化率 < 45°/s（没有噪声方位角混进来）", max_dtrk < 45.0);
    free(r.pts);
}

/* ── 5. 清洗 ③：非整秒采样的时间戳撞车 ─────────────────────────────── */
static void test_time_collision(void)
{
    printf("cleanup ③ 时间戳去撞车\n");
    sb_t s; sb_init(&s);
    sb_add(&s, GPX_HEAD);
    /* 0.4 s 采样：四舍五入后 0.4→0、0.8→1、1.2→1 会撞车。加上大幅转弯让
     * 抽稀留下密集的点，撞车才真的会发生。 */
    for (int i = 0; i < 400; ++i) {
        const double t = i * 0.4;
        const double ang = t * 0.05;                 /* 慢慢转一个大圈 */
        sb_pt(&s, t, BASE_LAT + 0.02 * sin(ang), BASE_LON + 0.02 * cos(ang), 500.0);
    }
    sb_add(&s, GPX_TAIL);

    pk_demo_gpx_result_t r; const char *err;
    chk("加载成功", load(write_file("collide.gpx", s.buf), &r, &err));
    sb_free(&s);
    if (!r.pts) return;

    bool mono = true;
    for (uint32_t i = 1; i < r.n; ++i)
        if (r.pts[i].t_s <= r.pts[i - 1].t_s) mono = false;
    /* 不做去撞车的话这里必挂，而屏上的表现是某一瞬间位置/姿态定住不动
     * （回放插值的分母为 0）。 */
    chk("t_s 严格递增（二分查找的前提）", mono);
    chk("首点 t_s == 0", r.pts[0].t_s == 0);
    chk("dur == 末点 t_s", r.dur_s == r.pts[r.n - 1].t_s);
    printf("    (raw=%u kept=%u dur=%u)\n", r.raw_n, r.n, r.dur_s);
    free(r.pts);
}

/* ── 6. 清洗 ④：地速/航迹在原始采样率上算完再抽稀 ──────────────────── */
static void test_gs_before_simplify(void)
{
    printf("cleanup ④ 地速/航迹取自原始采样率\n");
    sb_t s; sb_init(&s);
    sb_add(&s, GPX_HEAD);
    /* 一个半径 2 NM、240 kt 的稳定盘旋。抽稀之后相邻点会隔十几秒，用弦长算
     * 地速会明显偏小（弦 < 弧），而这个数直接画在 PFD 上。 */
    const double R_DEG = 2.0 / 60.0;            /* 2 NM 转成纬度度数 */
    const double V_KT = 240.0;
    const double omega = (V_KT / 60.0) / R_DEG; /* 度/小时 → rad/s 的等效 */
    for (int i = 0; i < 300; ++i) {
        const double t = i;
        const double a = omega / 3600.0 * t;
        sb_pt(&s, t, BASE_LAT + R_DEG * sin(a),
              BASE_LON + R_DEG * cos(a) / cos(BASE_LAT * M_PI / 180.0), 3000.0);
    }
    sb_add(&s, GPX_TAIL);

    pk_demo_gpx_result_t r; const char *err;
    chk("加载成功", load(write_file("orbit.gpx", s.buf), &r, &err));
    sb_free(&s);
    if (!r.pts) return;

    /* 存进表里的地速应该贴着真值 240 kt。 */
    double gs_sum = 0.0;
    for (uint32_t i = 1; i < r.n; ++i) gs_sum += r.pts[i].gs_kt;
    chk_near("表里的地速 ≈ 真值 240 kt", gs_sum / (r.n - 1), 240.0, 8.0);

    /* 坡度：240 kt 稳定盘旋的协调转弯坡度 tanφ = Vω/g。R=2 NM、V=240 kt
     * → ω = V/R ≈ 0.0333 rad/s，φ = atan(123.5 × 0.0333 / 9.807) ≈ 22.7°。 */
    double roll_sum = 0.0;
    for (uint32_t i = 0; i < r.n; ++i) roll_sum += fabs(r.pts[i].roll_ddeg * 0.1);
    chk_near("⑤ 坡度由 tanφ=Vω/g 反算 ≈ 22.7°", roll_sum / r.n, 22.7, 3.0);
    free(r.pts);

    /*
     * 上面的盘旋证明不了「必须在抽稀前算」——匀速盘旋上弦长和弧长差不到 1%，
     * 因为位置容差 25 m 本来就把弦的偏差夹住了。真正压出差距的是**加速段**：
     * 起飞滑跑 0→250 kt。抽稀在这里由位置容差主导（沿直线的位置是 t 的二次
     * 函数，线性插值撑不过 10 s），段长约 10 s；而一段的弦长速度是这 10 s 的
     * **平均**速度，与端点的瞬时速度差着 a·T/2 ≈ 20 kt。
     *
     * 20 kt 直接画在 PFD 的速度带上。这就是「地速/航迹用原始采样率算完再抽稀」
     * 不能省的理由。
     */
    sb_init(&s);
    sb_add(&s, GPX_HEAD);
    const double A_MS2 = 128.6 / 60.0;              /* 60 s 加到 250 kt */
    const double M_PER_DEG_LON = 111320.0 * cos(BASE_LAT * M_PI / 180.0);
    for (int i = 0; i <= 60; ++i) {
        const double t = i;
        const double x = 0.5 * A_MS2 * t * t;
        sb_pt(&s, t, BASE_LAT, BASE_LON + x / M_PER_DEG_LON, 0.0);
    }
    sb_add(&s, GPX_TAIL);
    chk("加速段加载成功", load(write_file("accel.gpx", s.buf), &r, &err));
    sb_free(&s);
    if (!r.pts) return;

    double max_truth_err = 0.0, max_chord_err = 0.0, edge_err = 0.0;
    const double dur = (double)r.pts[r.n - 1].t_s;
    for (uint32_t i = 1; i < r.n; ++i) {
        const double t = r.pts[i].t_s;
        const double truth_kt = A_MS2 * t * 1.943844;
        const double e = fabs(r.pts[i].gs_kt - truth_kt);
        /* 首尾各 5 s 内的点单独看：航迹/地速的 ±4 s 跨度窗口在轨迹两端只能
         * **单边**取（前面/后面没有点了），于是算出来的是 [0,4] 这半个窗口的
         * 平均速度，即 v(2) 而不是 v(0)。加速度 2.14 m/s² 下就是 8 kt 的偏差。
         *
         * 这是算法固有的，Python 侧一模一样（对拍零差异就是证据），不是缺陷：
         * 真实轨迹的首尾都在停机坪上、地速接近 0，那里的地速偏差在屏上等于
         * 看不见。这里把它显式量出来，免得将来有人以为是回归。 */
        if (t < 5.0 || t > dur - 5.0) {
            if (e > edge_err) edge_err = e;
        } else if (e > max_truth_err) {
            max_truth_err = e;
        }

        const double dt = t - (double)r.pts[i - 1].t_s;
        const double dlon = (r.pts[i].lon_e7 - r.pts[i - 1].lon_e7) * 1e-7;
        const double chord_kt = (dt > 0.0)
            ? dlon * M_PER_DEG_LON / dt * 1.943844 : 0.0;
        const double ce = fabs(chord_kt - r.pts[i].gs_kt);
        if (ce > max_chord_err) max_chord_err = ce;
    }
    chk_near("表里的地速贴着瞬时真值 a·t（轨迹中段）", max_truth_err, 0.0, 2.0);
    chk("端点的单边窗口偏差有界（≤ a·2s ≈ 9 kt）", edge_err <= 9.5);
    chk("抽稀后按弦长现算会差 >10 kt（这一步不能省的量化理由）",
        max_chord_err > 10.0);
    printf("    (kept=%u, 中段对真值最大差 %.1f kt, 端点 %.1f kt, "
           "弦长法最大差 %.1f kt)\n",
           r.n, max_truth_err, edge_err, max_chord_err);
    free(r.pts);
}

/* ── 7. 清洗 ⑤：坡度钳位 ────────────────────────────────────────────── */
static void test_roll_clamp(void)
{
    printf("cleanup ⑤ 坡度钳 ±30°\n");
    sb_t s; sb_init(&s);
    sb_add(&s, GPX_HEAD);
    /* 400 kt 上做一个 15°/s 的急转 —— 物理上 tanφ = Vω/g 会算出 80° 以上，
     * 那一定是位置噪声放大出来的，不是真的。 */
    for (int i = 0; i < 200; ++i) {
        const double t = i * 0.5;
        const double a = t * 15.0 * M_PI / 180.0;
        const double R = 0.05;
        sb_pt(&s, t, BASE_LAT + R * sin(a), BASE_LON + R * cos(a), 5000.0);
    }
    sb_add(&s, GPX_TAIL);

    pk_demo_gpx_result_t r; const char *err;
    chk("加载成功", load(write_file("steep.gpx", s.buf), &r, &err));
    sb_free(&s);
    if (!r.pts) return;

    int16_t worst = 0;
    for (uint32_t i = 0; i < r.n; ++i)
        if (abs(r.pts[i].roll_ddeg) > abs(worst)) worst = r.pts[i].roll_ddeg;
    chk("|roll| ≤ 30.0°（钳位生效）", abs(worst) <= 300);
    chk("这个用例确实压到了钳位（|roll| 触到 30°）", abs(worst) >= 295);
    printf("    (worst roll = %.1f°)\n", worst * 0.1);

    /* 顺带钉住表的值域约束（回放与 test_demo_track.c 都依赖它们）。 */
    bool range_ok = true;
    for (uint32_t i = 0; i < r.n; ++i)
        if (r.pts[i].trk_ddeg > 3599) range_ok = false;
    chk("trk_ddeg ∈ [0,3599]", range_ok);
    free(r.pts);
}

/* ── 8. 大文件：块边界 + 截断上限 ───────────────────────────────────── */
static void test_big_and_truncate(void)
{
    printf("big file / 4 KB 块边界 / 截断上限\n");

    /* (a) 把 <trkpt 顶到每一种块内偏移上：在文件头塞 0..200 字节的注释，
     *     逐个跑一遍。标签跨 4 KB 读块边界时被吞点的话，点数会少。 */
    bool boundary_ok = true;
    int worst_pad = -1;
    for (int pad = 0; pad <= 200; ++pad) {
        sb_t s; sb_init(&s);
        sb_add(&s, "<?xml version=\"1.0\"?><!--");
        for (int i = 0; i < pad; ++i) sb_add(&s, "x");
        sb_add(&s, "--><gpx><trk><trkseg>\n");
        sb_straight(&s, 300, 100.0);
        sb_add(&s, GPX_TAIL);
        pk_demo_gpx_result_t r; const char *err;
        const bool ok = load(write_file("pad.gpx", s.buf), &r, &err);
        sb_free(&s);
        if (!ok || r.raw_n != 300) { boundary_ok = false; worst_pad = pad; }
        if (r.pts) free(r.pts);
        if (!boundary_ok) break;
    }
    chk("201 种块内对齐下点数都是 300（标签跨块不丢点）", boundary_ok);
    if (!boundary_ok) printf("    (第一个出错的 pad = %d)\n", worst_pad);

    /* (b) 超过 PK_DEMO_GPX_MAX_RAW_DEFAULT 的文件被截断，但产出仍然可用。 */
    sb_t s; sb_init(&s);
    sb_add(&s, GPX_HEAD);
    const int N = (int)PK_DEMO_GPX_MAX_RAW_DEFAULT + 500;
    for (int i = 0; i < N; ++i)
        sb_pt(&s, i, BASE_LAT + 1e-5 * i, BASE_LON + DLON_PER_S * i, 100.0 + i * 0.01);
    sb_add(&s, GPX_TAIL);
    const char *big = write_file("big.gpx", s.buf);
    struct stat st;
    stat(big, &st);

    pk_demo_gpx_result_t r; const char *err;
    chk("超大文件加载成功", load(big, &r, &err));
    sb_free(&s);
    if (r.pts) {
        chk("truncated 置位", r.truncated);
        chk("raw_n == 上限", r.raw_n == PK_DEMO_GPX_MAX_RAW_DEFAULT);
        chk("截断后的表仍然可用（≥2 点、t_s 递增）", r.n >= 2);
        bool mono = true;
        for (uint32_t i = 1; i < r.n; ++i)
            if (r.pts[i].t_s <= r.pts[i - 1].t_s) mono = false;
        chk("  t_s 严格递增", mono);
        printf("    (源文件 %.1f MB, raw=%u, kept=%u, 表体 %u B)\n",
               st.st_size / 1048576.0, r.raw_n, r.n,
               (unsigned)(r.n * sizeof(pk_demo_track_pt_t)));
        free(r.pts);
    }
}

int main(void)
{
    printf("== demo_gpx ==\n");
    tmpdir_make();
    test_list_dir();
    test_malformed();
    test_alt_step();
    test_gate_heading();
    test_time_collision();
    test_gs_before_simplify();
    test_roll_clamp();
    test_big_and_truncate();
    tmpdir_drop();
    printf(g_fail ? "\nFAILED (%d)\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
