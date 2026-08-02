/*
 * pk_aero_layer.h — 地图页的航空数据叠加层（机场 / 导航台 / FIX）。
 *
 * 设计依据 docs/internal/2026-08-02-aero-map-search-design-zh_CN.md §2。
 * 一句话架构：**后台任务查库写快照，渲染线程只读快照**。
 *
 *   [aero_layer 任务，prio 2/core 0]        [map_page render，30 FPS]
 *     视图变化超阈值 → pk_aero_db_nearest_*   pk_aero_layer_notify_view()
 *     → LOD 过滤 → snprintf 拷字符串         → pk_aero_layer_render_symbols()
 *                                            → …ADS-B… → …_render_labels()
 *     → 写非活动快照缓冲 → 翻转 s_active        只读快照，绝不调 pk_aero_db_*
 *
 * 为什么必须这么分：pk_aero_db 的 nearest 系列是**毫秒级**（东京最坏
 * ~16 ms，见 pk_aero_db.h:16-18），而地图页每帧只有 33 ms 预算——查库进
 * 渲染循环就是直接掉帧。同理，查询返回的 name/ident 是指向 PSRAM 池的裸
 * 指针，拔卡即悬空（pk_aero_db.h:23-25），所以快照里存的一律是拷贝。
 *
 * 未 READY（没卡 / 没 bin / 还在加载）时 render 静默返回，不画也不报错——
 * 与地图页其它降级路径一致：数据没有就什么都别画，别画一个我编出来的机场。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 标签避让占位矩形。与 map_page.c 的 rect_t 是**同一个类型**（那边直接
 * typedef 过来），这样两个图层共用一个占位池，不必走 void* 丢类型。
 * 定义在这里而不是 map_page.h，是因为占位池的语义属于"谁画标签谁定"，
 * 而本层是后加入的那个，让先来的 map_page 复用更省改动。 */
typedef struct { int x0, y0, x1, y1; } pk_aero_rect_t;

/* 本层最多往占位池里塞多少条。**两倍**于快照三类之和的上限（3×32=96）：
 * 符号趟每个上屏的要素登记一条包围盒，标签趟每条标签再登记一条。
 * 调用方按「自己的需要 + 这个数」定池子大小。
 * 代价是 map_page 那个静态池从 161 条涨到 257 条（rect 16 B，+1.5 KB 内部
 * .bss）——比起"标签压着符号"，1.5 KB 买得值；真要省，省的应该是 LOD 的
 * limit（那才是把 96 这个数摆上来的源头），不是省避让的正确性。 */
#define PK_AERO_LAYER_OCC_MAX  192

/* ── 点击地理要素（2026-08-02 加）────────────────────────────────────
 *
 * 命中判定的**候选只能来自本层已发布的快照**，绝不能在触摸回调里查库：
 * pk_aero_db 的 nearest 系列最坏 16 ms（pk_aero_db.h:16-18），而触摸回调
 * 跑在 pfd 渲染任务里，一次点击掉半帧还算轻的。快照里本来就存着"这一屏
 * 画了哪些要素"，正是命中测试要的那份候选集。 */
typedef enum {
    PK_AERO_LAYER_KIND_AIRPORT = 0,
    PK_AERO_LAYER_KIND_NAVAID,
    PK_AERO_LAYER_KIND_FIX,
} pk_aero_layer_kind_t;

typedef struct {
    uint8_t  kind;        /* pk_aero_layer_kind_t */
    uint32_t idx;         /* 段内记录下标，供详情页回查跑道/频率 */
    double   lat, lon;
    char     code[8];     /* ICAO / ident；机场无码时为 ""（快照里就是空的）*/
} pk_aero_layer_hit_t;

/* PK_AERO_LAYER_SIM_IMPL：sim 也要真的画这一层（配色评审用）。由
 * sim/CMakeLists.txt 定义，数据来自 sim/compat/pk_aero_layer_sim.c 桩掉的
 * pk_aero_db_*。固件侧两个宏都没有定义，走的还是原来那条路，行为不变。 */
#if !defined(PK_SIM_BUILD) || defined(PK_AERO_LAYER_SIM_IMPL)

/* 创建后台快照任务。须晚于 pk_aero_db_init()（本层只是它的消费者）。幂等。 */
void pk_aero_layer_init(void);

/* 渲染线程每帧告知当前视图。**极廉价**：只存值 + 置 dirty，不做任何计算、
 * 不查库、不加锁——真正的判阈值与查询都在后台任务里。 */
void pk_aero_layer_notify_view(double center_lat, double center_lon, uint8_t zoom);

/* 把快照画到 fb 上，**分两趟**，中间由 map_page 插进 ADS-B 目标：
 *
 *     render_symbols()   ← 静态设施垫在飞机之下
 *     [ADS-B 符号 + 呼号] ← 交通优先占位
 *     render_labels()    ← 机场/导航台/FIX 标签让路
 *
 * 顺序不是审美问题：一屏 5 个飞机时，"静态先占"会把呼号一个不剩地挤掉
 * （2026-08-02 实测），而机场标签被挤掉时符号还在、位置不丢。航空安全上
 * 交通信息优先——机场不会撞你，飞机会。设计文档 §2.4 的"静态先占"作废。
 *
 * 占位池只有标签趟用，进出都是池内已占用条数。标签趟开头会把本层每个上屏
 * **符号**的包围盒也塞进池子（解决"长标签横穿压住邻近符号"），所以池子容量
 * 要按 PK_AERO_LAYER_OCC_MAX 给足。
 * 符号趟不碰占位池是有意的：登记若放在符号趟，ADS-B 呼号就得躲机场符号，
 * 目标压在机场上时呼号会被整批挤掉——那正是要修的问题。
 *
 * 两趟共享同一份快照：符号趟把当时的快照记下来给标签趟用，所以**必须成对
 * 按序调用**（symbols 在前）。单独调 labels 什么都不画。 */
void pk_aero_layer_render_symbols(uint16_t *fb, double center_lat, double center_lon,
                                  uint8_t zoom);
void pk_aero_layer_render_labels(uint16_t *fb, double center_lat, double center_lon,
                                 uint8_t zoom, pk_aero_rect_t *occ, int *nocc,
                                 int occ_max);

/* 屏幕坐标 → 当前快照里最近的那个要素。命中返回 true 并写 *out。
 *
 * 投影参数必须与**本帧渲染用的那一份**一致（地图页把它自己的 s_center_* /
 * s_zoom 传进来），否则"看得见的"与"点得中的"会错位。
 * 容差与优先级见 pk_aero_hit_pick 的注释。未 READY / 无快照时返回 false，
 * 调用方据此走原来的空白点击路径——**绝不能把没命中的点击吞掉**。 */
bool pk_aero_layer_hit_test(int x, int y, double center_lat, double center_lon,
                            uint8_t zoom, pk_aero_layer_hit_t *out);

#else  /* PK_SIM_BUILD 且未开 PK_AERO_LAYER_SIM_IMPL */

/* host 预览（sim/）没有 SD、没有 FreeRTOS，本层整体空转。做成 inline 空实现
 * 而不是让 map_page.c 里撒 #ifdef——照 compat/pk_tile_loader_sim.c 的先例，
 * 桩的成本应当由 sim 侧承担，页面代码保持一份。真要在 sim 里预览这一层，
 * 加 sim/compat/pk_aero_layer_sim.c 喂假快照即可。 */
static inline void pk_aero_layer_init(void) { }
static inline void pk_aero_layer_notify_view(double lat, double lon, uint8_t z)
{ (void)lat; (void)lon; (void)z; }
static inline void pk_aero_layer_render_symbols(uint16_t *fb, double lat, double lon,
                                                uint8_t z)
{ (void)fb; (void)lat; (void)lon; (void)z; }
static inline void pk_aero_layer_render_labels(uint16_t *fb, double lat, double lon,
                                               uint8_t z, pk_aero_rect_t *occ,
                                               int *nocc, int occ_max)
{ (void)fb; (void)lat; (void)lon; (void)z; (void)occ; (void)nocc; (void)occ_max; }
static inline bool pk_aero_layer_hit_test(int x, int y, double lat, double lon,
                                          uint8_t z, pk_aero_layer_hit_t *out)
{ (void)x; (void)y; (void)lat; (void)lon; (void)z; (void)out; return false; }

#endif /* PK_SIM_BUILD */

/* ── 以下是纯函数区：无 OS / 无全局状态，host 单测直接编
 * （firmware/test/test_pk_aero_layer.c）。 ─────────────────────────── */

/* LOD 档位（设计文档 §2.2 的表）。limit=0 表示该类整个不画。 */
typedef struct {
    uint8_t  airport_limit;
    uint16_t airport_min_rwy_ft;  /* 跑道长度门槛；管制机场可豁免 */
    bool     airport_need_icao;   /* 低 zoom 只画有 ICAO 码的（≈有规模的场）*/
    uint8_t  navaid_limit;
    bool     navaid_vor_only;     /* 只画 VOR/VOR_DME/VORTAC */
    uint8_t  fix_limit;
    bool     fix_enroute_only;    /* 只画航路 FIX，不画进近 FIX */
    uint8_t  label;               /* 0=不画 1=代码 2=代码+名称 3=+标高 */
} pk_aero_lod_t;

pk_aero_lod_t pk_aero_lod_for_zoom(uint8_t zoom);

/* LOD 定的是"这个 zoom 最多显示到哪一档"，这里再按**当前屏上要素个数**往下
 * 压：>10 个不给标高，>20 个只留代码。同一个 zoom 下珠三角和塔克拉玛干的
 * 密度差一个数量级，光看 zoom 收不住。lod_label==0 恒返回 0。 */
uint8_t pk_aero_label_mode(uint8_t lod_label, int n_visible);

/* VOR 系导航台外面那圈罗盘玫瑰画不画（zoom ≥ 11 且不挤时才画）。
 * n_rose_navaid = 屏上 VOR/VOR-DME/VORTAC 的个数——只有它们有玫瑰，
 * NDB 不给方位、单独的 DME/TACAN 只给距离，航图上都不带这个圈。 */
bool pk_aero_rose_enabled(uint8_t zoom, int n_visible, int n_rose_navaid);

/* 三个准入判定。字段取值与 export_box_bin.py 的枚举表一一对应
 * （AIRPORT_TYPE_ENUM / CTRL_ENUM / NAVAID_TYPE_ENUM / FIX_SCOPE_ENUM）。 */
bool pk_aero_lod_airport_pass(const pk_aero_lod_t *lod, uint8_t type, uint8_t ctrl,
                              uint16_t longest_rwy_ft, bool has_icao);
bool pk_aero_lod_navaid_pass(const pk_aero_lod_t *lod, uint8_t type);
bool pk_aero_lod_fix_pass(const pk_aero_lod_t *lod, uint8_t scope);

/* Web Mercator 经纬度 → 世界像素。
 * **与 map_page.c:110 的 lonlat_to_world 数学完全一致（含 ±85.0511 钳位），
 * 改一处必须改两处。** 没有提炼成共享 helper 是有意的：map_page.c 里那份是
 * static，导出它会让地图页的改动面变大，而本层与地图页并行开发中——一个
 * 十行的纯函数复制一份，比两边抢改同一个文件划算。 */
void pk_aero_lonlat_to_world(double lon, double lat, uint8_t z,
                             double *wx, double *wy);

/* 贪心标签避让（设计文档 §2.4，照抄 App 规则）：以符号 (sx,sy) 为锚，按
 * bottom → right → left → top 四方向依次试位，**零重叠即停**；四个方向都
 * 撞就取重叠面积最小的那个，若其重叠仍 > 标签面积的 50% 则返回 false
 * （整个标签隐藏，只画符号）。命中时把选中的矩形写进 *out。 */
bool pk_aero_label_place(int sx, int sy, int sym_r, int lw, int lh,
                         const pk_aero_rect_t *occ, int nocc,
                         pk_aero_rect_t *out);

/* 命中测试候选：只有"在屏幕上的哪个点"与"是什么类"，不带任何数据库信息，
 * 所以这一段是纯的，host 单测直接跑（test_pk_aero_layer.c）。 */
typedef struct {
    int16_t sx, sy;
    uint8_t kind;      /* pk_aero_layer_kind_t */
} pk_aero_hit_cand_t;

/* 触摸容差（px）。App 用 20–28，盒子取**上限 28**：GT911 单点电容屏的定位
 * 抖动比手机大，且座舱里戴手套/颠簸时手指落点更散。28 px ≈ 3.3 mm，仍远小于
 * 9 mm 的触摸目标下限，不会让两个相邻机场互相抢。 */
#define PK_AERO_HIT_TOL_PX  28

/*
 * 在候选里挑一个：**机场优先，其次距离最近**。返回下标，无命中返回 -1。
 *
 * 两级规则而不是单纯"取最近"：密集区（珠三角 Z11 一屏能有十几个 FIX）里
 * FIX 三角形只有 8 px，常常压在机场圆盘边上；纯取最近的话，对准 ZGGG 点下去
 * 十有八九弹出旁边那个 FIX。机场是这一层里唯一有详情可看的东西，让它赢。
 * 同类之间比的是**平方距离**（不开方，整数运算，结果与比距离等价）。
 */
int pk_aero_hit_pick(const pk_aero_hit_cand_t *cands, int n, int x, int y, int tol);

#ifdef __cplusplus
}
#endif
