#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    have_fix;          /* RMC status == 'A' */
    double  lat, lon;          /* decimal degrees, +N/+E */
    bool    have_altitude;
    int     altitude_ft;       /* MSL, from GGA */
    int     ground_speed_kt;
    int     track_deg;         /* 0..359 true */
    int     sats;
    int64_t updated_us;        /* esp_timer_get_time() of last valid fix */
} pk_gps_state_t;

/* Start UART1 + parser task. Call once at boot, after aircraft_state_init(). */
void pk_gps_start(void);

/* Snapshot current GPS state into *out. Returns out->have_fix. */
bool pk_gps_get(pk_gps_state_t *out);
