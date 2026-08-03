/* pk_cal_advisor.c — 罗盘校准提示判定状态机实现。设计说明见 pk_cal_advisor.h。
 *
 * 判据共五段（配套缺一不可）：
 *   1. 重新武装滞回 —— 「稍后再说」的闸门，acc 连续保持 REARM_MS 才解除；
 *   2. 飞行相位门控 —— 只有地面静止（含 UNKNOWN）才允许抢页面；
 *   3. 磁干扰识别   —— accuracy 反复跨阈 → 一律闭嘴；
 *   4. 静止门控     —— vib_level 低 = 设备没在动 → 画 8 字物理上没用，降级 HINT；
 *   5. 冷启动宽限   —— 本次开机从未收敛过 → ENTER 窗口临时拉长到 120 s。
 * 另有 acc<EXIT_ACCURACY（0 或 1 都算低精度）的判据修正：BNO 冷启动停在 0 还是 1
 * 是随机的，旧逻辑只认 ==0 导致停在 1 时永不提示。 */
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
                                      pk_flight_phase_t phase,
                                      uint8_t vib_level)
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

        /* 记住本次开机是否到过 EXIT_ACCURACY：冷启动宽限只给「从未收敛过」。
         * 一帧 acc>=2 即置位——后续即便退化也不再享受宽限。 */
        if (acc_high) st->ever_converged = true;

        if (accuracy < PK_CAL_EXIT_ACCURACY) {
            /* D2(c)：低精度判据从 ==0 改成 <EXIT_ACCURACY（即 0 或 1）。
             * BNO 冷启动后停在 0 还是 1 是随机的：停在 0 → 旧逻辑起算；
             * 停在 1 → 旧逻辑的 else 分支不碰计时器 → 永不提示。可 acc==1
             * 同样是低精度、航向同样不准。靠静止门控(a)与冷启动宽限(b)兜住，
             * 三条配套才安全。 */
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
        /* accuracy 在 EXIT_ACCURACY 之上但不到 high？不存在——EXIT_ACCURACY 就是
         * high 的阈值，这两条分支互斥且穷尽。 */
    }
    /* !imu_valid：同上 —— 取样断流不是磁环境变化，不推进也不清零任何
     * 计时器，也不参与跨阈统计。 */

    /* 闸门重新武装。放在这里（而不是像旧实现那样塞在 acc>=2 的分支里）是为了
     * 让「连续保持」这件事每拍都被重新检验：一旦中途掉下阈值，high_active 被
     * 清掉，计时从下一次上行重新起算。 */
    if (st->suppressed && st->high_active &&
        (uint32_t)(now_ms - st->high_since_ms) >= PK_CAL_REARM_MS) {
        st->suppressed = false;
    }

    /* D3：用户主动打开的校准页永不自动退出。should_exit_wizard() 在该标志为真
     * 时恒返回 false（见只读结论那段）。exit_ready 照常算——它只反映「acc 是
     * 否连续达标」，是否据此退出由调用方按 user_opened 判断。这里不改 exit_ready
     * 是为了让 should_exit_wizard 的实现干净（一个标志 gate 一个 bool）。 */
    st->exit_ready = st->high_active &&
                     (uint32_t)(now_ms - st->high_since_ms) >= PK_CAL_EXIT_MS;

    jam_evaluate(st, now_ms);

    /* D2(b)：冷启动宽限。本次开机从未到过 EXIT_ACCURACY 时，ENTER 窗口临时拉长
     * 到 COLDSTART_GRACE_MS；到过一次后永久走 ENTER_MS 快通道。 */
    uint32_t enter_window = st->ever_converged ? PK_CAL_ENTER_MS
                                               : PK_CAL_COLDSTART_GRACE_MS;

    pk_cal_advice_t advice = PK_CAL_ADVICE_NONE;
    if (st->low_active && (uint32_t)(now_ms - st->low_since_ms) >= enter_window) {
        /* 相位门控：只有地面静止（含 UNKNOWN）才允许抢页面。
         * UNKNOWN 也放行的理由：它覆盖「刚开机、GPS 还没定位」这个最该提示的
         * 场景（pk_flight_phase.h 明确 GPS 无效时整段跳过、相位原样返回，而
         * 开机初值就是 UNKNOWN）。机库 / 廊桥同样是 UNKNOWN，但那种场合由下面
         * 的 jammed 兜住。 */
        bool phase_allows_page = (phase == PK_PHASE_GROUND_STOPPED ||
                                  phase == PK_PHASE_UNKNOWN);

        /* D2(a)：静止门控。磁力计校准物理上需要转动，静止时提示画 8 字是白提。
         * vib_level < VIB_STILL_THRESH 判为静止 → 降级成 HINT（图标照给，用户
         * 得知道精度不够，看到图标才会想起来拿起来转）。vib_level==0 是「振动
         * 传感器不可用」而不是「零振动」——不可用时**不**按静止处理（不抑制
         * 弹页）：冷启动宽限已经盖住了 BNO 本身还没起来的那段，等宽限期满精度
         * 仍然低、且不在飞行相位，说明设备确实没校准好，此时若振动子系统也挂
         * 了，与其静默吞掉提示，不如照常弹——漏弹比误弹危险（用户不知道精度
         * 不够）。这是「不可用 = 不抑制」的取舍，不是「不可用 = 零振动」。 */
        bool vib_available = (vib_level != 0);
        bool is_still = vib_available && (vib_level < PK_CAL_VIB_STILL_THRESH);

        /* 闸门关着 / 不在地面静止 / 静止 → 降级成状态栏图标，而不是彻底闭嘴：
         * 「稍后再说」表达的是「别抢我的页面」，不是「别再告诉我」；精度确实
         * 不够这件事仍然该有个地方看得见（静止时图标尤其重要——那是用户拿起来
         * 转一转的唯一触发线索）。 */
        advice = (!st->suppressed && phase_allows_page && !is_still)
                     ? PK_CAL_ADVICE_WIZARD
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

    /* D3：用户离开了校准页（无论是「稍后再说」还是 FAB→导航网格），清掉
     * user_opened。不清的话，下一次系统自动弹出也会被当成「用户主动打开」
     * 而永不自动退出——等于永久禁用自动退出。 */
    st->user_opened = false;

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

    /* D3：标记本次进入是用户主动的。should_exit_wizard() 据此恒返回 false，
     * 页面只能由用户自己离开。dismiss / 自动进入路径都会清位（自动进入路径
     * 在胶水层 pk_ui_cal_wizard_tick 里走 take_page 分支，不会调 user_open，
     * 所以 user_opened 保持初值 false）。 */
    st->user_opened = true;

    /* 两条计时器一并清零，各有各的原因（沿用 pk_ui_cal_wizard_enter() 的真机
     * 结论）：
     *   - 低精度：人已经在这一页了，自动进入分支本来就不该再触发；
     *   - 高精度：D3 之后 user_opened 已经拦住了自动退出，这里清零是冗余的
     *     安全层——即便将来有人改坏了 should_exit_wizard 的 user_opened gate，
     *     清零也能多撑 3 s 才退。 */
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
    /* D3：用户主动打开的校准页永不自动退出。用户的显式动作压过系统的自动
     * 判断——他点进来就是要校准（重新校准/验证/画 8 字顶精度都正当），系统凭
     * 一个 acc 读数判定「没事可做」然后收走页面是自作聪明。 */
    if (st->user_opened) return false;
    return st->exit_ready;
}
