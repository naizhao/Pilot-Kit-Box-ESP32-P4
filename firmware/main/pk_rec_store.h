/*
 * pk_rec_store.h — ADS-B / 本机数据落盘：session 目录管理 + 文件生命周期。
 *
 * 设计依据 docs/internal/2026-08-02-adsb-data-persistence-design-zh_CN.md
 * 「落盘布局」「文件句柄预算」「记录格式」三节。本文件是**阶段 3a**的交付：
 * 只管 session 目录怎么建、文件怎么开/滚/关、SD 满了怎么降级——不接
 * dsp_task / own_ship / 相位状态机（3b 的事），本模块自己不产生任何记录
 * 数据，只提供 3b 会调用的 append 接口。
 *
 * session = 一次「SD 卡可用」的连续区间（近似「一次开机」，但拔卡再插回
 * 会开启新 session——旧 fd 已经在 pre-unmount 里被关掉，续录跟
 * record_sink_file.c 的处理方式一样：重新扫号开新的，不假装能接着旧文件
 * 写下去）。
 *
 * 文件句柄预算（设计文档「文件句柄预算」节）：本模块峰值同时打开
 * adsb-NNN.tsl / traffic.trk / own.trk / own_adsb.tsl 四个常开 fd，
 * traffic.idx / session.json 只在关闭时短开。pk_sdcard.c 的 max_files
 * 已从 8 提到 16 与此配套。
 *
 * pre-unmount 回调预算：pk_sdcard 的槽位只有 4 个且已被 pk_tile_loader /
 * record_sink_file / pk_aero_db / aircraft_db 用满（实测，非文档所写的
 * "已用 3"——见 pk_rec_store_pre_unmount() 的调用方注释）。本模块**不**
 * 调用 pk_sdcard_register_pre_unmount_cb，而是被 record_sink_file.c 的
 * sd_close_log_cb 转调，复用它已经占的那一个槽位。
 *
 * 纯逻辑（序号分配/回绕、保留策略选谁删、降级档位判定）与真机文件系统
 * 操作分层：前者不摸文件系统、可直接进 host 单测
 * （firmware/test/test_pk_rec_store.c 把本 .c #include 进同一 TU）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "pk_rec_idx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ 纯逻辑 */

/* session 序号空间：四位十进制目录名 "0000".."9999"。 */
#define PK_REC_STORE_SEQ_SPACE     10000u

/* SD 保留最近这么多个 session，开机 / 每次新建 session 时清理一次
 * （设计文档「容量与保留」节，待确认项 1：32 是初版取值）。 */
#define PK_REC_STORE_KEEP_SESSIONS 32

/* 给定「哪些序号已被占用」的位图（长度 PK_REC_STORE_SEQ_SPACE，
 * used[seq]==true 表示该目录已存在），返回下一个应使用的四位序号：
 *   - 正常情况：已用最大序号 + 1；
 *   - 已用最大序号顶到 9999（数字空间用满）：回绕，复用最小的空闲号
 *     （设计文档「落盘布局」节：」到 9999 回绕则复用最小空闲号」）；
 *   - 位图全空：返回 0；
 *   - 位图全满（10000 个 session 都在——保留策略在 32 这个量级就会把
 *     旧的删掉腾出号，这个分支只是防越界，不会在正常运行中触发）：
 *     退化返回 0，调用方会覆盖掉 "0000"。
 */
uint16_t pk_rec_store_alloc_seq(const bool used[PK_REC_STORE_SEQ_SPACE]);

/* 保留策略：给定当前存在的 session 序号数组 seqs[n]（无需去重/排序），
 * 挑出要删除的——数值最小的那些，删到只剩 keep 个（keep<=0 视为 0，
 * 即全删）。写入 out[]（调用方保证容量 >= n），返回写入个数。
 *
 * 「数值最小 = 最旧」这个简化假设照抄 record_sink_file.c:143-169
 * prune_oldest 的做法：序号是顺序分配的，只有在 session 数远超 keep
 * （四位数量级）时才会因回绕失真，这个量级下保留策略早就该报警了，
 * 不是这里要处理的问题。
 */
size_t pk_rec_store_select_prune(const uint16_t *seqs, size_t n, int keep,
                                  uint16_t *out);

/* SD 剩余空间 → 写入降级档位（设计文档「SD 满 / 写失败降级」表）。 */
typedef enum {
    PK_REC_DEGRADE_FULL = 0,   /* > 500 MB：全部写 */
    PK_REC_DEGRADE_NO_RAW,     /* 100–500 MB：停写 adsb-NNN.tsl，保留 traffic.trk / own.trk */
    PK_REC_DEGRADE_OWN_ONLY,   /* < 100 MB：只保留 own.trk */
} pk_rec_degrade_t;

pk_rec_degrade_t pk_rec_store_degrade_tier(uint64_t free_bytes);

/* 某一路 sink 连续写失败次数达到这个阈值即判定失效、停止重试
 * （设计文档「写失败连续 N 次 → 标记该路 sink 失效」，N 未定案，取 8：
 * 落盘失效要比 record_sink_file 现有的 0xFF=256 次丢弃日志节流更快
 * 被感知——256 次持续失败才吭声，诊断页早该报错了）。 */
#define PK_REC_STORE_FAIL_THRESHOLD 8u
bool pk_rec_store_sink_should_disable(uint32_t consecutive_fail_count);

/* ------------------------------------------------------------ session.json 的 counts/bounds/own_icao_changes（阶段 5b）
 *
 * 设计文档「session.json」节：`counts{} / bounds{minLat,maxLat,minLon,maxLon}
 * / own_icao_changes[]` 三个字段此前（3a/3b）是占位。这三块本身逻辑很薄
 * （累加/求外包框/定长追加），但薄也是逻辑——照本文件的分层原则，能不摸
 * 文件系统的都留在这个纯 C 结构体里，真机专属的 FreeRTOS/VFS 胶水
 * （静态实例、递增调用点、JSON 序列化）留给 pk_rec_store_fs.c。 */

/* counts：各文件已写记录数。字段名与落盘文件一一对应。 */
typedef struct {
    uint32_t traffic_pos;      /* traffic.trk 位置记录(rec_type=0)数 */
    uint32_t traffic_id;       /* traffic.trk 身份记录(rec_type=1)数 */
    uint32_t own;               /* own.trk 记录数(采样+校时) */
    uint32_t adsb_lines;        /* adsb-NNN.tsl 行数(累计跨卷) */
    uint32_t own_adsb_lines;    /* own_adsb.tsl 行数 */
} pk_rec_counts_t;

/* bounds：本 session 见过的目标（traffic）位置外包框，1e-7 度定点——
 * 与 pk_trk_pos_t.lat_e7/lon_e7 同尺度，序列化成 JSON 时才转浮点度数。
 * 键名照抄客户端 coverage_bounds 约定：{minLat,maxLat,minLon,maxLon}。
 * has_any=false 表示这个 session 还没写过一条位置记录——序列化成 JSON
 * null，不能编造一个 0,0 的假包框。 */
typedef struct {
    bool    has_any;
    int32_t min_lat_e7, max_lat_e7;
    int32_t min_lon_e7, max_lon_e7;
} pk_rec_bounds_t;

void pk_rec_bounds_reset(pk_rec_bounds_t *b);
void pk_rec_bounds_update(pk_rec_bounds_t *b, int32_t lat_e7, int32_t lon_e7);

/* own_icao_changes[]：绑定机变更历史（改绑/解绑各记一条）。飞行中改绑
 * 次数是个位数量级，定长数组够用；满了就不再追加——丢新不丢旧，开局那次
 * 绑定最重要，顶到上限说明用户在反复点错，不是本模块要兜底的场景。 */
#define PK_REC_OWN_ICAO_CHANGES_MAX 16u

typedef struct {
    int64_t ts_ms;
    bool    bound;        /* true=绑定/改绑到 icao24；false=解绑 */
    uint8_t icao24[3];     /* bound=false 时无意义，恒为 0 */
} pk_rec_own_icao_change_t;

typedef struct {
    pk_rec_own_icao_change_t entries[PK_REC_OWN_ICAO_CHANGES_MAX];
    size_t count;
} pk_rec_own_icao_changes_t;

void pk_rec_own_icao_changes_reset(pk_rec_own_icao_changes_t *c);

/* 追加一条变更记录；数组已满则丢弃本次追加、不覆盖旧的。返回是否真的
 * 写入了（host 单测用它断言"满了就不再长"）。 */
bool pk_rec_own_icao_changes_append(pk_rec_own_icao_changes_t *c, int64_t ts_ms,
                                     bool bound, const uint8_t icao24[3]);

/* ------------------------------------------------------------ 诊断页只读状态（阶段 5b） */

/* diag_page.c LOG 卡片接入用的只读快照：当前降级档位 + 判定失效的 sink 数
 * （0..4）。不做告警触发——那由 pk_rec_store_fs.c 内部状态跳变时调
 * pk_ui_toast_show_blink，这里只给"持续状态"（设计文档「告警呈现」节：
 * "持续状态放到诊断页 LOG 卡片常驻显示"）。真机实现在 pk_rec_store_fs.c
 * （要摸 FreeRTOS 锁），host 测不到，同 pk_rec_store_rebuild_index 的分层。 */
typedef struct {
    pk_rec_degrade_t tier;
    uint8_t           disabled_count;
} pk_rec_store_health_t;

void pk_rec_store_get_health(pk_rec_store_health_t *out);

/* ------------------------------------------------------------ 文件系统 / 生命周期 */

/* 幂等；须晚于 pk_sdcard_init()。无卡时只记"待创建"状态，探测任务发现
 * 卡挂载后自动补建 session 目录（同 record_sink_file 的续录模式）。 */
void pk_rec_store_init(void);

/* pk_sdcard 卸载前静默：由 record_sink_file.c 的 sd_close_log_cb 转调
 * （见本文件头部说明——4 槽已满，不再注册第 5 个）。写一份完整
 * session.json（尽力而为），关闭本模块所有打开的 SD fd，不做其它 I/O。
 * 调用方须在自己的锁内调用（同 sd_close_log_cb 的既有约定）。 */
void pk_rec_store_pre_unmount(void);

/* 当前 session 目录的完整路径（如 "/sdcard/rec/0007"）。没有打开的
 * session（无卡 / 尚未探测到卡）时返回 false，out 不作保证。 */
bool pk_rec_store_session_dir(char *out, size_t cap);

/* 绑定本机 ICAO24（3 字节）。首次绑定/改绑时打开 own_adsb.tsl（换绑先关
 * 旧的）；传 NULL 解绑并关闭当前专属文件。3b（own_ship 绑定逻辑）调用。 */
void pk_rec_store_set_own_icao(const uint8_t icao24[3]);

/* 追加一行原始报文到 adsb-NNN.tsl（ts-line 格式，调用方已拼好整行含
 * 换行符）。按需打开、按 16 MiB 阈值滚动到下一卷。降级到 NO_RAW/
 * OWN_ONLY 档时直接返回 false、不落盘。 */
bool pk_rec_store_append_adsb_line(const char *line, size_t len);

/* 追加一行到绑定机专属 own_adsb.tsl。未绑定（从未调用过
 * pk_rec_store_set_own_icao）时返回 false，不创建文件。 */
bool pk_rec_store_append_own_adsb_line(const char *line, size_t len);

/* 追加一条 traffic.trk 定长记录（PK_TRK_RECORD_LEN=32 B，调用方已用
 * pk_rec_format 编码好）。 */
bool pk_rec_store_append_traffic_record(const uint8_t rec[32]);

/* 追加一条 own.trk 定长记录（PK_OWN_RECORD_LEN=48 B）。降级到 OWN_ONLY
 * 档之下（<100MB 仍写，只是它是最后被停写的一路；真到磁盘写失败由
 * 调用方按 pk_rec_store_sink_should_disable 处理）。 */
bool pk_rec_store_append_own_record(const uint8_t rec[48]);

/* --- traffic.idx（3b：构建 + 掉电重建） ------------------------------- */

/* fflush 所有当前打开的 fd（adsb / own_adsb / traffic.trk / own.trk），
 * 不 fclose。用于注入式自检（pk_rec_selftest.c）在读回校验前逼数据落盘——
 * append 系列函数平时只在轮转/关闭时才 fclose，stdio 缓冲区里的数据在那
 * 之前不保证可读。真机日常运行不需要调用它：5 s flush 周期（写入管线一节）
 * 或 session 关闭自然会落盘。 */
void pk_rec_store_flush_all(void);

/* 掉电重建：读 session_dir 下的 traffic.trk（含 32B 文件头），按
 * PK_TRK_RECORD_LEN 分块扫描，重建出 *out（先 reset）。own_icao 为 NULL
 * 表示不标记 IS_OWN。traffic.trk 不存在/文件头损坏返回 false。
 *
 * 真机专属实现在 pk_rec_store_fs.c（要开文件），纯逻辑部分（按 rec_type
 * 分派、聚合规则）在 pk_rec_idx.c，host 单测覆盖的是那一半。 */
bool pk_rec_store_rebuild_index(const char *session_dir, const uint8_t own_icao[3],
                                pk_rec_idx_table_t *out);

#ifdef __cplusplus
}
#endif
