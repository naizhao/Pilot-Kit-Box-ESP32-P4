/* pk_cal_advisor.c — 罗盘校准提示判定状态机实现。设计说明见 pk_cal_advisor.h。 */
#include "pk_cal_advisor.h"

#include <string.h>

/* 阈值常量（PK_CAL_ENTER_MS / REARM / EXIT / JAM_*）与「为什么是这个数」的
 * 全部依据都在 pk_cal_advisor.h——胶水层的日志要引用它们打出具体毫秒数。 */

/* ------------------------------------------------------------ 干扰识别 */

static void jam_push_crossing(pk_cal_advisor_t *st, uint32_t now_ms)
{
    st->cross_ms[st->cross_head] = now_ms;
    st->cross_head = (uint8_t)((st->cross_head + 1u) % PK_CAL_JAM_RING_CAP);
    if (st->cross_count < PK_CAL_JAM_RING_CAP) st->cross_count++;
    st->last_cross_ms = now_ms;
}

static void jam_evaluate(pk_cal_advisor_t *st, uint32_t now_ms)
{
    /* 先判解除、再判置位，顺序不能反——反了会在同一拍自相矛盾。
     * 解除时必须连窗口里的陈年跳变一起作废：静默期（30 s）比统计窗口（60 s）
     * 短，不清的话下面那句「窗口内 >= 3 次」当拍就把 jammed 顶回去，解除永远
     * 发生不了。作废的语义也说得通——环境已经稳了 30 s，之前那些跳变不该再
     * 参与「现在是不是干扰环境」的判定。 */
    if (st->jammed && (uint32_t)(now_ms - st->last_cross_ms) >= PK_CAL_JAM_CLEAR_MS) {
        st->jammed = false;
        st->cross_count = 0;
        st->cross_head = 0;
    }

    unsigned in_window = 0;
    for (unsigned i = 0; i < (unsigned)st->cross_count; i++) {
        if ((uint32_t)(now_ms - st->cross_ms[i]) <= PK_CAL_JAM_WINDOW_MS) in_window++;
    }
    if (in_window >= PK_CAL_JAM_CROSSINGS) st->jammed = true;
}

/* ------------------------------------------------------------ reset/update */

void pk_cal_advisor_reset(pk_cal_advisor_t *st)
{
    memset(st, 0, sizeof(*st));
}

pk_cal_advice_t pk_cal_advisor_update(pk_cal_advisor_t *st, uint32_t now_ms,
                                      bool imu_valid, uint8_t accuracy,
                                      pk_flight_phase_t phase)
{
    if (imu_valid) {
        bool acc_high = (accuracy >= PK_CAL_EXIT_ACCURACY);

        /* 跨阈跳变：定义是**相对上一有效样本跨过阈值 2**（<2 → >=2 或反向），
         * 不是「数值变了」——0↔1 都在阈值下方来回，那只是融合在途的正常抖动，
         * 不是磁环境在变。首条样本没有参照，不算跳变。 */
        if (st->acc_seeded && acc_high != st->acc_high_prev) {
            jam_push_crossing(st, now_ms);
        }
        st->acc_high_prev = acc_high;
        st->acc_seeded    = true;

        if (accuracy == 0) {
            if (!st->low_active) {
                st->low_active   = true;
                st->low_since_ms = now_ms;
            }
            st->high_active = false;
        } else if (acc_high) {
            if (!st->high_active) {
                st->high_active   = true;
                st->high_since_ms = now_ms;
            }
            st->low_active = false;
        }
        /* accuracy == 1：两条连续段都不动——既不起算也不清零（沿用
         * ui_state.c 的原行为，注释原话「fusion is in transit」）。注意
         * 「不动」指的是不碰起点：低精度累计用的是墙钟差值，所以 acc=1 这段
         * 时间照样算进 ENTER 窗口里，这与旧实现逐位等价。 */
    }
    /* !imu_valid：同 acc==1 —— 取样断流不是磁环境变化，不推进也不清零任何
     * 计时器，也不参与跨阈统计。 */

    /* 闸门重新武装。放在这里（而不是像旧实现那样塞在 acc>=2 的分支里）是为了
     * 让「连续保持」这件事每拍都被重新检验：一旦中途掉下阈值，high_active 被
     * 清掉，计时从下一次上行重新起算。 */
    if (st->suppressed && st->high_active &&
        (uint32_t)(now_ms - st->high_since_ms) >= PK_CAL_REARM_MS) {
        st->suppressed = false;
    }

    st->exit_ready = st->high_active &&
                     (uint32_t)(now_ms - st->high_since_ms) >= PK_CAL_EXIT_MS;

    jam_evaluate(st, now_ms);

    pk_cal_advice_t advice = PK_CAL_ADVICE_NONE;
    if (st->low_active && (uint32_t)(now_ms - st->low_since_ms) >= PK_CAL_ENTER_MS) {
        /* 相位门控：只有地面静止（含 UNKNOWN）才允许抢页面。
         * UNKNOWN 也放行的理由：它覆盖「刚开机、GPS 还没定位」这个最该提示的
         * 场景（pk_flight_phase.h 明确 GPS 无效时整段跳过、相位原样返回，而
         * 开机初值就是 UNKNOWN）。机库 / 廊桥同样是 UNKNOWN，但那种场合由下面
         * 的 jammed 兜住。 */
        bool phase_allows_page = (phase == PK_PHASE_GROUND_STOPPED ||
                                  phase == PK_PHASE_UNKNOWN);

        /* 闸门关着或不在地面静止 → 降级成状态栏图标，而不是彻底闭嘴：
         * 「稍后再说」表达的是「别抢我的页面」，不是「别再告诉我」；精度确实
         * 不够这件事仍然该有个地方看得见（旧实现那句注释也写了「这只挡自动
         * 进入」）。 */
        advice = (!st->suppressed && phase_allows_page) ? PK_CAL_ADVICE_WIZARD
                                                        : PK_CAL_ADVICE_HINT;
    }

    /* 干扰环境一律闭嘴：连状态栏图标都不给。这里画 8 字救不回来，图标只会让
     * 用户以为设备坏了而反复去转它。想知道「为什么什么都不提示」的人去诊断页
     * （阶段 C4）。 */
    if (st->jammed) advice = PK_CAL_ADVICE_NONE;

    return advice;
}

/* ------------------------------------------------------------ 用户事件 */

void pk_cal_advisor_dismiss(pk_cal_advisor_t *st, uint32_t now_ms)
{
    st->suppressed = true;

    /* 重新武装的 30 s 从「用户说别烦我」这一刻重新起算。不这么做的话，若他
     * 恰好是在 acc 已经高了 25 s 的时候按下「稍后再说」，5 s 后闸门就自己开
     * 了——用户的意图只保住了 5 s。注意这不影响 SC2 那种正常路径：那里高精度
     * 连续段本来就起始于 dismiss 之后。 */
    if (st->high_active) st->high_since_ms = now_ms;
}

void pk_cal_advisor_user_open(pk_cal_advisor_t *st, uint32_t now_ms)
{
    (void)now_ms; /* 当前策略下用不上：清零两条连续段等价于「下一拍重新起算」，
                   * 不需要再记一个时刻。签名保留 now_ms 是为了与 dismiss /
                   * update 统一时基，调用方不必区分哪个事件要传时间。 */

    st->suppressed = false;

    /* 两条计时器一并清零，各有各的原因（沿用 pk_ui_cal_wizard_enter() 的真机
     * 结论）：
     *   - 低精度：人已经在这一页了，自动进入分支本来就不该再触发；
     *   - 高精度：**不清零会让这一页当场闪一下就跑掉**。设备若已经校准好
     *     （acc>=2 保持了几分钟），下一拍就满足「acc>=2 超过 3 s」，用户刚点开
     *     就被弹回 PFD。清零后至少有 3 s 的窗口；3 s 后仍然自动退回是有意保留
     *     的——精度已经够了，这一页没事可做，而真需要校准（acc<2）时自动退出
     *     根本不会触发，页面会一直等着他。 */
    st->low_active  = false;
    st->high_active = false;
    st->exit_ready  = false;
}

/* ------------------------------------------------------------ 只读结论 */

bool pk_cal_advisor_is_jammed(const pk_cal_advisor_t *st)
{
    return st->jammed;
}

bool pk_cal_advisor_should_exit_wizard(const pk_cal_advisor_t *st)
{
    return st->exit_ready;
}
