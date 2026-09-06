#include "gps.h"
#include "gps_nmea.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "pk_clock.h"
#include "config_demo.h"
#include "demo_data.h"

static const char *TAG = "gps";

/* fix 后只在 GPS 与系统时钟偏差超过此值时才精校 —— 系统钟晶振级漂移很慢
 * (~1-2ms/min)，按偏差触发可避免每秒 settimeofday 的无谓跳变。 */
#define GPS_CLOCK_SKEW_MS  500

#define GPS_UART    UART_NUM_1
#define GPS_TX_PIN  49   /* P4 TX → GPS RXD; moved to J3 Pin 32 (upper) */
#define GPS_RX_PIN  51   /* GPS TXD → P4 RX; J3 Pin 36 = GPIO51 (J3 Pin 34 / GPIO50 is GPS_PPS) */
#define GPS_BAUD    9600
#define GPS_BUF_SZ  512

#define GPS_PPS_PIN  50          /* GNSS 1PPS → P4，J3 Pin 34（board_pinout.md §10 GPS 表） */
#define GPS_PPS_LOCK_US 2000000LL /* 时间锁定窗口：PPS 距今 <2 s 视为在锁 */
#define GPS_NMEA_LOCK_US 5000000LL /* 时间锁定还要求 NMEA 距今 <5 s（UART 数据面活着）*/

static pk_gps_state_t    s_gps;
static SemaphoreHandle_t s_lock;
static void take(void){ xSemaphoreTake(s_lock, portMAX_DELAY); }
static void give(void){ xSemaphoreGive(s_lock); }

/* --- 临时诊断计数器（排查 GPS no-fix；定位到根因后删除） --- */
static volatile uint32_t s_rx_bytes;    /* 累计从 UART RX 收到的原始字节 */
static volatile uint32_t s_nmea_lines;  /* 累计拼成的完整 NMEA 行 */

/* --- PPS（GPIO50 上升沿） -------------------------------------------------
 * ISR 只做两件事：计数 + 打时间戳（esp_timer_get_time() 纯读硬件计时器，
 * IRAM 内、ISR 安全）。fix 与否、2 s 窗口判定全部放 gps_task 的 1 Hz 快照
 * 路径——见 gps.h 里 time_locked 的语义注释。 */
static volatile uint32_t s_pps_count;    /* PPS 上升沿累计 */
static volatile int64_t  s_last_pps_us;  /* 最近上升沿时间戳；0 = 还没见过沿 */

static void IRAM_ATTR pps_isr(void *arg){
    (void)arg;
    /* 先写时间戳再自增 count：count 是快照侧的 release 标记——读到 count
     * 变化后，对应时间戳必然已写好，消除"先计数后写戳"留下的撕裂窗口。 */
    s_last_pps_us = esp_timer_get_time();
    s_pps_count++;
}

bool pk_gps_get(pk_gps_state_t *out){
    if(!out) return false;
    /* 演示模式接管点，理由同 pk_imu_sample_get()。
     *
     * 接管 GPS 一并把**本机**解算也接管了：pk_own_ship_resolve() 在没有绑定
     * ADS-B 本机时就是退到 GPS 的，于是高度带、速度带、交通页的距离/方位全部
     * 跟着有了数据，不必再单独去桩 own_ship。 */
    if(pk_demo_enabled()) return pk_demo_gps(esp_timer_get_time(), out);
    if(!s_lock) return false;
    take(); *out = s_gps; give();
    return out->have_fix;
}

/* NMEA ddmm.mmmm + hemisphere → decimal degrees (+N/+E) */
static double nmea_to_deg(const char *val, const char *hemi){
    if(!val || !*val) return 0.0;
    double raw = atof(val);
    int    deg = (int)(raw / 100.0);
    double min = raw - deg * 100.0;
    double d   = deg + min / 60.0;
    if(hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}

/* RMC 的 hhmmss(.sss) 与 ddmmyy 都是定长数字串 → epoch 毫秒。
 * 任一字段缺失/非法返回 false（不校时）。 */
static bool rmc_epoch_ms(const char *tod, const char *date, int64_t *out){
    if(strlen(tod) < 6 || strlen(date) < 6) return false;
    for(int i = 0; i < 6; i++){
        if(tod[i]  < '0' || tod[i]  > '9') return false;
        if(date[i] < '0' || date[i] > '9') return false;
    }
    int hh = (tod[0]-'0')*10 + (tod[1]-'0');
    int mi = (tod[2]-'0')*10 + (tod[3]-'0');
    int ss = (tod[4]-'0')*10 + (tod[5]-'0');
    int dd = (date[0]-'0')*10 + (date[1]-'0');
    int mo = (date[2]-'0')*10 + (date[3]-'0');
    int yy = (date[4]-'0')*10 + (date[5]-'0');
    if(mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 60)
        return false;
    *out = pk_clock_civil_utc_to_epoch_ms(2000 + yy, mo, dd, hh, mi, ss, 0);
    return true;
}

/* 以下 parse_* 的字段切分/checksum 门已迁入 gps_nmea.c（WP-B Task 1）；
 * 这里只做「字段 → gps 状态」的落地，函数体保持原样，仅字段来源从老的
 * fields[]/n 换成 msg->f/msg->n。 */

static void parse_rmc(const gps_nmea_msg_t *msg){
    if(msg->n < 10) return;                 /* 需含日期字段 f[9] */
    bool valid = (msg->f[2][0] == 'A');
    take();
    s_gps.have_fix = valid;
    if(valid){
        s_gps.lat = nmea_to_deg(msg->f[3], msg->f[4]);
        s_gps.lon = nmea_to_deg(msg->f[5], msg->f[6]);
        s_gps.ground_speed_kt = (int)(atof(msg->f[7]) + 0.5);
        s_gps.track_deg       = (int)(atof(msg->f[8]) + 0.5);
        s_gps.updated_us = esp_timer_get_time();
    }
    give();

    /* 两段式校时（锁外做：settimeofday 不碰 s_gps）。 */
    int64_t gps_ms;
    if(!rmc_epoch_ms(msg->f[1], msg->f[9], &gps_ms)) return;
    if(valid){
        /* 精校：fix 有效 = 卫星授时；仅当偏差 > 阈值才写，避免无谓跳变。 */
        struct timeval tv; gettimeofday(&tv, NULL);
        int64_t sys_ms = (int64_t)tv.tv_sec*1000LL + tv.tv_usec/1000LL;
        int64_t d = gps_ms - sys_ms; if(d < 0) d = -d;
        if(d > GPS_CLOCK_SKEW_MS) pk_clock_apply_epoch_ms(gps_ms, "gps");
    } else if(!pk_clock_is_synced()){
        /* 粗校兜底：未 fix 但有合法时间、且还没人校过钟 → 先粗校一次。 */
        pk_clock_apply_epoch_ms(gps_ms, "gps-coarse");
    }
}

static void parse_gga(const gps_nmea_msg_t *msg){
    if(msg->n < 10) return;
    int    q     = atoi(msg->f[6]);          /* fix quality, 0 = no fix */
    int    sats  = atoi(msg->f[7]);
    double alt_m = atof(msg->f[9]);
    take();
    s_gps.sats = sats;
    s_gps.hdop = (float)atof(msg->f[8]);     /* GGA field 8 = HDOP */
    if(q > 0){
        s_gps.altitude_ft  = (int)(alt_m * 3.28084 + 0.5);
        s_gps.have_altitude = true;
    } else {
        s_gps.have_altitude = false;
    }
    give();
}

/* --- GSV 累积器：gps_task 单线程访问(无锁)，主循环 1 Hz 提交到 s_gps。 --- */
static int     s_acc_view, s_acc_view_gps, s_acc_view_bds;
static uint8_t s_acc_snr[PK_GPS_SNR_MAX];
static uint8_t s_acc_con[PK_GPS_SNR_MAX];   /* 与 s_acc_snr 平行：0=GPS 1=北斗 2=其它 */
static int     s_acc_snr_n;

/* NMEA talker 前缀 → 星座 ID（pk_gnss_t）。 */
static uint8_t gsv_constellation(const char *t){
    char a = t[0], b = t[1];
    if(a == 'G' && b == 'P') return PK_GNSS_GPS;
    if((a == 'B' && b == 'D') || (a == 'G' && b == 'B')) return PK_GNSS_BDS;
    if(a == 'G' && b == 'L') return PK_GNSS_GLO;
    if(a == 'G' && b == 'A') return PK_GNSS_GAL;
    if(a == 'G' && b == 'Q') return PK_GNSS_QZSS;
    return PK_GNSS_OTHER;
}

/* GSV: $xxGSV,numMsg,msgNum,totalInView, {prn,elev,az,snr}×N [,signalID] */
static void parse_gsv(const gps_nmea_msg_t *msg){
    if(msg->n < 4) return;
    uint8_t con = gsv_constellation(msg->f[0]);
    int msgNum = atoi(msg->f[2]);
    int total  = atoi(msg->f[3]);
    if(msgNum == 1){                       /* 同星座多句 total 相同，只首句计入 */
        s_acc_view += total;
        if(con == PK_GNSS_GPS)      s_acc_view_gps += total;
        else if(con == PK_GNSS_BDS) s_acc_view_bds += total;
    }
    for(int i = 4; i + 3 < msg->n; i += 4){ /* 每颗星 4 字段；尾随 signalID 自然落空 */
        const char *snr = msg->f[i + 3];
        if(snr && *snr && s_acc_snr_n < PK_GPS_SNR_MAX){
            s_acc_snr[s_acc_snr_n] = (uint8_t)atoi(snr);
            s_acc_con[s_acc_snr_n] = con;
            s_acc_snr_n++;
        }
    }
}

/* TXT: $GPTXT,01,01,01,ANTENNA OK|OPEN|SHORT */
static void parse_txt(const gps_nmea_msg_t *msg){
    if(msg->n < 5) return;
    const char *m = msg->f[4];
    pk_gps_ant_t a;
    if      (strstr(m, "ANTENNA OPEN"))  a = PK_GPS_ANT_OPEN;
    else if (strstr(m, "ANTENNA SHORT")) a = PK_GPS_ANT_SHORT;
    else if (strstr(m, "ANTENNA OK"))    a = PK_GPS_ANT_OK;
    else return;                           /* 其它 TXT 不关心 */
    take(); s_gps.ant_status = a; give();
}

static void handle_line(char *line){
    s_nmea_lines++;
    /* 收到任何一行就更新——诊断页据此区分「模块没插」与「模块在讲话但没星」。
     * checksum 不过的行同样算「在讲话」，所以计数/时间戳必须在解析之前。 */
    take(); s_gps.last_nmea_us = esp_timer_get_time(); give();
    /* 原始 NMEA 行：默认不刷屏，需要时把 gps TAG 调到 DEBUG 即可调出。
     * 解析器在自家内部缓冲里切分，line 本身不再被改写。 */
    ESP_LOGD(TAG, "NMEA: %s", line);
    /* 切分/checksum 门在 gps_nmea 里（WP-B Task 1）：坏 checksum、非 $ 句
     * 直接整行丢弃——老代码不验 *hh，损坏句子的字段会一路进 fix/校时。 */
    gps_nmea_msg_t msg;
    if(!gps_nmea_feed_line(&msg, line)) return;
    if(strncmp(msg.type, "RMC", 3) == 0)      parse_rmc(&msg);
    else if(strncmp(msg.type, "GGA", 3) == 0) parse_gga(&msg);
    else if(strncmp(msg.type, "GSV", 3) == 0) parse_gsv(&msg);
    else if(strncmp(msg.type, "TXT", 3) == 0) parse_txt(&msg);
    /* 其余类型（GSA/VTG/GLL…）解析照返回 1，这里不接就是忽略——与老行为一致。 */
}

static void gps_task(void *arg){
    (void)arg;
    uint8_t buf[GPS_BUF_SZ];
    static char line[128]; int li = 0;
    ESP_LOGI(TAG, "gps task running — UART%d TX=GPIO%d RX=GPIO%d @%d",
             GPS_UART, GPS_TX_PIN, GPS_RX_PIN, GPS_BAUD);
    for(;;){
        int len = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if(len > 0) s_rx_bytes += (uint32_t)len;   /* 诊断：原始字节计数 */
        for(int i = 0; i < len; i++){
            char c = (char)buf[i];
            if(c == '\n' || c == '\r'){
                if(li > 0){ line[li] = '\0'; handle_line(line); li = 0; }
            } else if(li < (int)sizeof(line) - 1){
                line[li++] = c;
            } else { li = 0; }   /* overflow → drop line */
        }
        static int64_t last_log = 0;
        int64_t now = esp_timer_get_time();
        if(now - last_log > 1000000){   /* 1 Hz */
            last_log = now;
            /* 提交本周期 GSV 累积 → 快照后清零（累积器同线程，无锁）。 */
            take();
            s_gps.sats_in_view     = s_acc_view;
            s_gps.sats_in_view_gps = s_acc_view_gps;
            s_gps.sats_in_view_bds = s_acc_view_bds;
            s_gps.snr_count        = s_acc_snr_n;
            int mx = 0;
            for(int i = 0; i < s_acc_snr_n; i++){
                s_gps.snr[i]     = s_acc_snr[i];
                s_gps.snr_con[i] = s_acc_con[i];
                if(s_acc_snr[i] > mx) mx = s_acc_snr[i];
            }
            s_gps.snr_max = mx;
            give();
            s_acc_view = 0; s_acc_view_gps = 0; s_acc_view_bds = 0; s_acc_snr_n = 0;

            /* PPS/NMEA 快照（seqlock-lite）：ISR 先写时间戳后自增计数，
             * 计数即序标——读计数 → 读时间戳 → 复读计数；计数变了重试一次，
             * 还在变就放弃本拍（last_pps_us=0，视为本拍无新鲜 PPS，
             * time_locked 下个 1 Hz 拍自愈）。u64 时间戳在 32 位核上非
             * 原子，双读计数把撕裂读挡在重试/放弃里。 */
            uint32_t pps_n  = s_pps_count;
            int64_t  pps_us = s_last_pps_us;
            if (pps_n != s_pps_count) { pps_us = s_last_pps_us; pps_n = s_pps_count; }
            if (pps_n != s_pps_count) pps_us = 0;   /* 重试仍撞上写入 → 本拍作废 */

            take();
            s_gps.pps_count   = pps_n;
            s_gps.last_pps_us = pps_us;
            /* 时间锁定 = fix 有效 + PPS <2 s + NMEA <5 s（gps.h 语义注释）。
             * NMEA 新鲜度挡住「UART 已死、PPS 还在跳」的假锁定（2026-09
             * 审计 P2）。 */
            s_gps.time_locked = s_gps.have_fix && pps_us != 0 &&
                                (now - pps_us) < GPS_PPS_LOCK_US &&
                                s_gps.last_nmea_us != 0 &&
                                (now - s_gps.last_nmea_us) < GPS_NMEA_LOCK_US;
            give();
            /* 注意：生产链路的时间可信度（pk_clock_is_synced()，消费方
             * pk_own_sampler/pk_rec_ingest）仍是「校过一次就永久 latched」，
             * 与这里的 time_locked 刻意不同——把 PPS/NMEA 新鲜度接进
             * pk_clock 是后续 time-service 任务（2026-09 审计记录在案）。 */

            /* 直接读快照而不是走 pk_gps_get()：那个入口在演示模式下会返回合成
             * 数据，于是没插 GPS 板卡时串口上照样印着 "fix=1 sats=11"——这条
             * 心跳存在的唯一目的就是排查真实模块，绝不能被演示数据污染。 */
            pk_gps_state_t g; take(); g = s_gps; give();
            /* 1 Hz GPS 运行心跳：fix/可见星(G/B)/SNR/天线/HDOP 一目了然。
             * 原始 NMEA 已降 DEBUG;这条保留为常驻状态行(rx/lines 仍便于看 UART 活性)。 */
            ESP_LOGI(TAG, "fix=%d sats=%d view=%d(G%dB%d) snr=%d ant=%d lat=%.6f lon=%.6f "
                          "alt=%dft gs=%dkt trk=%d hdop=%.1f pps=%u tl=%d rx=%u lines=%u",
                     g.have_fix, g.sats, g.sats_in_view, g.sats_in_view_gps,
                     g.sats_in_view_bds, g.snr_max, (int)g.ant_status,
                     g.lat, g.lon, g.altitude_ft, g.ground_speed_kt, g.track_deg,
                     (double)g.hdop, (unsigned)g.pps_count, (int)g.time_locked,
                     (unsigned)s_rx_bytes, (unsigned)s_nmea_lines);
        }
    }
}

void pk_gps_start(void){
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    memset(&s_gps, 0, sizeof(s_gps));
    uart_config_t cfg = {
        .baud_rate  = GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART, GPS_BUF_SZ * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART, GPS_TX_PIN, GPS_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* PPS 输入 GPIO50：board_pinout.md §10 GPS 表只规定 PPS→GPIO50（J3-34）
     * 走线，未规定上/下拉——故配浮空输入：1PPS 由 GNSS 模块推挽驱动（1PPS
     * 的常规输出形态），无需内部上下拉。待台架实测确认：若发现无沿/误沿，
     * 再评估内部下拉。 */
    const gpio_config_t pps = {
        .pin_bit_mask = 1ULL << GPS_PPS_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&pps));
    /* firmware/main 目前无其它 GPIO ISR 用户（IMU/BARO INT 都是轮询），
     * 服务通常由这里首次安装；INVALID_STATE = 已被装好，同样放行。
     * 引脚级 gpio_isr_handler_add 挂在共享默认服务上，与日后其它中断用户
     * 互不干扰。 */
    esp_err_t isr_err = gpio_install_isr_service(0);
    configASSERT(isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPS_PPS_PIN, pps_isr, NULL));

    BaseType_t ok = xTaskCreatePinnedToCore(gps_task, "gps", 4096, NULL, 4, NULL, 0);
    configASSERT(ok == pdTRUE);
}
