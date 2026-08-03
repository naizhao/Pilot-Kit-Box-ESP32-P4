/*
 * pk_cal_advisor.h — 「要不要提示罗盘校准」的判定状态机。
 *
 * 立项依据 IMPLEMENTATION_PLAN.md「罗盘校准提示『去骚扰』改造（2026-08-04）」
 * 阶段 C1。把判定从 ui_state.c 里抽出来，是因为那边的三条实测问题都出在
 * 「判定和切页搅在一起」：
 *
 *   1. 信号本身抖 —— accuracy 取自 BNO085 Rotation Vector(0x05) 报文的 status
 *      位（imu_task.c 的 s_sample.accuracy = status & 0x03），机坪 / 机库 /
 *      GPU 电源车附近它会在 0↔1↔2 之间反复跳；
 *   2. 闸门解除没有滞回 —— 旧实现单帧 acc>=2 就把「稍后再说」作废，而 tick 是
 *      每帧调的（30+ FPS），于是约 13 s 一轮把用户强拽回校准页；
 *   3. 强抢页面且不看飞行阶段 —— 空中也会把 PFD 换成校准向导。
 *
 * 另有产品层面的一条：强磁干扰环境下画 8 字**物理上救不回来**，此时提示是纯
 * 无效骚扰，所以本模块还负责识别干扰环境并闭嘴。
 *
 * 本文件**不依赖任何 IDF 头**，host 与固件共用（同 pk_flight_phase.h 的惯例，
 * 测试文件直接 #include 这份 .c）。模块内无静态变量、无浮点、无动态分配；
 * 状态全部显式放在调用方持有的 pk_cal_advisor_t 里，单测按场景各开一份互不
 * 干扰。
 *
 * 时间基一律是 uint32_t 毫秒，所有比较都写成无符号差值 (now - then) >= X，
 * 以正确跨过 49.7 天回绕——盒子在机库里长期插着电，撞得上（见
 * test_pk_cal_advisor.c 的 SC8）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pk_flight_phase.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ 阈值常量
 *
 * 放在头文件而不是 .c 里：胶水层（ui_state.c）的日志要把具体毫秒数打出来，
 * 真机排障的人才不必翻代码才知道那一句 "acc=0 满窗口" 说的是 20 s 还是 10 s。
 * 同 pk_flight_phase.h 的惯例——判定用的常量与它们的依据一起摆在头文件里，
 * 供调用方引用。 */

/* 低精度（acc==0）连续累计到这么久，才认为「确实需要校准」。
 * 旧值是 10 s（ui_state.c 的 UI_CAL_WIZARD_ENTER_MS）。这次反而拉长到 20 s：
 * 旧实现之所以只敢 10 s，是因为它没有别的护栏，短窗口能让「真没校准」早点被
 * 发现；现在相位门控 + 重新武装滞回 + 干扰识别三道都上了，误弹的路径已经堵
 * 死，窗口就该按「BNO085 开机后磁场融合本来要几十秒才收敛」来定——10 s 太容
 * 易在还在收敛的时候插一脚。20 s 不是什么临界值，是「明显长过典型收敛时间、
 * 又不至于让真需要校准的人干等」的折中。 */
#define PK_CAL_ENTER_MS 20000u

/* 闸门（suppressed）重新武装：acc 必须**连续保持**在阈值之上这么久才解除。
 * 这是断骚扰循环的关键一条。旧实现是「见到一帧 acc>=2 就解除」，原注释的前提
 * 是「acc 上过 2 说明确实完成过一次校准」——这个前提在抖动信号上不成立：机坪
 * 上 acc 会自己蹦到 2 再掉回来，于是用户按下的「稍后再说」被一帧噪声作废。
 * 30 s 的依据是「画 8 字校准本身就要十几秒，之后精度还得稳住」：真校准完的
 * 设备轻松保持 30 s，而噪声毛刺撑不过 1 s，两者差着一个数量级，取中间任何值
 * 都能分开，30 s 兼顾了「别让真校准完的人等太久」。 */
#define PK_CAL_REARM_MS 30000u

/* 向导自动退出：acc 保持在阈值之上这么久 → 融合确实收敛了，页面没事可做。
 * 沿用旧值 UI_CAL_WIZARD_EXIT_MS / UI_CAL_WIZARD_EXIT_ACCURACY，一个字没动
 * ——这两个数在真机上没出过问题，这次改造也不碰自动退出这条路径。 */
#define PK_CAL_EXIT_MS       3000u
#define PK_CAL_EXIT_ACCURACY    2u

/* 干扰识别的滑动窗口。acc 在 60 s 里跨过阈值 3 次以上，说明磁环境本身在变，
 * 不是设备没校准——这种地方画 8 字物理上救不回来，提示纯属骚扰。
 * 为什么是「跨阈次数」而不是「acc 的方差/均值」：状态机不碰浮点，而跨阈计数
 * 恰好抓的就是「反复越过判定线」这件事，正是骚扰循环的成因。
 * 3 次是下限：一次真实的「校准好了又退化」在 60 s 内最多产生 2 次跨阈
 * （上行一次、下行一次），取 3 就把正常的一来一回排除在外了。
 * 60 s 与 ENTER 窗口（20 s）的关系：窗口必须明显长过 ENTER，否则干扰还没数够
 * 次数，提示就已经弹出去了。 */
#define PK_CAL_JAM_WINDOW_MS 60000u
#define PK_CAL_JAM_CROSSINGS     3u

/* 干扰解除：跳变停止这么久才认为磁环境稳定下来。
 * 为什么不是「窗口内计数掉到 3 以下就解除」：那样在第 3 新的一次跳变滑出 60 s
 * 窗口的瞬间就解除，紧接着来一次跳变又置位，边界上反复横跳。改成「静默计时」
 * 后解除只发生一次，且判据直观：30 s 一次跳变都没有 = 环境稳了。
 * 30 s 取自 REARM_MS 的同一量级——两者都是在回答「稳定多久才算数」。 */
#define PK_CAL_JAM_CLEAR_MS 30000u

/* ------------------------------------------------------------ 建议等级 */

typedef enum {
    PK_CAL_ADVICE_NONE = 0, /* 什么都不做 */
    PK_CAL_ADVICE_HINT,     /* 只在状态栏画一枚图标，不抢页面（阶段 C3） */
    PK_CAL_ADVICE_WIZARD,   /* 建议自动弹整页校准向导 */
} pk_cal_advice_t;

/* ------------------------------------------------------------ 状态 */

/* 干扰识别的跨阈跳变环形数组容量（任务书定的 N=8）。
 * 判定只问「窗口内是否 >= PK_CAL_JAM_CROSSINGS 次」，所以容量只要不低于阈值
 * 就不影响结论：满了顶掉的一定是最旧的一条，而最旧的那条本来就是最先滑出
 * 60 s 窗口的。留到 8 而不是刚好 3，是为了阈值日后上调、以及阶段 C4 诊断页
 * 想看跳变密度时手里还有数据，代价 32 字节。 */
#define PK_CAL_JAM_RING_CAP 8u

typedef struct {
    /* 低精度（acc==0）连续段的起点。*_active=false 表示当前没有这样的连续段。
     * 用独立的 bool 而不是「0 表示没有」的哨兵：now_ms 是 uint32 毫秒，0 本身
     * 是合法时刻（回绕后必然出现），拿它当哨兵会在回绕那一拍误判。 */
    bool     low_active;
    uint32_t low_since_ms;

    /* 高精度（acc >= PK_CAL_EXIT_ACCURACY）连续段的起点。它同时喂两件事：
     * 闸门重新武装（REARM）与向导自动退出（EXIT）。 */
    bool     high_active;
    uint32_t high_since_ms;

    /* 自动弹页的闸门。true = 暂不自动弹（降级成 HINT）。见上面 PK_CAL_REARM_MS
     * 的注释：这是断骚扰循环的关键一环。 */
    bool     suppressed;

    /* 上一个 imu_valid 样本的 acc 是否在阈值之上，判「跨阈跳变」用。
     * acc_seeded=false 表示还没有过任何有效样本，首条样本不算跳变。 */
    bool     acc_seeded;
    bool     acc_high_prev;

    /* 跨阈跳变时刻的环形数组。cross_count 是已写入条数（<= CAP），前 count
     * 个下标都是有效的，所以统计时直接线性扫 0..count-1 即可，不必绕 head。 */
    uint32_t cross_ms[PK_CAL_JAM_RING_CAP];
    uint8_t  cross_head;
    uint8_t  cross_count;
    uint32_t last_cross_ms;

    bool     jammed;     /* 当前判定为强磁干扰环境 → 一律闭嘴 */
    bool     exit_ready; /* 高精度已连续保持 PK_CAL_EXIT_MS → 向导该退出 */
} pk_cal_advisor_t;

/* ------------------------------------------------------------ API */

/* 全清即正确初值：没有任何连续段、闸门开着、无跳变记录、不算干扰。 */
void pk_cal_advisor_reset(pk_cal_advisor_t *st);

/* 每帧调一次。imu_valid=false 的样本只代表「这一拍没取到数」，不推进也不清零
 * 任何计时器，更不参与跨阈统计（沿用 ui_state.c 里 valid 为假时两个计时器都
 * 不动的行为）。返回值是**电平**不是边沿：调用方每帧照读即可，模块不会在触发
 * 的那一拍把计时器清掉来防重复触发（那是胶水层用「当前不在校准页」判掉的事）。 */
pk_cal_advice_t pk_cal_advisor_update(pk_cal_advisor_t *st, uint32_t now_ms,
                                      bool imu_valid, uint8_t accuracy,
                                      pk_flight_phase_t phase);

/* 用户按了「稍后再说」，或用别的方式离开了校准页。 */
void pk_cal_advisor_dismiss(pk_cal_advisor_t *st, uint32_t now_ms);

/* 用户从设置页那一行主动进入校准页——他改主意了，闸门必须跟着重新武装，
 * 否则他在这一页没转够就退出去，本次开机内既不会自动提醒也不会有第二次提示。 */
void pk_cal_advisor_user_open(pk_cal_advisor_t *st, uint32_t now_ms);

/* 当前是否判定为强磁干扰环境。阶段 C4 的诊断页要显示它：jammed 时设备「什么
 * 都不提示」，若无处可查，一台在机坪上安静的盒子看起来像坏了。 */
bool pk_cal_advisor_is_jammed(const pk_cal_advisor_t *st);

/* 校准向导是否该自动退回 PFD（高精度已连续保持 PK_CAL_EXIT_MS）。
 *
 * 任务书的接口清单里没有这一条，是实现时加的（任务书原话「细节以实现为准」）：
 * 阶段 C2 的成功标准是 ui_state.c 删掉 s_cal_acc_first_low_us /
 * s_cal_acc_first_high_us 两个私有计时器、判定全部下沉到 advisor，而自动退出
 * 用的正是后者。不导出它，ui_state.c 就得把同一条计时器再实现一遍，C2 等于
 * 没做干净。取值口径与 is_jammed 一致：都是「截至上一次 update」的结论，
 * 所以是不带 now_ms 的 const getter。 */
bool pk_cal_advisor_should_exit_wizard(const pk_cal_advisor_t *st);

#ifdef __cplusplus
}
#endif
