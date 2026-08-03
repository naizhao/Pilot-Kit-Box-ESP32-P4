/* pk_i2c0_policy.c — 见 pk_i2c0_policy.h 的设计说明。纯逻辑，零依赖。 */

#include "pk_i2c0_policy.h"

/* ── 探测器 ────────────────────────────────────────────────────────── */

void pk_i2c0_detector_init(pk_i2c0_detector_t *d,
                           uint32_t min_fail_count,
                           int64_t  min_fail_span_us)
{
    if (d == NULL) return;
    /* 次数门槛下限为 1：填 0 等于"一次都不用失败就喊"，那是配置写错，
     * 与其静默地把总线 reset 掉，不如钳成最保守的 1。 */
    d->min_fail_count   = (min_fail_count < 1) ? 1 : min_fail_count;
    d->min_fail_span_us = (min_fail_span_us < 0) ? 0 : min_fail_span_us;
    d->fail_count       = 0;
    d->first_fail_us    = 0;
}

void pk_i2c0_detector_reset(pk_i2c0_detector_t *d)
{
    if (d == NULL) return;
    d->fail_count    = 0;
    d->first_fail_us = 0;
}

bool pk_i2c0_detector_report(pk_i2c0_detector_t *d, bool ok, int64_t now_us)
{
    if (d == NULL) return false;

    if (ok) {
        d->fail_count    = 0;
        d->first_fail_us = 0;
        return false;
    }

    if (d->fail_count == 0) d->first_fail_us = now_us;
    d->fail_count++;

    const int64_t span = now_us - d->first_fail_us;
    if (d->fail_count >= d->min_fail_count && span >= d->min_fail_span_us) {
        /* 喊完立刻重新开始计数，见头文件里为什么不用 latch 布尔。 */
        d->fail_count    = 0;
        d->first_fail_us = 0;
        return true;
    }
    return false;
}

/* ── 闸门 ──────────────────────────────────────────────────────────── */

void pk_i2c0_gate_init(pk_i2c0_gate_t *g, int64_t base_us, int64_t max_us)
{
    if (g == NULL) return;
    g->cooldown_base_us = (base_us < 0) ? 0 : base_us;
    g->cooldown_max_us  = (max_us < g->cooldown_base_us) ? g->cooldown_base_us : max_us;
    g->busy             = false;
    g->consec_fail      = 0;
    g->next_allowed_us  = 0;
    g->armed            = false;
    g->generation       = 0;
}

int64_t pk_i2c0_gate_cooldown_us(const pk_i2c0_gate_t *g)
{
    if (g == NULL) return 0;
    if (g->consec_fail == 0) return g->cooldown_base_us;

    /* base << (consec_fail-1)，封顶 max。移位前先判上限，避免 consec_fail
     * 涨到 63 以上时 int64 左移越界（UB）。 */
    int64_t v = g->cooldown_base_us;
    for (uint32_t i = 1; i < g->consec_fail; ++i) {
        if (v >= g->cooldown_max_us) break;
        v *= 2;
    }
    return (v > g->cooldown_max_us) ? g->cooldown_max_us : v;
}

pk_i2c0_gate_decision_t pk_i2c0_gate_begin(pk_i2c0_gate_t *g, int64_t now_us)
{
    if (g == NULL) return PK_I2C0_GATE_BUSY;
    if (g->busy) return PK_I2C0_GATE_BUSY;
    if (g->armed && now_us < g->next_allowed_us) return PK_I2C0_GATE_COOLDOWN;
    g->busy = true;
    return PK_I2C0_GATE_GO;
}

void pk_i2c0_gate_finish(pk_i2c0_gate_t *g, bool recovered, int64_t now_us)
{
    if (g == NULL) return;

    if (recovered) {
        g->consec_fail = 0;
        g->generation++;
    } else {
        /* 封顶，防止长时间坏总线把计数涨到溢出（顺带让 cooldown_us 的
         * 循环不至于跑很多圈）。 */
        if (g->consec_fail < 32) g->consec_fail++;
    }

    g->next_allowed_us = now_us + pk_i2c0_gate_cooldown_us(g);
    g->armed           = true;
    g->busy            = false;
}
