#include "airband_record.h"
#include "airband_wav.h"
#include "pk_sdcard.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_timer.h"

typedef struct {
    uint32_t segment, session, mount, channel, carrier;
    uint16_t count;
    bool end, interrupted;
    int16_t pcm[PK_AIRBAND_RECORD_BLOCK_SAMPLES];
} record_block_t;
/* Copy-only critical section, independent of the filesystem mutex. The tail
 * remains owned until I/O finishes, so a close/overflow can mark its end even
 * when the writer has already copied that block. */
static EXT_RAM_BSS_ATTR record_block_t s_blocks[PK_AIRBAND_RECORD_QUEUE_BLOCKS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_io;
static uint32_t s_head, s_tail, s_segment, s_next_segment, s_session;
static uint32_t s_channel, s_carrier, s_mount;
static bool s_ready, s_holdoff;
static pk_airband_record_status_t s_status;
/* Everything below is writer/I/O-mutex owned. Never take the SD module lock. */
static pk_airband_wav_t s_wav;
static FILE *s_fp;
static char s_root[192], s_part[320];
static uint32_t s_prepared_mount, s_sequence, s_file_session, s_io_session, s_io_segment, s_volume;
static int64_t s_checkpoint_at;

static int file_write(void *ctx, const uint8_t *p, size_t n, size_t *written)
{ *written = fwrite(p, 1, n, ctx); return *written == n ? 0 : -1; }
static int file_seek(void *ctx, uint32_t offset)
{ return fseek(ctx, (long)offset, SEEK_SET); }
static int file_sync(void *ctx)
{ int a = fflush(ctx); int b = fsync(fileno(ctx)); return a || b ? -1 : 0; }
static int file_close(void *ctx) { return fclose(ctx); }
static pk_airband_wav_io_t file_io(FILE *fp)
{ return (pk_airband_wav_io_t){fp,file_write,file_seek,file_sync,file_close}; }

static void errors_locked(pk_airband_wav_result_t e)
{
    if (!e) return;
    portENTER_CRITICAL(&s_mux);
    s_status.error = true;
    if (e & (PK_AIRBAND_WAV_ERR_WRITE | PK_AIRBAND_WAV_ERR_SHORT_WRITE |
             PK_AIRBAND_WAV_ERR_ODD_WRITE | PK_AIRBAND_WAV_ERR_SEEK)) s_status.write_errors++;
    if (e & PK_AIRBAND_WAV_ERR_SYNC) s_status.sync_errors++;
    if (e & PK_AIRBAND_WAV_ERR_CLOSE) s_status.close_errors++;
    portEXIT_CRITICAL(&s_mux);
}

/* Caller holds the producer mux. Control does not consume a queue slot. */
static void end_segment(bool interrupted)
{
    if (s_segment && s_head != s_tail) {
        record_block_t *last = &s_blocks[(s_head - 1u) % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
        if (last->segment == s_segment) {
            last->end = true;
            last->interrupted |= interrupted;
        }
    }
    if (interrupted && s_segment) s_status.interrupted = true;
    s_segment = 0;
}

static bool publish_final(const char *part, bool interrupted, char *final, size_t capacity)
{
    const size_t n = strlen(part);
    if (n < 9 || strcmp(part+n-9,".part.wav") != 0) return false;
    int used = snprintf(final,capacity,"%.*s%s.wav",(int)(n-9),part,
                        interrupted ? "-interrupted" : "");
    if (used < 0 || (size_t)used >= capacity) return false;
    struct stat st;
    if (stat(final,&st) == 0 || errno != ENOENT) return false;
    return rename(part,final) == 0;
}

static bool close_segment(bool interrupted)
{
    if (!s_fp) return true;
    /* Odd short writes must not leave a dangling half-sample. Failure leaves
     * the .part.wav for recovery and is surfaced; it is never published. */
    bool truncate_ok = true;
    if (s_wav.data_bytes & 1u) {
        truncate_ok = fflush(s_fp) == 0 &&
            ftruncate(fileno(s_fp),pk_airband_wav_file_size(&s_wav)) == 0;
    }
    pk_airband_wav_result_t e = pk_airband_wav_close(&s_wav);
    s_fp = NULL;
    if (!truncate_ok) e |= PK_AIRBAND_WAV_ERR_WRITE;
    char final[320];
    bool published = !e && publish_final(s_part,interrupted,final,sizeof(final));
    if (!e && !published) e |= PK_AIRBAND_WAV_ERR_WRITE;
    errors_locked(e);
    portENTER_CRITICAL(&s_mux);
    s_status.active = false;
    if (e) { s_ready=false;s_holdoff=true;end_segment(true); }
    if (published) {
        s_status.segments++;
        snprintf(s_status.file,sizeof(s_status.file),"%s",strrchr(final,'/')+1);
    }
    portEXIT_CRITICAL(&s_mux);
    s_io_segment = 0;
    return !e;
}

static bool recover_part(const char *path)
{
    struct stat st;
    if (stat(path,&st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 44) return false;
    FILE *fp = fopen(path,"r+b");
    if (!fp) return false;
    unsigned char header[44];
    static const unsigned char format[28] = {
        'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
        0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,
    };
    bool ok = setvbuf(fp,NULL,_IONBF,0) == 0 && fread(header,1,44,fp) == 44 &&
        memcmp(header,"RIFF",4) == 0 && memcmp(header+8,format,sizeof(format)) == 0 &&
        memcmp(header+36,"data",4) == 0;
    if (!ok) { fclose(fp); return false; }
    pk_airband_wav_t wav = {0};
    const pk_airband_wav_io_t io = file_io(fp);
    pk_airband_wav_result_t e = pk_airband_wav_recover(&wav,&io,(uint64_t)st.st_size);
    if (!e && ftruncate(fileno(fp),pk_airband_wav_file_size(&wav)) != 0)
        e |= PK_AIRBAND_WAV_ERR_WRITE;
    if (wav.is_open) e |= pk_airband_wav_close(&wav);
    else if (fclose(fp) != 0) e |= PK_AIRBAND_WAV_ERR_CLOSE;
    char final[320];
    ok = !e && publish_final(path,true,final,sizeof(final));
    return ok;
}

static bool prepare_mount(uint32_t mount)
{
    int n = snprintf(s_root,sizeof(s_root),"%s/airband",pk_sdcard_mount_point());
    if (n < 0 || (size_t)n >= sizeof(s_root)) return false;
    if (mkdir(s_root,0775) != 0 && errno != EEXIST) return false;
    DIR *d = opendir(s_root);
    if (!d) return false;
    bool ok = true;
    s_sequence = 0;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(d)) != NULL) {
        unsigned seq;
        if (sscanf(ent->d_name,"s%10u-",&seq) == 1 && seq > s_sequence) s_sequence = seq;
        size_t len = strlen(ent->d_name);
        if (len >= 9 && strcmp(ent->d_name+len-9,".part.wav") == 0) {
            char path[320];
            n = snprintf(path,sizeof(path),"%s/%s",s_root,ent->d_name);
            bool recovered = n > 0 && (size_t)n < sizeof(path) && recover_part(path);
            portENTER_CRITICAL(&s_mux);
            if (recovered) s_status.recovered++;
            else { s_status.recovery_errors++; s_status.error = true; }
            portEXIT_CRITICAL(&s_mux);
            if (!recovered) ok = false;
        }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(d) != 0) ok = false;
    s_prepared_mount = mount;
    s_io_session = 0;
    return ok;
}

static bool open_segment(const record_block_t *b)
{
    if (s_io_session != b->session) {
        if (s_sequence == UINT32_MAX) return false;
        s_file_session = ++s_sequence;
        s_io_session = b->session;
        s_volume = 0;
    }
    int n = snprintf(s_part,sizeof(s_part),"%s/s%010lu-c%06lu-f%09lu-%010lu.part.wav",
        s_root,(unsigned long)s_file_session,(unsigned long)b->channel,
        (unsigned long)b->carrier,(unsigned long)++s_volume);
    if (n < 0 || (size_t)n >= sizeof(s_part)) return false;
    /* Exclusive creation: an existing recording is never truncated. */
    int fd = open(s_part,O_WRONLY|O_CREAT|O_EXCL,0664);
    if (fd < 0) return false;
    s_fp = fdopen(fd,"wb");
    if (!s_fp) { close(fd); return false; }
    if (setvbuf(s_fp,NULL,_IONBF,0) != 0) { fclose(s_fp);s_fp=NULL;return false; }
    s_wav = (pk_airband_wav_t){0};
    const pk_airband_wav_io_t io = file_io(s_fp);
    pk_airband_wav_result_t e = pk_airband_wav_init(&s_wav,&io);
    errors_locked(e);
    if (e) { close_segment(true); return false; }
    s_io_segment = b->segment;
    s_checkpoint_at = esp_timer_get_time() + 1000000;
    portENTER_CRITICAL(&s_mux);
    s_status.active = true;
    snprintf(s_status.file,sizeof(s_status.file),"%s",strrchr(s_part,'/')+1);
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static void pre_unmount(void)
{
    portENTER_CRITICAL(&s_mux);
    s_ready = false;
    s_mount++;
    end_segment(true);
    s_holdoff = true;
    s_status.sd_missing = true;
    portEXIT_CRITICAL(&s_mux);
    /* SD already published not-mounted. New producer copies are stopped;
     * taking our mutex waits for any in-flight FAT operation to finish. */
    xSemaphoreTake(s_io,portMAX_DELAY);
    close_segment(true);
    portENTER_CRITICAL(&s_mux);
    s_head = s_tail = 0;
    s_status.stopping = false;
    portEXIT_CRITICAL(&s_mux);
    s_prepared_mount = 0;
    xSemaphoreGive(s_io);
}

static void record_poll(void)
{
    xSemaphoreTake(s_io,portMAX_DELAY);
    const uint32_t mount = pk_sdcard_mount_generation();
    if (!pk_sdcard_is_mounted()) {
        close_segment(true);
        portENTER_CRITICAL(&s_mux);
        s_ready = false;s_status.sd_missing = true;
        if (pk_sdcard_media_error()) s_status.error = true;
        portEXIT_CRITICAL(&s_mux);
        xSemaphoreGive(s_io);return;
    }
    if (s_prepared_mount != mount) {
        close_segment(true);
        bool ready = prepare_mount(mount);
        portENTER_CRITICAL(&s_mux);
        s_head = s_tail = 0;s_segment = 0;s_mount = mount;
        s_ready = ready && pk_sdcard_is_mounted() && pk_sdcard_mount_generation() == mount;
        s_status.mount_generation = mount;
        s_status.sd_missing = !s_ready;
        if (!ready) { s_status.error = true;s_status.write_errors++; }
        portEXIT_CRITICAL(&s_mux);
    }
    record_block_t b;
    portENTER_CRITICAL(&s_mux);
    bool have = s_ready && s_tail != s_head;
    if (have) b = s_blocks[s_tail % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
    const bool enabled = s_status.enabled;
    portEXIT_CRITICAL(&s_mux);
    if (have) {
        bool ok = b.mount == mount && pk_sdcard_is_mounted();
        if (s_fp && s_io_segment != b.segment) ok = close_segment(false) && ok;
        if (ok && !s_fp) ok = open_segment(&b);
        size_t offset = 0;
        while (ok && offset < b.count) {
            size_t consumed = 0;
            const uint32_t bytes_before = s_wav.data_bytes;
            pk_airband_wav_result_t e = pk_airband_wav_append(&s_wav,b.pcm+offset,b.count-offset,&consumed);
            offset += consumed;
            portENTER_CRITICAL(&s_mux);s_status.bytes += s_wav.data_bytes-bytes_before;portEXIT_CRITICAL(&s_mux);
            if (e == PK_AIRBAND_WAV_FULL) {
                ok = close_segment(false);
                if (ok && offset < b.count) ok = open_segment(&b);
            } else if (e) { errors_locked(e);ok = false; }
        }
        if (ok && s_fp && esp_timer_get_time() >= s_checkpoint_at) {
            pk_airband_wav_result_t e = pk_airband_wav_checkpoint(&s_wav);
            errors_locked(e);ok = !e;s_checkpoint_at = esp_timer_get_time()+1000000;
        }
        portENTER_CRITICAL(&s_mux);
        /* Re-read the end marker written by stop/overflow while I/O ran. */
        b.end = s_blocks[s_tail % PK_AIRBAND_RECORD_QUEUE_BLOCKS].end;
        b.interrupted = s_blocks[s_tail % PK_AIRBAND_RECORD_QUEUE_BLOCKS].interrupted;
        s_tail++;
        if (!ok) {
            s_status.error = true;s_status.write_errors++;
            end_segment(true);s_holdoff = true;s_ready = false;s_tail = s_head;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!ok || b.end) close_segment(!ok || b.interrupted);
    }
    portENTER_CRITICAL(&s_mux);
    bool done = s_tail == s_head && !s_segment;
    portEXIT_CRITICAL(&s_mux);
    if (done) close_segment(false);
    if (done && !enabled) {
        portENTER_CRITICAL(&s_mux);s_status.stopping=false;portEXIT_CRITICAL(&s_mux);
    }
    xSemaphoreGive(s_io);
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) { record_poll();vTaskDelay(pdMS_TO_TICKS(1)); }
}

bool pk_airband_record_init(void)
{
    if (s_status.started) return true;
    portENTER_CRITICAL(&s_mux);
    s_status.sd_missing=true;s_status.error=false;
    portEXIT_CRITICAL(&s_mux);
    if (!s_io) {
        s_io = xSemaphoreCreateMutex();
        if (!s_io) {
            portENTER_CRITICAL(&s_mux);s_status.error=true;portEXIT_CRITICAL(&s_mux);
            return false;
        }
        if (!pk_sdcard_register_pre_unmount_cb(pre_unmount)) {
            vSemaphoreDelete(s_io);s_io=NULL;
            portENTER_CRITICAL(&s_mux);s_status.error=true;portEXIT_CRITICAL(&s_mux);
            return false;
        }
    }
    if (xTaskCreatePinnedToCore(writer_task,"airband_rec",4096,NULL,2,NULL,0) != pdTRUE) {
        /* The registered callback owns this one mutex for the application
         * lifetime. Retain it on task failure so a later init can retry safely. */
        portENTER_CRITICAL(&s_mux);s_status.error=true;portEXIT_CRITICAL(&s_mux);
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    s_status.started=true;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

bool pk_airband_record_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_mux);
    bool ok = s_status.started && (!enabled || (s_ready && !s_status.stopping));
    if (ok && enabled != s_status.enabled) {
        if (enabled) { s_session++;s_holdoff=false;s_status.interrupted=false;s_status.error=false; }
        else { end_segment(false);s_status.stopping=s_status.active || s_head!=s_tail; }
        s_status.enabled=enabled;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool pk_airband_record_submit(const int16_t *pcm,size_t frames,bool rf,bool sq,
                              uint32_t channel,uint32_t carrier)
{
    portENTER_CRITICAL(&s_mux);
    const bool gate = rf && sq && pcm && frames;
    if (!gate || channel != s_channel || carrier != s_carrier) end_segment(!rf || (sq && !frames));
    if (!gate) {
        if (!sq) s_holdoff=false;
        portEXIT_CRITICAL(&s_mux);return false;
    }
    s_channel=channel;s_carrier=carrier;
    if (!s_status.enabled || !s_ready || s_holdoff) { portEXIT_CRITICAL(&s_mux);return false; }
    if (frames > PK_AIRBAND_RECORD_BLOCK_SAMPLES) {
        s_status.dropped_blocks++;
        s_status.dropped_samples += frames > UINT32_MAX ? UINT32_MAX : (uint32_t)frames;
        end_segment(true);s_holdoff=true;s_status.interrupted=true;
        portEXIT_CRITICAL(&s_mux);return false;
    }
    bool ok = true;
    for (size_t offset=0;offset<frames;offset+=PK_AIRBAND_RECORD_BLOCK_SAMPLES) {
        if (s_head-s_tail == PK_AIRBAND_RECORD_QUEUE_BLOCKS) {
            s_status.dropped_blocks++;
            s_status.dropped_samples+=(uint32_t)(frames-offset);
            end_segment(true);s_holdoff=true;ok=false;break;
        }
        if (!s_segment) s_segment=++s_next_segment;
        record_block_t *b = &s_blocks[s_head % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
        *b=(record_block_t){.segment=s_segment,.session=s_session,.mount=s_mount,
            .channel=channel,.carrier=carrier};
        size_t n=frames-offset;if(n>PK_AIRBAND_RECORD_BLOCK_SAMPLES)n=PK_AIRBAND_RECORD_BLOCK_SAMPLES;
        b->count=(uint16_t)n;memcpy(b->pcm,pcm+offset,n*sizeof(*pcm));s_head++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void pk_airband_record_get(pk_airband_record_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);*out=s_status;out->queued_blocks=s_head-s_tail;portEXIT_CRITICAL(&s_mux);
}
const char *pk_airband_record_label(const pk_airband_record_status_t *s)
{
    if (s->stopping) return "WRITING";
    if (s->error) return "ERROR";
    if (s->sd_missing) return "SD MISSING";
    if (s->interrupted) return "DROP";
    if (s->active) return "REC";
    return s->enabled ? "ARMED" : "IDLE";
}
