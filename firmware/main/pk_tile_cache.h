/*
 * pk_tile_cache.h — PSRAM 上 24 槽 256×256 RGB565 瓦片 LRU 缓存。
 * 设计依据 docs/superpowers/specs/2026-08-01-sd-offline-map-design.md
 * 「盒子端架构」第 4 点：
 *   - key=(pack_id,z,x,y)；
 *   - 缺瓦片负缓存（key 存在但 data=NULL 表示"确认缺失"，带时间戳，过期后
 *     视为未缓存，允许重新尝试加载）；
 *   - generation 作废：拔卡/rescan 时调用方一次性把整缓存標記失效，不用逐条
 *     淘汰；
 *   - 无回调、无事件——loader 解出瓦片后 put()，map_page 每帧 render 时
 *     直接 get() 查，异步到达的瓦片自然出现在下一帧。
 *
 * 内存分配抽象照 pk_pmtiles.c 的 #if defined(ESP_PLATFORM) 分支惯例：固件用
 * heap_caps_malloc(MALLOC_CAP_SPIRAM)，host 单测用 malloc。分配/释放必须配对
 * 用同一后端，所以对外只暴露 pk_tile_cache_alloc_tile_buffer()——调用方
 * （pk_tile_loader，本阶段不实现）解码进这块缓冲区再 put()，cache 内部释放
 * 时用同一 free 函数。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 48 槽 × 128KB = 6MB 硬上限（缓冲区按需分配，装不满就不占）。
 *
 * 24 槽为什么不够：800×480 一屏最多要 15 张瓦片，换一级 zoom 就是另外 15 张
 * ——24 槽连两级都装不下，来回缩放必然把上一级全挤掉，表现为"每次缩放都在
 * 重新加载"（罩哥 2026-08-01 实测反馈）。48 槽 ≈ 三级视口（15×3=45），来回
 * 缩放不会被挤掉。
 *
 * 96 槽为什么必须降回来（2026-08-02 真机自检，见下面的实测数字）：a14c529
 * 定 96 槽时机型库还在 flash EMBED、不占 PSRAM。机型库搬到 SD 之后，稳态
 * PSRAM 余量只剩 **9.15 MB**（pk_aero.bin 10.92MB + pk_actdb.bin 8.21MB +
 * 帧缓冲约 2.9MB 之后的剩余），而 96 槽的上限是 12MB——**缓存上限比可用
 * PSRAM 还大 2.85MB，数学上必然把内存吃穿**。吃穿之后不是"少一张瓦片"，
 * 是整条链路一起垮：
 *   - lodepng 分配 256KB RGBA 失败 → `PNG 解码失败 … err=83`
 *   - SD 读回 `sdmmc_read_blocks failed (0x106)`(ESP_ERR_NO_MEM)
 *   - pmtiles 目录读不出来 → 被记成 `所有包目录均未命中`（误判成数据缺失）
 * 三者都走 put_negative()，负缓存条目再把正常瓦片从 LRU 里挤出去，缓存清空
 * → PSRAM 又空出来 → 30 s 负缓存 TTL 到期 → 重新灌满 → 再撞墙。实测这个
 * 循环周期约 35 s，屏上就是"整片瓦片缺失、过一阵自己好、再过一阵又没了"。 */
#define PK_TILE_CACHE_SLOTS       48

/* PSRAM 水位线：瓦片缓存永远不把 PSRAM 压到这条线以下，宁可先淘汰最久未用
 * 的瓦片。硬上限管不了未来——v4 航空库（16.63MB，比 v3 多 5.71MB）上卡后
 * 稳态余量还要再掉 5.71MB，写死的槽数到那天又会重演一遍。
 *
 * 3MB 的依据：解一张瓦片同时要 png scratch（≤1MB）+ lodepng 的 RGBA8888
 * 256KB + inflate 中间缓冲，两个 worker 并行约 2MB 峰值；余下 1MB 留给
 * 航空图层查询/LVGL/BLE 的零散分配。实测把余量压到 1.1MB、最大连续块
 * 256KB 时，上面那三条失败全部出现。 */
#define PK_TILE_CACHE_PSRAM_FLOOR_BYTES (3u * 1024u * 1024u)
#define PK_TILE_PIXELS            256
#define PK_TILE_BUF_PIXELS        (PK_TILE_PIXELS * PK_TILE_PIXELS)
#define PK_TILE_BUF_BYTES         (PK_TILE_BUF_PIXELS * (int)sizeof(uint16_t))

/* 负缓存有效期：SD 卡上的包清单在这段时间内不会变化（拔卡走
 * pk_tile_cache_bump_generation 整体作废，不靠这个 TTL），纯粹是给"这块瓦片
 * 确认缺失"一个不至于永久卡死的有效期，防止极端场景下的误判长期粘住。 */
#define PK_TILE_CACHE_NEGATIVE_TTL_MS (30u * 1000u)

/* 临时负缓存（IO 失败退避，2026-08-04）。与上面那条"瓦片真缺失"的 30s 负缓存
 * 是两条独立的路——真机抓包证实 sdmmc_read_blocks 0x106（SD 并发过载超时）
 * 被标 TRANSIENT 每帧重试，会刷屏 + 火上浇油抢更多 SD。临时负缓存给 IO 类
 * 失败一个短 TTL 退避：这段时间内 map_page 走 ancestor blit（糊但不空白）、
 * 不再 request → 不再抢 SD。失败计数到阈值才插（偶发 1-2 次不插，只计数），
 * 持续失败（≥5 次）升级到长 TTL。read 成功后计数清零——一次 SD 拥塞不该
 * 永久污染这块瓦片。 */
#define PK_TILE_CACHE_TEMP_NEG_TTL_MS      (2u * 1000u)   /* 3-4 次连续失败 */
#define PK_TILE_CACHE_TEMP_NEG_LONG_TTL_MS (5u * 1000u)   /* ≥5 次持续失败 */
#define PK_TILE_CACHE_SD_FAIL_BACKOFF      3u              /* 计数到这开始退避 */

typedef struct {
    uint32_t pack_id;   /* 调用方定义的稳定包标识（例如 pk_map_store 里的包下标或路径哈希） */
    uint8_t  z;
    uint32_t x, y;
} pk_tile_key_t;

typedef struct {
    bool          used;
    bool          negative;          /* true: 确认缺失（data 恒 NULL） */
    bool          temp_negative;     /* true: IO 失败退避（短 TTL，区别于上面的 30s） */
    pk_tile_key_t key;
    uint16_t     *data;              /* used&&!negative 时非 NULL，PK_TILE_BUF_BYTES 字节 */
    uint32_t      generation;
    uint32_t      last_used_seq;     /* LRU：数值越大越新近使用 */
    uint32_t      neg_timestamp_ms;  /* negative 或 temp_negative 时有意义 */
    uint8_t       sd_fail_count;     /* 连续 IO(0x106) 失败次数；read 成功后清零 */
} pk_tile_cache_slot_t;

typedef struct {
    pk_tile_cache_slot_t slots[PK_TILE_CACHE_SLOTS];
    uint32_t              generation;
    uint32_t              seq_counter;
} pk_tile_cache_t;

void pk_tile_cache_init(pk_tile_cache_t *cache);

/* 释放所有仍持有的瓦片缓冲区（用同一分配后端配对的 free），清零缓存。
 * 调用后 cache 回到刚 init 的状态，可以再 init 或直接销毁。 */
void pk_tile_cache_deinit(pk_tile_cache_t *cache);

/* 按当前分配后端申请一块 PK_TILE_BUF_BYTES 字节的瓦片缓冲区，供调用方解码
 * 进去后 pk_tile_cache_put()。失败返回 NULL。
 * 裸分配，不看水位线、不淘汰——固件请走 pk_tile_cache_acquire_buffer()。 */
uint16_t *pk_tile_cache_alloc_tile_buffer(void);

/* 取一块瓦片缓冲区，必要时先让路（这是固件该用的那个）：
 *   1. PSRAM 余量低于 PK_TILE_CACHE_PSRAM_FLOOR_BYTES + 一张瓦片时，先淘汰
 *      最久未用的瓦片，直到回到水位线之上或无可淘汰；
 *   2. 分配仍失败就再淘汰一条重试，直到缓存让干净为止。
 * 全部让完还是拿不到才返回 NULL（此时 alloc_fail_count 加一）。
 * 调用方必须持有保护 cache 的锁。 */
uint16_t *pk_tile_cache_acquire_buffer(pk_tile_cache_t *cache);

/* 淘汰一条最久未用、且真正占着瓦片缓冲区的条目并释放其内存。
 * 空槽和负缓存不占瓦片内存，不算候选。没有可淘汰的返回 false。 */
bool pk_tile_cache_evict_lru(pk_tile_cache_t *cache);

/* 拔卡/rescan 时整体作废：所有已缓存条目（含负缓存）立即释放并清空，
 * generation 计数器递增。旧 generation 缓存的数据不会污染重扫后的结果。 */
void pk_tile_cache_bump_generation(pk_tile_cache_t *cache);

/* 查缓存。
 *   命中且是正常瓦片 → 返回数据指针，*out_is_negative=false，并把这条记为
 *     最近使用（LRU 提升）。
 *   命中且是未过期的负缓存 → 返回 NULL，*out_is_negative=true（调用方不必
 *     重新发起加载）。
 *   命中但负缓存已过期，或压根没命中 → 返回 NULL，*out_is_negative=false
 *     （调用方应该发起加载；过期的负缓存条目会被就地清空腾出槽位）。
 *   generation 与当前不符的条目一律当未命中处理（不提升、不续期）。
 * now_ms 由调用方传入（host 单测用假时钟，固件侧用 esp_timer/pk_clock 毫秒
 * 时间戳），保持纯函数式可测。 */
const uint16_t *pk_tile_cache_get(pk_tile_cache_t *cache, pk_tile_key_t key,
                                   uint32_t now_ms, bool *out_is_negative);

/* 存入一块已解码的瓦片（data 必须来自 pk_tile_cache_alloc_tile_buffer()，
 * 所有权转移给 cache——put 之后调用方不能再碰这块内存）。
 * 满槽时淘汰 last_used_seq 最小（最久未用）的一条。 */
void pk_tile_cache_put(pk_tile_cache_t *cache, pk_tile_key_t key, uint16_t *data);

/* 标记 key 为"确认缺失"（例如包路由/查目录未命中，或 PNG 解码失败）。
 * 同样参与 LRU 淘汰（满槽时可能挤掉最久未用的条目，含负缓存）。 */
void pk_tile_cache_put_negative(pk_tile_cache_t *cache, pk_tile_key_t key, uint32_t now_ms);

/* ── IO 失败退避（临时负缓存，2026-08-04）────────────────────────────
 * 真机抓包：sdmmc_read_blocks 0x106（SD 并发过载）被标 TRANSIENT 每帧重试，
 * 刷屏且火上浇油。这三个函数让 IO 类连续失败先累积计数、到阈值再插短 TTL
 * 临时负缓存（map_page 退避期间走 ancestor blit），成功后清零。
 * 见 pk_tile_cache.h 顶部 PK_TILE_CACHE_TEMP_NEG_* 常量的说明。 */

/* 递增 key 的连续 IO 失败计数。找不到既有条目时占一个空槽、计数置 1
 * （不淘汰真实瓦片——退避条目本身就要能占住槽位才挡得住每帧重试）。
 * 返回递增后的计数（供调用方判断是否到退避阈值）。 */
uint8_t pk_tile_cache_bump_sd_fail(pk_tile_cache_t *cache, pk_tile_key_t key, uint32_t now_ms);

/* 按 key 当前的 sd_fail_count 插临时负缓存：3-4 次→短 TTL，≥5 次→长 TTL。
 * 必须在 bump_sd_fail 之后调（它读 count 决定 TTL）。幂等：重复插只刷新
 * 时间戳。调用方仍要 return TRANSIENT（临时负缓存不等于"确认缺失"，
 * 只是"这段时间内别再 request"）。 */
void pk_tile_cache_put_temp_negative(pk_tile_cache_t *cache, pk_tile_key_t key, uint32_t now_ms);

/* read 成功后清掉 key 的失败计数（一次 SD 拥塞不该永久污染这块瓦片）。
 * 找不到条目时空操作。 */
void pk_tile_cache_reset_sd_fail(pk_tile_cache_t *cache, pk_tile_key_t key);

/* ── 诊断 ────────────────────────────────────────────────────────────
 * 「地图整片瓦片缺失」这类问题里，光看屏幕分不清是"没请求""读失败"还是
 * "内存不够装不下"。这两个口子让排查方能直接读到缓存的真实占用与分配失败
 * 累计次数，不用再猜。 */

/* out_used：装着真实瓦片的槽数；out_negative：负缓存槽数；
 * out_bytes：瓦片缓冲区实际占用字节（= out_used × PK_TILE_BUF_BYTES）。
 * 任一 out 指针可为 NULL。只统计当前 generation 的条目。 */
void pk_tile_cache_stats(const pk_tile_cache_t *cache, int *out_used,
                         int *out_negative, size_t *out_bytes);

/* pk_tile_cache_alloc_tile_buffer() 返回 NULL 的累计次数（模块级，非按实例）。 */
uint32_t pk_tile_cache_alloc_fail_count(void);

#ifdef __cplusplus
}
#endif
