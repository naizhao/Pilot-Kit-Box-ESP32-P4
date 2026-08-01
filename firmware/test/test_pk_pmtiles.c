/* test_pk_pmtiles.c — host proof for pk_pmtiles（PMTiles v3 只读解析器）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_pmt \
 *      firmware/test/test_pk_pmtiles.c && /tmp/test_pmt
 *
 * 样本包 pk_map_prd_pilot.pmtiles（珠三角 z0-12，出包记录见
 * docs/superpowers/specs/2026-08-01-sd-offline-map-design.md）：
 *   852 addressed tiles / 740 root entries / 698 contents，
 *   bounds 112.5,21.5,114.6,23.5，internal_compression=gzip(2)，
 *   tile_compression=none(1)，tile_type=png(2)，root_len=2071B，无叶目录
 *   （leaf_len=0，样本包小，740 条一层目录装得下）。
 *
 * 除 hilbert 最简单的 z0/z1 手算向量外，其余 tileid/(z,x,y) 对照值和
 * runlength 命中用例都是用 /tmp/pmtiles_probe.py（本地一次性脚本，
 * Wikipedia xy2d 公式 + Python zlib 直接解出该包根目录）反推、并现场对着
 * 这个样本包文件的目录条目、瓦片字节验证过的——不是抄来的"spec 测试向
 * 量"，是"跑通了这个具体文件"这件事本身的证据。 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* pk_tinfl.c 直接把实现拉进本 TU（同 geo.c/traffic_geom.c 的惯例：test 文件
   负责把多个 .c 拼成一个翻译单元）。third_party/ 前缀相对 -I firmware/main。 */
#include "../main/third_party/pk_tinfl.c"
#include "../main/pk_pmtiles.c"

#ifndef PK_PMTILES_TEST_SAMPLE
#define PK_PMTILES_TEST_SAMPLE "/Users/samwu/hosts/Pilot-Kit-Box-ESP32-P4/tmp/sd-maps/pk_map_prd_pilot.pmtiles"
#endif

static int g_fail = 0;

static void chk_true(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static void chk_u64(const char *what, uint64_t got, uint64_t want)
{
    bool ok = got == want;
    printf("  [%s] %-28s got=%llu want=%llu\n", ok ? "PASS" : "FAIL", what,
           (unsigned long long)got, (unsigned long long)want);
    if (!ok) g_fail++;
}

static void chk_i32(const char *what, int32_t got, int32_t want)
{
    bool ok = got == want;
    printf("  [%s] %-28s got=%d want=%d\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) g_fail++;
}

/* ---------------------------------------------------------- Hilbert 向量 */

static void test_hilbert_vectors(void)
{
    printf("-- zxy_to_tileid: Hilbert 曲线向量 --\n");
    /* z0/z1 手算：唯一的 z0 瓦片必是 0；z1 四个子瓦片按 Hilbert 顺序应为
       (0,0)=1 (0,1)=2 (1,1)=3 (1,0)=4 —— 标准 Hilbert 曲线 U 形第一段。 */
    chk_u64("z0 (0,0)", pk_pmtiles_zxy_to_tileid(0, 0, 0), 0);
    chk_u64("z1 (0,0)", pk_pmtiles_zxy_to_tileid(1, 0, 0), 1);
    chk_u64("z1 (0,1)", pk_pmtiles_zxy_to_tileid(1, 0, 1), 2);
    chk_u64("z1 (1,1)", pk_pmtiles_zxy_to_tileid(1, 1, 1), 3);
    chk_u64("z1 (1,0)", pk_pmtiles_zxy_to_tileid(1, 1, 0), 4);

    /* z2..z12 逐级对照 pk_map_prd_pilot.pmtiles 珠三角包里真实存在的瓦片
       （用 /tmp/pmtiles_probe.py 独立实现的 xy2d 算出 tileid，再从包的根
       目录里查到同一 tileid、且瓦片字节是合法 PNG，双重验证）。 */
    static const struct { uint8_t z; uint32_t x, y; uint64_t tileid; } v[] = {
        {2,  3,    1,    17},
        {3,  6,    3,    72},
        {4,  13,   6,    290},
        {5,  26,   13,   1164},
        {6,  52,   27,   4660},
        {7,  104,  55,   18644},
        {8,  208,  111,  74580},
        {9,  417,  223,  298323},
        {10, 834,  446,  1193293},
        {11, 1669, 893,  4773175},
        {12, 3339, 1787, 19092703},
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        char label[32];
        snprintf(label, sizeof(label), "z%u (%u,%u)", v[i].z, v[i].x, v[i].y);
        chk_u64(label, pk_pmtiles_zxy_to_tileid(v[i].z, v[i].x, v[i].y), v[i].tileid);
    }
}

/* -------------------------------------------------------- runlength 目录 */

/* 手搓一个不经过 gzip、internal_compression=none 的最小目录 blob，专门测
   parse_directory 的 runlength 语义（不依赖真实样本包里恰好有没有长 run，
   独立验证这条路径）：3 条 entry，
     tile_id=10 run_length=5 length=100 offset=0
     tile_id=20 run_length=1 length=50  offset=100   (offset 编码=101，非0)
     tile_id=21 run_length=1 length=50  offset=150   (offset 编码=0 → 紧接上条)
   查 tile_id=12（落在第一条 run 中间）、20、21、9（run 前一格，应 miss）、
   15（第一条 run 之后、第二条之前的空洞，应 miss）。 */
static void put_varint(uint8_t *buf, size_t *pos, uint64_t v)
{
    while (v >= 0x80) {
        buf[(*pos)++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    buf[(*pos)++] = (uint8_t)v;
}

static void test_runlength_and_binary_search(void)
{
    printf("-- 目录 runlength / 二分查找（手搓 blob，non-gzip） --\n");
    uint8_t blob[64];
    size_t pos = 0;
    put_varint(blob, &pos, 3); /* entry_count */
    /* tile_id 增量: 10, 10(→20), 1(→21) */
    put_varint(blob, &pos, 10);
    put_varint(blob, &pos, 10);
    put_varint(blob, &pos, 1);
    /* run_length */
    put_varint(blob, &pos, 5);
    put_varint(blob, &pos, 1);
    put_varint(blob, &pos, 1);
    /* length */
    put_varint(blob, &pos, 100);
    put_varint(blob, &pos, 50);
    put_varint(blob, &pos, 50);
    /* offset 编码 */
    put_varint(blob, &pos, 1);   /* 0+1 = 第一条恒非 0 */
    put_varint(blob, &pos, 101); /* 100+1 */
    put_varint(blob, &pos, 0);   /* 紧接上条: 100+50=150 */

    pk_pmtiles_dir_entry_t *entries = NULL;
    size_t count = 0;
    bool ok = parse_directory(blob, pos, &entries, &count);
    chk_true("parse_directory ok", ok);
    chk_u64("entry count", count, 3);
    if (ok && count == 3) {
        chk_u64("entry[0].tile_id", entries[0].tile_id, 10);
        chk_u64("entry[0].run_length", entries[0].run_length, 5);
        chk_u64("entry[0].offset", entries[0].offset, 0);
        chk_u64("entry[1].tile_id", entries[1].tile_id, 20);
        chk_u64("entry[1].offset", entries[1].offset, 100);
        chk_u64("entry[2].tile_id", entries[2].tile_id, 21);
        chk_u64("entry[2].offset (延续上条)", entries[2].offset, 150);

        int idx = find_entry_index(entries, count, 12);
        chk_true("tile_id=12 命中 run 中段(entry0)", idx == 0);
        idx = find_entry_index(entries, count, 14);
        chk_true("tile_id=14 命中 run 末尾(entry0, 10+5=15不含)", idx == 0);
        idx = find_entry_index(entries, count, 20);
        chk_true("tile_id=20 精确命中 entry1", idx == 1);
        idx = find_entry_index(entries, count, 21);
        chk_true("tile_id=21 精确命中 entry2", idx == 2);
        idx = find_entry_index(entries, count, 9);
        chk_true("tile_id=9 (run 之前) miss", idx == -1);
        idx = find_entry_index(entries, count, 15);
        chk_true("tile_id=15 (run0 结束, run1 之前的空洞) miss", idx == -1);
        idx = find_entry_index(entries, count, 22);
        chk_true("tile_id=22 (超出最后一条) miss", idx == -1);
    }
    free(entries);
}

/* ------------------------------------------------------- 损坏数据防御 */

/* 内存读回调：喂固定字节数组，越界读一律失败——用来构造损坏/截断输入，
   验证 pk_pmtiles_open 在魔数错误、header 截断、目录截断时干净返回 false
   而不是崩溃（对应设计文档"PNG 解码失败/缺瓦片"这类错误态背后的同一条
   原则：坏数据只能导致功能降级，不能导致进程挂掉）。 */
typedef struct { const uint8_t *buf; size_t len; } mem_ctx_t;
static int mem_read(void *ctx, uint64_t off, void *out, size_t len)
{
    mem_ctx_t *m = (mem_ctx_t *)ctx;
    if (off + len > m->len) return -1;
    memcpy(out, m->buf + off, len);
    return 0;
}

static void test_corrupt_input_no_crash(void)
{
    printf("-- 损坏输入防御（不崩、干净返回 false） --\n");
    pk_pmtiles_t pm;

    /* 魔数错误 */
    uint8_t bad_magic[PK_PMTILES_HEADER_LEN] = {0};
    memcpy(bad_magic, "NOTPMTIL", 7);
    mem_ctx_t ctx1 = {bad_magic, sizeof(bad_magic)};
    chk_true("bad magic → open 失败", !pk_pmtiles_open(&pm, mem_read, &ctx1));

    /* header 截断（只给 10 字节，读 127 字节的 header 会越界） */
    uint8_t truncated[10] = "PMTiles";
    mem_ctx_t ctx2 = {truncated, sizeof(truncated)};
    chk_true("header 截断 → open 失败", !pk_pmtiles_open(&pm, mem_read, &ctx2));

    /* 版本号不是 3 */
    uint8_t bad_version[PK_PMTILES_HEADER_LEN] = {0};
    memcpy(bad_version, "PMTiles", 7);
    bad_version[7] = 99;
    mem_ctx_t ctx3 = {bad_version, sizeof(bad_version)};
    chk_true("version=99 → open 失败", !pk_pmtiles_open(&pm, mem_read, &ctx3));

    /* header 合法但 root 目录段声明的长度比实际提供的数据还长（截断的目录） */
    uint8_t trunc_dir[PK_PMTILES_HEADER_LEN] = {0};
    memcpy(trunc_dir, "PMTiles", 7);
    trunc_dir[7] = 3; /* version */
    /* root_dir_offset=127, root_dir_length=1000（但整个缓冲只有 127 字节） */
    trunc_dir[8] = 127;
    trunc_dir[16] = 0xe8; trunc_dir[17] = 0x03; /* 1000 小端 */
    trunc_dir[97] = PK_PMTILES_COMPRESSION_NONE;
    mem_ctx_t ctx4 = {trunc_dir, sizeof(trunc_dir)};
    chk_true("root 目录声明长度超出缓冲 → open 失败", !pk_pmtiles_open(&pm, mem_read, &ctx4));
}

/* ----------------------------------------------------- 真实样本包测试 */

static void test_real_sample(void)
{
    printf("-- 真实样本包 %s --\n", PK_PMTILES_TEST_SAMPLE);
    pk_pmtiles_t pm;
    bool opened = pk_pmtiles_open_file(&pm, PK_PMTILES_TEST_SAMPLE);
    chk_true("open_file", opened);
    if (!opened) {
        printf("  样本文件缺失，后续断言全部跳过——检查路径是否还在\n");
        g_fail++;
        return;
    }

    printf("-- header 字段 --\n");
    chk_u64("version", pm.header.version, 3);
    chk_u64("addressed_tiles_count", pm.header.addressed_tiles_count, 852);
    chk_u64("tile_entries_count", pm.header.tile_entries_count, 740);
    chk_u64("tile_contents_count", pm.header.tile_contents_count, 698);
    chk_u64("min_zoom", pm.header.min_zoom, 0);
    chk_u64("max_zoom", pm.header.max_zoom, 12);
    chk_u64("internal_compression=GZIP", pm.header.internal_compression, PK_PMTILES_COMPRESSION_GZIP);
    chk_u64("tile_compression=NONE", pm.header.tile_compression, PK_PMTILES_COMPRESSION_NONE);
    chk_u64("tile_type=PNG", pm.header.tile_type, PK_PMTILES_TYPE_PNG);
    chk_true("clustered", pm.header.clustered);
    /* bounds 112.5,21.5,114.6,23.5 (设计文档实测数字表)，e7 定点 */
    chk_i32("min_lon_e7", pm.header.min_lon_e7, 1125000000);
    chk_i32("min_lat_e7", pm.header.min_lat_e7, 215000000);
    chk_i32("max_lon_e7", pm.header.max_lon_e7, 1146000000);
    chk_i32("max_lat_e7", pm.header.max_lat_e7, 235000000);
    chk_u64("root_count (缓存的根目录条目数)", pm.root_count, 740);

    printf("-- 已知存在瓦片 (z12 珠三角范围内) --\n");
    pk_pmtiles_tile_loc_t loc;
    bool hit = pk_pmtiles_find_tile(&pm, 12, 3339, 1787, &loc);
    chk_true("z12 (3339,1787) 命中", hit);
    if (hit) {
        chk_u64("loc.length", loc.length, 39736);
        chk_u64("loc.offset", loc.offset, 11793610);
        uint8_t magic[8];
        bool rd = posix_file_read(pm.owned_file, pm.header.tile_data_offset + loc.offset, magic, sizeof(magic)) == 0;
        chk_true("瓦片数据可读", rd);
        static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        chk_true("瓦片首 8 字节是 PNG magic", rd && memcmp(magic, png_magic, 8) == 0);
    }

    printf("-- runlength entry (root 目录最后一条, run_length=20) --\n");
    /* z12 (3346,1792) 是 run 起点 tile_id=19093761；(3348,1793) 是 run 内
       第 7 个 tile_id=19093768——两者应解析到同一段 (offset,length)。 */
    pk_pmtiles_tile_loc_t loc_a, loc_b;
    bool hit_a = pk_pmtiles_find_tile(&pm, 12, 3346, 1792, &loc_a);
    bool hit_b = pk_pmtiles_find_tile(&pm, 12, 3348, 1793, &loc_b);
    chk_true("run 起点命中", hit_a);
    chk_true("run 中段命中", hit_b);
    if (hit_a && hit_b) {
        chk_u64("run 起点 offset", loc_a.offset, 1338265);
        chk_u64("run 起点 length", loc_a.length, 1833);
        chk_true("run 中段与起点解析到同一段瓦片(去重复用)",
                 loc_a.offset == loc_b.offset && loc_a.length == loc_b.length);
    }

    printf("-- 包外区域 (北京，明显在珠三角包覆盖范围之外) --\n");
    pk_pmtiles_tile_loc_t miss;
    chk_true("z12 (3372,1552) 未命中", !pk_pmtiles_find_tile(&pm, 12, 3372, 1552, &miss));
    chk_true("z8 (210,97) 未命中", !pk_pmtiles_find_tile(&pm, 8, 210, 97, &miss));

    pk_pmtiles_close(&pm);
    chk_true("close 后 root_entries 清零", pm.root_entries == NULL && pm.root_count == 0);
}

/* ------------------------------------------------- 真实叶目录下潜测试 */

/* pk_map_global.pmtiles（全球 z0-9，557MB）不进 git（tmp/ 已在 .gitignore
   里），只存在于罩哥本机 /Users/samwu/hosts/.../tmp/sd-maps/。它跟珠三角
   样本包（叶目录为空，740 条根目录一层装完）不同——header 里
   leaf_dirs_length=704863（>0），root 目录 58 条全部是 run_length==0 的叶
   目录指针，实测任何一次 find_tile 都会真的走一次 pk_pmtiles.c 里
   load_directory() 的下潜分支。这段专门补这条此前只在手搓 blob 里测过的
   路径，用真实数据校验。
   下面这些 (z,x,y)/(offset,length) 数值不是编的：用 /tmp/pmtiles_probe_
   global.py（本地一次性脚本，独立 Python 实现 + zlib 直接解这个包的根/
   叶目录）跑出来、并现场对着瓦片字节验证过的：
     - z9 (419,222) 落在珠三角（跟另一个样本包同一经纬点位）：解出合法 PNG；
     - z9 (29,239)/(29,230)/(29,227) 三个不相邻的太平洋海面瓦片，在同一张
       叶目录里 tile_id 152163/152220/152243 全部指向同一个
       offset=286082008 length=1862——这是 PMTiles 构建期内容去重
       （identical content dedup），不是相邻 tile_id 的 runlength 合并，
       是更强的一种复用场景；
     - z10（超出这个包 max_zoom=9）任意坐标必然查不到。 */
#ifndef PK_PMTILES_TEST_SAMPLE_GLOBAL
#define PK_PMTILES_TEST_SAMPLE_GLOBAL "/Users/samwu/hosts/Pilot-Kit-Box-ESP32-P4/tmp/sd-maps/pk_map_global.pmtiles"
#endif

static void test_global_leaf_directory(void)
{
    printf("-- 全球包叶目录下潜 (%s) --\n", PK_PMTILES_TEST_SAMPLE_GLOBAL);
    pk_pmtiles_t pm;
    if (!pk_pmtiles_open_file(&pm, PK_PMTILES_TEST_SAMPLE_GLOBAL)) {
        fprintf(stderr,
                "SKIP: %s 不存在（该文件不进 git，只在开发机 tmp/sd-maps/ 下）——"
                "跳过叶目录下潜测试，不计入失败\n",
                PK_PMTILES_TEST_SAMPLE_GLOBAL);
        return;
    }

    chk_true("leaf_dirs_length > 0（确有叶目录，不是单层根目录）", pm.header.leaf_dirs_length > 0);
    chk_u64("min_zoom", pm.header.min_zoom, 0);
    chk_u64("max_zoom", pm.header.max_zoom, 9);
    chk_u64("root_count（58 条全是叶目录指针）", pm.root_count, 58);

    printf("-- 已知陆地瓦片 (z9 419,222 珠三角) 经叶目录解出合法 PNG --\n");
    pk_pmtiles_tile_loc_t land;
    bool land_hit = pk_pmtiles_find_tile(&pm, 9, 419, 222, &land);
    chk_true("z9 (419,222) 命中", land_hit);
    if (land_hit) {
        chk_u64("land offset", land.offset, 463413882);
        chk_u64("land length", land.length, 14253);
        uint8_t magic[8];
        bool rd = posix_file_read(pm.owned_file, pm.header.tile_data_offset + land.offset, magic, sizeof(magic)) == 0;
        static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        chk_true("陆地瓦片首 8 字节是 PNG magic", rd && memcmp(magic, png_magic, 8) == 0);
    }

    printf("-- 海洋瓦片内容去重: 3 个不相邻坐标共享同一段瓦片数据 --\n");
    pk_pmtiles_tile_loc_t sea_a, sea_b, sea_c;
    bool hit_a = pk_pmtiles_find_tile(&pm, 9, 29, 239, &sea_a);
    bool hit_b = pk_pmtiles_find_tile(&pm, 9, 29, 230, &sea_b);
    bool hit_c = pk_pmtiles_find_tile(&pm, 9, 29, 227, &sea_c);
    chk_true("z9 (29,239) 命中", hit_a);
    chk_true("z9 (29,230) 命中", hit_b);
    chk_true("z9 (29,227) 命中", hit_c);
    if (hit_a && hit_b && hit_c) {
        chk_u64("海洋瓦片 offset", sea_a.offset, 286082008);
        chk_u64("海洋瓦片 length", sea_a.length, 1862);
        chk_true("(29,239)/(29,230)/(29,227) 三点共享同一 offset+length（内容去重，非 runlength）",
                 sea_a.offset == sea_b.offset && sea_a.length == sea_b.length &&
                 sea_b.offset == sea_c.offset && sea_b.length == sea_c.length);
        uint8_t magic[8];
        bool rd = posix_file_read(pm.owned_file, pm.header.tile_data_offset + sea_a.offset, magic, sizeof(magic)) == 0;
        static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        chk_true("海洋瓦片首 8 字节是 PNG magic", rd && memcmp(magic, png_magic, 8) == 0);
    }

    printf("-- 越界未命中 (z10, 超出该包 max_zoom=9) --\n");
    pk_pmtiles_tile_loc_t miss;
    chk_true("z10 (800,400) 未命中", !pk_pmtiles_find_tile(&pm, 10, 800, 400, &miss));

    pk_pmtiles_close(&pm);
}

int main(void)
{
    test_hilbert_vectors();
    test_runlength_and_binary_search();
    test_corrupt_input_no_crash();
    test_real_sample();
    test_global_leaf_directory();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
