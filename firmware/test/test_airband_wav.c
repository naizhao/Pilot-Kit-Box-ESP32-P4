/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 * -o /tmp/test_airband_wav firmware/test/test_airband_wav.c \
 * firmware/main/airband_wav.c && /tmp/test_airband_wav
 */
#include "airband_wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); fail++; } } while (0)

/* The byte store implements real seek/write semantics; injected failures can
 * leave a partial write, matching the file API's actual-byte contract. */
typedef struct {
    uint8_t *bytes;
    size_t capacity, size, pos;
    unsigned writes, seeks, syncs, closes;
    unsigned fail_write, fail_seek, fail_sync, fail_close;
    size_t partial_bytes;
    bool write_error, closed;
} file_t;

static file_t new_file(size_t capacity)
{
    file_t f = {.bytes = calloc(capacity, 1), .capacity = capacity};
    if (!f.bytes) { fputs("allocation failed\n", stderr); exit(2); }
    return f;
}
static int file_write(void *ctx, const uint8_t *data, size_t bytes, size_t *written)
{
    file_t *f = ctx;
    CHECK(!f->closed);
    f->writes++;
    bool inject = f->writes == f->fail_write;
    size_t n = inject ? f->partial_bytes : bytes;
    if (n > bytes) n = bytes;
    if (n > f->capacity - f->pos) n = f->capacity - f->pos;
    memcpy(f->bytes + f->pos, data, n);
    f->pos += n;
    if (f->pos > f->size) f->size = f->pos;
    *written = n;
    return inject && f->write_error ? -1 : 0;
}
static int file_seek(void *ctx, uint32_t offset)
{
    file_t *f = ctx;
    CHECK(!f->closed);
    f->seeks++;
    if (f->seeks == f->fail_seek || offset > f->capacity) return -1;
    f->pos = offset;
    return 0;
}
static int file_sync(void *ctx)
{
    file_t *f = ctx;
    CHECK(!f->closed);
    f->syncs++;
    return f->syncs == f->fail_sync ? -1 : 0;
}
static int file_close(void *ctx)
{
    file_t *f = ctx;
    CHECK(!f->closed);
    f->closes++;
    f->closed = true;
    return f->closes == f->fail_close ? -1 : 0;
}
static pk_airband_wav_io_t io_for(file_t *f)
{ return (pk_airband_wav_io_t){f, file_write, file_seek, file_sync, file_close}; }
static void check_header(const file_t *f, const uint8_t size_bytes[4],
                         const uint8_t riff_bytes[4])
{
    /* All format bytes are hand calculated: 16000=0x3e80, 32000=0x7d00. */
    static const uint8_t expected[44] = {
        'R','I','F','F', 0x24,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 0x10,0,0,0, 1,0, 1,0,
        0x80,0x3e,0,0, 0,0x7d,0,0, 2,0, 0x10,0,
        'd','a','t','a', 0,0,0,0
    };
    CHECK(memcmp(f->bytes, expected, 4) == 0);
    CHECK(memcmp(f->bytes + 4, riff_bytes, 4) == 0);
    CHECK(memcmp(f->bytes + 8, expected + 8, 32) == 0);
    CHECK(memcmp(f->bytes + 40, size_bytes, 4) == 0);
}

static void test_empty_header_is_durable(void)
{
    file_t f = new_file(4096);
    pk_airband_wav_io_t io = io_for(&f);
    pk_airband_wav_t w = {0};
    CHECK(pk_airband_wav_init(&w, &io) == PK_AIRBAND_WAV_OK);
    CHECK(f.size == 44 && f.pos == 44 && f.syncs == 1);
    check_header(&f, (uint8_t[]){0,0,0,0}, (uint8_t[]){36,0,0,0});
    CHECK(pk_airband_wav_remaining_samples(&w) == 8388586u);
    CHECK(pk_airband_wav_file_size(&w) == 44);
    CHECK(pk_airband_wav_close(&w) == PK_AIRBAND_WAV_OK);
    CHECK(f.closed && f.closes == 1 && !w.is_open);
    free(f.bytes);
}

static void test_pcm_bytes_and_checkpoint_cursor(void)
{
    file_t f = new_file(4096);
    pk_airband_wav_io_t io = io_for(&f);
    pk_airband_wav_t w = {0};
    const int16_t samples[] = {0, 1, -1, 32767, -32768, 0x1234};
    const uint8_t want[] = {0,0, 1,0, 255,255, 255,127, 0,128, 52,18};
    size_t consumed = 99;
    CHECK(pk_airband_wav_init(&w, &io) == PK_AIRBAND_WAV_OK);
    CHECK(pk_airband_wav_append(&w, samples, 6, &consumed) == PK_AIRBAND_WAV_OK);
    CHECK(consumed == 6 && w.data_bytes == 12 && f.size == 56);
    CHECK(memcmp(f.bytes + 44, want, sizeof(want)) == 0);
    CHECK(pk_airband_wav_checkpoint(&w) == PK_AIRBAND_WAV_OK);
    check_header(&f, (uint8_t[]){12,0,0,0}, (uint8_t[]){48,0,0,0});
    CHECK(f.pos == 56 && f.syncs == 2);
    CHECK(pk_airband_wav_append(&w, samples + 5, 1, &consumed) == PK_AIRBAND_WAV_OK);
    CHECK(f.bytes[56] == 52 && f.bytes[57] == 18 && f.size == 58);
    CHECK(pk_airband_wav_close(&w) == PK_AIRBAND_WAV_OK);
    check_header(&f, (uint8_t[]){14,0,0,0}, (uint8_t[]){50,0,0,0});
    unsigned calls = f.closes;
    CHECK(pk_airband_wav_close(&w) == PK_AIRBAND_WAV_OK && f.closes == calls);
    free(f.bytes);
}

static void test_partial_writes_are_counted_and_stop_appending(void)
{
    const struct {
        size_t bytes;
        bool error;
        pk_airband_wav_result_t want;
    } cases[] = {
        {0, true, PK_AIRBAND_WAV_ERR_WRITE | PK_AIRBAND_WAV_ERR_SHORT_WRITE},
        {2, false, PK_AIRBAND_WAV_ERR_SHORT_WRITE},
        {3, false, PK_AIRBAND_WAV_ERR_SHORT_WRITE | PK_AIRBAND_WAV_ERR_ODD_WRITE},
        {4, true, PK_AIRBAND_WAV_ERR_WRITE | PK_AIRBAND_WAV_ERR_SHORT_WRITE},
        {6, true, PK_AIRBAND_WAV_ERR_WRITE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        file_t f = new_file(4096);
        pk_airband_wav_io_t io = io_for(&f);
        pk_airband_wav_t w = {0};
        CHECK(pk_airband_wav_init(&w, &io) == PK_AIRBAND_WAV_OK);
        f.fail_write = f.writes + 1;
        f.partial_bytes = cases[i].bytes;
        f.write_error = cases[i].error;
        size_t consumed = 99;
        CHECK(pk_airband_wav_append(&w, (int16_t[]){0x1234, -1, 1}, 3,
                                     &consumed) == cases[i].want);
        CHECK(w.data_bytes == cases[i].bytes && consumed == cases[i].bytes / 2);
        CHECK(f.size == 44 + cases[i].bytes);
        CHECK(pk_airband_wav_file_size(&w) == 44 + (cases[i].bytes & ~(size_t)1));
        const unsigned writes = f.writes;
        consumed = 99;
        CHECK(pk_airband_wav_append(&w, (int16_t[]){2}, 1, &consumed) == cases[i].want);
        CHECK(consumed == 0 && f.writes == writes);
        CHECK(pk_airband_wav_close(&w) == cases[i].want);
        CHECK(f.closed && f.closes == 1);
        CHECK(f.bytes[40] == (cases[i].bytes & ~(size_t)1));
        free(f.bytes);
    }
}

static void test_initial_header_failures_keep_close_ownership(void)
{
    for (unsigned which = 0; which < 4; which++) {
        file_t f = new_file(4096);
        pk_airband_wav_io_t io = io_for(&f);
        pk_airband_wav_t w = {0};
        pk_airband_wav_result_t want = PK_AIRBAND_WAV_ERR_SEEK;
        if (which == 0) f.fail_seek = 1;
        if (which == 1) {
            f.fail_write = 1; f.partial_bytes = 12;
            want = PK_AIRBAND_WAV_ERR_SHORT_WRITE;
        }
        if (which == 2) {
            f.fail_write = 1; f.write_error = true;
            want = PK_AIRBAND_WAV_ERR_WRITE | PK_AIRBAND_WAV_ERR_SHORT_WRITE;
        }
        if (which == 3) { f.fail_sync = 1; want = PK_AIRBAND_WAV_ERR_SYNC; }
        CHECK(pk_airband_wav_init(&w, &io) == want);
        CHECK(w.is_open && w.data_bytes == 0);
        CHECK(pk_airband_wav_close(&w) == want);
        CHECK(f.closed && f.closes == 1);
        free(f.bytes);
    }
}

static void test_checkpoint_and_close_accumulate_failures(void)
{
    for (unsigned which = 0; which < 5; which++) {
        file_t f = new_file(4096);
        pk_airband_wav_io_t io = io_for(&f);
        pk_airband_wav_t w = {0};
        size_t consumed;
        CHECK(pk_airband_wav_init(&w, &io) == PK_AIRBAND_WAV_OK);
        CHECK(pk_airband_wav_append(&w, (int16_t[]){0x1234, -1, 1}, 3,
                                     &consumed) == PK_AIRBAND_WAV_OK);
        pk_airband_wav_result_t want = PK_AIRBAND_WAV_ERR_SEEK;
        if (which == 0) f.fail_seek = f.seeks + 1;
        if (which == 1) f.fail_seek = f.seeks + 2;
        if (which == 2) {
            f.fail_write = f.writes + 1; f.partial_bytes = 10;
            want = PK_AIRBAND_WAV_ERR_SHORT_WRITE;
        }
        if (which == 3) {
            f.fail_write = f.writes + 1; f.write_error = true;
            want = PK_AIRBAND_WAV_ERR_WRITE | PK_AIRBAND_WAV_ERR_SHORT_WRITE;
        }
        if (which == 4) { f.fail_sync = f.syncs + 1; want = PK_AIRBAND_WAV_ERR_SYNC; }
        CHECK(pk_airband_wav_checkpoint(&w) == want);
        CHECK(w.data_bytes == 6);
        CHECK(memcmp(f.bytes + 44, (uint8_t[]){52,18, 255,255, 1,0}, 6) == 0);
        f.fail_sync = f.syncs + 1;
        f.fail_close = 1;
        want |= PK_AIRBAND_WAV_ERR_SYNC | PK_AIRBAND_WAV_ERR_CLOSE;
        CHECK(pk_airband_wav_close(&w) == want);
        CHECK(f.closed && f.closes == 1 && !w.is_open);
        CHECK(pk_airband_wav_close(&w) == want && f.closes == 1);
        free(f.bytes);
    }
}

static void test_recovery_uses_real_even_file_length(void)
{
    file_t f = new_file(4096);
    pk_airband_wav_io_t io = io_for(&f);
    pk_airband_wav_t w = {0};
    memset(f.bytes, 0xFF, 51); /* Stale/corrupt size fields must be ignored. */
    memcpy(f.bytes + 44, (uint8_t[]){52,18, 255,255, 1,0, 0xaa}, 7);
    f.size = 51;
    CHECK(pk_airband_wav_recover(&w, &io, 51) == PK_AIRBAND_WAV_OK);
    CHECK(w.data_bytes == 6 && pk_airband_wav_file_size(&w) == 50);
    CHECK(f.pos == 50 && f.syncs == 1);
    check_header(&f, (uint8_t[]){6,0,0,0}, (uint8_t[]){42,0,0,0});
    CHECK(memcmp(f.bytes + 44, (uint8_t[]){52,18, 255,255, 1,0, 0xaa}, 7) == 0);
    CHECK(pk_airband_wav_close(&w) == PK_AIRBAND_WAV_OK && f.closes == 1);
    free(f.bytes);
}

static void test_volume_boundary_leaves_unconsumed_samples_for_next_file(void)
{
    file_t f = new_file(16777216u), next = new_file(4096);
    pk_airband_wav_io_t io = io_for(&f), next_io = io_for(&next);
    pk_airband_wav_t w = {0}, second = {0};
    /* A nearly full existing volume allows the boundary to be checked without
     * millions of unrelated writes. The last four bytes are two samples. */
    f.size = 16777212u;
    CHECK(pk_airband_wav_recover(&w, &io, f.size) == PK_AIRBAND_WAV_OK);
    CHECK(pk_airband_wav_remaining_samples(&w) == 2);
    int16_t samples[] = {0x1234, -32768, -1, 32767};
    size_t consumed = 0, remainder = 0;
    CHECK(pk_airband_wav_append(&w, samples, 4, &consumed) == PK_AIRBAND_WAV_FULL);
    CHECK(consumed == 2 && f.size == 16777216u);
    CHECK(pk_airband_wav_remaining_samples(&w) == 0);
    CHECK(memcmp(f.bytes + 16777212u, (uint8_t[]){52,18, 0,128}, 4) == 0);
    const unsigned writes = f.writes;
    CHECK(pk_airband_wav_append(&w, samples + consumed, 2, &remainder) == PK_AIRBAND_WAV_FULL);
    CHECK(remainder == 0 && f.writes == writes);
    CHECK(pk_airband_wav_close(&w) == PK_AIRBAND_WAV_OK);
    check_header(&f, (uint8_t[]){0xd4,0xff,0xff,0}, (uint8_t[]){0xf8,0xff,0xff,0});
    CHECK(pk_airband_wav_init(&second, &next_io) == PK_AIRBAND_WAV_OK);
    CHECK(pk_airband_wav_append(&second, samples + consumed, 4 - consumed,
                                 &remainder) == PK_AIRBAND_WAV_OK);
    CHECK(remainder == 2 && memcmp(next.bytes + 44, (uint8_t[]){255,255,255,127}, 4) == 0);
    CHECK(pk_airband_wav_close(&second) == PK_AIRBAND_WAV_OK);
    free(f.bytes); free(next.bytes);
}

static void test_large_input_keeps_chunk_order(void)
{
    file_t f = new_file(4096);
    pk_airband_wav_io_t io = io_for(&f);
    pk_airband_wav_t w = {0};
    int16_t samples[769];
    for (size_t i = 0; i < 769; i++) samples[i] = (i & 1) ? -32768 : 0x1234;
    size_t consumed = 0;
    CHECK(pk_airband_wav_init(&w, &io) == PK_AIRBAND_WAV_OK);
    CHECK(pk_airband_wav_append(&w, samples, 769, &consumed) == PK_AIRBAND_WAV_OK);
    CHECK(consumed == 769 && w.data_bytes == 1538);
    for (size_t i = 0; i < 769; i++) {
        const uint8_t *want = (i & 1) ? (uint8_t[]){0,128} : (uint8_t[]){52,18};
        CHECK(memcmp(f.bytes + 44 + i * 2, want, 2) == 0);
    }
    CHECK(pk_airband_wav_close(&w) == PK_AIRBAND_WAV_OK);
    free(f.bytes);
}

static void test_invalid_input_has_no_io_side_effect(void)
{
    file_t f = new_file(4096);
    pk_airband_wav_io_t io = io_for(&f), bad = io;
    pk_airband_wav_t w = {0};
    bad.sync = NULL;
    CHECK(pk_airband_wav_init(NULL, &io) == PK_AIRBAND_WAV_ERR_INVALID);
    CHECK(pk_airband_wav_init(&w, NULL) == PK_AIRBAND_WAV_ERR_INVALID);
    CHECK(pk_airband_wav_init(&w, &bad) == PK_AIRBAND_WAV_ERR_INVALID);
    CHECK(pk_airband_wav_recover(&w, &io, 43) == PK_AIRBAND_WAV_ERR_INVALID);
    CHECK(pk_airband_wav_recover(&w, &io, 16777217u) == PK_AIRBAND_WAV_ERR_INVALID);
    CHECK(f.writes == 0 && f.seeks == 0 && f.syncs == 0 && f.closes == 0);
    CHECK(pk_airband_wav_init(&w, &io) == PK_AIRBAND_WAV_OK);
    const unsigned writes = f.writes;
    CHECK(pk_airband_wav_init(&w, &io) == PK_AIRBAND_WAV_ERR_STATE);
    size_t consumed = 99;
    CHECK(pk_airband_wav_append(&w, NULL, 1, &consumed) == PK_AIRBAND_WAV_ERR_INVALID);
    CHECK(consumed == 0 && f.writes == writes && w.errors == 0);
    CHECK(pk_airband_wav_append(&w, NULL, 0, &consumed) == PK_AIRBAND_WAV_OK);
    CHECK(consumed == 0 && f.writes == writes);
    CHECK(pk_airband_wav_close(&w) == PK_AIRBAND_WAV_OK);
    CHECK(pk_airband_wav_append(&w, (int16_t[]){1}, 1, &consumed) == PK_AIRBAND_WAV_ERR_STATE);
    CHECK(pk_airband_wav_remaining_samples(&w) == 0);
    free(f.bytes);
}

int main(void)
{
    test_empty_header_is_durable();
    test_pcm_bytes_and_checkpoint_cursor();
    test_partial_writes_are_counted_and_stop_appending();
    test_initial_header_failures_keep_close_ownership();
    test_checkpoint_and_close_accumulate_failures();
    test_recovery_uses_real_even_file_length();
    test_volume_boundary_leaves_unconsumed_samples_for_next_file();
    test_large_input_keeps_chunk_order();
    test_invalid_input_has_no_io_side_effect();
    puts(fail ? "FAILED" : "OK");
    return fail != 0;
}
