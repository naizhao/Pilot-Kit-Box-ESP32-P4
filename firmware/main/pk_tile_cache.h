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

#define PK_TILE_CACHE_SLOTS       24
#define PK_TILE_PIXELS            256
#define PK_TILE_BUF_PIXELS        (PK_TILE_PIXELS * PK_TILE_PIXELS)
#define PK_TILE_BUF_BYTES         (PK_TILE_BUF_PIXELS * (int)sizeof(uint16_t))

/* 负缓存有效期：SD 卡上的包清单在这段时间内不会变化（拔卡走
 * pk_tile_cache_bump_generation 整体作废，不靠这个 TTL），纯粹是给"这块瓦片
 * 确认缺失"一个不至于永久卡死的有效期，防止极端场景下的误判长期粘住。 */
#define PK_TILE_CACHE_NEGATIVE_TTL_MS (30u * 1000u)

typedef struct {
    uint32_t pack_id;   /* 调用方定义的稳定包标识（例如 pk_map_store 里的包下标或路径哈希） */
    uint8_t  z;
    uint32_t x, y;
} pk_tile_key_t;

typedef struct {
    bool          used;
    bool          negative;          /* true: 确认缺失（data 恒 NULL） */
    pk_tile_key_t key;
    uint16_t     *data;              /* used&&!negative 时非 NULL，PK_TILE_BUF_BYTES 字节 */
    uint32_t      generation;
    uint32_t      last_used_seq;     /* LRU：数值越大越新近使用 */
    uint32_t      neg_timestamp_ms;  /* 仅 negative 时有意义 */
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
 * 进去后 pk_tile_cache_put()。失败返回 NULL。 */
uint16_t *pk_tile_cache_alloc_tile_buffer(void);

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

#ifdef __cplusplus
}
#endif
