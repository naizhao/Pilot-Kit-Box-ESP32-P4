/*
 * pk_win.c — 实现说明见 pk_win.h。
 */
#include "pk_win.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PK_WIN_ENABLE


#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "aircraft_state.h"
#include "own_ship.h"
#include "pk_aero_span.h"
#include "pk_sdcard.h"
#include "pk_tile_loader.h"
#include "pk_win_nearest.h"    /* W1.4：nearest 纯算法（pk_win_nearest_compute）*/

#if PK_WIN_SELFTEST
#include "pk_aero_db.h"        /* 自检的对拍基准：全量加载路径 */
#endif

static const char *TAG = "pk_win";

/* ── 任务参数 ─────────────────────────────────────────────────────────
 * prio 2 / core 1：与 pk_aero_db 完全同档（见 pk_aero_db.c 那段"为什么钉在
 * 核 1"）。核 1 上是 sdr(6)/dsp(4)/tile_ld1(3)，全都高于本任务，所以窗口
 * 只吃它们让出来的空闲片，抢不到 ADS-B 解调头上去；也不会去抢核 0 的
 * pfd/baro/gps。栈 8 KB：本任务的栈上工作区只有 48 个 cand_t（480 B）+
 * 一个 4 KB 的元数据扫描缓冲已挪到 .bss，余量充足。 */
#define WIN_TASK_STACK      (8 * 1024)
#define WIN_TASK_PRIO       2
#define WIN_TASK_CORE       1

/* 推进周期：1 Hz，跟 GPS 同频（文档 §1.3：不引入"移动 N 海里才触发"这个
 * 参数——阈值必须比格边长小才不漏格，而一旦小于格边长它就退化成"格集合
 * 变了"，那还不如直接判格集合。少一个参数，少一处调不准的地方）。 */
#define WIN_TICK_MS         1000
/* 开机静默期：让 splash→PFD→tile_loader→pk_aero_db 都先起来。窗口是懒的。 */
#define WIN_STARTUP_MS      8000
/* 句柄打不开（无卡/无文件）时的重试间隔。 */
#define WIN_RETRY_MS        3000

/* 最短驻留 60 s（文档 §1.3 的迟滞）：格边长 43–60 NM，1.3× 环给 13–20 NM
 * 缓冲带，150 kt 巡航下是 5–8 分钟——60 s 只是再兜一层"贴着边界飞"。 */
#define WIN_MIN_DWELL_MS    60000

/* 让路规则 R1/R2 的分界：格边界离本机 30 NM。
 * 30 NM @ 500 kt = 3.6 min，仍有余量；但这是"数据该在却不在"的兜底，
 * 宁可抢一次卡（文档 §1.7 的 R2）。 */
#define WIN_URGENT_NM       30.0
/* R1 让路上限：每次 100 ms，最多 300 次 = 30 s，之后强读一次并计数。 */
#define WIN_YIELD_STEP_MS   100
#define WIN_YIELD_MAX_STEPS 300
/* R4 地面限流：两次加载之间至少间隔 300 ms。 */
#define WIN_GROUND_GAP_MS   300

/*
 * 状态日志的节流。
 *
 * 这个模块上线时一条日志都不打，"让路规则到底生不生效"只能靠猜——数据全在
 * PSRAM 里烂着。补一条，但**必须低频**：核 0 的 PFD 每秒打一行帧率，这里再
 * 高频一点就会把真正要看的东西冲出屏幕（串口 grep 也会被噪音淹）。
 *
 * 两档：计数有变化时最快 10 s 一行；完全没动静时降到 60 s 一行当心跳。
 * 稳态（窗口填满、飞机没动）就是每分钟一行，看得见"它还活着"，又不刷屏。
 */
#define WIN_LOG_MIN_MS       10000
#define WIN_LOG_HEARTBEAT_MS 60000

/* 转弯保护：|Δtrack| > 30° / 10 s → 退化为 60 NM 圆（文档 §1.4）。 */
#define WIN_TURN_DEG        30.0
#define WIN_TRACK_HIST      10          /* 1 Hz × 10 槽 = 10 s */
/* track 一阶低通时间常数 30 s（避免湍流/小修正抖动椭圆朝向）。 */
#define WIN_TRACK_TAU_S     30.0
/* 地速门槛：低于它 track 无意义（同 pk_own_heading_resolve 的 2 kt）。 */
#define WIN_MIN_GS_KT       2

/* 名称片段的尾部余量：字符串池按偏移取片段时，最后一条的长度未知，
 * 多读这么多字节保证它的 NUL 也在片段内。机场名池实测平均 19.5 B，
 * 128 B 覆盖到极长的机场全名仍绰绰有余；取名时还会再验一次 NUL 在不在。 */
#define WIN_STR_TAIL_PAD    128

/*
 * 单个字符串片段的上限 8 KB —— 这是本轮真机实测逼出来的一条硬约束，
 * 值得把来龙去脉写清楚，免得下一个人再踩。
 *
 * 窗口取名字的办法是"这一格所有记录的池内偏移求 [min, max] 区间，一次读进来"。
 * 前提是**同一格的记录，名字在池里也挨着**。文档 §1.5 给了这个前提的证据
 * （机场 name_off 只有 4.4% 逆序、最密格的名称跨度只有 30,752 B）。
 *
 * 实测下来这个前提对 name 成立、对 **city 不成立**：
 *   /tmp/pk_aero_out/pk_aero_plain.bin（就是卡上那份 v3）逐格统计——
 *     内蒙 10 格：apt.name 跨度合计 1,186 B，apt.city 合计 1,152 B  ✅
 *     北京 10 格：apt.name 3,144 B，**apt.city 178,340 B**          ❌
 *              （单格最坏：8 个机场的 city_off 散在 44,207 B 里，有效 243 B）
 *     珠三角  ：apt.name 10,858 B，**apt.city 57,824 B**            ❌
 *   原因是城市名去重更狠（几百个机场共用一个城市串），池里 name 与 city
 *   是两个不同的带，一格里少数几条 city 就能横跨整条带。
 *
 * 两条处方，都做了：
 *   ① name 与 city **分开成两个片段**，不再合并成一个大区间（只这一条就把
 *      内蒙从 349,550 B 打到 4,455 B）；
 *   ② 再加这个 8 KB 上限：超了就**不取这个片段**（记进 str_skipped），
 *      那一段的名字回落到 pk_aero_db 老路径。8 KB ≈ 一格 400 个名字的有效量，
 *      正常聚簇的片段远在这条线以下，只有"散带"才会被拦。
 *
 * 真正的解法是生成端把池按 grid_cell 重排——和文档 §6.4 对航路提的
 * 同一类要求。列进遗留问题，不在 W1。
 */
#define WIN_STR_FRAG_MAX    (8 * 1024)

/* ── 驻留结构 ───────────────────────────────────────────────────────── */

typedef enum {
    WIN_SLOT_EMPTY = 0,
    WIN_SLOT_READY,
    WIN_SLOT_FAILED,
} win_slot_state_t;

/* 格大块内的一段。off 是块内偏移，first 是该段内的**全局记录下标**。 */
typedef struct {
    uint32_t off, bytes, n, first;
} win_blob_t;

/* 字符串池片段：base = 片段起点在池内的偏移，取名时 ptr = blk+off+(s_off-base)。 */
typedef struct {
    uint32_t off, bytes, base;
} win_strfrag_t;

typedef struct {
    uint16_t cell;
    uint8_t  state;
    uint32_t span_gen;       /* 打开时的 pk_aero_span_generation()，对不上即作废 */
    uint32_t loaded_ms;      /* 最短驻留 60 s 的计时起点 */
    uint32_t last_seen_ms;   /* 最近一次落在 W_keep 内的时刻 */
    uint8_t *blk;            /* 整格一大块（PSRAM），一次 malloc / 一次 free */
    uint32_t blk_bytes;
    win_blob_t    apt, nav, fix, rwy, freq;
    /* 四个字符串片段：机场名 / 机场城市 / 导航台名 / FIX 名。
     * 机场的 name 与 city 必须分开（见 WIN_STR_FRAG_MAX 那段的实测）。 */
    win_strfrag_t apt_name_str, apt_city_str, nav_str, fix_str;
} win_cell_t;

/* 48 槽元数据 ≈ 48 × 100 B ≈ 4.7 KB，放 PSRAM .bss（同 pk_tile_loader 的
 * 硬约束：内部 dram0 那 65 KB 的"调度器启动前窗口"一个字节都不能加）。 */
EXT_RAM_BSS_ATTR static win_cell_t s_cells[PK_WIN_MAX_CELLS];

/* 常驻格目录：三个段的 cell→区间表原样驻留（每项 8 B：cell u16 @0、
 * first u32 @2（未对齐）、count u16 @6）。v3 卡实测约 17.7k 项 ≈ 142 KB，
 * v4 全量 381,760 B（文档 §2.4）。它是"与全球数据量线性相关"的那一小块，
 * 也是窗口机制唯一必须全球常驻的东西。 */
typedef struct {
    uint8_t *tbl;
    uint32_t n_grid;
    uint16_t sec_type;
    uint16_t rec_size;
    uint32_t data_off;      /* 段 data 区在 payload 内的偏移 */
    uint32_t n_rec;
    uint32_t str_off, str_size;
} win_dir_t;

/* 同 s_cells：全部进 PSRAM .bss。内部 dram0 的开机前窗口只剩一千多字节余量。 */
EXT_RAM_BSS_ATTR static win_dir_t s_dir_apt, s_dir_nav, s_dir_fix;
EXT_RAM_BSS_ATTR static win_dir_t s_dir_rwy, s_dir_freq;  /* 无格索引，只用 data_off/rec_size */
static bool      s_dir_ready;

/* 元数据预扫缓冲：读记录只为算 rwy/freq/名称的区间，不进最终块。
 * 4 KB 是 40/32/24 三种记录长度的公倍数够用的整块（每次按整条数取）。 */
#define WIN_SCAN_BYTES  4096
EXT_RAM_BSS_ATTR static uint8_t s_scan[WIN_SCAN_BYTES];

static SemaphoreHandle_t s_lock;
static volatile bool     s_inited;

/* 视口约束（渲染线程写、窗口任务读；两个 double 对不齐也只影响一帧判断） */
static volatile bool   s_vp_valid;
EXT_RAM_BSS_ATTR static double s_vp[4];   /* min_lat, min_lon, max_lat, max_lon */

/* 调试覆盖 */
static volatile bool s_ovr_on;
EXT_RAM_BSS_ATTR static double s_ovr_lat, s_ovr_lon, s_ovr_track;

/* track 平滑与转弯检测 */
static bool   s_track_valid;
static double s_track_smooth;
EXT_RAM_BSS_ATTR static double s_track_hist[WIN_TRACK_HIST];
static int    s_track_hist_n;
static int    s_track_hist_at;

/* 诊断计数 */
EXT_RAM_BSS_ATTR static pk_win_status_t s_st;

/* ── 小工具 ─────────────────────────────────────────────────────────── */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* 记录里的 24-bit 大端（字符串池偏移、rwy_first/freq_first 都是这个口径） */
static uint32_t rd_u24be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static double ang_diff(double a, double b)
{
    double d = a - b;
    while (d > 180.0)  d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

/* ── 格目录 ─────────────────────────────────────────────────────────── */

static void dir_free(win_dir_t *d)
{
    free(d->tbl);
    memset(d, 0, sizeof(*d));
}

static void dirs_free(void)
{
    dir_free(&s_dir_apt);
    dir_free(&s_dir_nav);
    dir_free(&s_dir_fix);
    memset(&s_dir_rwy,  0, sizeof(s_dir_rwy));
    memset(&s_dir_freq, 0, sizeof(s_dir_freq));
    s_dir_ready = false;
}

/* 载入一个段的格目录。need_grid=false 的段（跑道/频率）只记 data_off。 */
static bool dir_load(win_dir_t *d, uint16_t sec_type, bool need_grid)
{
    memset(d, 0, sizeof(*d));
    const pk_aero_section_t *sec = pk_aero_span_section(sec_type);
    if (sec == NULL) return !need_grid;   /* 段缺席：非必需段视为成功（n=0） */

    d->sec_type = sec_type;
    d->rec_size = sec->rec_size;
    d->data_off = sec->data_off;
    d->n_rec    = sec->n;
    d->str_off  = sec->strings_off;
    d->str_size = sec->strings_size;
    if (!need_grid) return true;

    pk_aero_index_t gi;
    if (!pk_aero_span_index(sec_type, &gi)) return false;

    const uint32_t bytes = gi.n_grid * 8u;
    uint8_t *tbl = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (tbl == NULL) {
        ESP_LOGE(TAG, "grid dir alloc %lu B failed (sec %u)",
                 (unsigned long)bytes, (unsigned)sec_type);
        return false;
    }
    if (!pk_aero_span_read(gi.grid_off, tbl, bytes)) {
        free(tbl);
        return false;
    }
    d->tbl    = tbl;
    d->n_grid = gi.n_grid;
    return true;
}

/* 与 pk_aero_grid_lookup 同一套 lower_bound，只是表在我们自己的 PSRAM 里。 */
static void dir_lookup(const win_dir_t *d, uint16_t cell,
                       uint32_t *first, uint32_t *count)
{
    *first = 0;
    *count = 0;
    if (d->tbl == NULL || d->n_grid == 0) return;
    uint32_t lo = 0, hi = d->n_grid;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rd_u16(d->tbl + (size_t)mid * 8) < cell) lo = mid + 1;
        else                                          hi = mid;
    }
    if (lo < d->n_grid) {
        const uint8_t *e = d->tbl + (size_t)lo * 8;
        if (rd_u16(e) == cell) {
            *first = rd_u32(e + 2);    /* 项内偏移 2：未对齐，逐字节 */
            *count = rd_u16(e + 6);
        }
    }
}

static bool dirs_load_all(void)
{
    if (s_dir_ready) return true;
    if (!dir_load(&s_dir_apt, PK_AERO_SEC_AIRPORTS,      true))  goto fail;
    if (!dir_load(&s_dir_nav, PK_AERO_SEC_NAVAIDS,       true))  goto fail;
    /* FIX 段在某些数据集里可以为空（n_grid=0）——不当失败。 */
    (void)dir_load(&s_dir_fix, PK_AERO_SEC_WAYPOINTS_FIX, true);
    if (!dir_load(&s_dir_rwy,  PK_AERO_SEC_RUNWAY_DIRS,  false)) goto fail;
    if (!dir_load(&s_dir_freq, PK_AERO_SEC_FREQUENCIES,  false)) goto fail;

    s_dir_ready = true;
    ESP_LOGI(TAG, "grid dir resident: apt=%lu nav=%lu fix=%lu cells (%lu B)",
             (unsigned long)s_dir_apt.n_grid, (unsigned long)s_dir_nav.n_grid,
             (unsigned long)s_dir_fix.n_grid,
             (unsigned long)((s_dir_apt.n_grid + s_dir_nav.n_grid +
                              s_dir_fix.n_grid) * 8u));
    return true;
fail:
    dirs_free();
    return false;
}

/* ── 单格加载 ───────────────────────────────────────────────────────── */

/* 记录区间的元数据预扫：算出跑道/频率/字符串池各自要读的区间。
 * 为什么要预扫一遍：格大块是**一次分配**的，得先知道总字节。多读一遍
 * 记录区（典型 30–80 KB，顺序 6.45 MB/s ≈ 12 ms）换"每格恰好一次
 * malloc/free"，在文档 §1.7 算出的 757× 富余下完全不值一提，而碎片
 * （风险 R3）是真会咬人的。 */
/* 字符串池内的一段取值域。 */
typedef struct { uint32_t lo, hi; bool has; } win_srange_t;

typedef struct {
    uint32_t rwy_first, rwy_end;     /* 段内记录下标区间 [first, end) */
    uint32_t freq_first, freq_end;
    bool     has_rwy, has_freq;
    /* str[0] = name，str[1] = city（只有机场段有第二个字段）。
     * 分成两个而不是一个合并区间，理由见 WIN_STR_FRAG_MAX。 */
    win_srange_t str[2];
} win_scan_t;

static void scan_init(win_scan_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void scan_str(win_scan_t *s, int which, uint32_t off)
{
    if (off == PK_AERO_STR_NONE) return;   /* 0 = 无字符串，不参与取值域 */
    win_srange_t *r = &s->str[which];
    if (!r->has) { r->lo = r->hi = off; r->has = true; return; }
    if (off < r->lo) r->lo = off;
    if (off > r->hi) r->hi = off;
}

static void scan_range(uint32_t first, uint32_t count,
                       uint32_t *lo, uint32_t *hi, bool *has)
{
    if (count == 0) return;
    if (!*has) { *lo = first; *hi = first + count; *has = true; return; }
    if (first < *lo)           *lo = first;
    if (first + count > *hi)   *hi = first + count;
}

/*
 * 记录内的字段偏移。**权威实现是 pk_aero_reader.c 的 *_get()**，这里只抄
 * 本模块用得到的那几个（跑道/频率聚簇区间 + 三个字符串池偏移）。
 * 单独列成常量而不是散在代码里写裸数字，是因为这几个偏移全都不对齐
 * （rwy_first@23、name_off@31…），抄错了不会崩，只会读出一段乱字节——
 * 那种 bug 最难查。pk_win.c 的自检 verify_against_full_db 就是钉死这一处的。
 */
#define APT_OFF_RWY_FIRST   23   /* u24be */
#define APT_OFF_RWY_COUNT   26   /* u8    */
#define APT_OFF_FREQ_FIRST  27   /* u24be */
#define APT_OFF_FREQ_COUNT  30   /* u8    */
#define APT_OFF_NAME        31   /* u24be */
#define APT_OFF_CITY        34   /* u24be */
#define NAV_OFF_NAME        21   /* u24be */
#define FIX_OFF_NAME        17   /* u24be */

/* 顺序扫过一个段的记录区间，回调式地收集元数据。 */
static bool scan_records(const win_dir_t *d, uint32_t first, uint32_t count,
                         win_scan_t *sc, uint16_t sec_type)
{
    if (count == 0) return true;
    const uint32_t rs = d->rec_size;
    const uint32_t per = WIN_SCAN_BYTES / rs;      /* 每批整条数 */
    for (uint32_t at = 0; at < count; ) {
        uint32_t batch = count - at;
        if (batch > per) batch = per;
        const uint32_t off = d->data_off + (first + at) * rs;
        if (!pk_aero_span_read(off, s_scan, batch * rs)) return false;
        for (uint32_t i = 0; i < batch; i++) {
            const uint8_t *r = s_scan + i * rs;
            if (sec_type == PK_AERO_SEC_AIRPORTS) {
                scan_range(rd_u24be(r + APT_OFF_RWY_FIRST), r[APT_OFF_RWY_COUNT],
                           &sc->rwy_first, &sc->rwy_end, &sc->has_rwy);
                scan_range(rd_u24be(r + APT_OFF_FREQ_FIRST), r[APT_OFF_FREQ_COUNT],
                           &sc->freq_first, &sc->freq_end, &sc->has_freq);
                scan_str(sc, 0, rd_u24be(r + APT_OFF_NAME));
                scan_str(sc, 1, rd_u24be(r + APT_OFF_CITY));
            } else if (sec_type == PK_AERO_SEC_NAVAIDS) {
                scan_str(sc, 0, rd_u24be(r + NAV_OFF_NAME));
            } else if (sec_type == PK_AERO_SEC_WAYPOINTS_FIX) {
                scan_str(sc, 0, rd_u24be(r + FIX_OFF_NAME));
            }
        }
        at += batch;
    }
    return true;
}

static void cell_release(win_cell_t *c)
{
    if (c->blk != NULL) {
        s_st.bytes -= c->blk_bytes;
        s_st.n_apt -= c->apt.n;
        s_st.n_nav -= c->nav.n;
        s_st.n_fix -= c->fix.n;
        free(c->blk);
    }
    memset(c, 0, sizeof(*c));
    c->state = WIN_SLOT_EMPTY;
}

/* 把 [first,count) 的记录读进块内 off 处，并填 blob。 */
static bool blob_fill(win_blob_t *b, const win_dir_t *d,
                      uint32_t first, uint32_t count,
                      uint8_t *blk, uint32_t *cursor)
{
    b->first = first;
    b->n     = count;
    b->off   = *cursor;
    b->bytes = count * d->rec_size;
    if (count == 0) return true;
    if (!pk_aero_span_read(d->data_off + first * d->rec_size,
                           blk + *cursor, b->bytes)) return false;
    *cursor += b->bytes;
    return true;
}

/* 片段字节数。0 = 不取（无字符串 / 段无池 / 跨度超过 WIN_STR_FRAG_MAX）。
 * 超限时置 *skipped，调用方计进诊断。 */
static uint32_t strfrag_bytes(const win_dir_t *d, const win_srange_t *r,
                              bool *skipped)
{
    if (!r->has || d->str_size == 0) return 0;
    uint32_t hi = r->hi + WIN_STR_TAIL_PAD;   /* 尾部余量兜住最后一条的 NUL */
    if (hi > d->str_size) hi = d->str_size;
    if (hi <= r->lo) return 0;
    const uint32_t bytes = hi - r->lo;
    if (bytes > WIN_STR_FRAG_MAX) {
        if (skipped) *skipped = true;
        return 0;      /* 散带：这一格的这一段名字回落到老路径 */
    }
    return bytes;
}

static bool strfrag_fill(win_strfrag_t *f, const win_dir_t *d,
                         const win_srange_t *r, uint8_t *blk, uint32_t *cursor)
{
    memset(f, 0, sizeof(*f));
    const uint32_t bytes = strfrag_bytes(d, r, NULL);
    if (bytes == 0) return true;
    f->base  = r->lo;
    f->off   = *cursor;
    f->bytes = bytes;
    if (!pk_aero_span_read(d->str_off + r->lo, blk + *cursor, bytes))
        return false;
    *cursor += bytes;
    return true;
}

/* 加载一个格。成功返回 true，slot 置 READY。 */
static bool cell_load(win_cell_t *c, uint16_t cell)
{
    const int64_t t0 = esp_timer_get_time();

    uint32_t apt_f, apt_n, nav_f, nav_n, fix_f, fix_n;
    dir_lookup(&s_dir_apt, cell, &apt_f, &apt_n);
    dir_lookup(&s_dir_nav, cell, &nav_f, &nav_n);
    dir_lookup(&s_dir_fix, cell, &fix_f, &fix_n);

    /* 预扫：三段各扫一遍，收跑道/频率区间与三个字符串池片段的值域 */
    win_scan_t sc_apt, sc_nav, sc_fix;
    scan_init(&sc_apt); scan_init(&sc_nav); scan_init(&sc_fix);
    if (!scan_records(&s_dir_apt, apt_f, apt_n, &sc_apt, PK_AERO_SEC_AIRPORTS))
        return false;
    if (!scan_records(&s_dir_nav, nav_f, nav_n, &sc_nav, PK_AERO_SEC_NAVAIDS))
        return false;
    if (!scan_records(&s_dir_fix, fix_f, fix_n, &sc_fix,
                      PK_AERO_SEC_WAYPOINTS_FIX))
        return false;

    const uint32_t rwy_n  = sc_apt.has_rwy  ? sc_apt.rwy_end  - sc_apt.rwy_first  : 0;
    const uint32_t freq_n = sc_apt.has_freq ? sc_apt.freq_end - sc_apt.freq_first : 0;

    uint32_t total = apt_n * s_dir_apt.rec_size
                   + nav_n * s_dir_nav.rec_size
                   + fix_n * (s_dir_fix.rec_size ? s_dir_fix.rec_size : 0)
                   + rwy_n * s_dir_rwy.rec_size
                   + freq_n * s_dir_freq.rec_size;
    bool skipped = false;
    total += strfrag_bytes(&s_dir_apt, &sc_apt.str[0], &skipped);   /* 机场名 */
    total += strfrag_bytes(&s_dir_apt, &sc_apt.str[1], &skipped);   /* 机场城市 */
    total += strfrag_bytes(&s_dir_nav, &sc_nav.str[0], &skipped);
    total += strfrag_bytes(&s_dir_fix, &sc_fix.str[0], &skipped);
    if (skipped) s_st.str_skipped++;

    if (total == 0) {
        /* 空格：不分配，但仍占一个槽标 READY——否则每秒都会重试它。
         * 全球 64,800 格里只有 30,807 个有数据（文档 §2.1），空格很常见。 */
        memset(c, 0, sizeof(*c));
        c->cell     = cell;
        c->state    = WIN_SLOT_READY;
        c->span_gen = pk_aero_span_generation();
        c->loaded_ms = c->last_seen_ms = now_ms();
        s_st.last_load_us = (uint32_t)(esp_timer_get_time() - t0);
        return true;
    }

    uint8_t *blk = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (blk == NULL) {
        ESP_LOGW(TAG, "cell %u: PSRAM alloc %lu B failed", (unsigned)cell,
                 (unsigned long)total);
        return false;
    }

    memset(c, 0, sizeof(*c));
    c->cell      = cell;
    c->blk       = blk;
    c->blk_bytes = total;
    c->span_gen  = pk_aero_span_generation();

    uint32_t cur = 0;
    bool ok = blob_fill(&c->apt, &s_dir_apt, apt_f, apt_n, blk, &cur)
           && blob_fill(&c->nav, &s_dir_nav, nav_f, nav_n, blk, &cur)
           && blob_fill(&c->fix, &s_dir_fix, fix_f, fix_n, blk, &cur)
           && blob_fill(&c->rwy, &s_dir_rwy, sc_apt.rwy_first, rwy_n, blk, &cur)
           && blob_fill(&c->freq, &s_dir_freq, sc_apt.freq_first, freq_n,
                        blk, &cur)
           && strfrag_fill(&c->apt_name_str, &s_dir_apt, &sc_apt.str[0], blk, &cur)
           && strfrag_fill(&c->apt_city_str, &s_dir_apt, &sc_apt.str[1], blk, &cur)
           && strfrag_fill(&c->nav_str, &s_dir_nav, &sc_nav.str[0], blk, &cur)
           && strfrag_fill(&c->fix_str, &s_dir_fix, &sc_fix.str[0], blk, &cur);

    if (!ok) {
        free(blk);
        memset(c, 0, sizeof(*c));
        return false;
    }

    c->state     = WIN_SLOT_READY;
    c->loaded_ms = c->last_seen_ms = now_ms();
    s_st.bytes += total;
    s_st.n_apt += apt_n;
    s_st.n_nav += nav_n;
    s_st.n_fix += fix_n;
    s_st.last_load_us = (uint32_t)(esp_timer_get_time() - t0);
    return true;
}

/* ── 槽表 ───────────────────────────────────────────────────────────── */

static win_cell_t *slot_find(uint16_t cell)
{
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++)
        if (s_cells[i].state != WIN_SLOT_EMPTY && s_cells[i].cell == cell)
            return &s_cells[i];
    return NULL;
}

static win_cell_t *slot_free_one(void)
{
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++)
        if (s_cells[i].state == WIN_SLOT_EMPTY) return &s_cells[i];
    return NULL;
}

static void slots_clear_all(void)
{
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++) cell_release(&s_cells[i]);
    s_st.bytes = s_st.n_apt = s_st.n_nav = s_st.n_fix = 0;
}

static void slots_count(uint8_t *n_cells, uint8_t *n_ready)
{
    uint8_t a = 0, r = 0;
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++) {
        if (s_cells[i].state == WIN_SLOT_EMPTY) continue;
        a++;
        if (s_cells[i].state == WIN_SLOT_READY) r++;
    }
    *n_cells = a;
    *n_ready = r;
}

/* ── 让路规则 R1–R4（文档 §1.7）───────────────────────────────────────
 *
 * 论证在文档里：前向 100 NM 在 500 kt 下是 12 分钟提前量、150 kt 巡航下
 * 40 分钟，而加载一个格是 25 ms（独占）到 412 ms（被瓦片压着）——差 3 个
 * 数量级。所以**常规预取无条件给瓦片让路**不是妥协，是账算完之后的显然选择。
 *
 * 返回 true = 可以读了。 */
static bool yield_to_tiles(double dist_nm, bool on_ground, bool boot_fill)
{
    /* 开机例外：窗口是空的，此时让窗口优先于瓦片一次（文档 §1.7）。 */
    if (boot_fill) return true;
    /* R2 紧急预取：格已进 30 NM 内还没就绪 → 不让路，直接读。 */
    if (!on_ground && dist_nm <= WIN_URGENT_NM) return true;

    /* R1（空中常规）/ R4（地面）：等瓦片队列空。
     * 地面那一档不设强读兜底——文档 R4 明确"一律让路"，且产品已定
     * "地面上人有的是时间"。空中那一档最多让 30 s 然后强读一次。 */
    const int max_steps = on_ground ? WIN_YIELD_MAX_STEPS * 4
                                    : WIN_YIELD_MAX_STEPS;
    for (int i = 0; i < max_steps; i++) {
        if (pk_tile_loader_pending() == 0) return true;
        s_st.yields++;
        vTaskDelay(pdMS_TO_TICKS(WIN_YIELD_STEP_MS));
    }
    if (on_ground) return false;     /* 地面：这一轮不读，下一 tick 再说 */
    s_st.forced++;
    return true;
}

/* ── 中心与形状 ─────────────────────────────────────────────────────── */

/* 解析窗口中心与形状。返回 false = 现在没有可用中心（不加载任何格）。 */
static bool resolve_shape(pk_win_shape_t *out, bool *out_ground)
{
    *out_ground = false;

    if (s_ovr_on) {
        pk_win_shape_ellipse(out, s_ovr_lat, s_ovr_lon, s_ovr_track);
        s_st.lat = s_ovr_lat; s_st.lon = s_ovr_lon; s_st.track_deg = s_ovr_track;
        s_st.circle = false;
        return true;
    }

    aircraft_t own;
    pk_own_src_t src;
    /* 10 s 陈旧上限：GPS 1 Hz、ADS-B 更快，10 s 已经很宽松了 */
    if (!pk_own_ship_resolve(esp_timer_get_time(), 10 * 1000000LL, &own, &src))
        return false;
    if (!own.have_position) return false;

    /* 用 track（地面航迹）而不是 heading（机头朝向）：预取要的是"地面往哪
     * 走"，侧风下机头与航迹能差十几度（文档 §1.4）。ADS-B 时 heading_deg
     * 是自报 track、GPS 时是 GPS track，都是真北。 */
    const bool have_track = own.have_velocity &&
                            own.ground_speed_kt >= WIN_MIN_GS_KT;
    bool turning = false;

    if (have_track) {
        const double t = (double)own.heading_deg;
        if (!s_track_valid) {
            s_track_smooth = t;
            s_track_valid  = true;
            s_track_hist_n = 0;
            s_track_hist_at = 0;
        } else {
            /* 一阶低通，τ = 30 s，1 Hz 采样 → α = 1/30。差值走 ±180 归一，
             * 免得 359°→1° 被当成掉头。 */
            const double a = 1.0 / WIN_TRACK_TAU_S;
            s_track_smooth += a * ang_diff(t, s_track_smooth);
            if (s_track_smooth < 0.0)     s_track_smooth += 360.0;
            if (s_track_smooth >= 360.0)  s_track_smooth -= 360.0;
        }
        /* 转弯检测：拿 10 s 前的**原始** track 比，不用平滑值——平滑值本身
         * 就跟不上转弯，用它比会永远判不出转弯。 */
        if (s_track_hist_n >= WIN_TRACK_HIST) {
            const double old = s_track_hist[s_track_hist_at];
            if (fabs(ang_diff(t, old)) > WIN_TURN_DEG) turning = true;
        }
        s_track_hist[s_track_hist_at] = t;
        s_track_hist_at = (s_track_hist_at + 1) % WIN_TRACK_HIST;
        if (s_track_hist_n < WIN_TRACK_HIST) s_track_hist_n++;
    } else {
        s_track_valid = false;
    }

    /* 地面判据：ADS-B 的 on_ground 位 + 地速门槛。pk_flight_phase 的全局态
     * 依赖 IMU/气压的融合，这里只需要"要不要退化成圆 + 要不要一律让路"
     * 这两个粗结论，用最直接的信号即可。 */
    *out_ground = own.on_ground || !have_track;

    if (!have_track || turning) {
        pk_win_shape_circle(out, own.lat, own.lon, PK_WIN_CIRCLE_NM);
        s_st.circle = true;
        s_st.track_deg = s_track_valid ? s_track_smooth : 0.0;
    } else {
        pk_win_shape_ellipse(out, own.lat, own.lon, s_track_smooth);
        s_st.circle = false;
        s_st.track_deg = s_track_smooth;
    }
    s_st.lat = own.lat;
    s_st.lon = own.lon;
    return true;
}

/* ── 推进一步 ───────────────────────────────────────────────────────── */

static void advance(void)
{
    pk_win_shape_t shape;
    bool on_ground = false;
    if (!resolve_shape(&shape, &on_ground)) return;

    pk_win_cellset_t load_set, keep_set;
    pk_win_cells(&shape, &load_set);
    s_st.win_cells = load_set.n;
    s_st.truncated = load_set.truncated;

    pk_win_shape_t keep = shape;
    pk_win_shape_grow(&keep, PK_WIN_KEEP_SCALE);
    pk_win_cells(&keep, &keep_set);

    /* "屏上可见的格永不卸载"（文档 §6.2 缓解 2）：视口格并进 W_keep。 */
    if (s_vp_valid) {
        pk_win_cellset_t vp;
        pk_win_cells_bbox(s_vp[0], s_vp[1], s_vp[2], s_vp[3], &vp);
        pk_win_cellset_union(&keep_set, &vp);
    }

    const uint32_t t = now_ms();
    const uint32_t span_gen = pk_aero_span_generation();

    /* 1) 刷新 last_seen + 作废换卡后的老格 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++) {
        win_cell_t *c = &s_cells[i];
        if (c->state == WIN_SLOT_EMPTY) continue;
        if (c->span_gen != span_gen) { cell_release(c); continue; }
        if (pk_win_cellset_has(&keep_set, c->cell)) c->last_seen_ms = t;
    }

    /* 2) 卸载：不在 W_keep 内 且 驻留超过 60 s */
    uint32_t out_n = 0;
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++) {
        win_cell_t *c = &s_cells[i];
        if (c->state == WIN_SLOT_EMPTY) continue;
        if (pk_win_cellset_has(&keep_set, c->cell)) continue;
        if ((uint32_t)(t - c->loaded_ms) < WIN_MIN_DWELL_MS) continue;
        cell_release(c);
        out_n++;
        s_st.evicts++;
    }
    xSemaphoreGive(s_lock);

    /* 3) 加载：W_load − 已驻留。每 tick 最多补一个格——文档 §1.7 的账里
     *    一列新格（3–4 个）有 5.2–7.2 分钟才全部进入，1 Hz 一个格远远够；
     *    一次补一批只会把让路窗口拉长。开机首填是例外（下面 boot_fill）。 */
    const bool boot_fill = (s_st.n_ready == 0);
    uint32_t in_n = 0;
    for (int i = 0; i < (int)load_set.n; i++) {
        const uint16_t cell = load_set.cell[i];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool present = slot_find(cell) != NULL;
        xSemaphoreGive(s_lock);
        if (present) continue;

        const double d = pk_win_cell_dist_nm(cell, shape.lat, shape.lon);
        if (!yield_to_tiles(d, on_ground, boot_fill)) break;
        if (on_ground) vTaskDelay(pdMS_TO_TICKS(WIN_GROUND_GAP_MS));

        /* 加载在锁外做（几十毫秒的 SD IO 不该堵住读接口），
         * 装进临时槽再持锁挂上去。 */
        win_cell_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (!cell_load(&tmp, cell)) {
            s_st.load_fail++;
            break;      /* 多半是卡没了；下一 tick 重来 */
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        win_cell_t *slot = slot_find(cell);       /* 复检：可能被并发装过 */
        if (slot != NULL) {
            cell_release(&tmp);   /* 连同 s_st 的计数一起退回 */
            xSemaphoreGive(s_lock);
            continue;
        }
        slot = slot_free_one();
        if (slot == NULL) {
            /* 槽满：淘汰"最久没被 W_keep 碰过"的那个（LRU）。48 槽相对
             * max 15 格 × 1.3× 有近 2× 余量，正常飞不该走到这里。 */
            int worst = -1;
            for (int k = 0; k < PK_WIN_MAX_CELLS; k++) {
                if (worst < 0 ||
                    (int32_t)(s_cells[k].last_seen_ms -
                              s_cells[worst].last_seen_ms) < 0)
                    worst = k;
            }
            cell_release(&s_cells[worst]);
            s_st.evicts++;
            slot = &s_cells[worst];
        }
        *slot = tmp;       /* 字节/条数计数已在 cell_load 里加过 */
        xSemaphoreGive(s_lock);
        s_st.loads++;
        in_n++;
        if (!boot_fill) break;     /* 常规每 tick 一个格 */
    }

    s_st.last_in  = in_n;
    s_st.last_out = out_n;
    slots_count(&s_st.n_cells, &s_st.n_ready);
    s_st.bytes_read = pk_aero_span_bytes_read();
    s_st.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_st.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

/* ── 状态日志 ───────────────────────────────────────────────────────── */

/*
 * 一行打全，不拆多行：拆开之后 grep 到的每一行都只有半个故事，还得靠时间戳
 * 把它们拼回去。诊断页只放得下"驻留格数 + 字节"两个数，剩下的指标全靠这一行
 * ——它才是完整的观测口。
 *
 * 变量名与 pk_win_status_t 的字段名一一对应，看日志时不用再翻译一遍。
 */
static void status_log_maybe(void)
{
    /* 上一次打印时的计数快照。只在本函数里读写，且只有窗口任务这一个调用方。 */
    static uint32_t s_log_last_ms;
    static uint32_t p_loads, p_evicts, p_fail, p_skip, p_yields, p_forced;
    static uint8_t  p_cells, p_ready;
    static bool     s_log_first = true;

    const uint32_t t = now_ms();
    const bool changed = s_st.loads       != p_loads  ||
                         s_st.evicts      != p_evicts ||
                         s_st.load_fail   != p_fail   ||
                         s_st.str_skipped != p_skip   ||
                         s_st.yields      != p_yields ||
                         s_st.forced      != p_forced ||
                         s_st.n_cells     != p_cells  ||
                         s_st.n_ready     != p_ready;

    if (!s_log_first) {
        const uint32_t age = t - s_log_last_ms;
        if (age < (changed ? WIN_LOG_MIN_MS : WIN_LOG_HEARTBEAT_MS)) return;
    }
    s_log_first   = false;
    s_log_last_ms = t;
    p_loads  = s_st.loads;   p_evicts = s_st.evicts;  p_fail   = s_st.load_fail;
    p_skip   = s_st.str_skipped; p_yields = s_st.yields; p_forced = s_st.forced;
    p_cells  = s_st.n_cells; p_ready  = s_st.n_ready;

    ESP_LOGI(TAG,
             "status ready=%u/%u win=%u%s %s bytes=%lu apt=%lu nav=%lu fix=%lu "
             "in=%lu out=%lu loads=%lu evicts=%lu fail=%lu strskip=%lu "
             "yields=%lu forced=%lu last_load=%luus read=%lluKB "
             "psram=%luK/%luK ctr=%.4f,%.4f trk=%.0f",
             (unsigned)s_st.n_ready, (unsigned)s_st.n_cells,
             (unsigned)s_st.win_cells, s_st.truncated ? "+TRUNC" : "",
             s_st.circle ? "circle" : "ellipse",
             (unsigned long)s_st.bytes, (unsigned long)s_st.n_apt,
             (unsigned long)s_st.n_nav, (unsigned long)s_st.n_fix,
             (unsigned long)s_st.last_in, (unsigned long)s_st.last_out,
             (unsigned long)s_st.loads, (unsigned long)s_st.evicts,
             (unsigned long)s_st.load_fail, (unsigned long)s_st.str_skipped,
             (unsigned long)s_st.yields, (unsigned long)s_st.forced,
             (unsigned long)s_st.last_load_us,
             (unsigned long long)(s_st.bytes_read / 1024),
             (unsigned long)(s_st.psram_free / 1024),
             (unsigned long)(s_st.psram_largest / 1024),
             s_st.lat, s_st.lon, s_st.track_deg);
}

/* ── 自检 ───────────────────────────────────────────────────────────── */
#if PK_WIN_SELFTEST
#include "pk_win_selftest.inc"
#endif

/* ── 任务 ───────────────────────────────────────────────────────────── */

static void win_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIN_STARTUP_MS));

    uint32_t span_gen_seen = 0;

    while (1) {
        if (!pk_sdcard_is_mounted()) {
            if (pk_aero_span_is_open()) {
                pk_aero_span_close();
                xSemaphoreTake(s_lock, portMAX_DELAY);
                slots_clear_all();
                xSemaphoreGive(s_lock);
                dirs_free();
            }
            vTaskDelay(pdMS_TO_TICKS(WIN_RETRY_MS));
            continue;
        }
        if (!pk_aero_span_is_open()) {
            if (!pk_aero_span_open()) {
                vTaskDelay(pdMS_TO_TICKS(WIN_RETRY_MS));
                continue;
            }
        }
        if (pk_aero_span_generation() != span_gen_seen) {
            /* 换卡 / 重挂：格目录与所有格全部作废重建（风险 R9）。 */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            slots_clear_all();
            xSemaphoreGive(s_lock);
            dirs_free();
            span_gen_seen = pk_aero_span_generation();
        }
        if (!dirs_load_all()) {
            ESP_LOGW(TAG, "grid dir load failed — retry in %d ms", WIN_RETRY_MS);
            pk_aero_span_close();
            vTaskDelay(pdMS_TO_TICKS(WIN_RETRY_MS));
            continue;
        }

        s_st.open    = true;
        s_st.version = pk_aero_span_version();

#if PK_WIN_SELFTEST
        win_selftest_step();
#endif
        advance();
        status_log_maybe();
        vTaskDelay(pdMS_TO_TICKS(WIN_TICK_MS));
    }
}

/* ── 公共 API ───────────────────────────────────────────────────────── */

void pk_win_init(void)
{
    if (s_inited) return;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex create failed");
        return;
    }
    s_inited = true;
    s_st.enabled = true;
    pk_aero_span_register_unmount_cb();

    BaseType_t ok = xTaskCreatePinnedToCore(win_task, "pk_win",
                                            WIN_TASK_STACK, NULL,
                                            WIN_TASK_PRIO, NULL,
                                            WIN_TASK_CORE);
    if (ok != pdTRUE) ESP_LOGE(TAG, "pk_win task create failed");
}

void pk_win_status_get(pk_win_status_t *out)
{
    if (out == NULL) return;
    *out = s_st;    /* 无锁快照：字段只在窗口任务里写，读到半新半旧也只是
                     * 诊断页闪一帧旧数字，没有解引用风险 */
}

void pk_win_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void pk_win_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

bool pk_win_cell_records(uint16_t cell, uint16_t sec_type,
                         const uint8_t **out_recs, uint32_t *out_n,
                         uint32_t *out_first)
{
    win_cell_t *c = slot_find(cell);
    if (c == NULL || c->state != WIN_SLOT_READY || c->blk == NULL) return false;
    const win_blob_t *b;
    switch (sec_type) {
    case PK_AERO_SEC_AIRPORTS:      b = &c->apt;  break;
    case PK_AERO_SEC_NAVAIDS:       b = &c->nav;  break;
    case PK_AERO_SEC_WAYPOINTS_FIX: b = &c->fix;  break;
    case PK_AERO_SEC_RUNWAY_DIRS:   b = &c->rwy;  break;
    case PK_AERO_SEC_FREQUENCIES:   b = &c->freq; break;
    default: return false;
    }
    if (b->n == 0) return false;
    if (out_recs)  *out_recs  = c->blk + b->off;
    if (out_n)     *out_n     = b->n;
    if (out_first) *out_first = b->first;
    return true;
}

/* ── 窗口 nearest（W1.4）──────────────────────────────────────────────
 * pk_win_nearest.c 的纯算法要一个 pk_win_rec_fn 回调；这里把 cell_records
 * 包成那个签名。ctx 透传 sec_type（rec_fn 按 cell 查时要知道取哪个段）。 */
typedef struct { uint16_t sec_type; } wn_ctx_t;
static bool wn_rec_fn(uint16_t cell, const uint8_t **recs,
                      uint32_t *n, uint32_t *first, void *ctx)
{
    return pk_win_cell_records(cell, ((wn_ctx_t *)ctx)->sec_type, recs, n, first);
}

int pk_win_nearest(uint16_t sec_type, double lat, double lon,
                   pk_aero_near_t *out, int max)
{
    /* 跑道/频率没有经纬度，查无意义；同 cell_records 的 default 分支拒掉。 */
    if (sec_type != PK_AERO_SEC_AIRPORTS &&
        sec_type != PK_AERO_SEC_NAVAIDS  &&
        sec_type != PK_AERO_SEC_WAYPOINTS_FIX) return 0;

    const pk_aero_section_t *sec = pk_aero_span_section(sec_type);
    if (sec == NULL || sec->rec_size == 0) return 0;

    /* query 点的 3×3 格号（与 nearest_generic 同一套 floor+90 / %360 环绕）。
     * 扫哪些格只取决于 query 点，与本机/窗口中心无关——窗口只是"这些格里
     * 哪些已驻留"的过滤器（回调对未驻留格返回 false）。 */
    uint16_t cells[9];
    int nc = 0;
    int row0 = (int)floor(lat) + 90;
    if (row0 < 0)   row0 = 0;
    if (row0 > 179) row0 = 179;
    int col0 = ((int)floor(lon) + 180) % 360;
    if (col0 < 0) col0 += 360;
    for (int dr = -1; dr <= 1; dr++) {
        int row = row0 + dr;
        if (row < 0 || row > 179) continue;
        for (int dc = -1; dc <= 1; dc++)
            cells[nc++] = (uint16_t)(row * 360 + (col0 + dc + 360) % 360);
    }

    wn_ctx_t ctx = { .sec_type = sec_type };
    pk_win_lock();
    int n = pk_win_nearest_compute(cells, nc, sec->rec_size, lat, lon,
                                wn_rec_fn, &ctx, out, max);
    pk_win_unlock();
    return n;
}

void pk_win_resident_cells(pk_win_cellset_t *out)
{
    if (out == NULL) return;
    pk_win_cellset_clear(out);
    for (int i = 0; i < PK_WIN_MAX_CELLS; i++)
        if (s_cells[i].state != WIN_SLOT_EMPTY)
            pk_win_cellset_add(out, s_cells[i].cell);
}

void pk_win_set_viewport(double min_lat, double min_lon,
                         double max_lat, double max_lon)
{
    if (min_lat > max_lat) { s_vp_valid = false; return; }
    s_vp[0] = min_lat; s_vp[1] = min_lon;
    s_vp[2] = max_lat; s_vp[3] = max_lon;
    s_vp_valid = true;
}

void pk_win_debug_override(bool on, double lat, double lon, double track_deg)
{
    s_ovr_lat = lat;
    s_ovr_lon = lon;
    s_ovr_track = track_deg;
    s_ovr_on = on;
}

#else  /* !PK_WIN_ENABLE — 回滚开关：整模块编译成空壳 */

void pk_win_init(void) {}
void pk_win_status_get(pk_win_status_t *out) { if (out) memset(out, 0, sizeof(*out)); }
void pk_win_lock(void) {}
void pk_win_unlock(void) {}
bool pk_win_cell_records(uint16_t cell, uint16_t sec_type,
                         const uint8_t **out_recs, uint32_t *out_n,
                         uint32_t *out_first)
{
    (void)cell; (void)sec_type; (void)out_recs; (void)out_n; (void)out_first;
    return false;
}
void pk_win_resident_cells(pk_win_cellset_t *out) { if (out) pk_win_cellset_clear(out); }
void pk_win_set_viewport(double a, double b, double c, double d)
{ (void)a; (void)b; (void)c; (void)d; }
void pk_win_debug_override(bool on, double lat, double lon, double t)
{ (void)on; (void)lat; (void)lon; (void)t; }
int pk_win_nearest(uint16_t sec_type, double lat, double lon,
                   pk_aero_near_t *out, int max)
{ (void)sec_type; (void)lat; (void)lon; (void)out; (void)max; return 0; }

#endif /* PK_WIN_ENABLE */
