#include "gps.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gps";

#define GPS_UART    UART_NUM_1
#define GPS_TX_PIN  32   /* P4 TX → GPS RXD */
#define GPS_RX_PIN  33   /* GPS TXD → P4 RX */
#define GPS_BAUD    9600
#define GPS_BUF_SZ  512

static pk_gps_state_t    s_gps;
static SemaphoreHandle_t s_lock;
static void take(void){ xSemaphoreTake(s_lock, portMAX_DELAY); }
static void give(void){ xSemaphoreGive(s_lock); }

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

static void parse_rmc(char *f[], int n){
    if(n < 9) return;
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
}

static void parse_gga(char *f[], int n){
    if(n < 10) return;
    int    q     = atoi(f[6]);          /* fix quality, 0 = no fix */
    int    sats  = atoi(f[7]);
    double alt_m = atof(f[9]);
    take();
    s_gps.sats = sats;
    if(q > 0){
        s_gps.altitude_ft  = (int)(alt_m * 3.28084 + 0.5);
        s_gps.have_altitude = true;
    } else {
        s_gps.have_altitude = false;
    }
    give();
}

static void handle_line(char *line){
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
}

static void gps_task(void *arg){
    (void)arg;
    uint8_t buf[GPS_BUF_SZ];
    static char line[128]; int li = 0;
    ESP_LOGI(TAG, "gps task running — UART%d TX=GPIO%d RX=GPIO%d @%d",
             GPS_UART, GPS_TX_PIN, GPS_RX_PIN, GPS_BAUD);
    for(;;){
        int len = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(200));
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
            pk_gps_state_t g; pk_gps_get(&g);
            /* TODO: 硬件验证后降级为 ESP_LOGD 或删除 */
            ESP_LOGI(TAG, "fix=%d sats=%d lat=%.6f lon=%.6f alt=%dft gs=%dkt trk=%d",
                     g.have_fix, g.sats, g.lat, g.lon, g.altitude_ft,
                     g.ground_speed_kt, g.track_deg);
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
