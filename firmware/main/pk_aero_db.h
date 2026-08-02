/*
 * pk_aero_db.h — SD 卡航空数据库（/sdcard/aero/pk_aero.bin）懒加载 + 查询。
 *
 * 设计定案（docs/internal/2026-08-01-sd-aero-db-design-zh_CN.md）：
 *   - 开机不加载。低优先级后台任务等 UI 起来、SD 挂载后再分块 fread +
 *     PSA 流式 AES-128-CTR 解密到 PSRAM，SHA-256 校验通过才置 READY，
 *     全程不挡 PFD 渲染（v2 全量库 9.88 MB 实测加载约 2 s）。
 *   - 拔卡 → 释放缓冲、回 ABSENT；重插 → 自动重加载。文件不存在时保持
 *     ABSENT 并周期性重试（卡后拷文件、插拔换卡都能自动跟进）。
 *   - 查询 API 是 pk_aero_reader 的薄封装：未 READY 一律返回 -1/0/false，
 *     调用方不需要自己看状态再查。
 *
 * 性能约定（p4_bench 真机实测，360 MHz）：
 *   - by_icao / fix_by_ident / 聚簇区间读取：µs 级（6–10 µs），随便用。
 *   - nearest 系列：**毫秒级**（approx 模式全球 p95 数 ms，东京最坏
 *     ~16 ms）——只许后台任务 / 事件路径调用，禁止渲染循环同步调。
 *     距离模式固定 approx（等距柱状粗排 + Haversine 精算）：P4 无 double
 *     FPU，exact 全 Haversine 东京最坏 avg 46 ms 会卡帧，不暴露。
 *
 * 并发约定：
 *   - 每次查询全程持模块内部 mutex，卸载（拔卡）也要先拿同一把锁再释放
 *     缓冲——查询进行中绝不会踩到 free 掉的内存。
 *   - 但记录里的 name/city/callsign 是指向 PSRAM 缓冲的 const 指针，
 *     查询返回后锁已放开：**拿到就用（本帧内 snprintf/拷走），禁止跨帧
 *     缓存指针**，否则拔卡后就是悬空引用。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pk_aero_reader.h"   /* 记录/结果类型（pk_aero_airport_t 等） */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PK_AERO_DB_ABSENT = 0,   /* 无卡 / 无文件（周期性重试中） */
    PK_AERO_DB_LOADING,      /* 后台加载 + 解密 + 校验进行中 */
    PK_AERO_DB_READY,        /* 校验通过，可查询 */
    PK_AERO_DB_ERROR,        /* 文件坏/校验失败等（原因见 status.err；
                              * 拔卡重插后自动重试） */
} pk_aero_db_state_t;

/* 诊断页快照：无锁读取（字段只在发布/卸载两个瞬间变化），渲染路径每帧
 * 调也不会被后台 nearest 查询的持锁窗口卡住。 */
typedef struct {
    pk_aero_db_state_t state;
    char     cycle[9];       /* AIRAC 周期，如 "2026-02"；未 READY 为 "" */
    uint16_t version;        /* bin 格式版本 2/3；未 READY 为 0。用户换卡后
                              * 靠它确认新库生效（v3 才有搜索索引） */
    uint32_t n_airports;
    uint32_t n_navaids;
    uint32_t n_fixes;
    uint8_t  load_pct;       /* LOADING 时 0–100，其余无意义 */
    const char *err;         /* ERROR 时的简短原因（静态串），否则 NULL */
} pk_aero_db_status_t;

/* 只创建后台加载任务，不做任何 IO——开机零阻塞。
 * 须在 pk_sdcard_init() 之后调用（任务靠 pk_sdcard_is_mounted() 决定
 * 何时开始加载）。幂等。 */
void pk_aero_db_init(void);

pk_aero_db_state_t pk_aero_db_state(void);

/* 数据周期（"2026-02"）。未 READY 返回 ""。诊断/关于页用。 */
const char *pk_aero_db_cycle(void);

void pk_aero_db_status_get(pk_aero_db_status_t *out);

/* ---- 查询（薄封装；未 READY 返回 -1/0/false）---------------------- */

/* ICAO 精确查找 → 机场记录下标，未命中/未就绪 -1。µs 级。 */
int32_t pk_aero_db_airport_by_icao(const char *code);

/* 记录读取（idx 越界或未就绪返回 false）。 */
bool pk_aero_db_airport_get(uint32_t idx, pk_aero_airport_t *out);
bool pk_aero_db_rwy_dir_get(uint32_t idx, pk_aero_rwy_dir_t *out);
bool pk_aero_db_freq_get(uint32_t idx, pk_aero_freq_t *out);
bool pk_aero_db_navaid_get(uint32_t idx, pk_aero_navaid_t *out);
bool pk_aero_db_fix_get(uint32_t idx, pk_aero_fix_t *out);

/* 机场的跑道/频率聚簇区间 [first, first+count)，配套 *_get 逐条读。 */
bool pk_aero_db_airport_runways(uint32_t apt_idx,
                                uint32_t *first, uint32_t *count);
bool pk_aero_db_airport_freqs(uint32_t apt_idx,
                              uint32_t *first, uint32_t *count);

/* nearest 系列：写入 out 的条数（≤ max ≤ PK_AERO_NEAR_MAX）。
 * ！毫秒级（东京最坏 ~16 ms）——只许后台/事件路径，禁止渲染循环。 */
int pk_aero_db_nearest_airports(double lat, double lon,
                                pk_aero_near_t *out, int max);
int pk_aero_db_nearest_navaids(double lat, double lon,
                               pk_aero_near_t *out, int max);
int pk_aero_db_nearest_fixes(double lat, double lon,
                             pk_aero_near_t *out, int max);

/* FIX ident 精确查找：命中下标写入 out[]（最多 max 个），返回同名总条数
 * （可能 > max）；未命中/未就绪 0。 */
int pk_aero_db_fix_by_ident(const char *ident, uint32_t *out, int max);

/* 导航台 ident 精确查找（v3 索引）。语义同 fix_by_ident。
 * ！**v2 卡上恒返回 0**（v2 的导航台第二索引是空表）。 */
int pk_aero_db_navaid_by_ident(const char *ident, uint32_t *out, int max);

/* ---- 前缀枚举（µs 级，随便用；写满 max 就停，返回写入条数）---------
 * 三个都走定长键排序表二分 + 线性扫，实测 P4 上与 by_icao 同量级。
 * 版本差异（换卡窗口期必须能忍）：
 *   - airports_by_prefix：v3 的机场全量 key 索引 → **v2 卡返回 0**；
 *   - navaids_by_prefix ：v3 才填的导航台 ident 索引 → **v2 卡返回 0**；
 *   - fixes_by_prefix   ：吃的是 v2 就有的 FIX ident 索引 → 两版都正常。
 * 想知道当前是哪版看 pk_aero_db_status_get().version。 */
int pk_aero_db_airports_by_prefix(const char *prefix, uint32_t *out, int max);
int pk_aero_db_navaids_by_prefix(const char *prefix, uint32_t *out, int max);
int pk_aero_db_fixes_by_prefix(const char *prefix, uint32_t *out, int max);

/* ---- 子串搜索（名称/城市/IATA，分段让渡）--------------------------
 *
 * 唯一一个**不能**全程持锁的查询：顺扫 2.2 MB 字符串池，P4 真机实测 65 ms，
 * 而 s_lock 同时罩着地图叠加层的后台快照查询与拔卡卸载路径。这里按设计文档
 * §3.4 的方案一实现：每扫 64 KB give/take 一次锁，**每段开头复检
 * state + 挂载代数**，卡被拔掉/换过就整体放弃（半截结果属于上一张卡，
 * 不能拿来显示）。
 *
 * 因此它是**毫秒到百毫秒级**的：只许后台搜索任务调用，禁止渲染循环与触摸
 * 回调。写满 max 就停并返回条数；v2 卡（无搜索索引）恒返回 0。 */
int pk_aero_db_search_substring(const char *query, pk_aero_hit_t *out, int max);

/* 库代数：每次发布（加载成功）与卸载（拔卡）各 +1。分段查询靠它识别
 * "手里这份游标是不是上一张卡的"；UI 侧也可用它决定要不要重查。 */
uint32_t pk_aero_db_generation(void);

#ifdef __cplusplus
}
#endif
