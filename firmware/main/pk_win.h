/*
 * pk_win.h — 以本机为中心的滚动窗口：**状态机 + 增量加载 + 驻留内存**。
 *
 * 设计依据 docs/internal/2026-08-03-window-based-data-architecture-zh_CN.md，
 * 本模块落地的是 §5 的 **W1 阶段**（窗口骨架，只做机场/导航台/FIX）：
 *   W1.1 格集合求交 + 48 槽表 + 1.3× 迟滞 + 60 s 最短驻留   → 本文件 + pk_win_geom
 *   W1.2 常驻文件句柄 + pre-unmount 回调                     → pk_aero_span
 *   W1.3 按格区间读 + AES-CTR 随机解密                       → pk_aero_span
 *   W1.6 让路规则 R1–R4 + pk_tile_loader_pending()           → 本文件
 * **不在本轮**：W1.4/W1.5 把 nearest_* 与地图快照的数据源切过来。
 *
 * ── 与 pk_aero_db 全量加载路径的关系：并存，不替换 ────────────────────
 * pk_aero_db 一个字节都没动，仍然把整份 bin 读进 PSRAM，所有既有查询照走它。
 * 窗口是**另开一条读路**（另一个只读句柄、另一套按格区间读）。理由三条：
 *   1. 文档 §5 给 W1 定的回滚粒度就是"一个编译开关，pk_win 不启用即回到
 *      全量加载路径（代码不删）"——那就要求两条路能同时存在；
 *   2. 并存期间窗口读出来的记录可以**逐字段和 pk_aero_db 的查询结果对拍**，
 *      这是"区间读 + 随机解密对不对"最强的证据，比任何单测都硬（见
 *      PK_WIN_SELFTEST 的 verify_against_full_db）；
 *   3. 本轮不动 UI/nearest 的数据源，用户看到的东西完全不变，风险面为零。
 * 真正的替换（以及随之而来的常驻内存收益）发生在 W1.4/W1.5，不在本轮。
 *
 * ── 内存组织 ──────────────────────────────────────────────────────────
 * 一个格 = **一次 heap_caps_malloc(SPIRAM) 的整块**，该格所有段拼在这一块
 * 里用偏移访问，释放也是一次 free（文档 §1.6 对碎片风险 R3 的处方：
 * "分配粒度大而稀疏"）。48 个槽的元数据在 PSRAM .bss。
 *
 * ── 并发 ──────────────────────────────────────────────────────────────
 * 单后台任务（prio 2 / core 1，与 pk_aero_db 同档）1 Hz 推进；对外的读接口
 * 要求调用方自己 pk_win_lock() / pk_win_unlock()，因为返回的是**指向格
 * 大块的裸指针**——锁一放开那块内存就可能被淘汰掉。
 * 红线不变：**pfd 任务永不触发 SD IO**，所以 UI 侧只许读快照，不许调
 * 会触发加载的东西（本模块对外也没有那种 API）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pk_win_geom.h"
#include "pk_aero_reader.h"   /* PK_AERO_SEC_* / pk_aero_airport_t 等 */

#ifdef __cplusplus
extern "C" {
#endif

/* 编译开关（W1 的回滚粒度）：置 0 则 pk_win_init() 变成空函数，
 * 一行代码都不跑，回到纯 pk_aero_db 全量加载路径。 */
#ifndef PK_WIN_ENABLE
#define PK_WIN_ENABLE   1
#endif

/*
 * 真机自检（默认关闭）。打开后窗口任务会依次把中心喂到内蒙稀疏 / 北京典型 /
 * 珠三角密集三处，每次推进打印：进/出格数、加载字节、耗时、驻留内存、
 * PSRAM 余量，并把窗口读出的机场记录与 pk_aero_db 全量路径逐字段对拍。
 * **只在验收时打开**——它会强行改写窗口中心，与真实本机位置无关。
 */
#ifndef PK_WIN_SELFTEST
#define PK_WIN_SELFTEST 1
#endif

/* 诊断快照（无锁读；字段只在窗口任务里写）。 */
typedef struct {
    bool     enabled;
    bool     open;             /* pk_aero_span 句柄已打开 */
    uint16_t version;          /* bin 版本 2/3/4 */
    uint8_t  n_cells;          /* 驻留格数（含加载中） */
    uint8_t  n_ready;
    uint8_t  win_cells;        /* 最近一次求交出的窗口格数 */
    bool     truncated;        /* 窗口格数触顶（高纬） */
    bool     circle;           /* 当前是退化圆还是椭圆 */
    uint32_t bytes;            /* 驻留数据总字节（不含 48 槽元数据） */
    uint32_t n_apt, n_nav, n_fix;
    uint32_t last_in, last_out;      /* 最近一次推进的进 / 出格数 */
    uint32_t loads, evicts, load_fail;
    uint32_t str_skipped;            /* 因跨度超限被放弃的字符串片段格数 */
    uint32_t yields;                 /* R1 让路（每 100 ms 一次计一次） */
    uint32_t forced;                 /* 让路超时后的强读次数 */
    uint32_t last_load_us;           /* 最近一次整格加载耗时 */
    uint64_t bytes_read;             /* span 累计读盘字节 */
    uint32_t psram_free, psram_largest;
    double   lat, lon, track_deg;
} pk_win_status_t;

/* 创建窗口任务（不做任何 IO）。须在 pk_sdcard_init() 之后调用。幂等。 */
void pk_win_init(void);

void pk_win_status_get(pk_win_status_t *out);

/* ---- 读接口：必须夹在 lock/unlock 之间使用 ---- */
void pk_win_lock(void);
void pk_win_unlock(void);

/* 取某格某段的记录块。sec_type ∈ {PK_AERO_SEC_AIRPORTS, _NAVAIDS,
 * _WAYPOINTS_FIX, _RUNWAY_DIRS, _FREQUENCIES}。
 * 该格不驻留 / 未 READY / 段为空时返回 false。
 * *out_first 是这一块第一条记录在**段内的全局下标**——窗口与搜索索引、
 * 与 pk_aero_db 共用的货币就是这个下标（文档风险 R9）。 */
bool pk_win_cell_records(uint16_t cell, uint16_t sec_type,
                         const uint8_t **out_recs, uint32_t *out_n,
                         uint32_t *out_first);

/* 窗口里当前驻留的格集合（升序）。 */
void pk_win_resident_cells(pk_win_cellset_t *out);

/* 地图视口 → "屏上可见的格永不卸载"（文档 §1.3 / §6.2 的缓解 2）。
 * 传 min_lat > max_lat 表示清除视口约束。渲染线程可随时调，纯内存。 */
void pk_win_set_viewport(double min_lat, double min_lon,
                         double max_lat, double max_lon);

/* 调试用：强制窗口中心（自检与 host 联调）。on=false 恢复跟随本机。 */
void pk_win_debug_override(bool on, double lat, double lon, double track_deg);

#ifdef __cplusplus
}
#endif
