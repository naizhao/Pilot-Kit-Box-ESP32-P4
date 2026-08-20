/*
 * pk_rec_ingest.h — traffic.trk 的位置/身份记录：编码 + 落盘的薄胶水层。
 *
 * 设计依据 ADS-B 数据持久化设计（内部文档）
 * 「记录格式」「写入时机」两节。调用方（dsp_task.c 的 CPR fresh 分支 /
 * DF17 ident 分支；以及 pk_rec_selftest.c 的注入式自检，绕开射频直接调用
 * 这两个函数）把已经解出的字段递进来，本文件只管：sentinel 化 + 时间同步
 * flag + 调用 pk_rec_format 编码 + pk_rec_store_append_traffic_record()
 * 落盘。**不做 CPR / 呼号变化判定**——那是调用方的职责（前者在
 * cpr_decode.c，后者在 dsp_task.c 里用 aircraft_state 的旧值/新值对比）。
 *
 * 依赖 pk_rec_store（FreeRTOS + VFS），因此本文件不是 host 可测的纯逻辑
 * 模块，不进 test_pk_rec_format.c 的 #include 列表——它本身没有可测的分支
 * （sentinel 判断已经在 pk_rec_format 的单测里覆盖过一次了），这里只是
 * "把字段搬进结构体再调用"，真正需要验证的行为在 pk_rec_selftest.c 的
 * 真机自检里跑一遍全链路。
 *
 * 写入管线（非阻塞入队 + 独立写任务）：pk_rec_ingest_position/identity
 * 挂在 dsp_task 的 Mode-S 解码热路径上，绝不能因为 pk_rec_store_append_
 * traffic_record() 内部 xSemaphoreTake(portMAX_DELAY) + fwrite 可能撞上
 * SD 拔卡时 ~2.01s 的固定超时而被拖停（同 record_sink_rec_store.c 对
 * ADS-B 原始行的处理、pk_own_sampler.c 对 own.trk 的处理）。两个 ingest
 * 函数只做编码 + xQueueSend(..., 0)，真正的 fwrite 挪到本模块自带的
 * writer task 上；队满直接丢并计数——traffic.trk 记录是可再生的（下一条
 * ADS-B 报文很快又来），不像 own.trk 那样值得为它超时阻塞采样任务。
 *
 * 队列选型：没有复用 record_sink_rec_store.c 现成的 ADS-B 原始行队列
 * （给队列元素加 union + 类型标签），而是新开一条专属队列——原因是两类
 * 数据形状差异太大（变长文本行 vs 定长 32B 二进制记录），union 方案要么
 * 浪费空间（按最大变体分配）要么引入标签分支，而 pk_trk_pos_encode /
 * pk_trk_id_encode 已经把两种记录统一编码成同一个 32B 缓冲区（rec_type
 * 编在第 8 字节，pk_trk_rec_type_peek 可读出），队列元素直接是这 32B，
 * 写任务不需要关心是位置还是身份记录，直接转调
 * pk_rec_store_append_traffic_record()——比 union 方案改动更小、更好读。
 * 也没有放进 pk_rec_store_fs.c：那个文件的文件头明确写着"只做摸文件系统
 * 这一件事"，不掺 FreeRTOS 队列生产者逻辑，新队列放在本文件（ingest 层，
 * 本来就在做编码）职责更贴切。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 建队列 + 起写任务；须在 dsp_task 开始跑解码之前调用一次（main.c 里紧跟
 * pk_rec_store_init() 之后）。幂等。 */
void pk_rec_ingest_init(void);

/*
 * 写一条 traffic.trk 位置记录（rec_type=0）。
 *
 *   icao24      24 位 ICAO 地址（0x000000..0xFFFFFF）
 *   ts_ms       epoch 毫秒
 *   lat/lon     WGS-84 十进制度
 *   have_alt    false 时 alt_d25 写 PK_REC_ALT_INVALID
 *   alt_ft      气压高度，ft（25 ft 的整数倍——ADS-B ac12/ac13 解码本就是
 *               25 ft 步进，见 spec「高度必须用 25 ft 单位」一节）
 *   have_gs/have_track/have_vs  各自独立——地面 CPR（阶段 4b）接入后，
 *               surface 报文只带地速+航迹、不带垂速（那几个比特被
 *               MOV/TRK 复用掉了），三者不再总是同生共死；false 时对应
 *               字段写各自的 PK_REC_*_INVALID。空中位置帧（metype 9-18）
 *               三者仍是一起来自 aircraft_state 融合表的当前已知值（来自
 *               metype 19），调用方直接把同一个 have_velocity 传三次即可。
 *   gs_kt/track_deg/vs_fpm  只在对应 have_*=true 时读取
 *   on_ground   PK_TRK_FLAG_ON_GROUND
 *   from_surface_cpr  PK_TRK_FLAG_SURFACE_CPR——位置是否由
 *               cpr_decode_surface_local() 解出（阶段 4b）；空中位置帧
 *               恒传 false
 */
void pk_rec_ingest_position(uint32_t icao24, int64_t ts_ms, double lat, double lon,
                             bool have_alt, int alt_ft,
                             bool have_gs, int gs_kt,
                             bool have_track, int track_deg,
                             bool have_vs, int vs_fpm,
                             bool on_ground, bool from_surface_cpr);

/*
 * 写一条 traffic.trk 身份记录（rec_type=1）。callsign 可以短于 8 字节
 * （已在 aircraft_state 里去掉了尾部下划线/空格），emitter_category 是
 * 已解码的 pk_wake_t（见 aircraft_state.h），不是原始 (metype,mesub)。
 */
void pk_rec_ingest_identity(uint32_t icao24, int64_t ts_ms, const char *callsign,
                             uint8_t emitter_category);

/* 队满丢弃计数（照 record_sink_file_stats() / pk_own_sampler_stats() 的
 * 做法暴露，诊断页/日志按需接；未 init 时返回 false）。 */
bool pk_rec_ingest_stats(uint32_t *out_written, uint32_t *out_dropped);

#ifdef __cplusplus
}
#endif
