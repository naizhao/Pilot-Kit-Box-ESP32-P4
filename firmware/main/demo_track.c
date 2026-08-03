/*
 * demo_track.c —— 见 demo_track.h。
 *
 * 回放的主函数必须是纯函数：不许有 static 缓存（哪怕是"上次查到的段下标"
 * 这种看起来无害的加速），因为模拟器会用 --shot 以任意顺序取任意时刻的帧，
 * 带缓存就不可复现。651 条记录的二分查找是 10 次比较，1 Hz 的调用频率下这点
 * 开销不值得用状态去换。
 *
 * 唯一的 static 是**轨迹来源指针**（内置表 or SD 卡上的 GPX 现算表）。它是
 * 启动阶段一次性选定的配置而不是随调用累积的状态，纯函数性质不受影响
 * ——详见 demo_track.h 里 pk_demo_track_use() 的说明。
 */

#include "demo_track.h"

#include <math.h>
#include <stddef.h>   /* NULL —— 轨迹来源指针 */

#define M_PER_FT      0.3048f
#define FT_PER_M      3.2808399f
/* m/s → fpm：1 m/s = 196.850394 ft/min */
#define FPM_PER_MPS   196.850394f
/* kt → ft/s：1 kt = 1.68780986 ft/s */
#define FTPS_PER_KT   1.68780986f

static float wrap360(float d)
{
    d = fmodf(d, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d;
}

static float wrap180(float d)
{
    d = fmodf(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

/* 沿短弧插值。359°→1° 必须走 2° 那条路，线性插值会绕 358°，
 * 屏上表现为 HSI 罗盘在过北点时反着猛转一圈。 */
static float lerp_angle(float a, float b, float u)
{
    return wrap360(a + wrap180(b - a) * u);
}

/* 当前来源。NULL = 内置表。见 demo_track.h 里对"无状态"与并发的说明。 */
static const pk_demo_track_src_t *s_src = NULL;

void pk_demo_track_use(const pk_demo_track_src_t *src)
{
    __atomic_store_n(&s_src, src, __ATOMIC_RELEASE);
}

const pk_demo_track_src_t *pk_demo_track_current(void)
{
    return __atomic_load_n(&s_src, __ATOMIC_ACQUIRE);
}

/* 最后一个 t_s <= t 的下标。表非空且严格递增（生成脚本 / demo_gpx.c 保证，
 * 两边都有单测）。 */
static uint32_t seek(const pk_demo_track_pt_t *tbl, uint32_t n, double t)
{
    uint32_t lo = 0, hi = n - 1;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        if ((double)tbl[mid].t_s <= t) lo = mid;
        else                           hi = mid - 1;
    }
    return lo;
}

bool pk_demo_track_at(int64_t t_us, pk_demo_track_state_t *out)
{
    /* 整个函数只在这里读一次来源，之后全程用局部量：中途被 SD 加载任务换掉
     * 来源的话，二分查到的下标就会去索引另一张表。 */
    const pk_demo_track_src_t *src = __atomic_load_n(&s_src, __ATOMIC_ACQUIRE);
    const pk_demo_track_pt_t *tbl = src ? src->pts   : pk_demo_track;
    const uint32_t tbl_n          = src ? src->n     : pk_demo_track_n;
    const uint32_t tbl_dur        = src ? src->dur_s : pk_demo_track_dur_s;

    if (!out || tbl == NULL || tbl_n < 2 || tbl_dur == 0) return false;

    const double dur = (double)tbl_dur;

    /* 用 double 全程中转：t_us 在开机几小时后已经上百亿，float 只有 24 位
     * 尾数，转进去就丢掉秒以下的位，回放会一顿一顿地跳。 */
    double s = (double)t_us / 1000000.0 * (double)PK_DEMO_TRACK_SPEED;
    if (s < 0.0) s = 0.0;

    /* 往返：一个周期 = 去程 + 回程。 */
    const double period = dur * 2.0;
    double ph = fmod(s, period);
    bool reverse = false;
    if (ph > dur) { ph = period - ph; reverse = true; }

    const uint32_t i = seek(tbl, tbl_n, ph);
    const uint32_t j = (i + 1 < tbl_n) ? i + 1 : i;
    const pk_demo_track_pt_t *a = &tbl[i];
    const pk_demo_track_pt_t *b = &tbl[j];

    const double span = (double)b->t_s - (double)a->t_s;
    double u = (span > 0.0) ? (ph - (double)a->t_s) / span : 0.0;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;

    out->lat = ((double)a->lat_e7 + ((double)b->lat_e7 - (double)a->lat_e7) * u) * 1e-7;
    out->lon = ((double)a->lon_e7 + ((double)b->lon_e7 - (double)a->lon_e7) * u) * 1e-7;

    const float alt_m = (float)a->alt_m + ((float)b->alt_m - (float)a->alt_m) * (float)u;
    out->alt_ft = alt_m * FT_PER_M;

    out->gs_kt = (float)a->gs_kt + ((float)b->gs_kt - (float)a->gs_kt) * (float)u;
    if (out->gs_kt < 0.0f) out->gs_kt = 0.0f;

    out->track_deg = lerp_angle((float)a->trk_ddeg * 0.1f,
                                (float)b->trk_ddeg * 0.1f, (float)u);
    out->roll_deg  = ((float)a->roll_ddeg + ((float)b->roll_ddeg - (float)a->roll_ddeg)
                      * (float)u) * 0.1f;

    /* 升降率是**段内常量**（本段高度差 / 本段真实秒数），不是插值出来的：
     * 抽稀保证了段内高度偏差不超 10 m，段常量的 VS 误差因此有上界；而对
     * 相邻段的 VS 再做插值只会凭空造出一条更平滑但更假的曲线。 */
    out->vs_fpm = 0;
    if (span > 0.0) {
        const float dh_m = (float)b->alt_m - (float)a->alt_m;
        out->vs_fpm = (int)(dh_m / (float)span * FPM_PER_MPS);
    }

    if (reverse) {
        /* 回程 = 沿同一条地面航迹反向飞：航迹掉头、转弯方向相反、
         * 原来的下降变成爬升。位置/高度/地速本身与方向无关，不用动。 */
        out->track_deg = wrap360(out->track_deg + 180.0f);
        out->roll_deg  = -out->roll_deg;
        out->vs_fpm    = -out->vs_fpm;
    }
    out->reverse = reverse;

    /* 俯仰取**航迹倾角**（VS 与地速的夹角），不额外加机体迎角：迎角随重量/
     * 构型变化，编一个常数上去只是给 PFD 的地平线加一个说不清来源的偏置。
     * 地速趋零（停机坪）时分母没意义，直接给 0。 */
    out->pitch_deg = 0.0f;
    if (out->gs_kt > 5.0f) {
        const float vs_ftps = (float)out->vs_fpm / 60.0f;
        const float gs_ftps = out->gs_kt * FTPS_PER_KT;
        out->pitch_deg = atan2f(vs_ftps, gs_ftps) * 180.0f / (float)M_PI;
    }
    return true;
}
