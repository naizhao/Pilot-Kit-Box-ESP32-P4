#include <string.h>
#include <stdatomic.h>
#include "uat_ingest.h"

/* 432 B 净数据落在静态缓冲（uat_uplink_t.info[].data 指向它，sink 在
 * feed 返回后仍可读——头文件契约）。单写者（adsb_link_task）串行调用，
 * 无并发写；与 modes_ingest 的 s_dec 同一存放策略。 */
static uint8_t s_data[UAT_UPLINK_DATA_BYTES];
static uat_ingest_sink_fn s_sink;
static void               *s_user;
/* 计数器由 adsb_link_task（feed 调用方）独占写，其他任务经
 * uat_ingest_get_stats 读取——C11 原子（relaxed）：各字段独立采样、
 * 单调计数，不做跨字段一致性承诺，与 modes_ingest 同一口径。 */
static atomic_uint s_frames_total;
static atomic_uint s_bad_rs;
static atomic_uint s_no_sync;

void uat_ingest_init(uat_ingest_sink_fn sink, void *user)
{
    s_sink = sink;
    s_user = user;
    atomic_store_explicit(&s_frames_total, 0, memory_order_relaxed);
    atomic_store_explicit(&s_bad_rs, 0, memory_order_relaxed);
    atomic_store_explicit(&s_no_sync, 0, memory_order_relaxed);
    uat_fec_init();     /* 幂等（uat_decode.h）；本模块是目标端首个消费者 */
}

void uat_ingest_feed(const uint8_t frame[UAT_UPLINK_FRAME_BYTES],
                     const uat_ingest_meta_t *meta)
{
    uat_uplink_t up;
    uint8_t rs_corrected = 0;
    if (!uat_uplink_decode(frame, s_data, &up, &rs_corrected)) {
        /* 全零闸在 RS 之前（事实卡 §8）：全零计 no_sync，其余即 RS 不可纠
         * （UAT 无 payload CRC，事实卡 §6）。二者共用解码器的 false 返回，
         * 靠输入形态区分——全零帧是确定性的字节模式，直接比对。 */
        bool all_zero = true;
        for (size_t i = 0; i < UAT_UPLINK_FRAME_BYTES; i++)
            if (frame[i] != 0) { all_zero = false; break; }
        if (all_zero)
            atomic_fetch_add_explicit(&s_no_sync, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&s_bad_rs, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&s_frames_total, 1, memory_order_relaxed);
    if (s_sink) {
        uat_ingest_meta_t m = {0};
        if (meta) m = *meta;
        s_sink(&up, &m, rs_corrected, s_user);
    }
}

void uat_ingest_get_stats(uint32_t *frames_total, uint32_t *bad_rs,
                          uint32_t *no_sync)
{
    if (frames_total)
        *frames_total = atomic_load_explicit(&s_frames_total,
                                             memory_order_relaxed);
    if (bad_rs)
        *bad_rs = atomic_load_explicit(&s_bad_rs, memory_order_relaxed);
    if (no_sync)
        *no_sync = atomic_load_explicit(&s_no_sync, memory_order_relaxed);
}
