/*
 * battery.c — GPIO20(BAT_ADC) → 电池电压 / 电量 / 充电状态。
 */
#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "batt";

#define BATT_ADC_GPIO      20
#define BATT_SAMPLE_US     (1000 * 1000)   /* 1 Hz：电池是慢变量 */
#define BATT_EMA_NUM       1               /* EMA 系数 1/8，压 ADC 噪声 */
#define BATT_EMA_DEN       8

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_chan;
static bool                      s_ready;

static int64_t s_last_us;
static int     s_ema_mv;        /* 平滑后的引脚电压 */
static int     s_prev_mv;       /* 上一次判充电用 */
static int     s_trend_mv;      /* 30 s 前的读数，长窗口趋势判据用 */
static int     s_raw_prev;      /* 上一次**原始**读数，阶跃检测用（不能用 EMA 后的） */
static int64_t s_trend_us;
static bool    s_charging;

void pk_batt_init(void)
{
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

    /* STAT 探测脚：只读不驱动。配成输入 + 上拉——ETA6098 的 STAT 是开漏，
     * 没有上拉的话高阻态读数会浮动。若这两个脚其实没接 STAT，上拉也无害。 */
    const gpio_config_t probe = {
        .pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_4),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&probe);

    s_ready = true;
    ESP_LOGI(TAG, "BAT_ADC on GPIO%d (unit %d chan %d), divider x%.2f",
             BATT_ADC_GPIO, (int)unit, (int)s_chan,
             CONFIG_PK_BATT_DIVIDER_X100 / 100.0);
}

/*
 * 锂电放电曲线 → 百分比。
 *
 * 不用线性映射：锂电在 3.7~4.0 V 之间平得像条直线，线性算法会让"还剩一半"
 * 停留很久然后突然掉到 0。下面这张分段表贴合典型 18650/软包放电曲线，
 * 拐点取在 3.85 / 3.70 / 3.50 V。
 */
static int mv_to_pct(int mv)
{
    if (mv >= 4150) return 100;
    if (mv >= 3850) return 75 + (mv - 3850) * 25 / 300;
    if (mv >= 3700) return 50 + (mv - 3700) * 25 / 150;
    if (mv >= 3500) return 20 + (mv - 3500) * 30 / 200;
    if (mv >= 3300) return      (mv - 3300) * 20 / 200;
    return 0;
}

bool pk_batt_get(pk_batt_t *out)
{
    if (!s_ready) { if (out) out->valid = false; return false; }

    const int64_t now = esp_timer_get_time();
    if (s_last_us == 0 || now - s_last_us >= BATT_SAMPLE_US) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) {
            int mv = raw;
            if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);

            /*
             * 阶跃检测：插拔充电线的那一瞬间，电压会跳一个台阶。
             *
             * 罩哥实测：拔掉线的瞬间 100% → 95%（约 65 mV）。物理上是
             * 电池端电压 = 开路电压 + 充电电流 × 内阻——接着线时被抬高，
             * 拔掉就回落到真实开路电压。
             *
             * 这个阶跃比缓慢趋势可靠得多，而且正好补上满电那个盲区：满电时
             * 插上线电压会跳 +65 mV，趋势法看不见（已经到顶了），阶跃法一眼
             * 就能看见。
             *
             * **必须用原始读数判，不能用 EMA 之后的**：EMA 1/8 会把 65 mV 的
             * 阶跃第一次只反映成 8 mV，正好埋在噪声里。阈值取 30 mV——远大于
             * ADC 噪声（实测同一状态下读数稳在 ±2 mV），也远小于真实阶跃。
             */
            if (s_raw_prev != 0) {
                const int step = mv - s_raw_prev;
                if (step > 30)       s_charging = true;    /* 接上了 */
                else if (step < -30) s_charging = false;   /* 拔掉了 */
            }
            s_raw_prev = mv;

            if (s_ema_mv == 0) s_ema_mv = mv;
            else s_ema_mv += (mv - s_ema_mv) * BATT_EMA_NUM / BATT_EMA_DEN;

            /*
             * 充电判据：电压在**持续上升**。
             *
             * 板上的 ETA 充电 IC 有 STAT 脚，但它没引到任何 P4 GPIO（wiki 的
             * 引脚表里找不到），所以读不到硬件充电状态，只能从电压趋势推。
             * 阈值取 8 mV/次（1 Hz）：放电时电压只会降或持平，能连续上升
             * 就是在充。噪声已被 EMA 压掉，误判概率低。
             *
             * 这是**推断不是事实**——真要可靠，得把 STAT 引到一个空闲 GPIO。
             */
            /*
             * 充电判据：与 **30 秒前** 比，不是与上一次比。
             *
             * 初版比相邻两次（1 Hz、阈值 8 mV），在低电量快充时能用，一到
             * 涓流阶段就失效——2026-07-29 罩哥实测 98% 时不显示充电符号，
             * 就是这个原因：涓流阶段电压上升 <1 mV/s，再经 EMA 1/8 平滑，
             * 相邻两次的差值永远够不到阈值。
             *
             * 改成长窗口：留一份 30 s 前的读数，比它高 5 mV 就算在充。
             * 涓流 0.5 mV/s × 30 s = 15 mV > 5 mV，检得出；而放电时电压
             * 只会往下走，不会误判。
             *
             * 仍然是**推断**——真正可靠要读 ETA6098 的 STAT 脚，见下面那段
             * 探测日志。
             */
            if (now - s_trend_us >= 30000000LL) {
                if (s_trend_mv != 0) {
                    if (s_ema_mv > s_trend_mv + 5)      s_charging = true;
                    else if (s_ema_mv < s_trend_mv - 5) s_charging = false;
                    /* 差值落在 ±5 mV 内：维持原判断。满电静置和涓流末期都
                     * 长这样，翻来覆去改状态比保持上一次更糟。 */
                }
                s_trend_mv  = s_ema_mv;
                s_trend_us  = now;
            }
            s_prev_mv = s_ema_mv;

            /* ETA6098 的 STAT 脚探测（临时）。
             *
             * 原理图里 STAT 在 U19 pin 9，文本上邻近 GPIO3/GPIO4，但**文本
             * 邻近不等于电气连接**——这个项目已经因为照抄过时文档给出过两次
             * 错误结论，所以不写死，先把两个脚的电平打出来。
             *
             * ETA6098 的 STAT 是开漏：充电中拉低，充满或未接输入时高阻
             * （被上拉拉高）。插拔 USB 各抓一次日志，看哪个脚跟着变，就能
             * 确定接的是哪一个；都不变说明没引到 GPIO，那就只能继续用电压
             * 趋势推断。 */
            ESP_LOGD(TAG, "STAT probe: GPIO3=%d GPIO4=%d mv=%d chg=%d",
                     gpio_get_level(GPIO_NUM_3), gpio_get_level(GPIO_NUM_4),
                     s_ema_mv, (int)s_charging);
            s_last_us = now;
            /* 标定用：万用表量到的电池电压 ÷ 这里的 raw = 分压比。
             * 2026-07-29 已用它标出 296（raw 1387 mV ↔ 实测 4.10 V），故降到
             * DEBUG。换板子或换电芯要重标时，esp_log_level_set("batt",
             * ESP_LOG_DEBUG) 打开即可，不必回头改代码。 */
            ESP_LOGD(TAG, "raw %d mV (x%.2f -> %d mV)", s_ema_mv,
                     CONFIG_PK_BATT_DIVIDER_X100 / 100.0,
                     s_ema_mv * CONFIG_PK_BATT_DIVIDER_X100 / 100);
        }
    }

    if (out) {
        /*
         * 一个已知的系统性偏差，暂不补偿：**充电时电量虚高**。
         *
         * 同一块电池，接着线读 4165 mV（100%）、拔掉立刻回落到 4100 mV
         * （95%）——差的 65 mV 是充电电流在内阻上的压降，不是真实电量。
         *
         * 不做补偿是因为压降随充电电流变化（快充阶段远大于涓流），减一个
         * 固定值会在别的阶段引入新的错。要做准得测内阻或读电量计，这块板
         * 两样都没有。记在这里，免得下次有人把它当 bug 查。
         */
        out->raw_mv   = s_ema_mv;
        out->batt_mv  = s_ema_mv * CONFIG_PK_BATT_DIVIDER_X100 / 100;
        out->pct      = mv_to_pct(out->batt_mv);
        /* 满电仍如实报充电状态：阶跃检测能证明线插着，不必再靠"电压还在涨"
         * 来推断，上一版那条 <4150 的抑制反而会把已知事实盖掉。 */
        out->charging = s_charging;
        /* 合理量程之外判为无效：没接电池时引脚是浮空的，读数会乱跳，
         * 显示一个煞有介事的百分比比不显示更糟。 */
        out->valid    = (out->batt_mv > 2500 && out->batt_mv < 4500);
    }
    return s_ema_mv > 0;
}
