/*
 * battery.c — GPIO20(BAT_ADC) → 电池电压 / 电量 / 充电状态。
 */
#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
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
            if (s_prev_mv != 0) {
                if (s_ema_mv > s_prev_mv + 8)      s_charging = true;
                else if (s_ema_mv < s_prev_mv - 8) s_charging = false;
            }
            s_prev_mv = s_ema_mv;
            s_last_us = now;
        }
    }

    if (out) {
        out->raw_mv   = s_ema_mv;
        out->batt_mv  = s_ema_mv * CONFIG_PK_BATT_DIVIDER_X100 / 100;
        out->pct      = mv_to_pct(out->batt_mv);
        out->charging = s_charging;
        /* 合理量程之外判为无效：没接电池时引脚是浮空的，读数会乱跳，
         * 显示一个煞有介事的百分比比不显示更糟。 */
        out->valid    = (out->batt_mv > 2500 && out->batt_mv < 4500);
    }
    return s_ema_mv > 0;
}
