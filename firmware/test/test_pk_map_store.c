/* test_pk_map_store.c — host proof for pk_map_store（包清单扫描 + (z,x,y)
 * 路由 + overzoom 回退 + 拔卡/rescan）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -o /tmp/test_ms \
 *      firmware/test/test_pk_map_store.c && /tmp/test_ms
 *
 * 两段：
 *   1) pk_map_route_find 纯路由——合成的 pk_map_pack_meta_t，不碰磁盘，覆盖
 *      设计文档「取瓦片…按 zoom 与 bounds 路由」那句话本身：多包重叠选
 *      maxzoom 最深者、overzoom 回退、无覆盖、min_zoom_floor 边界。
 *   2) pk_map_store_scan/get_tile 真实 I/O——用 SD 卡上的两个真实样本包
 *      （pk_map_global.pmtiles 全球 z0-9、pk_map_prd_pilot.pmtiles 珠三角
 *      z0-12）+ 一个手造的坏包，验证坏包跳过、真实路由选包、拔卡/rescan。
 *      样本包缺失时优雅 skip（不算失败），跟 test_pk_pmtiles.c 一致。
 *
 * 样本包是几十 MB~GB 级地图数据，不进 git。默认到仓库内 tmp/sd-maps/ 下找
 * （tmp/ 已在 .gitignore 里），可用环境变量 PK_MAP_TEST_DATA_DIR 指到别处。
 * 默认值是相对路径，所以上面那行 cc 命令要在仓库根目录下跑。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* 同 test_pk_pmtiles.c 的翻译单元惯例：把依赖的 .c 一起拉进来。 */
#include "../main/third_party/pk_tinfl.c"
#include "../main/pk_pmtiles.c"
#include "../main/pk_map_store.c"

/* 模拟 SD 卡 maps/ 目录的临时装配目录：两个样本包的软链 + 一个手造坏包。
   自己建：mkdir -p /tmp/pk_map_test_dir && ln -sf <样本目录>/pk_map_*.pmtiles
   到该目录，再 printf 一串垃圾字节到 pk_map_broken.pmtiles。 */
#ifndef PK_MAP_TEST_DIR
#define PK_MAP_TEST_DIR "/tmp/pk_map_test_dir"
#endif

/* 样本包所在目录：默认仓库内 tmp/sd-maps/（相对仓库根目录），
   PK_MAP_TEST_DATA_DIR 可覆盖。 */
#ifndef PK_MAP_TEST_DATA_DIR_DEFAULT
#define PK_MAP_TEST_DATA_DIR_DEFAULT "tmp/sd-maps"
#endif

/* 缺样本包时统一打这段，告诉别人怎么把它弄回来（唯一的 %s 是当前样本目录）。*/
#define PK_MAP_TEST_HOWTO \
    "      样本包不进 git（几十 MB~GB 级地图数据，tmp/ 已在 .gitignore 里）。\n" \
    "      获取：按项目的 SD 离线地图出包链路（tileserver-gl 渲染 → MBTiles →\n" \
    "      pmtiles convert）自己出包，或从已刷好的 SD 卡 maps/ 目录拷贝，放到\n" \
    "      %s/ 下；也可用 PK_MAP_TEST_DATA_DIR 环境变量指向别处。\n"

static char g_sample_global[512];
static char g_sample_prd[512];

static const char *test_data_dir(void)
{
    const char *dir = getenv("PK_MAP_TEST_DATA_DIR");
    return (dir && dir[0]) ? dir : PK_MAP_TEST_DATA_DIR_DEFAULT;
}

static void init_sample_paths(void)
{
    const char *dir = test_data_dir();
    snprintf(g_sample_global, sizeof(g_sample_global), "%s/pk_map_global.pmtiles", dir);
    snprintf(g_sample_prd, sizeof(g_sample_prd), "%s/pk_map_prd_pilot.pmtiles", dir);
}

static int g_fail = 0;
static int g_skip = 0;

static void chk_true(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

static void chk_u32(const char *what, uint32_t got, uint32_t want)
{
    bool ok = got == want;
    printf("  [%s] %-40s got=%u want=%u\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) g_fail++;
}

static void chk_size(const char *what, size_t got, size_t want)
{
    bool ok = got == want;
    printf("  [%s] %-40s got=%zu want=%zu\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) g_fail++;
}

/* ==================================================================== *
 * 段 1：纯路由逻辑（合成 meta，无 I/O）
 * ==================================================================== */

static pk_map_pack_meta_t mk_meta(double min_lon, double min_lat, double max_lon, double max_lat,
                                   uint8_t min_zoom, uint8_t max_zoom)
{
    pk_map_pack_meta_t m = {
        .valid = true, .min_lon = min_lon, .min_lat = min_lat,
        .max_lon = max_lon, .max_lat = max_lat,
        .min_zoom = min_zoom, .max_zoom = max_zoom,
    };
    return m;
}

/* 全球包：z0-9，覆盖整个世界。 */
static pk_map_pack_meta_t global_meta(void)
{
    return mk_meta(-180.0, -85.0511287, 180.0, 85.0511287, 0, 9);
}

/* 珠三角包：z0-12，bounds 取自设计文档实测数字表 112.5,21.5,114.6,23.5。 */
static pk_map_pack_meta_t prd_meta(void)
{
    return mk_meta(112.5, 21.5, 114.6, 23.5, 0, 12);
}

static void test_route_multi_pack_overlap(void)
{
    printf("-- pk_map_route_find: 多包重叠选 maxzoom 最深者 --\n");
    pk_map_pack_meta_t packs[2] = { global_meta(), prd_meta() };

    /* z12 (3339,1787) 落在珠三角包内，全球包 maxzoom=9 覆盖不到 z12 —— 只有
     * 珠三角包是候选。 */
    pk_map_route_result_t r = pk_map_route_find(packs, 2, 12, 3339, 1787, 0);
    chk_true("z12 PRD 坐标: found", r.found);
    chk_size("z12 PRD 坐标: 选中珠三角包(index1)", r.pack_index, 1);
    chk_u32("z12 PRD 坐标: actual_z 不降级", r.actual_z, 12);
    chk_u32("z12 PRD 坐标: scale=1(精确命中)", r.scale, 1);

    /* z5 (26,13) 同样落在珠三角范围内：两个包都覆盖 z5（全球 minzoom0<=5<=9，
     * 珠三角 minzoom0<=5<=12），应选 maxzoom 更深的珠三角包。 */
    r = pk_map_route_find(packs, 2, 5, 26, 13, 0);
    chk_true("z5 PRD 坐标: found", r.found);
    chk_size("z5 PRD 坐标: 两包重叠时选 maxzoom 深的珠三角包(index1)", r.pack_index, 1);
    chk_u32("z5 PRD 坐标: scale=1", r.scale, 1);

    /* z5 (16,10) 是德国（不在珠三角 bounds 内），只有全球包覆盖。 */
    r = pk_map_route_find(packs, 2, 5, 16, 10, 0);
    chk_true("z5 德国坐标: found", r.found);
    chk_size("z5 德国坐标: 只有全球包覆盖(index0)", r.pack_index, 0);
}

static void test_route_overzoom_fallback(void)
{
    printf("-- pk_map_route_find: overzoom 回退 --\n");
    pk_map_pack_meta_t packs[1] = { prd_meta() }; /* 只有珠三角包，z0-12 */

    /* z15 超出 maxzoom=12：z12 (3339,1787) 的某个 z15 子瓦片，期待回退到
     * z12、scale=2^(15-12)=8，actual_x/y = 原始 x/y >> 3。 */
    uint32_t x15 = (3339u << 3) | 3; /* 子瓦片编号任取，right-shift 后应还原 */
    uint32_t y15 = (1787u << 3) | 5;
    pk_map_route_result_t r = pk_map_route_find(packs, 1, 15, x15, y15, 0);
    chk_true("z15 overzoom: found", r.found);
    chk_u32("z15 overzoom: actual_z 回退到 12", r.actual_z, 12);
    chk_u32("z15 overzoom: scale=8", r.scale, 8);
    chk_u32("z15 overzoom: actual_x = x15>>3", r.actual_x, 3339);
    chk_u32("z15 overzoom: actual_y = y15>>3", r.actual_y, 1787);
}

static void test_route_no_coverage(void)
{
    printf("-- pk_map_route_find: 无包覆盖 --\n");

    /* 空包列表 */
    pk_map_route_result_t r = pk_map_route_find(NULL, 0, 10, 100, 100, 0);
    chk_true("空包列表: not found", !r.found);

    /* 单一小范围包，查询点在 bounds 之外，且 min_zoom_floor 挡住了"退到 z0
     * 世界一定命中"这条路（z0 瓦片包围盒是整个世界，任何 bounds 都会
     * 相交——所以必须把 floor 设在 1 以上才能真正测出"落在包外"这件事）。 */
    pk_map_pack_meta_t packs[1] = { prd_meta() };
    r = pk_map_route_find(packs, 1, 10, 0, 0, /*min_zoom_floor=*/1);
    chk_true("floor=1 时查询点在 bounds 外: not found", !r.found);

    /* 同样的查询不设 floor（默认 0）会一路 overzoom 到 z0 命中——用来确认
     * "无覆盖"是 floor 约束下的结果，不是查询点真的无论如何都够不到任何包。 */
    r = pk_map_route_find(packs, 1, 10, 0, 0, 0);
    chk_true("floor=0 时同一查询会 overzoom 到 z0 命中(佐证上一条不是巧合)", r.found);
    if (r.found) chk_u32("floor=0: 落到 z0", r.actual_z, 0);
}

static void test_route_min_zoom_floor(void)
{
    printf("-- pk_map_route_find: min_zoom_floor 边界 --\n");
    /* 包只在 z5 这一层有效（min_zoom=max_zoom=5），世界范围 bounds。 */
    pk_map_pack_meta_t packs[1] = { mk_meta(-180, -85, 180, 85, 5, 5) };

    pk_map_route_result_t r = pk_map_route_find(packs, 1, 10, 500, 500, /*floor=*/6);
    chk_true("floor=6 挡在 z5 之上: not found（永远到不了 z5）", !r.found);

    r = pk_map_route_find(packs, 1, 10, 500, 500, /*floor=*/0);
    chk_true("floor=0 能一路退到 z5: found", r.found);
    if (r.found) chk_u32("floor=0: 命中在 z5", r.actual_z, 5);
}

/* ==================================================================== *
 * 段 2：真实 I/O（scan/get_tile/invalidate），样本目录缺失则优雅 skip
 * ==================================================================== */

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* 样本包/装配目录缺失 → 整段跳过：既不算失败，也不静默通过
   （末尾会汇总 skipped 数）。 */
static void skip_missing(const char *what, const char *path, bool print_howto)
{
    g_skip++;
    fprintf(stderr, "SKIP: %s 不存在——跳过%s，不计入失败\n", path, what);
    if (print_howto) fprintf(stderr, PK_MAP_TEST_HOWTO, test_data_dir());
}

/* 段 2/3 共用的前置检查：两个样本包 + 装配目录都在才跑。 */
static bool real_io_prereq_ok(const char *what)
{
    if (!file_exists(g_sample_global)) { skip_missing(what, g_sample_global, true); return false; }
    if (!file_exists(g_sample_prd))    { skip_missing(what, g_sample_prd, true); return false; }
    if (!file_exists(PK_MAP_TEST_DIR)) {
        skip_missing(what, PK_MAP_TEST_DIR, false);
        fprintf(stderr, "      该目录应含两个样本包的软链 + 一个手造坏包，建法见文件顶部注释。\n");
        return false;
    }
    return true;
}

static void test_real_store(void)
{
    printf("-- pk_map_store_scan/get_tile: 真实样本包 (%s) --\n", PK_MAP_TEST_DIR);

    if (!real_io_prereq_ok("段 2（scan/get_tile 真实 I/O）")) return;

    pk_map_store_t store;
    memset(&store, 0, sizeof(store));

    size_t n = pk_map_store_scan(&store, PK_MAP_TEST_DIR);
    chk_size("scan: 坏包被跳过，只装载 2 个有效包", n, 2);
    chk_size("scan: store->count 同步", store.count, 2);

    /* 找出哪个下标是 global / prd_pilot（scan 顺序取决于文件系统 readdir
     * 顺序，不应该硬编码下标）。 */
    int idx_global = -1, idx_prd = -1;
    for (size_t i = 0; i < store.count; i++) {
        if (strstr(store.packs[i].path, "pk_map_global.pmtiles")) idx_global = (int)i;
        if (strstr(store.packs[i].path, "pk_map_prd_pilot.pmtiles")) idx_prd = (int)i;
    }
    chk_true("scan: 找到 global 包", idx_global >= 0);
    chk_true("scan: 找到 prd_pilot 包", idx_prd >= 0);

    printf("-- get_tile: z12 珠三角坐标——只有 prd_pilot 覆盖 --\n");
    pk_map_pack_t *pack = NULL;
    pk_pmtiles_tile_loc_t loc;
    pk_map_route_result_t route;
    bool ok = pk_map_store_get_tile(&store, 12, 3339, 1787, &pack, &loc, &route);
    chk_true("z12 PRD: 命中", ok);
    if (ok) {
        chk_true("z12 PRD: 命中的是 prd_pilot 包", strstr(pack->path, "pk_map_prd_pilot.pmtiles") != NULL);
        chk_u32("z12 PRD: actual_z=12(精确)", route.actual_z, 12);
        chk_u32("z12 PRD: scale=1", route.scale, 1);
        /* 与 test_pk_pmtiles.c 里独立验证过的已知值交叉核对。 */
        chk_u32("z12 PRD: loc.length 与 pk_pmtiles 单测交叉核对", loc.length, 39736);
    }

    printf("-- get_tile: z5 珠三角坐标——两包重叠，选 prd_pilot（maxzoom 更深） --\n");
    ok = pk_map_store_get_tile(&store, 5, 26, 13, &pack, &loc, &route);
    chk_true("z5 PRD: 命中", ok);
    if (ok) {
        chk_true("z5 PRD: 命中的是 prd_pilot 包(maxzoom 12>9 优先)",
                 strstr(pack->path, "pk_map_prd_pilot.pmtiles") != NULL);
    }

    printf("-- get_tile: z5 德国坐标——只有 global 覆盖 --\n");
    ok = pk_map_store_get_tile(&store, 5, 16, 10, &pack, &loc, &route);
    chk_true("z5 德国: 命中", ok);
    if (ok) {
        chk_true("z5 德国: 命中的是 global 包", strstr(pack->path, "pk_map_global.pmtiles") != NULL);
        chk_u32("z5 德国: loc.length 与 probe 交叉核对", loc.length, 25205);
    }

    printf("-- pk_map_store_invalidate: 拔卡整体失效 --\n");
    pk_map_store_invalidate(&store);
    chk_size("invalidate 后 count=0", store.count, 0);
    ok = pk_map_store_get_tile(&store, 12, 3339, 1787, &pack, &loc, &route);
    chk_true("invalidate 后 get_tile 失败（无包可查）", !ok);

    printf("-- pk_map_store_scan: 插回卡后 rescan 恢复 --\n");
    n = pk_map_store_scan(&store, PK_MAP_TEST_DIR);
    chk_size("rescan: 重新装载 2 个包", n, 2);
    ok = pk_map_store_get_tile(&store, 12, 3339, 1787, &pack, &loc, &route);
    chk_true("rescan 后 get_tile 恢复正常", ok);

    pk_map_store_invalidate(&store);
}

/* ==================================================================== *
 * 段 3：close_files（拔卡预卸载）——热插拔回归修复的核心场景：拔卡瞬间只关
 * 文件句柄，meta/count 原样保留（产品行为「拔卡后已缓存瓦片继续显示」靠
 * 路由的 meta 活着），后续读盘干净失败不崩；重挂时 invalidate+rescan 恢复。
 * ==================================================================== */

static void test_close_files_hot_unplug(void)
{
    printf("-- pk_map_store_close_files: 拔卡前关句柄、保清单 --\n");

    if (!real_io_prereq_ok("段 3（close_files 拔卡预卸载）")) return;

    pk_map_store_t store;
    memset(&store, 0, sizeof(store));
    size_t n = pk_map_store_scan(&store, PK_MAP_TEST_DIR);
    chk_size("close_files 前置: scan 装载 2 个包", n, 2);

    pk_map_store_close_files(&store);
    chk_size("close_files 后 count 保留", store.count, 2);

    /* meta 保留 → 纯路由仍命中（map_page 拔卡后每帧还在跑这条路） */
    pk_map_pack_meta_t meta[PK_MAP_STORE_MAX_PACKS];
    for (size_t i = 0; i < store.count; i++) meta[i] = store.packs[i].meta;
    pk_map_route_result_t r = pk_map_route_find(meta, store.count, 12, 3339, 1787, 0);
    chk_true("close_files 后 meta 保留: 路由仍命中 z12 PRD", r.found);

    /* get_tile 不崩：根目录驻留内存时目录查找可能仍命中并返回 true，但此后
     * 任何 pm.read（读瓦片字节）必须干净返回非 0；目录在叶目录（要读盘）时
     * get_tile 自己就干净失败。两种结局都合法，共同点是不 use-after-free。 */
    pk_map_pack_t *pack = NULL;
    pk_pmtiles_tile_loc_t loc;
    pk_map_route_result_t route;
    bool ok = pk_map_store_get_tile(&store, 12, 3339, 1787, &pack, &loc, &route);
    if (ok) {
        uint8_t b[16];
        chk_true("get_tile 命中(目录驻留)但 pm.read 干净失败",
                 pack->pm.read(pack->pm.read_ctx,
                               pack->pm.header.tile_data_offset + loc.offset,
                               b, sizeof(b)) != 0);
    } else {
        chk_true("get_tile 干净失败(目录在叶、读盘被拒)", true);
    }

    /* 幂等：拔卡回调理论上只跑一次，但 format 失败路径也走 unmount，
     * 连关两次不能出事。 */
    pk_map_store_close_files(&store);
    chk_size("close_files 幂等: count 不变", store.count, 2);

    /* 重挂序列：invalidate 要能消化「文件已被 close_files 关掉」的包 */
    pk_map_store_invalidate(&store);
    chk_size("invalidate 对已关文件的包幂等: count=0", store.count, 0);
    n = pk_map_store_scan(&store, PK_MAP_TEST_DIR);
    chk_size("rescan 恢复 2 个包", n, 2);
    ok = pk_map_store_get_tile(&store, 12, 3339, 1787, &pack, &loc, &route);
    chk_true("rescan 后 get_tile 恢复可读", ok);
    if (ok) chk_u32("rescan 后 loc.length 复核", loc.length, 39736);

    pk_map_store_invalidate(&store);
}

int main(void)
{
    init_sample_paths();
    test_route_multi_pack_overlap();
    test_route_overzoom_fallback();
    test_route_no_coverage();
    test_route_min_zoom_floor();
    test_real_store();
    test_close_files_hot_unplug();
    printf("%s (%d fail, %d skipped)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_skip);
    return g_fail ? 1 : 0;
}
