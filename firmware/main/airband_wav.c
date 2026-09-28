#include "airband_wav.h"
#include <string.h>

static void put_u16(uint8_t *p, uint16_t value)
{ p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void encode_header(uint8_t out[PK_AIRBAND_WAV_HEADER_BYTES], uint32_t bytes)
{
    memcpy(out, "RIFF", 4);
    put_u32(out + 4, 36u + bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_u32(out + 16, 16);
    put_u16(out + 20, 1);
    put_u16(out + 22, 1);
    put_u32(out + 24, PK_AIRBAND_WAV_SAMPLE_RATE);
    put_u32(out + 28, PK_AIRBAND_WAV_SAMPLE_RATE * 2u);
    put_u16(out + 32, 2);
    put_u16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put_u32(out + 40, bytes);
}

static pk_airband_wav_result_t take_io(pk_airband_wav_t *w,
                                      const pk_airband_wav_io_t *io)
{
    if (!w || !io || !io->write || !io->seek || !io->sync || !io->close)
        return PK_AIRBAND_WAV_ERR_INVALID;
    if (w->is_open) return PK_AIRBAND_WAV_ERR_STATE;
    *w = (pk_airband_wav_t){.io = *io, .is_open = true};
    return PK_AIRBAND_WAV_OK;
}

static size_t write_bytes(pk_airband_wav_t *w, const uint8_t *bytes, size_t count)
{
    size_t written = 0;
    if (w->io.write(w->io.ctx, bytes, count, &written) != 0)
        w->errors |= PK_AIRBAND_WAV_ERR_WRITE;
    if (written > count) {
        /* A broken adapter cannot make the writer overflow its volume budget. */
        w->errors |= PK_AIRBAND_WAV_ERR_WRITE;
        written = count;
    }
    if (written != count) w->errors |= PK_AIRBAND_WAV_ERR_SHORT_WRITE;
    return written;
}

pk_airband_wav_result_t pk_airband_wav_init(pk_airband_wav_t *w,
                                          const pk_airband_wav_io_t *io)
{
    const pk_airband_wav_result_t result = take_io(w, io);
    if (result != PK_AIRBAND_WAV_OK) return result;
    return pk_airband_wav_checkpoint(w);
}

pk_airband_wav_result_t pk_airband_wav_recover(pk_airband_wav_t *w,
                                             const pk_airband_wav_io_t *io,
                                             uint64_t actual_size)
{
    if (actual_size < PK_AIRBAND_WAV_HEADER_BYTES ||
        actual_size > PK_AIRBAND_WAV_MAX_FILE_BYTES)
        return PK_AIRBAND_WAV_ERR_INVALID;
    const pk_airband_wav_result_t result = take_io(w, io);
    if (result != PK_AIRBAND_WAV_OK) return result;
    w->data_bytes = ((uint32_t)actual_size - PK_AIRBAND_WAV_HEADER_BYTES) & ~1u;
    return pk_airband_wav_checkpoint(w);
}

uint32_t pk_airband_wav_remaining_samples(const pk_airband_wav_t *w)
{
    if (!w || !w->is_open || w->errors ||
        w->data_bytes > PK_AIRBAND_WAV_MAX_FILE_BYTES - PK_AIRBAND_WAV_HEADER_BYTES)
        return 0;
    return (PK_AIRBAND_WAV_MAX_FILE_BYTES - PK_AIRBAND_WAV_HEADER_BYTES -
            w->data_bytes) / 2u;
}

uint32_t pk_airband_wav_file_size(const pk_airband_wav_t *w)
{ return w ? PK_AIRBAND_WAV_HEADER_BYTES + (w->data_bytes & ~1u) : 0; }

pk_airband_wav_result_t pk_airband_wav_append(pk_airband_wav_t *w,
                                            const int16_t *samples,
                                            size_t sample_count,
                                            size_t *consumed_samples)
{
    if (consumed_samples) *consumed_samples = 0;
    if (!w || !consumed_samples || (!samples && sample_count))
        return PK_AIRBAND_WAV_ERR_INVALID;
    if (!w->is_open) return PK_AIRBAND_WAV_ERR_STATE;
    if (w->errors) return w->errors;
    size_t to_write = pk_airband_wav_remaining_samples(w);
    if (to_write > sample_count) to_write = sample_count;
    uint8_t packed[512];
    while (*consumed_samples < to_write) {
        size_t n = to_write - *consumed_samples;
        if (n > sizeof(packed) / 2) n = sizeof(packed) / 2;
        for (size_t i = 0; i < n; i++)
            put_u16(packed + i * 2, (uint16_t)samples[*consumed_samples + i]);
        const size_t written = write_bytes(w, packed, n * 2);
        w->data_bytes += (uint32_t)written;
        *consumed_samples += written / 2;
        if (written & 1u) w->errors |= PK_AIRBAND_WAV_ERR_ODD_WRITE;
        if (w->errors) return w->errors;
    }
    return to_write < sample_count ? PK_AIRBAND_WAV_FULL : PK_AIRBAND_WAV_OK;
}

pk_airband_wav_result_t pk_airband_wav_checkpoint(pk_airband_wav_t *w)
{
    if (!w) return PK_AIRBAND_WAV_ERR_INVALID;
    if (!w->is_open) return PK_AIRBAND_WAV_ERR_STATE;
    if (w->io.seek(w->io.ctx, 0) != 0) {
        w->errors |= PK_AIRBAND_WAV_ERR_SEEK;
    } else {
        uint8_t header[PK_AIRBAND_WAV_HEADER_BYTES];
        encode_header(header, w->data_bytes & ~1u);
        (void)write_bytes(w, header, sizeof(header));
    }
    if (w->io.seek(w->io.ctx, PK_AIRBAND_WAV_HEADER_BYTES + w->data_bytes) != 0)
        w->errors |= PK_AIRBAND_WAV_ERR_SEEK;
    if (w->io.sync(w->io.ctx) != 0) w->errors |= PK_AIRBAND_WAV_ERR_SYNC;
    return w->errors;
}

pk_airband_wav_result_t pk_airband_wav_close(pk_airband_wav_t *w)
{
    if (!w) return PK_AIRBAND_WAV_ERR_INVALID;
    if (!w->is_open) return w->errors;
    (void)pk_airband_wav_checkpoint(w);
    if (w->io.close(w->io.ctx) != 0) w->errors |= PK_AIRBAND_WAV_ERR_CLOSE;
    w->is_open = false;
    return w->errors;
}
