/*
 * demo_gpx.c —— 见 demo_gpx.h。
 *
 * 本文件是 firmware/scripts/gen_demo_track.py 的 C 移植。**改任何一条常量或
 * 算法都必须同时改 Python 那边，然后跑 check_demo_track_parity.py 对拍。**
 * 下面每个函数的名字都刻意与 Python 侧同名，好让两边并排看着改。
 */

#include "demo_gpx.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 抽稀容差（与 gen_demo_track.py 逐行对应）───────────────────────── */
#define POS_TOL_M        25.0
#define ALT_TOL_M        10.0
#define GS_TOL_KT         3.0
#define TRK_TOL_DEG       1.0
#define ROLL_TOL_DEG      1.5
#define MAX_SEG_S        30.0

#define EARTH_R_M   6371008.8
#define G_MS2             9.80665
#define KT_PER_MS         1.943844

/* 航迹角失效门槛：地速低于它就沿用上一点的航迹（否则停机坪上 HSI 疯转）。 */
#define TRK_MIN_GS_KT     3.0
/* 高度中值滤波窗口（奇数），对付**孤立**毛刺。 */
#define ALT_MEDIAN_N      5
/* 高度变化率上限 m/s（4000 fpm），对付记录跨段换基准造成的**台阶**。 */
#define ALT_MAX_RATE_MPS 20.32

#define TRK_SPAN_S        4.0
#define ROLL_SMOOTH_S     6.0

/* 原始采样点。全部用 double：float 会让抽稀的容差判断在边界上与 Python 分歧，
 * 保留下来的点数就对不上，对拍直接失去意义。 */
typedef struct {
    double t_s, lat, lon, alt_m, trk_deg, gs_kt, roll_deg;
} sample_t;

/* ── 分配钩子 ───────────────────────────────────────────────────────── */
static void *ac_alloc(const pk_demo_gpx_alloc_t *ac, size_t n)
{
    return (ac && ac->alloc) ? ac->alloc(n) : malloc(n);
}

static void ac_free(const pk_demo_gpx_alloc_t *ac, void *p)
{
    if (!p) return;
    if (ac && ac->release) ac->release(p);
    else                   free(p);
}

/* ── 数值工具 ───────────────────────────────────────────────────────── */

/*
 * round-half-to-even。**不要换成 C 标准库的 round()**。
 *
 * Python 的内建 round() 是 banker's rounding，C 的 round() 是
 * half-away-from-zero。x.5 落在两者分歧上时量化结果差 1 —— 6000 个点里
 * 总会撞上几个，对拍就永远是"差不多但不相等"，那种状态下再有真 bug 混进来
 * 也看不出来了。这里照抄 Python 的规则，让对拍能要求**零差异**。
 */
static double round_half_even(double v)
{
    const double r = nearbyint(v);   /* 默认舍入模式就是 half-to-even */
    /* nearbyint 受 fesetround 影响；这里再自己判一次半整数，避免调用方
     * 改过舍入模式时静默走偏。 */
    const double f = floor(v);
    const double d = v - f;
    if (d == 0.5) {
        const double lo = f, hi = f + 1.0;
        return (fmod(lo, 2.0) == 0.0) ? lo : hi;
    }
    return r;
}

static int32_t clampi(double v, double lo, double hi)
{
    double r = round_half_even(v);
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    return (int32_t)r;
}

/* Python 的 % 对负数返回非负，C 的 fmod 不是。所有角度环绕都必须走这两个。 */
static double pywrap(double a, double m)
{
    double r = fmod(a, m);
    if (r != 0.0 && ((r < 0.0) != (m < 0.0))) r += m;
    return r;
}

static double wrap180(double deg) { return pywrap(deg + 180.0, 360.0) - 180.0; }

static double lerp_angle(double a, double b, double u)
{
    return pywrap(a + wrap180(b - a) * u, 360.0);
}

static double bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    const double dl = (lon2 - lon1) * M_PI / 180.0;
    const double y = sin(dl) * cos(p2);
    const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    return pywrap(atan2(y, x) * 180.0 / M_PI, 360.0);
}

/* 等距圆柱近似。相邻点最远几十公里，误差远小于 POS_TOL_M。 */
static double dist_m(double lat1, double lon1, double lat2, double lon2)
{
    const double dlat = (lat2 - lat1) * M_PI / 180.0;
    const double dlon = (lon2 - lon1) * M_PI / 180.0
                        * cos((lat1 + lat2) * 0.5 * M_PI / 180.0);
    return hypot(dlat, dlon) * EARTH_R_M;
}

/* ── 时间 ───────────────────────────────────────────────────────────── */

/* Howard Hinnant 的 days_from_civil：不依赖 timegm/时区，且负年份也对。 */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/*
 * GPX 的时间写法不止一种：`...Z`、`...+08:00`、flyGarmin 导出的 `...+0000`
 * （无冒号），还有干脆不带偏移的。三种都吃，不带偏移的按 UTC 处理——演示轨迹
 * 只用到**相对**首点的秒数，整体差一个时区不影响任何结果。
 *
 * 输出拆成整秒 + 微秒而不是直接给 double：epoch 秒已经 1.7e9，double 的 15~16
 * 位有效数字放到微秒上正好在边缘，先减后转才能与 Python 的 total_seconds()
 * （精确整微秒 / 1e6，只舍入一次）逐位相同。
 */
static bool parse_gpx_time(const char *s, int64_t *out_sec, int32_t *out_us)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    int nch = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &nch) != 6) {
        /* ISO 允许用空格当日期/时间分隔符。 */
        if (sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d%n",
                   &y, &mo, &d, &h, &mi, &sec, &nch) != 6) return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;

    const char *p = s + nch;
    int32_t us = 0;
    if (*p == '.' || *p == ',') {
        ++p;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (digits < 6) us = us * 10 + (*p - '0');
            ++digits; ++p;
        }
        if (digits == 0) return false;
        for (int i = digits; i < 6; ++i) us *= 10;   /* 补齐到微秒 */
    }

    int64_t off_s = 0;
    if (*p == 'Z' || *p == 'z') {
        ++p;
    } else if (*p == '+' || *p == '-') {
        const int sign = (*p == '-') ? -1 : 1;
        ++p;
        int oh = 0, om = 0;
        if (sscanf(p, "%2d:%2d", &oh, &om) == 2)      p += 5;
        else if (sscanf(p, "%2d%2d", &oh, &om) == 2)  p += 4;
        else if (sscanf(p, "%2d", &oh) == 1)          p += 2;
        else return false;
        off_s = sign * (int64_t)(oh * 3600 + om * 60);
    }

    *out_sec = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400
               + h * 3600 + mi * 60 + sec - off_s;
    *out_us = us;
    return true;
}

/* ── XML 片段取值（不引入 XML 解析器）──────────────────────────────── */

/* 属性 name 的值（双引号或单引号）。只在一个 trkpt 的起始标签片段里找，
 * 片段几十字节，线性扫足够。 */
static bool attr_double(const char *frag, const char *name, double *out)
{
    const size_t nl = strlen(name);
    for (const char *p = frag; (p = strstr(p, name)) != NULL; p += nl) {
        /* 前面必须是分隔符，否则 "lat" 会命中 "xlat"。 */
        if (p != frag) {
            const char c = p[-1];
            if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r')) continue;
        }
        const char *q = p + nl;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        const char quote = *q;
        if (quote != '"' && quote != '\'') continue;
        char *end = NULL;
        const double v = strtod(q + 1, &end);
        if (end == q + 1) continue;
        *out = v;
        return true;
    }
    return false;
}

/*
 * 子元素 <name> 的文本。命名空间前缀（<gpx:ele>）一并吃掉——GPX 1.0/1.1 的
 * xmlns 不同，写死一种会在另一种上静默返回 0 个点（Python 侧用 `{*}ele`
 * 通配是同一个理由）。
 */
static bool child_text(const char *body, const char *name, char *out, size_t outsz)
{
    const size_t nl = strlen(name);
    for (const char *p = body; (p = strchr(p, '<')) != NULL; ++p) {
        const char *q = p + 1;
        if (*q == '/' || *q == '?' || *q == '!') continue;
        const char *nm = q;
        for (const char *r = q; *r && *r != '>' && *r != ' ' && *r != '/'; ++r) {
            if (*r == ':') { nm = r + 1; break; }
        }
        if (strncmp(nm, name, nl) != 0) continue;
        if (nm[nl] != '>') continue;          /* 带属性的同名元素不吃，源数据没有 */
        const char *txt = nm + nl + 1;
        const char *end = strchr(txt, '<');
        if (!end) return false;
        size_t len = (size_t)(end - txt);
        if (len >= outsz) len = outsz - 1;
        memcpy(out, txt, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

/* ── 流式扫描 ───────────────────────────────────────────────────────── */

#define IO_BUF        4096
#define ATTR_BUF       512
#define BODY_BUF      2048

/* 扫描器状态。整块放堆上：ESP 侧这个函数跑在普通任务栈上，7 KB 的栈帧
 * 会把默认 4 KB 栈直接撑爆。 */
typedef struct {
    char io[IO_BUF];
    char attr[ATTR_BUF];
    char body[BODY_BUF];
} scan_buf_t;

typedef struct {
    sample_t *arr;
    uint32_t  n, cap, max;
    bool      truncated;
    bool      oom;
    const pk_demo_gpx_alloc_t *ac;
} sample_vec_t;

static bool vec_push(sample_vec_t *v, const sample_t *s)
{
    if (v->n >= v->max) { v->truncated = true; return false; }
    if (v->n == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 1024;
        if (cap > v->max) cap = v->max;
        sample_t *na = (sample_t *)ac_alloc(v->ac, (size_t)cap * sizeof(sample_t));
        if (!na) { v->oom = true; return false; }
        if (v->arr) {
            memcpy(na, v->arr, (size_t)v->n * sizeof(sample_t));
            ac_free(v->ac, v->arr);
        }
        v->arr = na;
        v->cap = cap;
    }
    v->arr[v->n++] = *s;
    return true;
}

/* 一个 trkpt 片段（起始标签属性 + 元素体）→ 一个采样点。
 * 缺 lat/lon/time 返回 false（静默跳过，与 Python 一致）。 */
static bool frag_to_sample(const char *attr, const char *body,
                           bool have_t0, int64_t t0_sec, int32_t t0_us,
                           sample_t *out, int64_t *sec, int32_t *us)
{
    double lat, lon;
    if (!attr_double(attr, "lat", &lat) || !attr_double(attr, "lon", &lon))
        return false;

    char buf[64];
    if (!child_text(body, "time", buf, sizeof(buf))) return false;
    if (!parse_gpx_time(buf, sec, us)) return false;

    double alt = 0.0;
    if (child_text(body, "ele", buf, sizeof(buf))) {
        char *end = NULL;
        const char *b = buf;
        while (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r') ++b;
        const double v = strtod(b, &end);
        if (end != b) alt = v;
    }

    const int64_t dus = have_t0 ? ((*sec - t0_sec) * 1000000 + (*us - t0_us)) : 0;
    memset(out, 0, sizeof(*out));
    out->t_s   = (double)dus / 1000000.0;
    out->lat   = lat;
    out->lon   = lon;
    out->alt_m = alt;
    return true;
}

/*
 * 按 4 KB 块扫全文件，找 `<trkpt ...>...</trkpt>`。
 *
 * 逐字节状态机而不是"读一大块再 strstr"：跨块边界的标签用状态机天然不用管，
 * 而"保留尾部再拼接"那种写法要为每种标记长度算保留量，写错一次就是某些点被
 * 静默吞掉——而且只在文件尺寸恰好让标签跨界时才复现（单测 test_big_and_truncate
 * 用 201 种块内对齐把这条压死）。
 *
 * **元素名一律先剥命名空间前缀再比**：GPX 1.0 / 1.1 的 xmlns 不同，有些导出器
 * 写 `<gpx:trkpt>`。写死 `<trkpt` 会在这类文件上静默返回 0 个点——文件明明有
 * 6000 个点，盒子却说"没有可用轨迹点"，排查起来毫无头绪。Python 侧用 `{*}trkpt`
 * 通配是同一个理由。
 */
enum { ST_SEEK, ST_OPEN, ST_NAME, ST_ATTRS, ST_BODY };

#define NAME_BUF   64

/* 剥掉 `ns:` 前缀后是否就是 want。 */
static bool name_is(const char *name, const char *want)
{
    const char *colon = strchr(name, ':');
    return strcmp(colon ? colon + 1 : name, want) == 0;
}

static bool scan_file(FILE *f, sample_vec_t *v, scan_buf_t *sb)
{
    int st = ST_SEEK;
    size_t alen = 0, blen = 0;
    size_t nlen = 0;             /* 正在收集的元素名长度 */
    char   nb[NAME_BUF];
    bool   over = false;         /* 本点的片段溢出，整点丢弃 */
    char   prev = '\0';          /* ST_ATTRS 下的上一个字符，用来认自闭合的 '/' */

    /* ST_BODY 下识别结束标记 `</[ns:]trkpt>` 的子状态：
     * 0 = 等 '<'，1 = 见了 '<' 等 '/'，2 = 正在收集结束标记的元素名。 */
    int    cs = 0;
    size_t cn_len = 0;
    char   cn[NAME_BUF];
    size_t mark = 0;             /* 结束标记的 '<' 写进 body 的位置，用来截掉它 */

    bool have_t0 = false;
    int64_t t0_sec = 0; int32_t t0_us = 0;

    size_t got;
    while ((got = fread(sb->io, 1, IO_BUF, f)) > 0) {
        for (size_t i = 0; i < got; ++i) {
            const char c = sb->io[i];
            /* ST_NAME 判完分隔符后要让**同一个字符**再走一遍 ST_ATTRS
             * （分隔符可能就是 '>'，那一点既没有属性也没有自闭合斜杠）。用
             * 重新分发代替 `--i`：i 是 size_t，i==0 时自减会绕成天文数字。 */
            bool again = true;
            while (again) {
                again = false;
                switch (st) {
                case ST_SEEK:
                    if (c == '<') st = ST_OPEN;
                    break;

                case ST_OPEN:
                    /* `</` 结束标记、`<?` 声明、`<!` 注释/CDATA 都不是我们要的。 */
                    if (c == '/' || c == '?' || c == '!') { st = ST_SEEK; }
                    else { nlen = 0; st = ST_NAME; again = true; }
                    break;

                case ST_NAME:
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                        c == '>' || c == '/') {
                        nb[nlen] = '\0';
                        if (name_is(nb, "trkpt")) {
                            alen = 0; blen = 0; over = false; prev = '\0';
                            cs = 0; cn_len = 0; mark = 0;
                            st = ST_ATTRS;
                            again = true;
                        } else {
                            /* 别的元素：'>' 就此结束，否则等它的 '>'。这里直接
                             * 回 ST_SEEK 即可——ST_SEEK 只找 '<'，标签内部不会
                             * 再出现 '<'（XML 里必须写成 &lt;）。 */
                            st = ST_SEEK;
                        }
                    } else if (c == '<') {
                        nlen = 0;                 /* 畸形输入：又来一个 '<' */
                        st = ST_OPEN;
                    } else if (nlen < NAME_BUF - 1) {
                        nb[nlen++] = c;
                    } else {
                        st = ST_SEEK;             /* 名字长得离谱，不是 trkpt */
                    }
                    break;

                case ST_ATTRS:
                    if (c == '>') {
                        sb->attr[alen] = '\0';
                        if (prev == '/') {
                            /* 自闭合 <trkpt .../>：没有 time，按缺字段丢弃。 */
                            st = ST_SEEK;
                        } else {
                            st = ST_BODY; blen = 0; cs = 0;
                        }
                    } else {
                        if (alen < ATTR_BUF - 1) sb->attr[alen++] = c;
                        else                     over = true;
                        prev = c;
                    }
                    break;

                case ST_BODY: {
                    bool closed = false;
                    switch (cs) {
                    case 0:
                        if (c == '<') { cs = 1; mark = blen; }
                        break;
                    case 1:
                        if (c == '/')      { cs = 2; cn_len = 0; }
                        else if (c == '<') { mark = blen; }
                        else               { cs = 0; }
                        break;
                    default:
                        if (c == '>') {
                            cn[cn_len] = '\0';
                            if (name_is(cn, "trkpt")) closed = true;
                            cs = 0;
                        } else if (cn_len < NAME_BUF - 1) {
                            cn[cn_len++] = c;
                        } else {
                            cs = 0;
                        }
                        break;
                    }

                    if (closed) {
                        /* 结束标记本身也被收进了 body（除了这个 '>'），截掉。 */
                        if (!over) {
                            sb->body[mark] = '\0';
                            sample_t s; int64_t sec = 0; int32_t us = 0;
                            if (frag_to_sample(sb->attr, sb->body, have_t0,
                                               t0_sec, t0_us, &s, &sec, &us)) {
                                if (!have_t0) {
                                    have_t0 = true; t0_sec = sec; t0_us = us;
                                    s.t_s = 0.0;
                                }
                                /* 时间必须严格递增：有的记录器在跨段拼接处会
                                 * 回跳一两秒，而回放的二分查找建立在单调之上。 */
                                if (v->n == 0 || s.t_s > v->arr[v->n - 1].t_s) {
                                    if (!vec_push(v, &s)) {
                                        if (v->oom) return false;
                                        return true;   /* 截断，已有的点照用 */
                                    }
                                }
                            }
                        }
                        st = ST_SEEK;
                    } else {
                        if (blen < BODY_BUF - 1) sb->body[blen++] = c;
                        else                     over = true;
                    }
                    break;
                }
                }
            }
        }
    }
    return true;
}

/* ── 清洗 ①：高度去毛刺 + 去台阶 ───────────────────────────────────── */
/* scratch 是调用方给的 n 个 double 的暂存区（存滤波前的原值）。 */
static void despike_alt(sample_t *s, uint32_t n, double *scratch)
{
    if (n < 2) return;

    if (n >= ALT_MEDIAN_N) {
        const uint32_t half = ALT_MEDIAN_N / 2;
        for (uint32_t i = 0; i < n; ++i) scratch[i] = s[i].alt_m;
        /* 中值而不是均值：均值会把尖刺抹平成一段两倍宽的假爬升，中值直接丢弃它。
         * 端点用**对称收缩**的窗口（末点 half=0，原值直通）——首末两点恰好是
         * 轨迹的起降高度基准，不能因为滤波的边界处理而被改掉。 */
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t h = half;
            if (i < h) h = i;
            if (n - 1 - i < h) h = n - 1 - i;
            double w[ALT_MEDIAN_N];
            uint32_t cnt = 0;
            for (uint32_t j = i - h; j <= i + h; ++j) w[cnt++] = scratch[j];
            for (uint32_t a = 1; a < cnt; ++a) {      /* 插入排序，cnt ≤ 5 */
                const double key = w[a];
                uint32_t b = a;
                while (b > 0 && w[b - 1] > key) { w[b] = w[b - 1]; --b; }
                w[b] = key;
            }
            s[i].alt_m = w[cnt / 2];
        }
    }

    /* 限速跟随：高度每秒最多变 ALT_MAX_RATE_MPS，超出的部分留到后面几秒补。
     * 源轨迹里有两处**台阶**而不是尖刺（记录跨段换了高度基准），中值滤波对
     * 台阶无能为力（中值本来就保边缘）。439 m 的台阶因此摊成约 22 s 的
     * 4000 fpm 爬升，限速器追平原始曲线之后自动不再起作用。 */
    for (uint32_t i = 1; i < n; ++i) {
        const double dt = s[i].t_s - s[i - 1].t_s;
        if (dt <= 0.0) continue;
        const double lim = ALT_MAX_RATE_MPS * dt;
        const double prev = s[i - 1].alt_m;
        double v = s[i].alt_m;
        if (v > prev + lim) v = prev + lim;
        if (v < prev - lim) v = prev - lim;
        s[i].alt_m = v;
    }
}

/* ── 清洗 ⑤：坡度由协调转弯反算 tan(φ) = V·ω / g ──────────────────── */
static void derive_roll(sample_t *s, uint32_t n, double *raw)
{
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t j = (i > 0) ? i - 1 : 0;
        const uint32_t k = (i + 1 < n) ? i + 1 : n - 1;
        raw[i] = 0.0;
        const double dt = s[k].t_s - s[j].t_s;
        if (dt <= 0.0) continue;
        const double omega = wrap180(s[k].trk_deg - s[j].trk_deg) * M_PI / 180.0 / dt;
        const double v = s[i].gs_kt / KT_PER_MS;
        raw[i] = atan2(v * omega, G_MS2) * 180.0 / M_PI;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = i;
        while (j > 0 && s[i].t_s - s[j].t_s < ROLL_SMOOTH_S) --j;
        uint32_t k = i;
        while (k < n - 1 && s[k].t_s - s[i].t_s < ROLL_SMOOTH_S) ++k;
        double sum = 0.0;
        for (uint32_t m = j; m <= k; ++m) sum += raw[m];
        double avg = sum / (double)(k - j + 1);
        /* 上限 ±30°：民航自动驾驶的常用限制就是 25–30°，超出的一定是位置
         * 噪声放大出来的，不是真的。 */
        if (avg < -30.0) avg = -30.0;
        if (avg >  30.0) avg =  30.0;
        s[i].roll_deg = avg;
    }
}

/* ── 清洗 ②④：在**原始**采样率上算航迹/地速，之后才抽稀 ───────────── */
static bool derive(sample_t *s, uint32_t n, const pk_demo_gpx_alloc_t *ac)
{
    if (n < 2) return true;

    /* 一块 n 个 double 的暂存区，先给中值滤波当原值副本，再给坡度当原始序列。
     * 两处用途不重叠，没必要分两次向 PSRAM 要。 */
    double *scratch = (double *)ac_alloc(ac, (size_t)n * sizeof(double));
    if (!scratch) return false;

    despike_alt(s, n, scratch);

    for (uint32_t i = 0; i < n; ++i) {
        /* 航迹用 ±TRK_SPAN_S 的跨度算而不是相邻两点：1 Hz 下相邻点相距不到
         * 250 m，GPS 位置噪声几十米就能让方位角抖好几度，拿去反算坡度会得到
         * 一串 ±30° 的假横滚。 */
        uint32_t j = i;
        while (j > 0 && s[i].t_s - s[j].t_s < TRK_SPAN_S) --j;
        uint32_t k = i;
        while (k < n - 1 && s[k].t_s - s[i].t_s < TRK_SPAN_S) ++k;
        if (j == k) {
            j = (i > 0) ? i - 1 : 0;
            k = (i + 1 < n) ? i + 1 : n - 1;
        }
        const sample_t *a = &s[j], *b = &s[k];
        const double dt = b->t_s - a->t_s;
        const double d = dist_m(a->lat, a->lon, b->lat, b->lon);
        if (dt > 0.0) s[i].gs_kt = d / dt * KT_PER_MS;
        if (s[i].gs_kt >= TRK_MIN_GS_KT && d > 1.0) {
            s[i].trk_deg = bearing_deg(a->lat, a->lon, b->lat, b->lon);
        } else if (i > 0) {
            /* 几乎没动时方位角是纯噪声，沿用上一点，别让 HSI 乱转。 */
            s[i].trk_deg = s[i - 1].trk_deg;
        }
    }

    /* 开场那段还停在廊桥上，上面的循环让它们全沿用了初值 0°，于是飞机一动
     * 就有一次 0° → 跑道方向的跳变。用第一个有效航迹回填，把跳变消掉。 */
    bool found = false;
    double first = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        if (s[i].gs_kt >= TRK_MIN_GS_KT) { first = s[i].trk_deg; found = true; break; }
    }
    if (found) {
        for (uint32_t i = 0; i < n; ++i) {
            if (s[i].gs_kt >= TRK_MIN_GS_KT) break;
            s[i].trk_deg = first;
        }
    }

    derive_roll(s, n, scratch);
    ac_free(ac, scratch);
    return true;
}

/* ── 抽稀 ───────────────────────────────────────────────────────────── */

/* a→b 直接连一段，中间每个点的插值误差是否都在容差内。 */
static bool within_tol(const sample_t *s, uint32_t a, uint32_t b)
{
    const sample_t *sa = &s[a], *sb = &s[b];
    const double span = sb->t_s - sa->t_s;
    /* 单段最长跨度：再长就算各项都在容差内也要插点，否则一条几百秒的直线段
     * 会让"进出格"发生在插值中途，而窗口是按 1 Hz 采本机位置的。 */
    if (span > MAX_SEG_S) return false;
    if (span <= 0.0) return true;
    for (uint32_t m = a + 1; m < b; ++m) {
        const sample_t *p = &s[m];
        const double u = (p->t_s - sa->t_s) / span;
        const double lat = sa->lat + (sb->lat - sa->lat) * u;
        const double lon = sa->lon + (sb->lon - sa->lon) * u;
        if (dist_m(lat, lon, p->lat, p->lon) > POS_TOL_M) return false;
        if (fabs(sa->alt_m + (sb->alt_m - sa->alt_m) * u - p->alt_m) > ALT_TOL_M)
            return false;
        if (fabs(sa->gs_kt + (sb->gs_kt - sa->gs_kt) * u - p->gs_kt) > GS_TOL_KT)
            return false;
        if (fabs(wrap180(lerp_angle(sa->trk_deg, sb->trk_deg, u) - p->trk_deg))
            > TRK_TOL_DEG) return false;
        if (fabs(sa->roll_deg + (sb->roll_deg - sa->roll_deg) * u - p->roll_deg)
            > ROLL_TOL_DEG) return false;
    }
    return true;
}

/* 贪心保点：从上一个保留点起向前伸，只要中间任意一点的线性插值误差超过任一
 * 容差就把前一点定下来。比 Douglas-Peucker 笨，但它一遍就把位置/高度/速度/
 * 航迹/坡度五个量一起管住（DP 只管一个量，五个量要跑五遍再求并集）。
 * 结果写回 idx[]，返回保留点数。 */
static uint32_t simplify(const sample_t *s, uint32_t n, uint32_t *idx)
{
    if (n <= 2) {
        for (uint32_t i = 0; i < n; ++i) idx[i] = i;
        return n;
    }
    uint32_t cnt = 0;
    idx[cnt++] = 0;
    uint32_t anchor = 0, i = 1;
    while (i < n - 1) {
        if (within_tol(s, anchor, i + 1)) { ++i; continue; }
        idx[cnt++] = i;
        anchor = i;
        ++i;
    }
    idx[cnt++] = n - 1;
    return cnt;
}

/* ── 清洗 ③：时间戳落成**严格递增**的整秒 ─────────────────────────── */
/*
 * 表里的时间是 uint32 秒，而原始 GPX 有 1.8 s 这种非整秒采样，四舍五入后相邻
 * 两点可能撞成同一个整数。回放的 seek() 是二分查找，前提就是严格递增；撞了
 * 之后那一段的插值分母为 0，屏上表现为某一瞬间位置/姿态定住不动。冲突时把后
 * 一点顶到 prev+1 而不是丢掉它：丢点会丢掉一次真实的机动，顶 1 秒引入的时间
 * 误差上限就是 1 s（10 倍速下是 0.1 墙钟秒）。
 */
static void emit_points(const sample_t *s, const uint32_t *idx, uint32_t cnt,
                        pk_demo_track_pt_t *out)
{
    int64_t prev = -1;
    for (uint32_t i = 0; i < cnt; ++i) {
        const sample_t *p = &s[idx[i]];
        int64_t t = (int64_t)round_half_even(p->t_s);
        if (t <= prev) t = prev + 1;
        prev = t;

        out[i].lat_e7    = clampi(p->lat * 1e7, -2147483648.0, 2147483647.0);
        out[i].lon_e7    = clampi(p->lon * 1e7, -2147483648.0, 2147483647.0);
        out[i].t_s       = (uint32_t)t;
        out[i].alt_m     = (int16_t)clampi(p->alt_m, -32768.0, 32767.0);
        out[i].roll_ddeg = (int16_t)clampi(p->roll_deg * 10.0, -32768.0, 32767.0);
        out[i].trk_ddeg  = (uint16_t)(clampi(p->trk_deg * 10.0, 0.0, 3599.0) % 3600);
        out[i].gs_kt     = (int16_t)clampi(p->gs_kt, -32768.0, 32767.0);
    }
}

/* ── 对外 ───────────────────────────────────────────────────────────── */
bool pk_demo_gpx_load_file(const char *path, uint32_t max_raw,
                           const pk_demo_gpx_alloc_t *ac,
                           pk_demo_gpx_result_t *out, const char **err)
{
    const char *dummy = NULL;
    if (!err) err = &dummy;
    *err = NULL;
    if (!path || !out) { *err = "参数为空"; return false; }
    if (max_raw == 0) max_raw = PK_DEMO_GPX_MAX_RAW_DEFAULT;

    FILE *f = fopen(path, "rb");
    if (!f) { *err = "打不开文件"; return false; }

    scan_buf_t *sb = (scan_buf_t *)ac_alloc(ac, sizeof(scan_buf_t));
    if (!sb) { fclose(f); *err = "扫描缓冲分配失败"; return false; }

    sample_vec_t v = { .arr = NULL, .n = 0, .cap = 0, .max = max_raw,
                       .truncated = false, .oom = false, .ac = ac };
    const bool scan_ok = scan_file(f, &v, sb);
    fclose(f);                       /* 解析一结束就关句柄：之后拔卡与本模块无关 */
    ac_free(ac, sb);

    if (!scan_ok || v.oom) {
        ac_free(ac, v.arr);
        *err = "内存不足";
        return false;
    }
    if (v.n < 2) {
        ac_free(ac, v.arr);
        *err = "可用轨迹点不足（缺 lat/lon/time？）";
        return false;
    }

    if (!derive(v.arr, v.n, ac)) {
        ac_free(ac, v.arr);
        *err = "内存不足（坡度）";
        return false;
    }

    uint32_t *idx = (uint32_t *)ac_alloc(ac, (size_t)v.n * sizeof(uint32_t));
    if (!idx) {
        ac_free(ac, v.arr);
        *err = "内存不足（抽稀）";
        return false;
    }
    const uint32_t cnt = simplify(v.arr, v.n, idx);

    pk_demo_track_pt_t *pts =
        (pk_demo_track_pt_t *)ac_alloc(ac, (size_t)cnt * sizeof(pk_demo_track_pt_t));
    if (!pts) {
        ac_free(ac, idx);
        ac_free(ac, v.arr);
        *err = "内存不足（轨迹表）";
        return false;
    }
    emit_points(v.arr, idx, cnt, pts);

    ac_free(ac, idx);
    ac_free(ac, v.arr);            /* 原始点数组到这里就没用了，立刻还给 PSRAM */

    out->pts       = pts;
    out->n         = cnt;
    out->dur_s     = pts[cnt - 1].t_s;
    out->raw_n     = v.n;
    out->truncated = v.truncated;
    return true;
}

/* ── 目录扫描 ───────────────────────────────────────────────────────── */
static bool ends_with_gpx(const char *name)
{
    const size_t l = strlen(name);
    if (l < 5) return false;                    /* 至少 "x.gpx" */
    const char *e = name + l - 4;
    return (e[0] == '.' &&
            (e[1] == 'g' || e[1] == 'G') &&
            (e[2] == 'p' || e[2] == 'P') &&
            (e[3] == 'x' || e[3] == 'X'));
}

int pk_demo_gpx_list_dir(const char *dir, char names[][PK_DEMO_GPX_NAME_MAX],
                         int max_names)
{
    if (!dir || !names || max_names <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;       /* . / .. / macOS 的 ._xxx */
        if (!ends_with_gpx(e->d_name)) continue;
        if (strlen(e->d_name) >= PK_DEMO_GPX_NAME_MAX) continue;
        if (n >= max_names) continue;            /* 满了就丢，但继续读完目录 */
        snprintf(names[n], PK_DEMO_GPX_NAME_MAX, "%s", e->d_name);
        ++n;
    }
    closedir(d);

    /* ASCII 升序：readdir 在 FAT 上的返回序取决于目录项的物理顺序，用户删一个
     * 再拷一个就可能换人。排序之后"哪个会被播放"是可预期、可控制的。 */
    for (int a = 1; a < n; ++a) {
        char key[PK_DEMO_GPX_NAME_MAX];
        snprintf(key, sizeof(key), "%s", names[a]);
        int b = a;
        while (b > 0 && strcmp(names[b - 1], key) > 0) {
            snprintf(names[b], PK_DEMO_GPX_NAME_MAX, "%s", names[b - 1]);
            --b;
        }
        snprintf(names[b], PK_DEMO_GPX_NAME_MAX, "%s", key);
    }
    return n;
}
