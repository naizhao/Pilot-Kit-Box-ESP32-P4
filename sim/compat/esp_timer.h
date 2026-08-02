/* esp_timer.h — 模拟器桩。
 *
 * 固件用 esp_timer_get_time() 取单调微秒时钟。PC 上默认拿 clock_gettime 的
 * 单调时钟等价替代 —— 渲染代码只用它做时间差与陈旧判定，绝对值无意义。
 *
 * 截图那条路（--shot）例外，它必须**冻结**：capture.py 同时是回归基线，
 * 「改完布局跑一次、用 git diff 看哪些图变了」只有在同一份二进制两次跑出
 * 逐字节相同的图时才成立。见下方 pk_sim_clock_freeze() 与 page_stub.c 里
 * 那段实现的注释。
 */
#pragma once

#include <stdint.h>
#include <time.h>

/*
 * 冻结后的开机时长（微秒）；0 = 不冻结，走 host 单调时钟。
 *
 * 定义在 compat/page_stub.c，跟那边冻墙钟的 gettimeofday 坐在一起——模拟器
 * 里「时间是从哪来的」集中在一处，下次再加时间桩不用满仓库找。
 */
extern int64_t pk_sim_clock_frozen_us;

/* 冻结时钟。只该由 headless 截图路径（sim/main.c 的 run_headless）调用，
 * 交互模式必须让时钟真的走——长按进度、无操作自动收起、绿闪倒计时都靠它。
 * PK_SIM_UPTIME_S=<秒> 可改，默认值与取值理由见 page_stub.c 的实现。 */
void pk_sim_clock_freeze(void);

static inline int64_t esp_timer_get_time(void)
{
    if (pk_sim_clock_frozen_us) return pk_sim_clock_frozen_us;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
