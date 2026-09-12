/*
 * power_eta6098.c — ETA6098 载板电源 backend（WP-D Task 2）。
 *
 * 原 battery.c 的 GPIO20(BAT_ADC) + GPIO21(STAT) 采集与判据整体迁入这里，
 * 作为 power_service 的第一个（兜底）backend：ETA6098 在两代载板上都必然
 * 在位，init 成功后注册；v4 powered 的 SY6970（Task 4）探测到 ACK 后注册
 * 在前，掉线时自动回落到本源（选择语义见 power_service.h 文件头）。
 *
 * 采样节奏从"读的时候顺手采一拍"变为服务的 1 Hz poll tick——这就是
 * WP-D 的目标架构，行为本身（STAT 判据、阶跃检测、EMA、CC/CV 补偿）
 * 一律逐字保留。日志 TAG 沿用 "batt"：esp_log_level_set("batt", ...)
 * 的标定入口不变。
 */
#include "power_eta6098.h"
#include "pk_batt_model.h"

#include "power_service.h"

#include <stdatomic.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "batt";

#define BATT_ADC_GPIO      20
#define BATT_SAMPLE_US     (1000 * 1000)   /* 1 Hz：电池是慢变量 */
#define BATT_EMA_NUM       1               /* EMA 系数 1/8，压 ADC 噪声 */
#define BATT_EMA_DEN       8
#define BATT_STAT_GPIO     21   /* TP1(ETA6098 STAT) 飞线 -> J3 pin 15 */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_chan;
static bool                      s_ready;

static int64_t s_last_us;   /* 最近一次**成功采样**的时刻（0=从未采到） */
/* EMA 平滑后的引脚电压。poll 任务写、诊断页经 raw_mv 读（UI 可抢占
 * poll）——C11 原子 relaxed（同 qmc5883p.c 的诊断计数口径）：单字访存
 * 不撕裂即可，该值本就是诊断/标定辅助量。其余 statics 只有 poll 任务
 * 摸（写者唯一、无跨任务读者），保持普通变量。 */
static _Atomic int s_ema_mv;
static int     s_prev_mv;       /* 上一次判充电用 */
static int     s_trend_mv;      /* 30 s 前的读数，长窗口趋势判据用 */
static int     s_raw_prev;      /* 上一次**原始**读数，阶跃检测用（不能用 EMA 后的） */
static int64_t s_trend_us;
static bool    s_charging;
static bool    s_stat_valid;   /* 曾观察到 STAT=LOW，确认飞线已接 */
static bool    s_vbus;        /* USB/电源在位。比 charging 更宽：充满停充时
                               * charging=false 但 vbus 仍为 true，这一档的
                               * 端电压补偿只看它（见 supply_drop_mv）。 */


/*
 * 插着电时的端电压补偿。
 *
 * 上面那张表是**放电曲线**，按静置开路电压标定。而只要 USB 插着，BAT_ADC
 * 测到的就不是电池开路电压：电池与充电器输出并在同一个 VBAT 节点上，读数被
 * 抬高，直接查表必然虚高。
 *
 * 判据是「插没插电」而不是「在不在充电」——这一点是 2026-08-04 真机数据逼出来
 * 的。三个场景，同一块电池：
 *
 *     充满**停充**(STAT=HIGH，线还插着)   4138 mV   ← 仍虚高 96 mV
 *     拔电                                4042 mV   ← 真实开路电压
 *     重插**充电中**(STAT=LOW)            4170 mV   ← 虚高 128 mV
 *
 * 头一行是关键：ETA6098 充满后停止充电、STAT 拉高，但线还插着，VBAT 被充电器
 * 维持在 4.14 V。此时"没在充电"却依然虚高 96 mV。上一版按 STAT 判断、停充就
 * 不补偿，于是这一档显示 99%、拔掉掉到 91%，还是跳 8 个点。
 *
 * 压降的形状（ETA6098 是 CC/CV 充电器）：
 *
 *   CC 恒流段（端电压还没顶到 4.15 V）：电流恒定，压降 ≈ I×R 也基本恒定；
 *   CV 恒压段：电压被钳住，电流从满流降到截止，压降**收敛到维持值**（不是 0);
 *   停充维持：电流为 0，只剩充电器维持电压高出开路电压的那一截。
 *
 * 所以 CV 段是从 CC 压降线性收敛到 HOLD 压降，而不是收敛到 0。用上面三个
 * 实测点验证，三种状态全部收敛到同一个 91%：
 *
 *     充电中 4170 − 128 = 4042 → 91%
 *     停充   4138 −  96 = 4042 → 91%
 *     拔电   4042 −   0 = 4042 → 91%
 *
 * 两个常量都是**经验值**，不是测出来的内阻：
 *   BATT_CC_DROP_MV   = 充电中稳定读数 − 拔电稳定读数（CC 段）
 *   BATT_HOLD_DROP_MV = 停充维持读数   − 拔电稳定读数
 * 换电芯或改充电电流后要重标，把 batt 日志开到 DEBUG，插拔一轮取差值即可。
 *
 * 仍是估算——这块板既没有电流采样也没有库仑计。但比「插电时系统性虚高 8~11
 * 个百分点」好得多。
 */
#define BATT_CC_DROP_MV     150   /* CC 恒流段的 I×R */
#define BATT_HOLD_DROP_MV    96   /* 停充维持：充电器维持电压 − 开路电压 */
#define BATT_CV_KNEE_MV    4150   /* CC→CV 拐点 */
#define BATT_CV_FULL_MV    4200   /* CV 段终点，压降收敛到 HOLD */

static int supply_drop_mv(int batt_mv, bool vbus, bool charging)
{
    if (!vbus)     return 0;                    /* 拔了：读数就是开路电压 */
    if (!charging) return BATT_HOLD_DROP_MV;    /* 插着但已停充（充满维持） */
    if (batt_mv <= BATT_CV_KNEE_MV) return BATT_CC_DROP_MV;
    if (batt_mv >= BATT_CV_FULL_MV) return BATT_HOLD_DROP_MV;
    return BATT_CC_DROP_MV - (batt_mv - BATT_CV_KNEE_MV)
                             * (BATT_CC_DROP_MV - BATT_HOLD_DROP_MV)
                             / (BATT_CV_FULL_MV - BATT_CV_KNEE_MV);
}

/*
 * 服务的 1 Hz 轮询入口：采一拍（内部保留原 1 Hz 节拍闸），把状态摊成一份
 * 公共快照。now_us 直接来自服务（时序合同见 power_service.h），stale 由
 * 服务端统一重算。updated_us 如实盖「最近一次**成功采样**」的时刻
 * （s_last_us）——审计 F1：ADC 持续失败时旧读数绝不能顶着 now_us 冒充
 * 新鲜数据；从未采到过则 updated_us=0（从未报数哨兵，服务端恒判 stale）。
 */
static power_snapshot_t eta6098_poll(int64_t now_us)
{
    power_snapshot_t out;

    /* 注册只在 init 成功后发生，走到这必然 s_ready——留着这道闸只是
     * 防御（服务层不假设 backend 的内部状态）。 */
    if (!s_ready) {
        out.source           = POWER_SRC_UNKNOWN;
        out.backend          = POWER_BACKEND_ETA6098;
        out.charging         = false;
        out.vbus_present     = false;
        out.batt_mv          = 0;
        out.pct_est          = 0;
        out.pct_valid        = false;
        out.time_degraded_na = true;
        out.updated_us       = 0;                  /* 从未报数哨兵 */
        return out;
    }

    const int64_t now = now_us;
    if (s_last_us == 0 || now - s_last_us >= BATT_SAMPLE_US) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) {
            int mv = raw;
            if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
            int ema = atomic_load_explicit(&s_ema_mv,
                                           memory_order_relaxed);

            /* --- 充电状态判断 ---
             *
             * 优先级：STAT 引脚 > 阶跃检测 > 30 s 趋势。
             *
             * STAT 引脚（TP1 -> GPIO21 飞线）是 ETA6098 的硬件状态输出，
             * 充电中拉低。一旦观察到 STAT=LOW 就确认飞线已接（s_stat_valid），
             * 之后完全信任 STAT：
             *   LOW  -> 正在充电
             *   HIGH -> 未充电（已充满或未接电源）
             *
             * STAT 是开漏，飞线未接时 GPIO21 被内部上拉恒为 HIGH，与"插着
             * 但充满"无法区分。所以 vbus 在位的最终判据交给阶跃检测（见下）：
             * 电压陡降 = 拔电。这里只管"此刻在不在充电"。
             */
            const int stat = gpio_get_level(BATT_STAT_GPIO);
            if (stat == 0) {
                /* 第一次看到 LOW = TP1 飞线确实接上了，而且此刻在充电。
                 * 这条走 INFO 打一次（不是 DEBUG）：飞线接没接好是装配问题，
                 * 装完就想立刻确认，不该要求先去改日志级别重烧一遍。
                 * 之后 s_stat_valid 恒为真，这里不会再打。 */
                if (!s_stat_valid) {
                    ESP_LOGI(TAG, "STAT(GPIO%d)=LOW —— TP1 飞线已确认接通，"
                                  "充电状态改用硬件信号（不再靠电压推断）",
                             BATT_STAT_GPIO);
                }
                s_stat_valid = true;
                s_charging = true;
                s_vbus     = true;     /* STAT=LOW 必然有电在喂 */
            } else if (s_stat_valid) {
                s_charging = false;
                /* STAT=HIGH 但不在这里清 vbus：充满停充时 vbus 仍在位，
                 * 端电压仍被维持（见 supply_drop_mv 的 HOLD 一档）。拔电靠
                 * 下面的阶跃检测翻 s_vbus。 */
            }

            /*
             * 阶跃检测：插拔充电线的那一瞬间，引脚电压会跳一个台阶。
             *
             * 这一段在 s_stat_valid 为真时**也跑**（不再只在 else 分支里）——
             * 因为 STAT 分不清"充满停充"和"拔了电源"，两者都是 HIGH，而它们
             * 的端电压补偿差了一档（HOLD vs 0）。只有电压陡降能区分：拔电
             * 那一下 VBAT 节点从"被充电器维持"变回"纯电池开路"，掉一截。
             *
             * 必须用原始读数判：EMA 1/8 会把台阶压平埋进噪声。阈值 30 mV
             * （引脚侧）远大于噪声（实测 ±2 mV）。用 s_raw_prev（上一次原始
             * 读数），不能用 EMA 后的。
             */
            if (s_raw_prev != 0) {
                const int step = mv - s_raw_prev;
                if (step > 30) {
                    s_vbus = true;
                    if (!s_stat_valid) s_charging = true;
                } else if (step < -30) {
                    s_vbus     = false;
                    s_charging = false;
                }
            }

            if (!s_stat_valid) {
                /*
                 * 30 s 长窗口趋势：与 30 秒前比，高 5 mV 就算在充。
                 * 涓流阶段相邻两次差值 <1 mV/s，短窗口检不出；30 s 窗口下
                 * 涓流 0.5 mV/s * 30 s = 15 mV > 5 mV，检得出。
                 */
                if (now - s_trend_us >= 30000000LL) {
                    if (s_trend_mv != 0) {
                        if (ema > s_trend_mv + 5)      s_charging = true;
                        else if (ema < s_trend_mv - 5) s_charging = false;
                    }
                    s_trend_mv  = ema;
                    s_trend_us  = now;
                }
            }
            s_raw_prev = mv;

            if (ema == 0) ema = mv;
            else ema += (mv - ema) * BATT_EMA_NUM / BATT_EMA_DEN;
            atomic_store_explicit(&s_ema_mv, ema, memory_order_relaxed);
            s_prev_mv = ema;

            ESP_LOGD(TAG, "STAT(%d)=%d valid=%d mv=%d chg=%d",
                     BATT_STAT_GPIO, stat, (int)s_stat_valid,
                     ema, (int)s_charging);
            s_last_us = now;   /* 只有成功采样才推进（新鲜度锚） */
            /* 标定用：万用表量到的电池电压 ÷ 这里的 raw = 分压比。
             * 2026-07-29 已用它标出 296（raw 1387 mV ↔ 实测 4.10 V），故降到
             * DEBUG。换板子或换电芯要重标时，esp_log_level_set("batt",
             * ESP_LOG_DEBUG) 打开即可，不必回头改代码。 */
            const int batt_mv_dbg = ema * CONFIG_PK_BATT_DIVIDER_X100 / 100;
            const int drop_dbg    = supply_drop_mv(batt_mv_dbg, s_vbus, s_charging);
            ESP_LOGD(TAG, "raw %d mV (x%.2f -> %d mV) vbus=%d chg=%d drop=%d mV "
                          "-> %d mV = %d%%",
                     ema, CONFIG_PK_BATT_DIVIDER_X100 / 100.0,
                     batt_mv_dbg, (int)s_vbus, (int)s_charging, drop_dbg,
                     batt_mv_dbg - drop_dbg,
                     pk_batt_mv_to_pct(batt_mv_dbg - drop_dbg));
        }
    }

    /* 摊成公共快照（原 pk_batt_get 的出参填充段）：
     * 百分比按**扣掉端电压抬升后**的电压算，见 supply_drop_mv() 顶部那段
     * 推导。batt_mv 本身仍 report 实测值——它是标定分压比和排查用的原始
     * 量，补偿只影响"给人看的电量"。
     *
     * 补偿看 s_vbus（插没插电），不看 s_charging（在不在充电）：充满停充
     * 时充电器仍在维持电压，读数照样虚高一档。见 supply_drop_mv 的 HOLD。 */
    const int ema    = atomic_load_explicit(&s_ema_mv,
                                            memory_order_relaxed);
    /* source 档位：这块板没有 power path——USB 在位时整机由 USB 供电、
     * 电池只在充电，所以 vbus 在位即"外部电"；不在位即电池放电。 */
    out.source       = s_vbus ? POWER_SRC_EXTERNAL : POWER_SRC_BATTERY;
    out.backend      = POWER_BACKEND_ETA6098;
    out.batt_mv      = (uint16_t)(ema * CONFIG_PK_BATT_DIVIDER_X100 / 100);
    const int drop   = supply_drop_mv(out.batt_mv, s_vbus, s_charging);
    out.pct_est      = (uint8_t)pk_batt_mv_to_pct(out.batt_mv - drop);
    /* 满电仍如实报充电状态：阶跃检测能证明线插着，不必再靠"电压还在涨"
     * 来推断，上一版那条 <4150 的抑制反而会把已知事实盖掉。 */
    out.charging     = s_charging;
    out.vbus_present = s_vbus;
    /* 合理量程之外判为无效：没接电池时引脚是浮空的，读数会乱跳，
     * 显示一个煞有介事的百分比比不显示更糟。 */
    out.pct_valid    = (out.batt_mv > 2500 && out.batt_mv < 4500);
    /* 没有电流采样/库仑计，剩余时间永远不可估，如实标注。 */
    out.time_degraded_na = true;
    /* 审计 F1：新鲜度锚 = 最近一次成功采样（s_last_us）。ADC 失败的拍
     * 原样带旧时间戳返回——读数与时间戳一致地"旧"，服务端 5 s 后判
     * stale；从未成功则为 0（从未报数哨兵）。绝不拿 now_us 给旧读数
     * 顶新鲜。 */
    out.updated_us   = s_last_us;
    return out;
}

bool power_eta6098_raw_mv(int *out_mv)
{
    if (out_mv == NULL) return false;
    const int ema = atomic_load_explicit(&s_ema_mv,
                                         memory_order_relaxed);
    if (ema == 0) return false;   /* 一拍都没采过：无从谈起 */
    *out_mv = ema;
    return true;
}

/* backend 登记项：name 仅用于日志；id 由服务盖进聚合快照。 */
static const power_backend_t s_backend = {
    .name = "eta6098",
    .id   = POWER_BACKEND_ETA6098,
    .poll = eta6098_poll,
};

void power_eta6098_init(void)
{
    static bool s_brought_up;
    if (s_brought_up) return;                      /* 幂等：只装一次 */
    s_brought_up = true;

    adc_unit_t unit;
    /* 用 IDF 的映射函数而不是查表硬编码：GPIO→ADC 通道的对应关系每颗芯片
     * 都不同，写死一次就得为每个新板号再核对一次。 */
    if (adc_oneshot_io_to_channel(BATT_ADC_GPIO, &unit, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pin", BATT_ADC_GPIO);
        return;
    }

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        return;
    }

    /* 12 dB 衰减：满量程约 3.1 V。1:1 分压下 4.2 V 电池对应 2.1 V，落在
     * 量程内还留了余量；用更小的衰减会在满电时削顶。 */
    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, s_chan, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
        return;
    }

    /* 曲线拟合校准：没有它，不同芯片间的 ADC 偏差能到 ±100 mV，
     * 而锂电 3.7→4.2 V 全程只有 500 mV，那点偏差直接毁掉电量刻度。 */
    adc_cali_curve_fitting_config_t cal = {
        .unit_id  = unit,
        .chan     = s_chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable — readings will be coarse");
        s_cali = NULL;
    }

    /* STAT 引脚（TP1 飞线到 GPIO21）：输入 + 上拉。
     * ETA6098 STAT 开漏——充电中拉低，充满/未接输入时高阻（上拉拉高）。
     * 飞线未接时 GPIO21 恒为高，逻辑见 eta6098_poll() 的回退处理。 */
    const gpio_config_t stat_cfg = {
        .pin_bit_mask = (1ULL << BATT_STAT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&stat_cfg);

    /* 开机初始化 s_vbus：没有阶跃可看（阶跃检测要两个采样点），只能用电压
     * 估。充满维持 / 充电中的端电压 ≥4.10 V，拔电开路通常已回落到这以下。
     * 阈值取 4100：拔电稳态 4042 mV 在它下面，插电维持 4138 mV 在它上面。
     * 估错的话下一次阶跃（拔/插）会纠正，最坏只错到下一次插拔。 */
    {
        int raw0 = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw0) == ESP_OK) {
            int mv0 = raw0;
            if (s_cali) adc_cali_raw_to_voltage(s_cali, raw0, &mv0);
            s_vbus = (mv0 * CONFIG_PK_BATT_DIVIDER_X100 / 100 >= 4100);
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "BAT_ADC on GPIO%d (unit %d chan %d), divider x%.2f",
             BATT_ADC_GPIO, (int)unit, (int)s_chan,
             CONFIG_PK_BATT_DIVIDER_X100 / 100.0);
    /* 开机报一次 STAT 初始电平，帮装配时定位飞线。
     * 注意判读方向：LOW 一定说明飞线通了（且在充电）；HIGH 则**分不出**
     * "没接线"和"接了但没充电"——因为没接线时内部上拉同样把它读成 HIGH。
     * 所以要确认飞线，插上充电线看有没有那条 "TP1 飞线已确认接通"。 */
    ESP_LOGI(TAG, "STAT(GPIO%d) 初始电平=%s（LOW=正在充电且飞线已通；"
                  "HIGH=未充电 或 飞线未接，两者此刻无法区分）",
             BATT_STAT_GPIO, gpio_get_level(BATT_STAT_GPIO) ? "HIGH" : "LOW");

    /* 硬件就位，注册进服务。注册次序=优先级（首个非 stale 者赢，见
     * power_service.h:15-20）：本调用发生在 main.c 里，当前是唯一的
     * backend，即当前最高优先级；v4 上 Task 4 必须在 power_eta6098_init()
     * **之前**探测注册 SY6970（权威源排前），届时本 backend 自然回落为
     * 兜底。 */
    power_service_register(&s_backend);
}
