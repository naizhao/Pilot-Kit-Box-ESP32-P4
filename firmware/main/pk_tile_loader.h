/*
 * pk_tile_loader.h — 独立 FreeRTOS 任务 + 请求队列，负责把 pk_map_store 找到
 * 的 PMTiles PNG 瓦片解码进 pk_tile_cache。设计依据
 * docs/superpowers/specs/2026-08-01-sd-offline-map-design.md「盒子端架构」
 * 第 3 点。
 *
 * 线程划分（这是本文件存在的唯一理由——FlightMate 在 UI 线程同步加载的教训）：
 *   - 渲染线程（map_page，每帧调用）：只做**纯内存**操作——pk_tile_loader_route()
 *     包一层 pk_map_route_find()（不碰磁盘），pk_tile_loader_try_blit() 在锁内
 *     查 pk_tile_cache 并直接 blit 到 framebuffer（拿着锁做内存拷贝，不做 I/O，
 *     临界区极短）。缺瓦片时调 pk_tile_loader_request() 把请求丢进队列，立刻
 *     返回，本帧画占位。
 *   - loader 任务：从队列取请求，调 pk_map_store_get_tile()（磁盘 I/O：读
 *     PMTiles 目录/瓦片数据）→ lodepng 解码 → RGBA→RGB565-swapped → 存进
 *     pk_tile_cache（同一把锁）。全程只有这一个任务碰 SD 卡。
 *
 * overzoom：loader 只按 pk_map_store_get_tile 返回的 actual_z/actual_x/actual_y
 * 存那张“实际”瓦片（cache key 用 actual 坐标，不是请求坐标）。父瓦片放大成
 * 请求瓦片的那次裁剪+最近邻放大，在 pk_tile_loader_try_blit() 里做——它知道
 * route 的 scale，从源瓦片裁出对应子块再放大填满目标 256×256 屏幕区域。
 *
 * generation：
 *   - “换 zoom / 大平移”作废——调用方（map_page）在这些时刻调
 *     pk_tile_loader_bump_view()，之后任何还没被 loader 取出处理的旧请求会被
 *     直接丢弃（不取消已在处理中的那一条，那条完成后正常入缓存，无害）。
 *   - “拔卡/rescan”作废——loader 任务自己探测 pk_sdcard 状态；插回卡时整体
 *     重扫包清单并 pk_tile_cache_bump_generation()（旧卡的瓦片数据可能已经
 *     不对应新卡）。**拔卡期间不做任何失效**：包清单与缓存原样保留，已缓存
 *     的瓦片继续显示（spec 错误态表格「运行中拔卡」一行），只是不再有新瓦片
 *     进来，直到卡回来触发重扫。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pk_map_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 loader 任务：若此刻 SD 已挂载，同步扫一遍 /sdcard/maps（与
 * pk_sdcard_init 的"先同步试一次，再起后台任务"是同一个模式）。必须在
 * pk_sdcard_init() 之后调用。幂等。 */
void pk_tile_loader_init(void);

/* 当前包清单里成功打开的包数——0 表示"无有效包"（配合 pk_sdcard_is_mounted()
 * 判无卡/无 maps 目录/无有效包三种错误态，见 map_page.c）。 */
size_t pk_tile_loader_pack_count(void);

/* 纯内存路由：包一层 pk_map_route_find()，喂当前包清单的 meta 快照。
 * 渲染线程安全，不做任何磁盘 I/O。返回 false 表示没有任何包覆盖该瓦片
 * （含 store 为空的情况）。 */
bool pk_tile_loader_route(uint8_t z, uint32_t x, uint32_t y,
                          pk_map_route_result_t *out_route);

/*
 * 按 route 结果尝试把瓦片画到 framebuffer 的 (dst_x0, dst_y0)..(+256,+256)
 * 区域（越界部分自动裁剪）。route 由 pk_tile_loader_route(z, req_x, req_y, ...)
 * 给出；req_x/req_y 就是那次调用传入的请求瓦片坐标——overzoom
 * （route.scale > 1）时需要它们算出「请求瓦片在 actual 瓦片里的哪个子块」：
 * local = req - (actual << log2(scale))，裁剪原点 = local × (256/scale)。
 *
 *   命中正常瓦片 → 按 route.scale 做最近邻裁剪放大 blit，返回 true；
 *   命中负缓存（确认缺失/解码失败）→ 不画，返回 false，*out_negative=true；
 *   未命中（还没请求过或已过期）→ 不画，返回 false，*out_negative=false
 *     （调用方应该调 pk_tile_loader_request() 发起加载）。
 *
 * 拿锁做内存拷贝，不做 I/O，临界区随 256×256 blit 的耗时（微秒级）。
 */
bool pk_tile_loader_try_blit(const pk_map_route_result_t *route,
                             uint32_t req_x, uint32_t req_y,
                             uint16_t *fb, int dst_x0, int dst_y0,
                             uint32_t now_ms, bool *out_negative);

/* 未命中时的兜底：从缓存里找**已有的上级瓦片**（z-1、z-2…最多 max_levels_up
 * 级），裁出对应子块放大填满目标区域。
 *
 * 为什么需要：放大一级时新一级的瓦片全是未命中，若直接画网格占位，用户看到的
 * 就是"整屏重新加载"；而上一级的瓦片明明还在缓存里，放大 2 倍顶上去虽然糊，
 * 但画面连续、且真瓦片一到就自然替换——所有滑动地图都是这么做的。
 * 命中返回 true（已画），没有任何可用祖先返回 false（调用方再画占位）。 */
bool pk_tile_loader_try_blit_ancestor(uint8_t z, uint32_t x, uint32_t y,
                                      uint16_t *fb, int dst_x0, int dst_y0,
                                      uint32_t now_ms, int max_levels_up);

/* 发起加载请求（对应 route 里的实际瓦片 pack_index/actual_z/actual_x/actual_y）。
 * 已在队列中或已缓存的重复请求被去重丢弃；队满时静默丢弃——map_page 每帧都会
 * 为可见的缺瓦片重新调用一次，下一帧自然重试。 */
void pk_tile_loader_request(const pk_map_route_result_t *route);

/* 换 zoom / 大平移时调用：之后 loader 从队列取出的“旧视图”请求直接丢弃、
 * 不处理（不影响已经在处理中的那一条）。 */
void pk_tile_loader_bump_view(void);

#ifdef __cplusplus
}
#endif
