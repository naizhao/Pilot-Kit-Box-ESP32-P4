/* Portable 16 kHz mono signed-16 PCM WAV writer; no filesystem/IDF dependency. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PK_AIRBAND_WAV_HEADER_BYTES 44u
#define PK_AIRBAND_WAV_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define PK_AIRBAND_WAV_SAMPLE_RATE 16000u

typedef uint32_t pk_airband_wav_result_t;
enum {
    PK_AIRBAND_WAV_OK              = 0,
    PK_AIRBAND_WAV_FULL            = 1u << 0,
    PK_AIRBAND_WAV_ERR_INVALID     = 1u << 1,
    PK_AIRBAND_WAV_ERR_STATE       = 1u << 2,
    PK_AIRBAND_WAV_ERR_WRITE       = 1u << 3,
    PK_AIRBAND_WAV_ERR_SHORT_WRITE = 1u << 4,
    PK_AIRBAND_WAV_ERR_ODD_WRITE   = 1u << 5,
    PK_AIRBAND_WAV_ERR_SEEK        = 1u << 6,
    PK_AIRBAND_WAV_ERR_SYNC        = 1u << 7,
    PK_AIRBAND_WAV_ERR_CLOSE       = 1u << 8,
};

/* All callbacks return 0 on success. write must always report actual bytes,
 * including on error, and never more than requested. close must release its
 * handle even when it returns an error. seek uses an absolute file offset. */
typedef struct {
    void *ctx;
    int (*write)(void *ctx, const uint8_t *data, size_t bytes, size_t *written);
    int (*seek)(void *ctx, uint32_t offset);
    int (*sync)(void *ctx);
    int (*close)(void *ctx);
} pk_airband_wav_io_t;

typedef struct {
    pk_airband_wav_io_t io;
    uint32_t data_bytes; /* Actual payload bytes, including an incomplete sample. */
    pk_airband_wav_result_t errors;
    bool is_open;
} pk_airband_wav_t;

/* Start with a zero-initialized writer and a new, empty seekable file.
 * Once arguments are accepted, this object owns io even on an I/O error;
 * the caller must eventually close it. Invalid arguments do not transfer io.
 * Initial empty header is written and synced before init succeeds. */
pk_airband_wav_result_t pk_airband_wav_init(pk_airband_wav_t *w,
                                          const pk_airband_wav_io_t *io);

/* Recover only files owned by this recording format. actual_size comes from
 * stat, not the stale header. Rebuilds a header for complete samples and syncs.
 * Caller truncates to file_size(), closes, and renames only after success.
 * Files shorter than the header or larger than the volume limit are rejected. */
pk_airband_wav_result_t pk_airband_wav_recover(pk_airband_wav_t *w,
                                             const pk_airband_wav_io_t *io,
                                             uint64_t actual_size);

/* Samples are converted explicitly to LE without changing the input. FULL
 * means some input did not fit; open a new volume and continue at consumed.
 * On I/O errors, consumed counts only complete successfully written samples.
 * Errors are sticky and prevent further appends; checkpoint/close remain valid. */
pk_airband_wav_result_t pk_airband_wav_append(pk_airband_wav_t *w,
                                            const int16_t *samples,
                                            size_t sample_count,
                                            size_t *consumed_samples);
uint32_t pk_airband_wav_remaining_samples(const pk_airband_wav_t *w);
uint32_t pk_airband_wav_file_size(const pk_airband_wav_t *w);

/* Rewrites header, returns to the payload end, then syncs. close also performs
 * a checkpoint and always invokes close, even when earlier operations failed.
 * Results include all sticky errors; a second close does not reuse the handle. */
pk_airband_wav_result_t pk_airband_wav_checkpoint(pk_airband_wav_t *w);
pk_airband_wav_result_t pk_airband_wav_close(pk_airband_wav_t *w);
