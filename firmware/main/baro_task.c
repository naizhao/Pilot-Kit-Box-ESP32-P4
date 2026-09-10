/*
 * baro_task.c — BMP388 气压计驱动。
 *
 * Task 1: 挂上 BMP388 device 并读到 CHIP_ID=0x50,证明 I²C0 复用通路打通。
 * Task 2: 读 calib + 配置 OSR/ODR/PWR_CTRL + 周期读温压 + 气压高度/VS 计算。
 *
 * BMP388 I²C 地址: 0x76
 * CHIP_ID 寄存器: 0x00,期望值: 0x50
 *
 * 与 BNO085 IMU 共享 I²C0 总线。scl_speed_hz 必须与 IMU 一致(400 kHz)。
 */

#include "baro.h"
#include "baro_compensate.h"  /* BMP388 补偿数学(WP-B Task 5 抽出的纯单元) */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "pk_i2c0_bus.h"      /* pk_i2c0_bus_get —— 总线已上移为板级模块 */
#include "pk_i2c0_recover.h"  /* 总线级恢复:BMP388 挂掉多半是总线塌了,不是它自己 */
#include "pk_bringup_retry.h" /* 必装器件的开机 bring-up 退避重试(见该头文件的病因) */
#include "config_qnh.h" /* pk_qnh_get() — 动态 QNH(修正海压) */
#include "qnh_math.h"   /* pk_qnh_from_pressure_alt() — auto-QNH 反解 */
#include "gps.h"        /* pk_gps_get() — auto-QNH 用 GPS 正高作基准 */
#include "config_demo.h"
#include "demo_data.h"

static const char *TAG = "baro";

#define BMP388_ADDR        0x76
#define BMP388_REG_CHIPID  0x00
#define BMP388_CHIPID      0x50
#define BARO_INT_PIN       31   /* 可选 data-ready；GPIO27 为板载 LCD RST */

/* BMP388 寄存器地址 */
#define BMP388_REG_DATA    0x04   /* PRESS_XLSB..TEMP_MSB (6 bytes) */
#define BMP388_REG_EVENT   0x10   /* EVENT 状态标志（DS §4.3.7 Table 31, p.33）。
                                   * ⚠ 地址是 0x10，不是审计单上误写的 0x19
                                   * （0x19 = INT_CTRL, Table 38, p.35）。 */
#define BMP388_REG_PWR     0x1B   /* PWR_CTRL */
#define BMP388_REG_OSR     0x1C   /* OSR */
#define BMP388_REG_ODR     0x1D   /* ODR */
#define BMP388_REG_CONFIG  0x1F   /* CONFIG: IIR 滤波(默认 0 = bypass) */
#define BMP388_REG_CALIB   0x31   /* 校准系数起始 (21 bytes) */

/* EVENT bit0 por_detected：'1' after device power up or softreset，
 * clear-on-read（DS §4.3.7 Table 31, p.33）。 */
#define BMP388_EVENT_POR   0x01

/* BMP388 与 BNO085 共享 I²C0 总线,scl_speed_hz 必须与 IMU 一致。
 * imu_task.c 中 IMU_I2C_HZ = 400000,故此处同样使用 400000。 */
#define BARO_I2C_HZ        400000

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_mutex;
static pk_baro_state_t         s_state;   /* guarded by s_mutex */

/* 量化校准系数 + t_lin 中间结果。补偿公式本体在 baro_compensate.c
 * (手册 §9 参考实现，WP-B Task 5 抽成纯单元供 host 单测)。 */
static baro_calib_t s_cal;

/* 配置+校准成功 gate:配置写入或校准读取任一失败前禁止输出 valid 数据 */
static volatile bool s_ready = false;

/* ─────────────────────────────────────────────────────────────────────── */
/*  寄存器读写辅助                                                          */
/* ─────────────────────────────────────────────────────────────────────── */

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return pk_i2c0_bus_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return pk_i2c0_bus_transmit(s_dev, b, 2, 100);
}

/* 前向声明:configure_and_calibrate 调用 load_calibration */
static esp_err_t load_calibration(void);

/* ─────────────────────────────────────────────────────────────────────── */
/*  配置 OSR/ODR/PWR_CTRL + 读校准系数。任一步失败返回非 ESP_OK。            */
/* ─────────────────────────────────────────────────────────────────────── */

static esp_err_t configure_and_calibrate(void)
{
    esp_err_t e;
    if ((e = reg_write(BMP388_REG_OSR, 0x02)) != ESP_OK) { ESP_LOGE(TAG, "OSR write: %s",  esp_err_to_name(e)); return e; }
    if ((e = reg_write(BMP388_REG_ODR, 0x04)) != ESP_OK) { ESP_LOGE(TAG, "ODR write: %s",  esp_err_to_name(e)); return e; }
    /* IIR 滤波 coef 15(bits[3:1]=100 → 0x08):传感器硬件层抑制气压测量噪声,
     * 这是高度/VS 抖动的根因修复——原来 CONFIG 未配置 = IIR bypass = 输出原始噪声。 */
    if ((e = reg_write(BMP388_REG_CONFIG, 0x08)) != ESP_OK) { ESP_LOGE(TAG, "CONFIG(IIR) write: %s", esp_err_to_name(e)); return e; }
    if ((e = reg_write(BMP388_REG_PWR, 0x33)) != ESP_OK) { ESP_LOGE(TAG, "PWR write: %s",  esp_err_to_name(e)); return e; }
    vTaskDelay(pdMS_TO_TICKS(100));   /* 等首次转换 (>= 80ms ODR 周期) */
    return load_calibration();        /* 读 21 字节校准 */
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  读并解析 21 字节校准系数(纯只读,不写配置)                               */
/* ─────────────────────────────────────────────────────────────────────── */

static esp_err_t load_calibration(void)
{
    uint8_t c[21];
    esp_err_t err = reg_read(BMP388_REG_CALIB, c, 21);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calib read failed: %s", esp_err_to_name(err));
        return err;
    }

    baro_calib_parse(c, &s_cal);

    ESP_LOGI(TAG, "calib loaded T1=%.1f T2=%.3e P5=%.1f P6=%.3e",
             s_cal.t1, s_cal.t2, s_cal.p5, s_cal.p6);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  bring-up:一次完整的"把 BMP388 弄到能出数"的尝试                        */
/* ─────────────────────────────────────────────────────────────────────── */

/* 退避涨到这一档才把失败升级成**总线级**恢复请求。
 *
 * 为什么不是每轮都请求:总线复位会打扰同一条总线上的 imu / touch / codec,
 * 而开机头几秒的失败多半是别人(GT911)还没让开——那种瞬态自己会好,复位反而
 * 是在别人的初始化中间插一脚。退避涨到 8 s 说明已经连败 4 轮 ≈ 7 s,不再像
 * 瞬态;同时"退避 ≥8 s"这件事本身就给了两次请求之间 ≥8 s 的间隔,不会打成
 * reset 风暴(下面仍显式按时间再卡一道,免得以后有人改了退避档位就破功)。 */
#define BARO_UP_RECOVER_MIN_BACKOFF_MS 8000

typedef struct {
    bool    ever_recovered;    /* 请求过总线恢复没有(区分"从没请求过"与 t=0) */
    int64_t last_recover_us;
} baro_up_ctx_t;

/* 一次 bring-up 尝试:验 CHIP_ID → 配置 OSR/ODR/IIR/PWR + 读标定 → 清 POR。
 * 全部走同一条判据,所以总线恢复后的重配、器件本地 POR 后的重配、开机首次
 * 配置都是同一段代码,不存在"只修了开机那条路径"的可能。 */
static bool baro_bring_up_attempt(void *ctx)
{
    (void)ctx;

    uint8_t id = 0;
    esp_err_t err = reg_read(BMP388_REG_CHIPID, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CHIP_ID read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (id != BMP388_CHIPID) {
        ESP_LOGW(TAG, "CHIP_ID=0x%02X (期望 0x%02X)", id, BMP388_CHIPID);
        return false;
    }

    if ((err = configure_and_calibrate()) != ESP_OK) {
        ESP_LOGW(TAG, "configure+calibrate failed: %s", esp_err_to_name(err));
        return false;
    }

    /* 读一次 EVENT 清掉上电 POR 标志（clear-on-read, Table 31）：
     * 上电/复位后本该就是 1，不留给主循环让第一拍白重配一遍。 */
    uint8_t ev = 0;
    if (reg_read(BMP388_REG_EVENT, &ev, 1) != ESP_OK) ev = 0;
    ESP_LOGI(TAG, "BMP388 chip_id=0x%02X OK (EVENT@bring-up=0x%02X)", id, ev);
    return true;
}

static void baro_on_failed_bring_up(void *ctx, uint32_t attempt_no, uint32_t backoff_ms)
{
    baro_up_ctx_t *up = (baro_up_ctx_t *)ctx;

    /* 没弄起来就说没弄起来:高度表的消费者(PFD / 飞行记录)看的是这面旗,
     * 绝不能让上一次工作态的读数一直挂着冒充当前高度。 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.valid = false;
    xSemaphoreGive(s_mutex);

    ESP_LOGW(TAG, "BMP388 bring-up 第 %u 次失败 — %u ms 后重试",
             (unsigned)attempt_no, (unsigned)backoff_ms);

    if (backoff_ms < BARO_UP_RECOVER_MIN_BACKOFF_MS) return;

    const int64_t now = esp_timer_get_time();
    if (up->ever_recovered &&
        now - up->last_recover_us < (int64_t)BARO_UP_RECOVER_MIN_BACKOFF_MS * 1000) {
        return;
    }
    up->ever_recovered  = true;
    up->last_recover_us = now;
    /* 这里用 recover_request 而不是 client_report:上层已经确信"连着几轮
     * 一个字节都读不出来 = 总线坏了",不需要再过一遍去抖计数器(见
     * pk_i2c0_recover.h 对 who 的说明)。两条路径并用会让节流失效。 */
    (void)pk_i2c0_recover_request("baro/bring-up");
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  baro_task                                                               */
/* ─────────────────────────────────────────────────────────────────────── */

static void baro_task(void *arg)
{
    (void)arg;

    /* 总线级故障的上报口。门槛「连续 5 次失败**且**这串失败已经持续 ≥2 s」:
     *   - 正常读数循环 100 ms 一轮 → 2.0 s 触发。2026-08-03 那次真机日志里
     *     baro 在 13619 ms 挂掉、之后再没恢复,按这个门槛 ~15.6 s 就会发起
     *     总线恢复,比 imu 那条 5 s stall 的路径快得多,也就成了主检测器。
     *   - bring-up 重试那条路径不喂它,自带退避与升级节流(见
     *     baro_on_failed_bring_up)。 */
    pk_i2c0_client_t i2c_client;
    pk_i2c0_client_init(&i2c_client, "baro", 5, 2 * 1000000LL);

    /* 总线恢复代数（住在板级总线模块里）。总线被谁救回来都要重来一遍配置+标定。 */
    uint32_t bus_gen = pk_i2c0_bus_generation();

    /* bring-up 的重试器。任务**先存在**,握手只是它循环里的一步:开机那一瞬
     * 总线塌了(2026-08-03 GT911 只 found 没 ready)不再等于"这次开机没有高度
     * 表"。器件真的没焊/坏了也照样留着任务——代价是每分钟一轮探测,收益是
     * 插回去最迟一分钟自己上线。 */
    baro_up_ctx_t up_ctx = { .ever_recovered = false, .last_recover_us = 0 };
    const pk_bringup_retry_t up_cfg = {
        .name              = "bmp388",
        .attempt           = baro_bring_up_attempt,
        .on_failed_attempt = baro_on_failed_bring_up,
        .ctx               = &up_ctx,
    };

    /* 开机时必然是"没就绪",走下面循环里那条统一的 bring-up 路径。 */
    s_ready = false;

    /* ── 循环读温压 → 补偿 → 高度/VS → 填 s_state ── */
    /* QNH_PA 已改为每轮调 pk_qnh_get() * 100.0f(Task 9) */
    /* 高度/VS 防抖(配合 BMP388 硬件 IIR):软件高度低通 + 显示滞回 + VS 基于平滑高度。 */
    static const float ALT_ALPHA   = 0.2f;   /* 高度 EMA(软件低通,补充硬件 IIR) */
    static const float VS_ALPHA    = 0.1f;   /* VS EMA */
    static const int   VS_DEADBAND = 50;     /* |VS|<50fpm 显 0(平飞不抖) */
    static const int   VS_STEP     = 50;     /* VS 显示量化到 50fpm 步进 */

    float   alt_filt      = 0.0f;            /* 高度 EMA 状态 */
    int     disp_alt_ft   = 0;               /* 显示高度(滞回,防量化边界来回抖) */
    float   prev_alt_filt = 0.0f;            /* 上一帧平滑高度(VS 微分用) */
    int64_t prev_time_us  = 0;
    float   vs_ema        = 0.0f;
    bool    has_prev      = false;
    int     log_tick     = 0;

    while (1) {
        /* ── 0. 总线被救回来了？配置和标定都得重来 ──
         *
         * 总线复位只是把线放开了,BMP388 的 PWR_CTRL/OSR/ODR/CONFIG 是不是
         * 还在、标定系数读得对不对,都得重新验一遍。复用下面 !s_ready 那条
         * 统一的 bring-up 重试路径,不另写一份。 */
        {
            const uint32_t gen = pk_i2c0_bus_generation();
            if (gen != bus_gen) {
                bus_gen = gen;
                ESP_LOGW(TAG, "I²C0 总线已复位(第 %lu 轮)— 重写 BMP388 配置并重读标定",
                         (unsigned long)gen);
                s_ready       = false;
                has_prev      = false;   /* 断档后别让 VS 出尖峰 */
                vs_ema        = 0.0f;
                pk_i2c0_client_reset(&i2c_client);
            }
        }

        /* 配置+校准 gate。开机首次、总线复位后重放、器件本地 POR 后重配
         * 三条路径都汇到这里:退避重试到成功为止,成功前 valid 恒 false。
         *
         * pk_bringup_retry_run() 只在成功时返回(不返回失败码、不删任务),
         * 所以这里不需要再写一份 "失败就 continue" 的循环——那正是过去
         * 每处各抄一遍、抄错一处就退回忙等的地方。 */
        if (!s_ready) {
            pk_bringup_retry_run(&up_cfg);
            s_ready = true;
            /* 重试期间别人(或我们自己)可能复位过总线;成功握手用的就是复位
             * 之后的总线,所以把代数对齐,免得下一轮又判一次"代数变了"白重来。 */
            bus_gen  = pk_i2c0_bus_generation();
            has_prev = false;      /* 断档后别让 VS 出尖峰 */
            vs_ema   = 0.0f;
            pk_i2c0_client_reset(&i2c_client);
        }

        /* ── 每拍先查 EVENT.por_detected，再读数据（DS §4.3.7 Table 31, p.33）──
         *
         * BMP388 本地 POR/软复位会把 PWR_CTRL/OSR/ODR/CONFIG 打回默认,
         * 但数据读仍然"成功"——不盯这个标志,器件就永远睡在错误配置里
         * (2026-09 审计 P1/legacy)。三轮审计把它从「每 32 拍读一次、且在
         * 数据读之后」改成**每拍、数据读之前**：旧时序下 POR 之后最多
         * 31 拍默认值读数会被当有效数据发布，甚至进 BARO_VALID 飞行记录。
         * 成本：每 100 ms 拍多一次 1 字节 I²C 读，相对同拍的 6 字节温压读
         * 可忽略。
         *
         * por_detected=1 → 本拍数据是复位后垃圾：整拍作废（valid=false、
         * 跳过发布），并把 s_ready 打掉交给上面那条 bring-up 重试路径
         * （它的第一次尝试不等待，所以仍然是"下一轮立刻重配"，只是失败之后
         * 有了退避而不是无限每拍重试），**不是**总线复位。EVENT 读失败不另
         * 生分支：总线真坏了走下面的 data read failed 主检测器。 */
        {
            uint8_t ev = 0;
            if (reg_read(BMP388_REG_EVENT, &ev, 1) == ESP_OK &&
                (ev & BMP388_EVENT_POR)) {
                ESP_LOGW(TAG, "EVENT.por_detected=1 — BMP388 本地复位,重写配置并重读标定");
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_state.valid = false;
                xSemaphoreGive(s_mutex);
                /* 交回上面那条统一的 bring-up 路径:下一轮立刻重试一次(重试器
                 * 第一次尝试不等待),失败才进退避。这里不另写一份重配代码,
                 * 也就不会出现"POR 这条路径的重试没有上限"这种同类缺陷。 */
                s_ready = false;
                continue;
            }
        }

        uint8_t d[6];
        if (reg_read(BMP388_REG_DATA, d, 6) != ESP_OK) {
            ESP_LOGW(TAG, "BMP388 data read failed");
            has_prev = false;   /* 读失败后清除前次状态,防止恢复后 VS 出现尖峰 */
            vs_ema = 0.0f;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state.valid = false;
            xSemaphoreGive(s_mutex);
            /* 这里是主检测器。以前这条路径除了刷日志什么都不做,总线一塌
             * 就永远刷下去——那正是要修的病。 */
            (void)pk_i2c0_client_report(&i2c_client, false);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        /* 读到了 = 总线活着,失败串清零。 */
        (void)pk_i2c0_client_report(&i2c_client, true);

        uint32_t raw_press = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16);
        uint32_t raw_temp  = (uint32_t)d[3] | ((uint32_t)d[4] << 8) | ((uint32_t)d[5] << 16);

        /* 顺序重要:先温度(更新 t_lin),再气压(依赖 t_lin) */
        float temp_c   = baro_compensate_temperature(&s_cal, raw_temp);
        float press_pa = baro_compensate_pressure(&s_cal, raw_press);

        /* 守卫:press_pa <= 0 会使 powf 底数为负,产生 NaN → (int)NaN UB */
        if (!(press_pa > 0.0f)) {
            has_prev = false;
            vs_ema = 0.0f;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state.valid = false;
            xSemaphoreGive(s_mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ── auto-QNH:用「本站气压 + GPS 正高」反解当地 QNH ──
         * 只在 AUTO 模式且 GPS 有新鲜正高时更新。慢速一阶滤波(GPS 高度
         * 有米级噪声,不该直接抖到 QNH 上):0.02/拍 @10Hz ≈ 5s 时间常数。
         * 不写 NVS(见 pk_qnh_set_auto);GPS 无解时保持上一次的值。 */
        if (pk_qnh_mode_get() == PK_QNH_MODE_AUTO) {
            pk_gps_state_t g;
            if (pk_gps_get(&g) && g.have_fix && g.have_altitude) {
                float target = pk_qnh_from_pressure_alt(
                    press_pa, (float)g.altitude_ft / 3.28084f);
                float cur = pk_qnh_get();
                pk_qnh_set_auto(cur + 0.02f * (target - cur));
            }
        }

        /* 气压高度(国际民航标准大气公式);QNH 每轮读取,支持运行时调整 */
        float qnh_pa   = pk_qnh_get() * 100.0f;   /* hPa → Pa */
        float alt_m    = 44330.0f * (1.0f - powf(press_pa / qnh_pa, 0.190295f));
        float alt_ft_f = alt_m * 3.28084f;

        /* 软件高度低通(补充 BMP388 硬件 IIR) */
        if (!has_prev) alt_filt = alt_ft_f;
        else           alt_filt = ALT_ALPHA * alt_ft_f + (1.0f - ALT_ALPHA) * alt_filt;
        /* 显示滞回:偏离当前显示值 ≥1ft 才跳档,消除整数量化边界(如 363.5)来回抖 */
        if (!has_prev || fabsf(alt_filt - (float)disp_alt_ft) >= 1.0f) {
            disp_alt_ft = (int)roundf(alt_filt);
        }
        int alt_ft = disp_alt_ft;

        /* VS: 平滑高度微分(无量化/测量噪声) + EMA + deadband + 步进 */
        int64_t now = esp_timer_get_time();
        int vs_fpm = 0;
        if (has_prev && (now - prev_time_us) > 0) {
            float dt_min  = (float)(now - prev_time_us) / 60000000.0f;    /* µs → min */
            float vs_inst = (alt_filt - prev_alt_filt) / dt_min;         /* ft/min   */
            vs_ema        = VS_ALPHA * vs_inst + (1.0f - VS_ALPHA) * vs_ema;
            int v = (int)vs_ema;
            if (v > -VS_DEADBAND && v < VS_DEADBAND) v = 0;              /* 平飞 deadband */
            else v = (v / VS_STEP) * VS_STEP;                           /* 50fpm 步进 */
            vs_fpm = v;
        }
        prev_alt_filt = alt_filt;
        prev_time_us  = now;
        has_prev      = true;

        /* 诊断 log:降频到每秒约一次(100ms * 10 = 1s) */
        log_tick++;
        if (log_tick >= 10) {
            ESP_LOGI(TAG, "P=%.0fPa T=%.1fC alt=%dft vs=%d",
                     press_pa, temp_c, alt_ft, vs_fpm);
            log_tick = 0;
        }

        /* 加锁更新 s_state */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.valid       = true;
        s_state.pressure_pa = press_pa;
        s_state.temp_c      = temp_c;
        s_state.alt_ft      = alt_ft;
        s_state.vs_fpm      = vs_fpm;
        s_state.updated_us  = now;
        xSemaphoreGive(s_mutex);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  公共 API                                                                */
/* ─────────────────────────────────────────────────────────────────────── */

void pk_baro_start(void)
{
    i2c_master_bus_handle_t bus = pk_i2c0_bus_get();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready (call after pk_i2c0_bus_init)");
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BMP388_ADDR,
        .scl_speed_hz    = BARO_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_mutex); s_mutex = NULL;
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(baro_task, "baro", 4096, NULL, 4, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "baro task create failed");
        i2c_master_bus_rm_device(s_dev);
        vSemaphoreDelete(s_mutex); s_mutex = NULL;
        return;
    }
    ESP_LOGI(TAG, "BMP388 polling task started (optional INT GPIO%d)",
             BARO_INT_PIN);
}

bool pk_baro_get(pk_baro_state_t *out)
{
    if (out == NULL) return false;
    /* 演示模式接管点，理由同 pk_imu_sample_get()。放在 s_mutex 判空之前：
     * BMP388 没焊时 baro_task 根本没起来，s_mutex 恒为 NULL。 */
    if (pk_demo_enabled()) return pk_demo_baro(esp_timer_get_time(), out);
    if (s_mutex == NULL) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mutex);
    return out->valid;
}
