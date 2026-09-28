/* Real NVS persistence with an in-memory backend:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_config_airband \
 *      firmware/test/test_config_airband.c firmware/main/airband_frequency_input.c \
 *      && /tmp/test_config_airband
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "config_airband.h"
#define PK_AIRBAND_CONFIG_HOST_TEST
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
static void log_noop(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGW log_noop
#define ESP_LOGI log_noop
static const char *esp_err_to_name(int e) { (void)e; return "fake"; }
static uint32_t test_now;
static int64_t esp_timer_get_time(void) { return (int64_t)test_now * 1000; }
static pk_airband_config_t stored;
static size_t stored_size;
static int commits, writes, write_error, inject_change;
static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ assert(strcmp(ns, "pk_airband") == 0); (void)mode; *h=1; return ESP_OK; }
static esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{ (void)h; assert(strcmp(key,"settings") == 0); if (!stored_size) return ESP_ERR_NVS_NOT_FOUND;
  if (stored_size <= *len) memcpy(out,&stored,stored_size); *len=stored_size; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *p, size_t len)
{ (void)h; (void)key; writes++; if (write_error) return write_error;
  assert(len == sizeof(stored)); memcpy(&stored,p,len); stored_size=len;
  if (inject_change) { inject_change=0; pk_airband_config_volume(75); }
  return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; commits++; return ESP_OK; }
static void nvs_close(nvs_handle_t h) { (void)h; }
#include "../main/config_airband.c"
int main(void)
{
    pk_airband_config_load();
    pk_airband_config_t c = pk_airband_config_get();
    assert(c.channel_khz == 121500 && c.carrier_hz == 121500000 && c.volume == 35);
    pk_airband_config_volume(80);
    pk_airband_config_poll(1999);
    assert(commits == 0);
    test_now = 1999;
    pk_airband_config_volume(85);
    pk_airband_config_poll(2000);
    assert(commits == 0);
    pk_airband_config_poll(3999);
    assert(commits == 1 && stored.volume == 85);
    pk_airband_config_tuned(118010,118008330);
    pk_airband_config_muted(true);
    pk_airband_config_squelch(false,-200,20);
    pk_airband_config_poll(4000);
    pk_airband_config_load();
    c=pk_airband_config_get();
    assert(c.channel_khz == 118010 && c.carrier_hz == 118008330);
    assert(c.volume == 85 && c.muted && !c.squelch_enabled && c.squelch_dbm_x2 == -200);
    stored.channel_khz=118896; stored.volume=255; stored.muted=99;
    stored.hysteresis_db_x2=65535; stored.squelch_dbm_x2=-32768;
    pk_airband_config_load(); c=pk_airband_config_get();
    assert(c.channel_khz == 121500 && c.carrier_hz == 121500000 && c.volume == 35);
    assert(!c.muted && c.squelch_dbm_x2 == -180 && c.hysteresis_db_x2 == 12);
    stored.channel_khz=118010; stored.carrier_hz=118010000;
    pk_airband_config_load(); assert(pk_airband_config_get().channel_khz == 121500);
    stored.version=999;
    pk_airband_config_load(); assert(pk_airband_config_get().volume == 35);
    stored.version=1; stored_size=4;
    pk_airband_config_load(); assert(pk_airband_config_get().channel_khz == 121500);
    stored_size=sizeof(stored); stored.version=1; stored.channel_khz=121500;
    stored.carrier_hz=121500000; pk_airband_config_load();
    test_now=5000; pk_airband_config_tuned(118896,118896000);
    assert(pk_airband_config_get().channel_khz == 121500);
    pk_airband_config_volume(60); write_error=5; pk_airband_config_poll(7000);
    int old_writes=writes; write_error=0; pk_airband_config_poll(8000);
    assert(writes == old_writes); pk_airband_config_poll(9000);
    assert(writes == old_writes+1 && stored.volume == 60);
    test_now=10000; pk_airband_config_volume(70); inject_change=1;
    pk_airband_config_poll(12000);
    assert(stored.volume == 70 && pk_airband_config_get().volume == 75);
    pk_airband_config_poll(12001);
    assert(stored.volume == 75);
    puts("config_airband: all OK");
}
