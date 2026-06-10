#include "gps.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "pk_clock.h"

static const char *TAG = "gps";

/* fix 后只在 GPS 与系统时钟偏差超过此值时才精校 —— 系统钟晶振级漂移很慢
 * (~1-2ms/min)，按偏差触发可避免每秒 settimeofday 的无谓跳变。 */
#define GPS_CLOCK_SKEW_MS  500

#define GPS_UART    UART_NUM_1
#define GPS_TX_PIN  32   /* P4 TX → GPS RXD */
#define GPS_RX_PIN  33   /* GPS TXD → P4 RX */
#define GPS_BAUD    9600
#define GPS_BUF_SZ  512

static pk_gps_state_t    s_gps;
static SemaphoreHandle_t s_lock;
static void take(void){ xSemaphoreTake(s_lock, portMAX_DELAY); }
static void give(void){ xSemaphoreGive(s_lock); }

/* --- 临时诊断计数器（排查 GPS no-fix；定位到根因后删除） --- */
static volatile uint32_t s_rx_bytes;    /* 累计从 UART RX 收到的原始字节 */
static volatile uint32_t s_nmea_lines;  /* 累计拼成的完整 NMEA 行 */

bool pk_gps_get(pk_gps_state_t *out){
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

/* split in-place by ',' → fields[]; returns field count */
static int split_csv(char *s, char *fields[], int maxf){
    int n = 0; char *p = s;
    fields[n++] = p;
    while(*p && n < maxf){ if(*p == ','){ *p = '\0'; fields[n++] = p + 1; } p++; }
    return n;
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

static void parse_rmc(char *f[], int n){
    if(n < 10) return;                 /* 需含日期字段 f[9] */
    bool valid = (f[2][0] == 'A');
    take();
    s_gps.have_fix = valid;
    if(valid){
        s_gps.lat = nmea_to_deg(f[3], f[4]);
        s_gps.lon = nmea_to_deg(f[5], f[6]);
        s_gps.ground_speed_kt = (int)(atof(f[7]) + 0.5);
        s_gps.track_deg       = (int)(atof(f[8]) + 0.5);
        s_gps.updated_us = esp_timer_get_time();
    }
    give();

    /* 两段式校时（锁外做：settimeofday 不碰 s_gps）。 */
    int64_t gps_ms;
    if(!rmc_epoch_ms(f[1], f[9], &gps_ms)) return;
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

static void parse_gga(char *f[], int n){
    if(n < 10) return;
    int    q     = atoi(f[6]);          /* fix quality, 0 = no fix */
    int    sats  = atoi(f[7]);
    double alt_m = atof(f[9]);
    take();
    s_gps.sats = sats;
    s_gps.hdop = (float)atof(f[8]);     /* GGA field 8 = HDOP */
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
static void parse_gsv(char *f[], int n){
    if(n < 4) return;
    uint8_t con = gsv_constellation(f[0]);
    int msgNum = atoi(f[2]);
    int total  = atoi(f[3]);
    if(msgNum == 1){                       /* 同星座多句 total 相同，只首句计入 */
        s_acc_view += total;
        if(con == PK_GNSS_GPS)      s_acc_view_gps += total;
        else if(con == PK_GNSS_BDS) s_acc_view_bds += total;
    }
    for(int i = 4; i + 3 < n; i += 4){     /* 每颗星 4 字段；尾随 signalID 自然落空 */
        const char *snr = f[i + 3];
        if(snr && *snr && s_acc_snr_n < PK_GPS_SNR_MAX){
            s_acc_snr[s_acc_snr_n] = (uint8_t)atoi(snr);
            s_acc_con[s_acc_snr_n] = con;
            s_acc_snr_n++;
        }
    }
}

/* TXT: $GPTXT,01,01,01,ANTENNA OK|OPEN|SHORT */
static void parse_txt(char *f[], int n){
    if(n < 5) return;
    const char *m = f[4];
    pk_gps_ant_t a;
    if      (strstr(m, "ANTENNA OPEN"))  a = PK_GPS_ANT_OPEN;
    else if (strstr(m, "ANTENNA SHORT")) a = PK_GPS_ANT_SHORT;
    else if (strstr(m, "ANTENNA OK"))    a = PK_GPS_ANT_OK;
    else return;                           /* 其它 TXT 不关心 */
    take(); s_gps.ant_status = a; give();
}

static void handle_line(char *line){
    s_nmea_lines++;
    /* 原始 NMEA 行：默认不刷屏，需要时把 gps TAG 调到 DEBUG 即可调出。
     * split_csv 会就地改写，必须在解析前打印。 */
    ESP_LOGD(TAG, "NMEA: %s", line);
    char *body = (*line == '$') ? line + 1 : line;
    if(strlen(body) < 6) return;
    char *fields[24];
    int   n    = split_csv(body, fields, 24);
    const char *type = fields[0];        /* e.g. "GNRMC" (any talker GP/GN/GL) */
    size_t tl = strlen(type);
    if(tl < 3) return;
    const char *suf = type + (tl - 3);   /* match last 3 chars */
    if(strncmp(suf, "RMC", 3) == 0)      parse_rmc(fields, n);
    else if(strncmp(suf, "GGA", 3) == 0) parse_gga(fields, n);
    else if(strncmp(suf, "GSV", 3) == 0) parse_gsv(fields, n);
    else if(strncmp(suf, "TXT", 3) == 0) parse_txt(fields, n);
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

            pk_gps_state_t g = {0}; pk_gps_get(&g);
            /* 1 Hz GPS 运行心跳：fix/可见星(G/B)/SNR/天线/HDOP 一目了然。
             * 原始 NMEA 已降 DEBUG;这条保留为常驻状态行(rx/lines 仍便于看 UART 活性)。 */
            ESP_LOGI(TAG, "fix=%d sats=%d view=%d(G%dB%d) snr=%d ant=%d lat=%.6f lon=%.6f "
                          "alt=%dft gs=%dkt trk=%d hdop=%.1f rx=%u lines=%u",
                     g.have_fix, g.sats, g.sats_in_view, g.sats_in_view_gps,
                     g.sats_in_view_bds, g.snr_max, (int)g.ant_status,
                     g.lat, g.lon, g.altitude_ft, g.ground_speed_kt, g.track_deg,
                     (double)g.hdop, (unsigned)s_rx_bytes, (unsigned)s_nmea_lines);
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
    BaseType_t ok = xTaskCreatePinnedToCore(gps_task, "gps", 4096, NULL, 4, NULL, 0);
    configASSERT(ok == pdTRUE);
}
