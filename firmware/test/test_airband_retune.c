/* Run the production capture loop against a serial BK register model and
 * injected I2S success->retune->timeout, then inspect its public snapshot.
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_airband_retune firmware/test/test_airband_retune.c \
 * firmware/main/airband_bk4819.c firmware/main/airband_audio.c \
 * firmware/main/airband_pcm16.c firmware/main/airband_am_gain.c \
 * firmware/main/airband_frequency_input.c firmware/main/airband_speaker_policy.c \
 * -lm && /tmp/test_airband_retune
 */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "driver/gpio.h"
#include "airband.h"
static int model_set_level(int pin,int level);
static int model_get_level(int pin);
static int model_set_direction(int pin,int mode);
static int model_set_pull_mode(int pin,int mode) { (void)pin;(void)mode;return ESP_OK; }
#define gpio_set_level model_set_level
#define gpio_get_level model_get_level
#define gpio_set_direction model_set_direction
#define gpio_set_pull_mode model_set_pull_mode
#define GPIO_PULLUP_ONLY 1
#define GPIO_NUM_29 29
#define GPIO_NUM_30 30
#define GPIO_NUM_47 47
#define GPIO_NUM_5 5
#define GPIO_NUM_22 22
#define GPIO_NUM_52 52
#define GPIO_NUM_48 48
#include "../main/airband_task.c"
static uint16_t regs[128];
static uint32_t command;
static unsigned bits,read_bit;
static int clk,data,direction;
static bool selected;
static int model_set_level(int pin,int level)
{
    if(pin==29) {
        if(!level) { selected=true;command=0;bits=0;read_bit=0; }
        else { if(selected && bits==24 && !(command&0x800000))regs[(command>>16)&127]=(uint16_t)command;selected=false; }
    } else if(pin==47) data=level;
    else if(pin==30) {
        if(selected && !clk && level && direction==GPIO_MODE_OUTPUT) { command=(command<<1)|(unsigned)data;bits++; }
        clk=level;
    }
    return ESP_OK;
}
static int model_get_level(int pin)
{ (void)pin;return (regs[command&127]>>(15-read_bit++))&1; }
static int model_set_direction(int pin,int mode) { (void)pin;direction=mode;return ESP_OK; }
static int64_t now_us;
static unsigned reads,disabled;
static jmp_buf done;
int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t i2s_channel_disable(i2s_chan_handle_t rx) { (void)rx;disabled++;return ESP_OK; }
esp_err_t i2s_channel_read(i2s_chan_handle_t rx,void *dst,size_t capacity,size_t *got,unsigned wait_ms)
{
    (void)rx;(void)wait_ms;
    if(++reads==3)longjmp(done,1);
    if(reads==1) {
        unsigned char *p=dst;
        for(size_t i=0;i<capacity;i+=6) { p[i]=0;p[i+1]=0x10;p[i+2]=0;p[i+3]=0;p[i+4]=0x20;p[i+5]=0; }
        *got=capacity;now_us=100000;
        (void)pk_airband_set_frequency(122000000u);
        return ESP_OK;
    }
    *got=0;now_us=1100000;return ESP_ERR_TIMEOUT;
}
esp_err_t pk_airband_speaker_start(void) { return ESP_OK; }
/* display.c 不在本测试的链接集里；underrun 观测计数在 host 上恒 0。 */
uint32_t pk_display_underrun_count(void) { return 0; }
bool pk_airband_speaker_submit(const int16_t *p,size_t n,bool play) { (void)p;(void)n;(void)play;return true; }
void pk_airband_speaker_set_program_playable(bool play) { (void)play; }
bool pk_airband_speaker_get(pk_airband_speaker_status_t *s) { memset(s,0,sizeof(*s));return true; }
void pk_airband_speaker_set_muted(bool muted) { (void)muted; }
bool pk_airband_speaker_is_muted(void) { return false; }
void pk_airband_speaker_set_volume(uint8_t volume) { (void)volume; }
pk_airband_speaker_test_click_t pk_airband_speaker_test_tone_action(void) { return 0; }
bool pk_airband_speaker_test_tone_active(void) { return false; }
bool pk_airband_speaker_test_tone(void) { return false; }
bool pk_airband_record_init(void) { return true; }
bool pk_airband_recording_store_start(void) { return true; }
bool pk_airband_playback_start(void) { return true; }
bool pk_airband_record_submit(const int16_t *p,size_t n,bool rf,bool sq,uint32_t channel,uint32_t carrier)
{ (void)p;(void)n;(void)rf;(void)sq;(void)channel;(void)carrier;return true; }
void pk_airband_record_get(pk_airband_record_status_t *out)
{ if(out) *out=(pk_airband_record_status_t){0}; }
bool ble_gatt_airband_submit(const int16_t *p,size_t n,bool valid)
{ (void)p;(void)n;(void)valid;return true; }
void ble_gatt_airband_stream_break(void) { }
void pk_airband_config_load(void) { }
pk_airband_config_t pk_airband_config_get(void) { return (pk_airband_config_t){0}; }
void pk_airband_config_tuned(uint32_t channel,uint32_t carrier) { (void)channel;(void)carrier; }
void pk_airband_config_volume(uint8_t v) { (void)v; }
void pk_airband_config_muted(bool muted) { (void)muted; }
void pk_airband_config_squelch(bool e,int16_t t,uint16_t h) { (void)e;(void)t;(void)h; }
bool pk_airband_config_start(void) { return true; }
static int failed;
#define CHECK(c,m) do { if(!(c)){puts("FAIL: " m);failed++;} }while(0)
int main(void)
{
    regs[0x67]=100;
    if(setjmp(done)==0)task(NULL);
    pk_airband_state_t snap;
    CHECK(pk_airband_get(&snap),"timeout after retune produced no snapshot");
    CHECK(disabled==2 && snap.online && snap.frequency_hz==122000000u,"fixture did not perform a real successful retune");
    CHECK(snap.audio_frames==0 && snap.audio_nonzero==0 && snap.audio_right_nonzero==0,"new station retained old raw frames");
    CHECK(snap.audio_rms==0 && snap.audio_right_rms==0,"new station retained old raw RMS");
    CHECK(snap.audio_min==0 && snap.audio_max==0 && snap.audio_right_min==0 && snap.audio_right_max==0,"new station retained old extrema");
    CHECK(snap.health!=PK_AIRBAND_HEALTH_READY,"new station with only a timeout reported READY");
    printf("%s: capture retune snapshot frames=%u RMS=%u health=%d\n",failed?"FAILED":"OK",snap.audio_frames,snap.audio_rms,snap.health);
    return failed!=0;
}
