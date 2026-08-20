/*
 * pk_win_geom.h — 以本机为中心的滚动窗口：**纯几何层**（纯 C11，零 OS 依赖）。
 *
 * 设计依据窗口化数据架构设计（内部文档）
 * 「几何定义」/「用现有 1° 网格求交」/「推进策略：格集合差分」三节。
 *
 * 本文件只做一件事：**给定 (lat, lon, track) 算出窗口压到哪些 1° 格**，
 * 以及格集合之间的进/出差集。零 IO、零分配、零锁——所以它能在 host 上
 * 逐点对拍（firmware/test/test_pk_win_geom.c）。
 *
 * 窗口形状是**沿地面航迹拉长的偏心椭圆**（前 100 / 后 40 / 侧 50 海里）。
 * 为什么不是圆：文档 §1.1 的实测表——同样 100 NM 前视距离，椭圆在最密集区
 * （费城）省 34%、苏黎世省 45%，而面积只有圆的 35%。
 *
 * 转弯 / 地速 < 2 kt / 地面 时退化为 60 NM 圆（文档 §1.4 三条保护）。
 * 退化圆在 7 个采样区的实测账**全部 ≤ 椭圆**，所以它是"更省"而不是"更贵"。
 *
 * 格编码复用 pk_aero_grid_cell()（pk_aero_reader.c）：row = floor(lat)+90
 * 钳位 [0,179]、col = (floor(lon)+180) % 360，cell = row*360+col。
 * **本文件不另造一套格编码**——两套编码不一致就是最难查的那类 bug。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 窗口参数（文档 §1.1 已定，改动前先看那张实测对比表）---- */
#define PK_WIN_A_FWD_NM      100.0   /* 前向半轴：产品已定 */
#define PK_WIN_A_AFT_NM       40.0   /* 后向半轴：给 180° 掉头留 40 NM 缓冲 */
#define PK_WIN_B_NM           50.0   /* 侧向半轴：实测拐点（40 打穿、60 白花） */
#define PK_WIN_CIRCLE_NM      60.0   /* 退化圆半径（转弯 / 低速 / 地面） */
#define PK_WIN_KEEP_SCALE      1.3   /* 卸载环倍率：13–20 NM 缓冲带 ≈ 5–8 min */

/*
 * 槽位上限 48。依据是文档 §1.2 的实测格数分布（每纬度带 200 个随机位置 ×
 * 随机航向）：55°N 的 max 是 15 格，乘 1.3× 卸载环 ≈ 25，48 给近 2× 余量。
 *
 * ⚠ 高纬（|lat| ≳ 85°）1° 格在经度方向被压扁，100 NM 会横跨几十个格，
 *   真实需求会超过 48。此时 pk_win_cells() **按到窗口中心的距离保留最近的
 *   48 个**并置 truncated=true（而不是随便截断成升序前 48 个——那会把北边
 *   一整片留下、把脚底下的格丢掉）。极区要素密度本就接近 0，接受此降级。
 */
#define PK_WIN_MAX_CELLS      48

/* ---- 窗口形状 ---- */
typedef struct {
    double lat, lon;        /* 中心（本机位置，或地面拖图时的视口中心） */
    double track_deg;       /* 真北地面航迹，circle=true 时忽略 */
    double a_fwd, a_aft;    /* 沿航迹前 / 后半轴（海里） */
    double b;               /* 侧向半轴（海里） */
    bool   circle;          /* true = 退化圆（半径取 b） */
} pk_win_shape_t;

/* 升序、互异的格集合。n <= PK_WIN_MAX_CELLS。 */
typedef struct {
    uint16_t cell[PK_WIN_MAX_CELLS];
    uint8_t  n;
    bool     truncated;     /* 候选超过容量，已按距离取最近的 n 个 */
} pk_win_cellset_t;

/* 标准偏心椭圆（100 / 40 / 50 NM）。 */
void pk_win_shape_ellipse(pk_win_shape_t *s, double lat, double lon,
                          double track_deg);
/* 退化圆（转弯 / 地速 < 2 kt / 地面）。radius_nm <= 0 时取 PK_WIN_CIRCLE_NM。 */
void pk_win_shape_circle(pk_win_shape_t *s, double lat, double lon,
                         double radius_nm);
/* 三个半轴同乘 k（k = PK_WIN_KEEP_SCALE 即得卸载环 W_keep）。 */
void pk_win_shape_grow(pk_win_shape_t *s, double k);

/* 点是否落在窗口内。椭圆判据（本机为原点、x = 航迹方向、y = 右侧向）：
 *     (x / a(x))² + (y / b)² <= 1 ，a(x) = a_fwd (x>=0) / a_aft (x<0) */
bool pk_win_shape_contains(const pk_win_shape_t *s, double lat, double lon);

/* 求交：窗口 → 升序格集合。返回格数（= out->n）。
 * 算法三步（文档 §1.2）：外接盒 → 逐格 5×5 采样精筛 + 中心落格兜底 →
 * 升序输出。全程浮点算术，零 IO。 */
int pk_win_cells(const pk_win_shape_t *s, pk_win_cellset_t *out);

/* 视口外接矩形 → 格集合（地面拖图 / "屏上可见的绝不卸载"用）。
 * 经度可跨 ±180（min_lon > max_lon 即视为跨日期变更线）。 */
int pk_win_cells_bbox(double min_lat, double min_lon,
                      double max_lat, double max_lon,
                      pk_win_cellset_t *out);

/* ---- 集合运算（升序不变式由这几个函数维持）---- */
void pk_win_cellset_clear(pk_win_cellset_t *s);
bool pk_win_cellset_has(const pk_win_cellset_t *s, uint16_t cell);
/* 插入并保持升序去重。已满且 cell 不在集合内时返回 false（不插）。 */
bool pk_win_cellset_add(pk_win_cellset_t *s, uint16_t cell);
/* out = a − b（进入集 / 移出集都用它）。返回条数。out 可以 == a。 */
int  pk_win_cellset_diff(const pk_win_cellset_t *a, const pk_win_cellset_t *b,
                         pk_win_cellset_t *out);
/* dst ∪= src，返回并入后的条数（溢出时静默丢弃多余项）。 */
int  pk_win_cellset_union(pk_win_cellset_t *dst, const pk_win_cellset_t *src);

/* 格 → 西南角经纬度（度）。cell 越界时写 0。诊断与单测用。 */
void pk_win_cell_sw_corner(uint16_t cell, double *out_lat, double *out_lon);

/* 点 (lat,lon) 到格矩形的最短距离（海里）；点落在格内返回 0。
 * 让路规则 R1/R2 的分界（"格边界距本机 > 30 NM"）靠它判。 */
double pk_win_cell_dist_nm(uint16_t cell, double lat, double lon);

#ifdef __cplusplus
}
#endif
