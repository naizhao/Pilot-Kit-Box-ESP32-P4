/* pk_pmtiles.c — PMTiles v3 只读解析器实现。设计与分层说明见 pk_pmtiles.h。
 *
 * gzip 解压后端二选一（编译期）：
 *   ESP_PLATFORM  → esp_rom 自带 miniz.h 声明，tinfl 实现在 ROM 里，零 flash；
 *   否则（host 单测等） → third_party/pk_tinfl.{h,c} vendor 版，同名 API。
 * 两侧共用的是本文件里的 gzip 容器头解析（跳过 FEXTRA/FNAME/FCOMMENT/FHCRC，
 * 从尾部 ISIZE 拿解压后长度），只有"谁提供 tinfl_decompress_mem_to_mem"这层
 * 不同，行为完全一致（都是标准 miniz tinfl 语义）。
 */
#include "pk_pmtiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "miniz.h"
static const char *TAG = "pk_pmtiles";
#define PK_PM_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define PK_PM_LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#else
#include "third_party/pk_tinfl.h"
#define PK_PM_LOGW(fmt, ...) fprintf(stderr, "W pk_pmtiles: " fmt "\n", ##__VA_ARGS__)
#define PK_PM_LOGE(fmt, ...) fprintf(stderr, "E pk_pmtiles: " fmt "\n", ##__VA_ARGS__)
#endif

/* 目录树最深往下挖几层叶目录才认输——正常 PMTiles 包 1~2 层，给足余量的
 * 同时防corrupt/循环数据把我们拖进死循环。 */
#define PK_PMTILES_MAX_DIR_DEPTH 8

/* ---------------------------------------------------------------- 读盘 */

static bool read_at(pk_pmtiles_t *pm, uint64_t off, void *buf, size_t len)
{
    if (len == 0) return true;
    return pm->read(pm->read_ctx, off, buf, len) == 0;
}

/* --------------------------------------------------------------- gzip */

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 解一段 gzip 容器：跳过头部可选字段，找到 raw deflate 数据区，用尾部
 * ISIZE（解压后长度模 2^32，PMTiles 目录体量下足够）预分配输出缓冲，交给
 * tinfl 一把梭解开。成功时 *out 是 malloc 出来的 *out_len 字节，调用方负责
 * free；失败返回 false，*out 不变。 */
static bool gzip_inflate(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len)
{
    /* 最短合法 gzip：10B 头 + 2B 空 deflate block + 8B 尾。 */
    if (in_len < 20 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
        PK_PM_LOGE("gzip: bad magic/method (len=%zu)", in_len);
        return false;
    }
    uint8_t flg = in[3];
    size_t pos = 10;

    if (flg & 0x04) { /* FEXTRA */
        if (pos + 2 > in_len) return false;
        uint16_t xlen = (uint16_t)(in[pos] | (in[pos + 1] << 8));
        pos += 2 + xlen;
    }
    if (flg & 0x08) { /* FNAME，NUL 结尾 */
        while (pos < in_len && in[pos] != 0) pos++;
        pos++;
    }
    if (flg & 0x10) { /* FCOMMENT，NUL 结尾 */
        while (pos < in_len && in[pos] != 0) pos++;
        pos++;
    }
    if (flg & 0x02) { /* FHCRC */
        pos += 2;
    }
    if (pos + 8 > in_len) {
        PK_PM_LOGE("gzip: header overruns buffer (pos=%zu len=%zu)", pos, in_len);
        return false;
    }

    size_t deflate_start = pos;
    size_t deflate_len   = in_len - pos - 8;
    uint32_t isize        = rd_le32(in + in_len - 4);

    if (isize == 0) {
        /* 空目录理论上不会出现（open 时至少有 1 个 entry），但別把 0 当错误，
           给个最小 1 字节缓冲让 tinfl 走一遍，避免 malloc(0) 的实现定义行为。 */
        *out = malloc(1);
        if (!*out) return false;
        *out_len = 0;
        return true;
    }

    uint8_t *outbuf = malloc(isize);
    if (!outbuf) {
        PK_PM_LOGE("gzip: OOM allocating %u bytes", isize);
        return false;
    }
    /* 不能用 tinfl_decompress_mem_to_mem：它把 ~11KB 的 tinfl_decompressor
       放在调用方栈上（ROM 实现同样），真机 main/loader 任务栈直接打穿
       （实测 Stack protection fault boot loop）。decompressor 必须走堆。 */
    tinfl_decompressor *decomp = malloc(sizeof(tinfl_decompressor));
    if (!decomp) {
        PK_PM_LOGE("gzip: OOM allocating decompressor (%zu bytes)", sizeof(tinfl_decompressor));
        free(outbuf);
        return false;
    }
    tinfl_init(decomp);
    size_t in_bytes  = deflate_len;
    size_t out_bytes = isize;
    tinfl_status st = tinfl_decompress(decomp, in + deflate_start, &in_bytes,
                                       outbuf, outbuf, &out_bytes,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decomp);
    if (st != TINFL_STATUS_DONE) {
        PK_PM_LOGE("gzip: tinfl_decompress failed (st=%d deflate_len=%zu isize=%u)",
                   (int)st, deflate_len, isize);
        free(outbuf);
        return false;
    }
    *out     = outbuf;
    *out_len = out_bytes;
    return true;
}

/* ------------------------------------------------------------- varint */

static uint64_t read_varint(const uint8_t *buf, size_t len, size_t *pos)
{
    uint64_t result = 0;
    int shift = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        result |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) break; /* 防corrupt varint 无限吃字节 */
    }
    return result;
}

/* --------------------------------------------------------- 目录解析 */

/* PMTiles 目录二进制布局（解压后）：
 *   varint entry_count
 *   entry_count 个 varint：tile_id 增量（累加得绝对 tile_id，升序）
 *   entry_count 个 varint：run_length
 *   entry_count 个 varint：length
 *   entry_count 个 varint：offset 编码——v==0（且非第一条）表示"紧接上一条
 *     entry 的 offset+length"（聚簇写入省的空间），否则实际 offset=v-1。
 */
static bool parse_directory(const uint8_t *buf, size_t len, pk_pmtiles_dir_entry_t **out_entries, size_t *out_count)
{
    size_t pos = 0;
    uint64_t count = read_varint(buf, len, &pos);
    if (count == 0) {
        *out_entries = NULL;
        *out_count   = 0;
        return true;
    }

    pk_pmtiles_dir_entry_t *entries = calloc((size_t)count, sizeof(*entries));
    if (!entries) {
        PK_PM_LOGE("dir: OOM for %llu entries", (unsigned long long)count);
        return false;
    }

    uint64_t tile_id = 0;
    for (uint64_t i = 0; i < count; i++) {
        tile_id += read_varint(buf, len, &pos);
        entries[i].tile_id = tile_id;
    }
    for (uint64_t i = 0; i < count; i++) {
        entries[i].run_length = (uint32_t)read_varint(buf, len, &pos);
    }
    for (uint64_t i = 0; i < count; i++) {
        entries[i].length = (uint32_t)read_varint(buf, len, &pos);
    }
    for (uint64_t i = 0; i < count; i++) {
        uint64_t v = read_varint(buf, len, &pos);
        if (i > 0 && v == 0) {
            entries[i].offset = entries[i - 1].offset + entries[i - 1].length;
        } else {
            entries[i].offset = v - 1; /* v>=1 恒成立：v==0 只在 i==0 时才会走到这支，
                                           而 i==0 的 0 编码同样是"offset=0-1"数学上会
                                           下溢——但 spec 规定首条 offset 字段必须存
                                           实际值+1（不能省），所以 i==0 处 v 必然 >=1。 */
        }
    }

    *out_entries = entries;
    *out_count   = (size_t)count;
    return true;
}

/* 从盘上读一段目录（可能是根目录，也可能是叶目录），按 header 里的
 * internal_compression 解开，解析成 entry 数组。 */
static bool load_directory(pk_pmtiles_t *pm, uint64_t off, uint64_t len,
                            pk_pmtiles_dir_entry_t **out_entries, size_t *out_count)
{
    if (len == 0 || len > 32u * 1024 * 1024) { /* 32MB 防corrupt length 把内存吃爆 */
        PK_PM_LOGE("dir: implausible length %llu at off %llu", (unsigned long long)len, (unsigned long long)off);
        return false;
    }
    uint8_t *raw = malloc((size_t)len);
    if (!raw) return false;
    if (!read_at(pm, off, raw, (size_t)len)) {
        PK_PM_LOGE("dir: read failed off=%llu len=%llu", (unsigned long long)off, (unsigned long long)len);
        free(raw);
        return false;
    }

    bool ok;
    if (pm->header.internal_compression == PK_PMTILES_COMPRESSION_NONE) {
        ok = parse_directory(raw, (size_t)len, out_entries, out_count);
    } else if (pm->header.internal_compression == PK_PMTILES_COMPRESSION_GZIP) {
        uint8_t *inflated = NULL;
        size_t inflated_len = 0;
        ok = gzip_inflate(raw, (size_t)len, &inflated, &inflated_len);
        if (ok) {
            ok = parse_directory(inflated, inflated_len, out_entries, out_count);
            free(inflated);
        }
    } else {
        PK_PM_LOGE("dir: unsupported internal_compression=%u (只实现了 none/gzip)", pm->header.internal_compression);
        ok = false;
    }
    free(raw);
    return ok;
}

/* ------------------------------------------------------------ header */

static bool parse_header(const uint8_t *b, pk_pmtiles_header_t *h)
{
    if (memcmp(b, "PMTiles", 7) != 0) {
        PK_PM_LOGE("header: bad magic");
        return false;
    }
    h->version = b[7];
    if (h->version != 3) {
        PK_PM_LOGE("header: unsupported version %u (只实现 v3)", h->version);
        return false;
    }

#define RD_U64(off) (uint64_t)(b[off]) | ((uint64_t)b[off+1]<<8) | ((uint64_t)b[off+2]<<16) | ((uint64_t)b[off+3]<<24) | \
                    ((uint64_t)b[off+4]<<32) | ((uint64_t)b[off+5]<<40) | ((uint64_t)b[off+6]<<48) | ((uint64_t)b[off+7]<<56)
#define RD_I32(off) (int32_t)((uint32_t)b[off] | ((uint32_t)b[off+1]<<8) | ((uint32_t)b[off+2]<<16) | ((uint32_t)b[off+3]<<24))

    h->root_dir_offset       = RD_U64(8);
    h->root_dir_length       = RD_U64(16);
    h->json_metadata_offset  = RD_U64(24);
    h->json_metadata_length  = RD_U64(32);
    h->leaf_dirs_offset      = RD_U64(40);
    h->leaf_dirs_length      = RD_U64(48);
    h->tile_data_offset      = RD_U64(56);
    h->tile_data_length      = RD_U64(64);
    h->addressed_tiles_count = RD_U64(72);
    h->tile_entries_count    = RD_U64(80);
    h->tile_contents_count   = RD_U64(88);
    h->clustered             = b[96] != 0;
    h->internal_compression  = b[97];
    h->tile_compression      = b[98];
    h->tile_type             = b[99];
    h->min_zoom               = b[100];
    h->max_zoom               = b[101];
    h->min_lon_e7             = RD_I32(102);
    h->min_lat_e7             = RD_I32(106);
    h->max_lon_e7             = RD_I32(110);
    h->max_lat_e7             = RD_I32(114);
    h->center_zoom            = b[118];
    h->center_lon_e7          = RD_I32(119);
    h->center_lat_e7          = RD_I32(123);

#undef RD_U64
#undef RD_I32
    return true;
}

/* --------------------------------------------------------------- API */

bool pk_pmtiles_open(pk_pmtiles_t *pm, pk_pmtiles_read_fn read_fn, void *read_ctx)
{
    memset(pm, 0, sizeof(*pm));
    if (!read_fn) return false;
    pm->read     = read_fn;
    pm->read_ctx = read_ctx;

    uint8_t hdr_buf[PK_PMTILES_HEADER_LEN];
    if (!read_at(pm, 0, hdr_buf, sizeof(hdr_buf))) {
        PK_PM_LOGE("open: header read failed");
        return false;
    }
    if (!parse_header(hdr_buf, &pm->header)) {
        return false;
    }

    if (!load_directory(pm, pm->header.root_dir_offset, pm->header.root_dir_length,
                         &pm->root_entries, &pm->root_count)) {
        PK_PM_LOGE("open: root directory load failed");
        memset(pm, 0, sizeof(*pm));
        return false;
    }
    return true;
}

/* --------------------------------------------------- fopen 便捷封装 */

static int posix_file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    FILE *f = (FILE *)ctx;
    /* ctx==NULL = 文件已被 pk_pmtiles_close_file 收走（拔卡预卸载）。这里必须
     * 干净失败而不是解引用：拔卡后 loader/查找路径还可能带着旧 loc 进来。 */
    if (f == NULL) return -1;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    if (fread(buf, 1, len, f) != len) return -1;
    return 0;
}

bool pk_pmtiles_open_file(pk_pmtiles_t *pm, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        PK_PM_LOGE("open_file: fopen(%s) failed", path);
        return false;
    }
    if (!pk_pmtiles_open(pm, posix_file_read, f)) {
        fclose(f);
        return false;
    }
    pm->owned_file = f;
    return true;
}

void pk_pmtiles_close_file(pk_pmtiles_t *pm)
{
    if (!pm || pm->owned_file == NULL) return;
    /* 只收 FILE*，header/根目录原样保留——拔卡瞬间必须把 fd 清干净（IDF 的
     * esp_vfs_fat_sdcard_unmount 会无条件 free 掉含 FIL 数组的 fat_ctx，开着
     * 文件卸载 = use-after-free），但元数据活着才能让路由继续命中、已缓存
     * 瓦片继续显示。read_ctx 置空后 posix_file_read 对后续读一律干净失败。 */
    fclose((FILE *)pm->owned_file);
    pm->owned_file = NULL;
    pm->read_ctx   = NULL;
}

void pk_pmtiles_close(pk_pmtiles_t *pm)
{
    if (!pm) return;
    free(pm->root_entries);
    if (pm->owned_file) fclose((FILE *)pm->owned_file);
    memset(pm, 0, sizeof(*pm));
}

/* ------------------------------------------------------- Hilbert tileid */

/* Wikipedia "Hilbert curve" xy2d 的标准实现，PMTiles spec 附录用的就是这个
 * 变体（rotate 里用的是格子边长 n，不是当前迭代步长 s——这点容易搞混，我们
 * 用真实样本包（pk_map_prd_pilot.pmtiles）里 z0~z12 逐级实测反推校验过，
 * 见 firmware/test/test_pk_pmtiles.c 里嵌的测试向量）。 */
static void hilbert_rotate(uint32_t n, uint32_t *x, uint32_t *y, uint32_t rx, uint32_t ry)
{
    if (ry == 0) {
        if (rx == 1) {
            *x = n - 1 - *x;
            *y = n - 1 - *y;
        }
        uint32_t t = *x;
        *x = *y;
        *y = t;
    }
}

static uint64_t hilbert_xy2d(uint32_t n, uint32_t x, uint32_t y)
{
    uint64_t d = 0;
    for (uint32_t s = n / 2; s > 0; s /= 2) {
        uint32_t rx = (x & s) > 0 ? 1 : 0;
        uint32_t ry = (y & s) > 0 ? 1 : 0;
        d += (uint64_t)s * s * ((3 * rx) ^ ry);
        hilbert_rotate(n, &x, &y, rx, ry);
    }
    return d;
}

uint64_t pk_pmtiles_zxy_to_tileid(uint8_t z, uint32_t x, uint32_t y)
{
    uint64_t acc = 0;
    for (uint8_t i = 0; i < z; i++) {
        uint64_t tiles_at_zoom = (uint64_t)1 << i;
        acc += tiles_at_zoom * tiles_at_zoom;
    }
    uint32_t n = (uint32_t)1 << z;
    return acc + hilbert_xy2d(n, x, y);
}

/* ------------------------------------------------------------- 查找 */

/* PMTiles spec 的 find_tile 二分：先精确命中 tile_id；找不到就退到"最大的
 * tile_id <= target"那条，run_length==0 说明它是叶目录指针（无条件下钻），
 * 否则要求 target 落在 [tile_id, tile_id+run_length) 里才算命中。 */
static int find_entry_index(const pk_pmtiles_dir_entry_t *entries, size_t count, uint64_t tile_id)
{
    if (count == 0) return -1;
    long m = 0, n = (long)count - 1;
    while (m <= n) {
        long k = (m + n) / 2;
        if (tile_id > entries[k].tile_id) {
            m = k + 1;
        } else if (tile_id < entries[k].tile_id) {
            n = k - 1;
        } else {
            return (int)k;
        }
    }
    if (n >= 0) {
        if (entries[n].run_length == 0) return (int)n;
        if (tile_id - entries[n].tile_id < entries[n].run_length) return (int)n;
    }
    return -1;
}

bool pk_pmtiles_find_tile(pk_pmtiles_t *pm, uint8_t z, uint32_t x, uint32_t y, pk_pmtiles_tile_loc_t *out)
{
    uint64_t tile_id = pk_pmtiles_zxy_to_tileid(z, x, y);

    const pk_pmtiles_dir_entry_t *entries = pm->root_entries;
    size_t count = pm->root_count;
    pk_pmtiles_dir_entry_t *leaf_owned = NULL; /* 当前这层若是叶目录，需要自己 free */

    bool found = false;
    for (int depth = 0; depth < PK_PMTILES_MAX_DIR_DEPTH; depth++) {
        int idx = find_entry_index(entries, count, tile_id);
        if (idx < 0) break;
        const pk_pmtiles_dir_entry_t *e = &entries[idx];

        if (e->run_length > 0) {
            out->offset = e->offset;
            out->length = e->length;
            found = true;
            break;
        }

        /* run_length==0：下钻叶目录。offset/length 是相对 leaf_dirs_offset 的。 */
        pk_pmtiles_dir_entry_t *next_entries = NULL;
        size_t next_count = 0;
        bool ok = load_directory(pm, pm->header.leaf_dirs_offset + e->offset, e->length,
                                  &next_entries, &next_count);
        free(leaf_owned);
        leaf_owned = NULL;
        if (!ok) break;

        entries    = next_entries;
        count      = next_count;
        leaf_owned = next_entries;
    }

    free(leaf_owned);
    return found;
}
