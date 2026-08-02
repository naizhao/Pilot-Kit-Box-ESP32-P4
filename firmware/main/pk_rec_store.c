/* pk_rec_store.c — pk_rec_store 的纯逻辑部分：序号分配/回绕、保留策略
 * 选谁删、SD 降级档位判定。
 *
 * 刻意**不依赖任何 IDF 头**——固件与 host 单测共用同一份源码
 * （firmware/test/test_pk_rec_store.c 直接 #include 本 .c 进同一 TU，
 * 同 pk_rec_format.c 的写法）。真机专属的文件系统 / FreeRTOS 胶水代码在
 * pk_rec_store_fs.c 里，那部分 host 测不到，注释里会说明为什么。
 */
#include "pk_rec_store.h"

#include <string.h>

/* ------------------------------------------------------------ 序号分配 */

uint16_t pk_rec_store_alloc_seq(const bool used[PK_REC_STORE_SEQ_SPACE])
{
    int max_used = -1;
    for (int i = 0; i < (int)PK_REC_STORE_SEQ_SPACE; i++) {
        if (used[i]) max_used = i;
    }

    if (max_used < 0) {
        return 0;   /* 位图全空：从 0 开始 */
    }

    if (max_used < (int)PK_REC_STORE_SEQ_SPACE - 1) {
        return (uint16_t)(max_used + 1);   /* 正常情况：max + 1 */
    }

    /* max_used == 9999：数字空间顶到头，回绕复用最小空闲号。 */
    for (int i = 0; i < (int)PK_REC_STORE_SEQ_SPACE; i++) {
        if (!used[i]) return (uint16_t)i;
    }

    /* 位图全满（10000 个 session 都在）——正常运行不会走到这，保留策略
     * 在 32 这个量级就会腾出号。退化返回 0，调用方会覆盖掉 "0000"。 */
    return 0;
}

/* ------------------------------------------------------------ 保留策略 */

size_t pk_rec_store_select_prune(const uint16_t *seqs, size_t n, int keep,
                                  uint16_t *out)
{
    if (keep < 0) keep = 0;
    if (n <= (size_t)keep) return 0;

    size_t victims = n - (size_t)keep;

    /* 简单选择排序取最小的 victims 个——n 是 session 数量，正常运行下是
     * 三十几个量级（PK_REC_STORE_KEEP_SESSIONS=32 附近），O(n*victims)
     * 完全够用，没必要为这个量级引入真正的排序算法。
     *
     * 不修改调用方的 seqs[]（它可能是从目录扫描结果借来的只读视图），
     * 用一个本地"已选中"标记表代替原地排序/交换。 */
    for (size_t v = 0; v < victims; v++) {
        int best_idx = -1;
        for (size_t i = 0; i < n; i++) {
            bool already_picked = false;
            for (size_t k = 0; k < v; k++) {
                /* out[k] 存的是值不是下标，用值比较判断这个 seqs[i] 是否
                 * 已经被选过——同一个序号在 seqs[] 里理论上不重复（调用
                 * 方保证去重），值比较即等价于下标比较。 */
                if (out[k] == seqs[i]) { already_picked = true; break; }
            }
            if (already_picked) continue;
            if (best_idx < 0 || seqs[i] < seqs[(size_t)best_idx]) {
                best_idx = (int)i;
            }
        }
        out[v] = seqs[(size_t)best_idx];
    }
    return victims;
}

/* ------------------------------------------------------------ 降级档位 */

pk_rec_degrade_t pk_rec_store_degrade_tier(uint64_t free_bytes)
{
    const uint64_t MB = 1024ull * 1024ull;
    if (free_bytes > 500ull * MB) return PK_REC_DEGRADE_FULL;
    if (free_bytes > 100ull * MB) return PK_REC_DEGRADE_NO_RAW;
    return PK_REC_DEGRADE_OWN_ONLY;
}

bool pk_rec_store_sink_should_disable(uint32_t consecutive_fail_count)
{
    return consecutive_fail_count >= PK_REC_STORE_FAIL_THRESHOLD;
}

/* ------------------------------------------------------------ bounds */

void pk_rec_bounds_reset(pk_rec_bounds_t *b)
{
    memset(b, 0, sizeof(*b));
}

void pk_rec_bounds_update(pk_rec_bounds_t *b, int32_t lat_e7, int32_t lon_e7)
{
    if (!b->has_any) {
        b->min_lat_e7 = b->max_lat_e7 = lat_e7;
        b->min_lon_e7 = b->max_lon_e7 = lon_e7;
        b->has_any = true;
        return;
    }
    if (lat_e7 < b->min_lat_e7) b->min_lat_e7 = lat_e7;
    if (lat_e7 > b->max_lat_e7) b->max_lat_e7 = lat_e7;
    if (lon_e7 < b->min_lon_e7) b->min_lon_e7 = lon_e7;
    if (lon_e7 > b->max_lon_e7) b->max_lon_e7 = lon_e7;
}

/* ------------------------------------------------------------ own_icao_changes */

void pk_rec_own_icao_changes_reset(pk_rec_own_icao_changes_t *c)
{
    memset(c, 0, sizeof(*c));
}

bool pk_rec_own_icao_changes_append(pk_rec_own_icao_changes_t *c, int64_t ts_ms,
                                     bool bound, const uint8_t icao24[3])
{
    if (c->count >= PK_REC_OWN_ICAO_CHANGES_MAX) return false;
    pk_rec_own_icao_change_t *e = &c->entries[c->count++];
    e->ts_ms = ts_ms;
    e->bound = bound;
    if (bound && icao24 != NULL) {
        memcpy(e->icao24, icao24, 3);
    } else {
        memset(e->icao24, 0, 3);
    }
    return true;
}
