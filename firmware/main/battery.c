/*
 * battery.c — 旧 battery API 的兼容壳（WP-D Task 2）。
 *
 * GPIO20(BAT_ADC) + GPIO21(STAT) 的采集与全部判据已整体迁入
 * power_eta6098.c，作为 power_service 的第一个（兜底）backend；
 * 这里只剩两个转发：
 *   pk_batt_init() → power_eta6098_init()（已弃用，main.c 直接调新口）
 *   pk_batt_get()  → power_service_snapshot() 薄包装 + 找 ETA6098
 *                    要 raw_mv（迁移合同见 power_service.h 文件头）
 * diag_page / pfd_statusbar 的既有调用点因此零改动。
 */
#include "battery.h"

#include "power_eta6098.h"
#include "power_service.h"

void pk_batt_init(void)
{
    power_eta6098_init();
}

bool pk_batt_get(pk_batt_t *out)
{
    if (out == NULL) return false;

    const power_snapshot_t s = power_service_snapshot();
    /* 返回值沿用"s_ready 闸"的精神：有新鲜数据才算可用。服务端已把
     * "从未报数"（updated_us=0）和"距上次报数 >5 s"都折进 stale，
     * 这里不再自己另立口径。今天两个调用方都只读字段不看出参返回值，
     * 但 false 路径必须把字段清干净——旧实现这条路上 raw_mv 是未初始化
     * 的垃圾，诊断页"无电池"分支会把它原样画出来。 */
    if (s.stale) {
        out->valid    = false;
        out->raw_mv   = 0;
        out->batt_mv  = 0;
        out->pct      = 0;
        out->charging = false;
        return false;
    }

    out->valid    = s.pct_valid;   /* 2500..4500 mV 量程闸在 ETA6098 backend 内 */
    out->batt_mv  = s.batt_mv;
    out->pct      = s.pct_est;
    out->charging = s.charging;
    /* raw_mv 没有公共快照字段：恒取 ETA6098 引脚侧 EMA 毫伏——没有
     * 赢家检测，也不做（YAGNI：消费方都不在 SY6970 赢家状态下依赖它，
     * 诊断页只在 ETA6098 分支渲染 raw）。注意 v4 上 SY6970 是快照赢家
     * 时（v4 供电），raw_mv × 分压比 ≠ batt_mv（batt_mv 来自 SY6970
     * 的 BATV，battery.h 注释里的等式在那个状态下不成立），消费方
     * 不得依赖该等式。 */
    int raw = 0;
    (void)power_eta6098_raw_mv(&raw);
    out->raw_mv = raw;
    return true;
}
