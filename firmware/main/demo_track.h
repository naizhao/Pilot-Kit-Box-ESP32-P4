/*
 * demo_track.h —— 演示模式的**真实飞行轨迹回放**（固件与模拟器共用）。
 *
 * 为什么本机必须动
 * ----------------
 * 演示模式最初把本机钉在北京上空一点不动（demo_data.c 的 DEMO_OWN_LAT/LON），
 * 只让 ADS-B 假目标绕着它转。两条理由推翻了这个设计：
 *
 *   产品：飞行员的原话是「演示模式就是要移动才行」。静止的本机让 PFD 的
 *         高度带、速度带、HSI 全是死数，展台上一眼假。
 *   工程：以本机为中心的滚动窗口（pk_win.c）与它的让路规则 R1–R4，在本机
 *         静止时**永远走不到让路分支**——窗口一次填满之后再没有新格要加载，
 *         `pk_win: status` 里的 loads/evicts/yields 恒为 0。实测静置 180 s
 *         yields 一次都没涨。本机跨过 1°×1° 网格边界，那条路径立刻可复现。
 *
 * 数据
 * ----
 * 一条真实的 ZGGG→ZBAA 全程飞行（内部飞行记录导出的 GPX），
 * firmware/scripts/gen_demo_track.py 抽稀成 demo_track_data.c 里的常量表。
 * 选它的理由：**门到门**（含起飞滑跑与落地）、纵贯 20 个 1°×1° 网格、
 * 1038 NM、原始采样几乎连续（只有 2 处 >10 s 的空洞）。
 *
 * ── 硬约束（与 demo_data.h 同）────────────────────────────────────────
 *   - **不依赖 ESP-IDF**：只用 stdint / math。模拟器要原样编译。
 *   - **无状态**：pk_demo_track_at() 是 t_us 的纯函数，没有需要 init 或
 *     reset 的内部积分量。演示模式可以随时开关，`--shot <秒>` 也才定得住帧。
 *     常量表存的是**绝对**时间戳而不是相邻增量，就是为了满足这一条（存增量
 *     就得先做一次前缀和，那是状态）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 回放倍速。
 *
 * 真实时长 10010 s（2 h 47 min）跨 20 个格 —— 1:1 播放平均 500 s 才过一个格
 * 边界，展台上看不到几次网格滚动，验收窗口机制也要蹲半小时。物理上没有别的
 * 出路：任何真实航线飞机跨一个 1° 格就是 8 分钟左右，换哪条轨迹都一样。
 *
 * 10 倍速下单程 16.7 min、格边界约每 50 s 一次，两头都够用。
 *
 * **代价说清楚**：加速之后「地图滚动速率」与「PFD 上的地速读数」不再自洽。
 * 两者只能保一个，这里保**读数为真**——PFD/HSI/高度带上的地速、航迹、升降率
 * 全部取自轨迹的真实时间戳（即那架飞机当时真实的 460 kt），地图推进得比它快。
 * 反过来把地速也乘 10 会在屏上写出 4600 kt，飞行员一眼就是垃圾，而且地速还
 * 会流进航向优先级（≥2 kt 才用 GPS track）与窗口椭圆的拉长方向这些判据里。
 * 一个肉眼几乎量不出的不一致，换掉一个一眼假、还会污染下游逻辑的读数。
 *
 * 想要完全自洽的实时演示，把它改成 1.0f 即可，别处不用动。
 */
#ifndef PK_DEMO_TRACK_SPEED
#define PK_DEMO_TRACK_SPEED   10.0f
#endif

/* 常量表的一条记录，20 B、4 字节对齐（RISC-V 上非对齐访问要陷入）。
 * 字段顺序与 gen_demo_track.py 的 emit_c() 一一对应，改一边必须改另一边。 */
typedef struct {
    int32_t  lat_e7;      /* 纬度 × 1e7 */
    int32_t  lon_e7;      /* 经度 × 1e7 */
    uint32_t t_s;         /* 距轨迹起点的真实秒数，严格递增 */
    int16_t  alt_m;       /* GPX <ele>，米 */
    int16_t  roll_ddeg;   /* 坡度 0.1°，由转弯率反算，已钳 ±30° */
    uint16_t trk_ddeg;    /* 航迹真北 0.1°，0..3599 */
    int16_t  gs_kt;       /* 地速，节 */
} pk_demo_track_pt_t;

/* 内置轨迹表（demo_track_data.c，由 gen_demo_track.py 生成）。
 * 它是**兜底**：没插卡 / 没有 demo 目录 / 目录里没有 .gpx / 解析失败，
 * 一律回到这张表上，演示模式永远有轨迹可放。 */
extern const pk_demo_track_pt_t pk_demo_track[];
extern const uint32_t pk_demo_track_n;
extern const uint32_t pk_demo_track_dur_s;

/*
 * 当前生效的轨迹来源。回放代码只认这个结构，不关心表是编在固件里的还是
 * 从 SD 卡的 GPX 现算出来的——两条路走**同一套**回放逻辑（往返、10× 速、
 * 二分插值、VS 段常量），只是数据来源不同。
 */
typedef struct {
    const pk_demo_track_pt_t *pts;
    uint32_t n;
    uint32_t dur_s;
} pk_demo_track_src_t;

/*
 * 切换轨迹来源。src 传 NULL 表示回到内置表。
 *
 * ── 与"无状态"约束的关系 ─────────────────────────────────────────────
 * 这不是把 pk_demo_track_at() 变成有状态的函数。来源是**启动阶段一次性选定**
 * 的配置（SD 上有 GPX 就换过去，之后不再变），不是随调用累积的积分量：给定
 * 来源之后，at() 仍然是 t_us 的纯函数，`--shot <秒>` 照样定得住帧，乱序重采样
 * 照样逐字节相同。模拟器从不调用本函数，永远跑内置表。
 *
 * ── 并发 ─────────────────────────────────────────────────────────────
 * 写方是 SD 加载任务，读方是 UI/采样任务。src 指向的内容必须**先填满再发布**，
 * 且发布之后永不释放、永不修改（PSRAM 上一块常驻内存）。指针本身用
 * __atomic_ 读写，读方要么看到旧来源要么看到新来源，不会看到撕裂的三元组
 * ——所以才把 pts/n/dur 打包成一个结构体，而不是三个各自发布的全局量。
 */
void pk_demo_track_use(const pk_demo_track_src_t *src);

/* 当前来源；从没调用过 pk_demo_track_use() 或传过 NULL 时返回 NULL（= 内置表）。 */
const pk_demo_track_src_t *pk_demo_track_current(void);

/* 某一时刻的本机状态。角度单位度、高度 ft、速度 kt、升降率 fpm。 */
typedef struct {
    double lat, lon;
    float  alt_ft;
    float  gs_kt;
    float  track_deg;     /* 真航迹，0..360 */
    float  roll_deg;
    float  pitch_deg;     /* 航迹倾角（由 VS / 地速反算），非机体迎角 */
    int    vs_fpm;
    bool   reverse;       /* 当前处在回程段（见下面的往返播放说明） */
} pk_demo_track_state_t;

/*
 * t_us（调用方自己的单调时钟，通常是开机至今的微秒）→ 本机状态。
 *
 * ── 循环点：往返播放，不瞬移 ─────────────────────────────────────────
 * 走到终点后**倒着飞回起点**，而不是跳回起点重来。理由是窗口：从北京瞬移回
 * 广州，1000 NM 的跳变会让 pk_win 的整个格集合一次性全部失效——48 个槽全淘汰
 * 再全部重加载，那一下的 IO 尖峰既不真实，也会把让路计数的观测污染成一次性
 * 的大爆发。倒放没有任何不连续：航迹 +180°、坡度取反、升降率取反之后，得到的
 * 是一次沿同一条地面航迹的返程飞行（下降段变成爬升段），物理上完全成立。
 * 掉头发生在停机坪上、地速接近 0 的那一刻，航向翻转不刺眼。
 *
 * 表为空时返回 false 且不动 *out —— 调用方据此退回旧的固定位置行为。
 */
bool pk_demo_track_at(int64_t t_us, pk_demo_track_state_t *out);

#ifdef __cplusplus
}
#endif
