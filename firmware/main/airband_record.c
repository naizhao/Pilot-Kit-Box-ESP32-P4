#include "airband_record.h"
#include "airband_wav.h"
#include "airband_recording_store.h"
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
#define RECORD_STDIO_BUFFER_BYTES 4096u
#define RECORD_CHECKPOINT_INTERVAL_US 2000000LL
#define RECORD_WRITE_BATCH_BLOCKS 32u
#define RECORD_PREPARE_RETRY_US 1000000LL
/* Copy-only critical section, independent of the filesystem mutex. The tail
 * remains owned until I/O finishes, so a close/overflow can mark its end even
 * when the writer has already copied that block. */
static EXT_RAM_BSS_ATTR record_block_t s_blocks[PK_AIRBAND_RECORD_QUEUE_BLOCKS];
static EXT_RAM_BSS_ATTR char s_stdio_buffer[RECORD_STDIO_BUFFER_BYTES];
static EXT_RAM_BSS_ATTR int16_t
    s_write_pcm[RECORD_WRITE_BATCH_BLOCKS * PK_AIRBAND_RECORD_BLOCK_SAMPLES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_io;
static uint32_t s_head, s_tail, s_segment, s_next_segment, s_session;
static uint32_t s_channel, s_carrier, s_mount;
static uint32_t s_source_gap_run;
static uint32_t s_pending_interrupted_segment;
static bool s_ready, s_holdoff;
static pk_airband_record_status_t s_status;
/* Everything below is writer/I/O-mutex owned. Never take the SD module lock. */
static pk_airband_wav_t s_wav;
static FILE *s_fp;
static char s_root[192], s_part[320];
static uint32_t s_prepared_mount, s_sequence, s_file_session, s_io_session, s_io_segment, s_volume;
static uint32_t s_prepare_mount;
static int64_t s_prepare_retry_at;
static int64_t s_checkpoint_at;

static void update_io_max(uint32_t *maximum, int64_t started)
{
    int64_t elapsed = esp_timer_get_time() - started;
    uint32_t value = elapsed <= 0 ? 0u : elapsed > UINT32_MAX
        ? UINT32_MAX : (uint32_t)elapsed;
    portENTER_CRITICAL(&s_mux);
    if (value > *maximum) *maximum = value;
    portEXIT_CRITICAL(&s_mux);
}
static int file_write(void *ctx, const uint8_t *p, size_t n, size_t *written)
{
    const int64_t started = esp_timer_get_time();
    *written = fwrite(p, 1, n, ctx);
    if (ctx == s_fp) update_io_max(&s_status.max_write_us, started);
    return *written == n ? 0 : -1;
}
static int file_seek(void *ctx, uint32_t offset)
{ return fseek(ctx, (long)offset, SEEK_SET); }
static int file_sync(void *ctx)
{
    const int64_t started = esp_timer_get_time();
    int a = fflush(ctx); int b = fsync(fileno(ctx));
    if (ctx == s_fp) update_io_max(&s_status.max_sync_us, started);
    return a || b ? -1 : 0;
}
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
    const uint32_t ended_segment = s_segment;
    bool marker_attached = false;
    if (s_segment && s_head != s_tail) {
        record_block_t *last = &s_blocks[(s_head - 1u) % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
        if (last->segment == s_segment) {
            last->end = true;
            last->interrupted |= interrupted;
            marker_attached = true;
        }
    }
    if (interrupted && ended_segment) {
        s_status.interrupted = true;
        /* The writer leaves a WAV open between polls.  If all PCM blocks have
         * already drained, there is no queue entry on which to carry the
         * interrupted bit, so retain it against that exact logical segment. */
        if (!marker_attached)
            s_pending_interrupted_segment = ended_segment;
    }
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
    portENTER_CRITICAL(&s_mux);
    if (s_pending_interrupted_segment == s_io_segment) {
        interrupted = true;
        s_pending_interrupted_segment = 0;
    }
    portEXIT_CRITICAL(&s_mux);
    /* Odd short writes must not leave a dangling half-sample. Failure leaves
     * the .part.wav for manual inspection and is surfaced; it is never published. */
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
    if (published) pk_airband_recording_store_request_refresh();
    s_io_segment = 0;
    return !e;
}

/* Automatic preparation never repairs or publishes an existing .part.wav.
 * A checkpoint is not proof of a complete recording: retaining the original
 * name and bytes also retains any PCM beyond it. Content, size and non-file
 * entries remain pending; genuine filesystem I/O failures stay visible.
 * Explicit low-level WAV recovery remains available to its existing callers. */
static bool inspect_part(const char *path)
{
    struct stat st;
    if (stat(path,&st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return true;
    FILE *fp = fopen(path,"rb");
    if (!fp) return false;
    unsigned char header[PK_AIRBAND_WAV_HEADER_BYTES];
    /* Read only a bounded prefix to expose read errors. No header is trusted
     * or used to decide whether an old file should become a final recording. */
    (void)fread(header,1,sizeof(header),fp);
    const bool read_ok = ferror(fp) == 0;
    const bool close_ok = fclose(fp) == 0;
    return read_ok && close_ok;
}

static bool prepare_mount(uint32_t mount)
{
    int n = snprintf(s_root,sizeof(s_root),"%s/airband",pk_sdcard_mount_point());
    if (n < 0 || (size_t)n >= sizeof(s_root)) return false;
    if (mkdir(s_root,0775) != 0 && errno != EEXIST) return false;
    DIR *d = opendir(s_root);
    if (!d) return false;
    /* Existing-part inspection is read-only; once a scan starts we do not retry
     * it against the same mount: the verdicts (pending / error)
     * are stable for an unchanged directory, and a failing card must not be
     * rescanned at poll rate.  Early mkdir/opendir failures remain safe to
     * retry at a bounded interval. */
    s_prepared_mount = mount;
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
            const bool inspected = n > 0 && (size_t)n < sizeof(path) && inspect_part(path);
            portENTER_CRITICAL(&s_mux);
            if (inspected) s_status.recovery_skipped++;
            else { s_status.recovery_errors++; s_status.error = true; }
            portEXIT_CRITICAL(&s_mux);
            /* Pending files are not failures: recording stays usable, the
             * .part.wav simply remains for a human decision. */
            if (!inspected) ok = false;
        }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(d) != 0) ok = false;
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
    /* The capture task produces 128 B every 4 ms.  A sector-sized stdio
     * buffer turns those tiny FAT calls into sequential 4 KiB writes; the
     * explicit checkpoints below remain the durability boundary. */
    if (setvbuf(s_fp,s_stdio_buffer,_IOFBF,sizeof(s_stdio_buffer)) != 0) {
        fclose(s_fp);s_fp=NULL;return false;
    }
    s_wav = (pk_airband_wav_t){0};
    const pk_airband_wav_io_t io = file_io(s_fp);
    pk_airband_wav_result_t e = pk_airband_wav_init(&s_wav,&io);
    errors_locked(e);
    if (e) { close_segment(true); return false; }
    s_io_segment = b->segment;
    s_checkpoint_at = esp_timer_get_time() + RECORD_CHECKPOINT_INTERVAL_US;
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
    s_prepare_mount = 0;
    s_prepare_retry_at = 0;
    xSemaphoreGive(s_io);
}

static void record_poll(void)
{
    xSemaphoreTake(s_io,portMAX_DELAY);
    const uint32_t mount = pk_sdcard_mount_generation();
    if (!pk_sdcard_is_mounted()) {
        close_segment(true);
        portENTER_CRITICAL(&s_mux);
        s_ready = false;s_status.sd_missing = true;s_status.stopping = false;
        if (pk_sdcard_media_error()) s_status.error = true;
        portEXIT_CRITICAL(&s_mux);
        xSemaphoreGive(s_io);return;
    }
    if (s_prepare_mount != mount) {
        close_segment(true);
        s_prepare_mount = mount;
        s_prepared_mount = 0;
        s_prepare_retry_at = 0;
        portENTER_CRITICAL(&s_mux);
        s_head = s_tail = 0;s_segment = 0;s_mount = mount;
        s_ready = false;
        s_status.mount_generation = mount;
        portEXIT_CRITICAL(&s_mux);
    }
    portENTER_CRITICAL(&s_mux);
    s_status.sd_missing = false;
    const bool needs_io = s_status.enabled || s_status.stopping || s_tail != s_head;
    portEXIT_CRITICAL(&s_mux);
    if (!needs_io && !s_fp) {
        xSemaphoreGive(s_io);return;
    }
    if (s_prepared_mount != mount && esp_timer_get_time() >= s_prepare_retry_at) {
        bool ready = prepare_mount(mount);
        s_prepare_retry_at = esp_timer_get_time() + RECORD_PREPARE_RETRY_US;
        portENTER_CRITICAL(&s_mux);
        s_ready = ready && pk_sdcard_is_mounted() && pk_sdcard_mount_generation() == mount;
        s_status.sd_missing = !pk_sdcard_is_mounted();
        if (s_ready) s_status.error = false;
        if (!ready) { s_status.error = true;s_status.write_errors++; }
        portEXIT_CRITICAL(&s_mux);
    }
    record_block_t b;
    uint32_t batch_blocks = 0;
    size_t batch_samples = 0;
    portENTER_CRITICAL(&s_mux);
    bool have = s_ready && s_tail != s_head;
    if (have) {
        b = s_blocks[s_tail % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
        while (batch_blocks < RECORD_WRITE_BATCH_BLOCKS &&
               s_tail + batch_blocks != s_head) {
            const record_block_t *next =
                &s_blocks[(s_tail + batch_blocks) % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
            if (next->segment != b.segment || next->session != b.session ||
                next->mount != b.mount || next->channel != b.channel ||
                next->carrier != b.carrier) break;
            batch_samples += next->count;
            batch_blocks++;
            if (next->end) break;
        }
    }
    const bool enabled = s_status.enabled;
    portEXIT_CRITICAL(&s_mux);
    if (have) {
        /* tail does not advance until I/O completes, so these owned PCM slots
         * cannot be overwritten.  Copy 4 KiB outside the critical section. */
        size_t copied = 0;
        for (uint32_t i = 0; i < batch_blocks; ++i) {
            const record_block_t *owned =
                &s_blocks[(s_tail + i) % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
            memcpy(s_write_pcm + copied, owned->pcm,
                   owned->count * sizeof(*owned->pcm));
            copied += owned->count;
        }
        bool ok = b.mount == mount && pk_sdcard_is_mounted();
        if (s_fp && s_io_segment != b.segment) ok = close_segment(false) && ok;
        if (ok && !s_fp) ok = open_segment(&b);
        size_t offset = 0;
        while (ok && offset < batch_samples) {
            size_t consumed = 0;
            const uint32_t bytes_before = s_wav.data_bytes;
            pk_airband_wav_result_t e = pk_airband_wav_append(
                &s_wav,s_write_pcm+offset,batch_samples-offset,&consumed);
            offset += consumed;
            portENTER_CRITICAL(&s_mux);s_status.bytes += s_wav.data_bytes-bytes_before;portEXIT_CRITICAL(&s_mux);
            if (e == PK_AIRBAND_WAV_FULL) {
                ok = close_segment(false);
                if (ok && offset < batch_samples) ok = open_segment(&b);
            } else if (e) { errors_locked(e);ok = false; }
        }
        if (ok && s_fp && esp_timer_get_time() >= s_checkpoint_at) {
            pk_airband_wav_result_t e = pk_airband_wav_checkpoint(&s_wav);
            errors_locked(e);ok = !e;
            s_checkpoint_at = esp_timer_get_time()+RECORD_CHECKPOINT_INTERVAL_US;
        }
        portENTER_CRITICAL(&s_mux);
        /* Re-read markers written by stop/overflow while the batch was in
         * FAT I/O.  The producer cannot overwrite these owned slots until
         * tail advances here. */
        b.end = false;
        b.interrupted = false;
        for (uint32_t i = 0; i < batch_blocks; ++i) {
            const record_block_t *done =
                &s_blocks[(s_tail + i) % PK_AIRBAND_RECORD_QUEUE_BLOCKS];
            b.end |= done->end;
            b.interrupted |= done->interrupted;
        }
        s_tail += batch_blocks;
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
    for (;;) {
        /* Drain a backlog immediately.  Sleeping after every 4 ms audio block
         * unnecessarily cuts the writer's recovery headroom under SD jitter. */
        record_poll();
        portENTER_CRITICAL(&s_mux);
        const bool pending = s_ready && s_tail != s_head;
        portEXIT_CRITICAL(&s_mux);
        if (!pending) vTaskDelay(pdMS_TO_TICKS(1));
    }
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
    if (xTaskCreatePinnedToCore(writer_task,"airband_rec",8192,NULL,2,NULL,0) != pdTRUE) {
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
    const bool mounted = !enabled || pk_sdcard_is_mounted();
    portENTER_CRITICAL(&s_mux);
    bool ok = s_status.started && (!enabled || (mounted && !s_status.stopping));
    if (ok && enabled != s_status.enabled) {
        s_source_gap_run=0;
        if (enabled) { s_session++;s_holdoff=false;s_status.interrupted=false;if(s_ready)s_status.error=false; }
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
    const bool channel_changed = channel != s_channel || carrier != s_carrier;
    const bool source_gap = rf && sq && (!pcm || frames == 0);
    if (channel_changed) { end_segment(false);s_source_gap_run=0; }
    if (!rf) { end_segment(true);s_source_gap_run=0; }
    else if (!sq) { end_segment(false);s_source_gap_run=0; }
    else if (source_gap && s_segment) {
        s_status.source_gaps++;
        s_source_gap_run++;
        if (s_source_gap_run >= PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS) {
            s_status.dropped_samples +=
                PK_AIRBAND_RECORD_SOURCE_GAP_LIMIT_BLOCKS *
                PK_AIRBAND_RECORD_BLOCK_SAMPLES;
            end_segment(true);
            s_holdoff=true;
        }
    } else if (gate) s_source_gap_run=0;
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
        const uint32_t queued = s_head-s_tail;
        if (queued > s_status.queue_high_water)
            s_status.queue_high_water = queued;
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
    if (s->enabled && s->interrupted) return "GAP";
    if (s->active) return "REC";
    return s->enabled ? "ARMED" : "IDLE";
}
