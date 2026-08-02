/* test_pk_aero_layer.c — host proof for pk_aero_layer 的纯函数区
 * （LOD 表 / Web Mercator 投影 / 标签避让）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -DPK_AERO_LAYER_HOST_TEST \
 *      -o /tmp/test_al firmware/test/test_pk_aero_layer.c -lm && /tmp/test_al
 *
 * 同 test_traffic_geom.c 的翻译单元惯例：把被测 .c 直接拉进来。
 * PK_AERO_LAYER_HOST_TEST 切掉后台任务与渲染两段（它们要 FreeRTOS/IDF），
 * 只留纯计算——纯的部分本来就是照这个边界切的。
 *
 * 四段：
 *   1) LOD 表与三个准入判定——覆盖设计文档 §2.2 那张表本身：低 zoom 只留
 *      大场、直升机坪的门槛、管制机场对跑道长度的豁免、VOR-only 档。
 *   2) Web Mercator——**与 map_page.c:110 逐值对拍**（本地复刻一份参考实现，
 *      两处数学一致是这一层不整体偏移的前提），外加 ±85.0511 钳位。
 *   3) 标签避让——零重叠取 bottom、bottom 被占则退 right、四向全撞时的
 *      最小重叠与 >50% 隐藏。
 *   4) 机场名截断——音译上线后名字平均 29 字节、最长 85，硬切会在屏上留下
 *      半截词；这一段钉住"词边界 + ... "的每条规则与两档显示预算。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/pk_aero_layer.c"

static int g_fail;

static void chk_true(const char *what, bool got)
{
    if (!got) { printf("FAIL %s: 期望 true\n", what); g_fail++; }
}

static void chk_false(const char *what, bool got)
{
    if (got) { printf("FAIL %s: 期望 false\n", what); g_fail++; }
}

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
}

static void chk_dbl(const char *what, double got, double want, double eps)
{
    if (fabs(got - want) > eps) {
        printf("FAIL %s: got %.9f want %.9f\n", what, got, want);
        g_fail++;
    }
}

/* ── 1. LOD ──────────────────────────────────────────────────────── */
static void test_lod_table(void)
{
    pk_aero_lod_t z4 = pk_aero_lod_for_zoom(4);
    chk_int("z4 无导航台", z4.navaid_limit, 0);
    chk_int("z4 无 FIX", z4.fix_limit, 0);
    chk_int("z4 无标签", z4.label, 0);
    chk_true("z4 要 ICAO", z4.airport_need_icao);

    pk_aero_lod_t z7 = pk_aero_lod_for_zoom(7);
    chk_true("z7 只 VOR 系", z7.navaid_vor_only);
    chk_int("z7 仅代码标签", z7.label, 1);
    chk_int("z7 仍不画 FIX", z7.fix_limit, 0);

    pk_aero_lod_t z10 = pk_aero_lod_for_zoom(10);
    chk_int("z10 代码+名称", z10.label, 2);
    chk_true("z10 只航路 FIX", z10.fix_enroute_only);
    chk_false("z10 不再要 ICAO", z10.airport_need_icao);

    pk_aero_lod_t z12 = pk_aero_lod_for_zoom(12);
    chk_int("z12 带标高", z12.label, 3);
    chk_false("z12 全部 FIX", z12.fix_enroute_only);

    /* limit 一律不得超过 pk_aero_db_nearest_* 的 max 上限，否则快照永远填不满，
     * 而调用方会以为自己配了个更大的数——这是设计表与 API 的硬冲突点。 */
    for (uint8_t z = 0; z <= 12; z++) {
        pk_aero_lod_t l = pk_aero_lod_for_zoom(z);
        chk_true("limit 收在 32 以内",
                 l.airport_limit <= 32 && l.navaid_limit <= 32 && l.fix_limit <= 32);
    }
}

/* 密度降级：LOD 只看 zoom，同一个 zoom 下珠三角与塔克拉玛干差一个数量级。
 * 钉住"屏上一挤就先丢名称、再丢标高"这条规则本身——它决定了一屏能不能读懂
 * （模拟器最糟情况一屏 24 个要素，不降级时长标签会横穿画面）。 */
static void test_label_mode_density(void)
{
    chk_int("空屏不降级", pk_aero_label_mode(3, 0), 3);
    chk_int("10 个仍带标高", pk_aero_label_mode(3, 10), 3);
    chk_int("11 个丢标高", pk_aero_label_mode(3, 11), 2);
    chk_int("20 个仍留名称", pk_aero_label_mode(2, 20), 2);
    chk_int("21 个只剩代码", pk_aero_label_mode(2, 21), 1);
    chk_int("21 个从标高档也只剩代码", pk_aero_label_mode(3, 21), 1);
    /* 只往下压，绝不往上抬：LOD 说不画就是不画（低 zoom 点状要素会糊成噪点）*/
    chk_int("不画档恒不画", pk_aero_label_mode(0, 0), 0);
    chk_int("不画档再稀也不画", pk_aero_label_mode(0, 100), 0);
    chk_int("仅代码档不会被抬到 2", pk_aero_label_mode(1, 0), 1);
}

/* 罗盘玫瑰的启用判定。它是这一层里最贵、最占地方的东西（直径 52 px、
 * 每个 36 条刻度短线），三条闸门缺一不可，所以逐条钉住边界。 */
static void test_rose_enabled(void)
{
    chk_true("Z11 稀疏时画", pk_aero_rose_enabled(11, 6, 2));
    chk_false("Z10 一律不画", pk_aero_rose_enabled(10, 6, 2));
    chk_true("Z12 也画", pk_aero_rose_enabled(12, 6, 2));
    /* VOR 系上限 4：第 5 个圈的外接盒就把标签顶光了 */
    chk_true("4 个 VOR 仍画", pk_aero_rose_enabled(11, 8, 4));
    chk_false("5 个 VOR 不画", pk_aero_rose_enabled(11, 8, 5));
    /* 总密度上限 16：与 pk_aero_label_mode 的 10/20 是两套阈值，
     * 因为一个玫瑰占的面积约等于 4.7 条标签 */
    chk_true("16 个要素仍画", pk_aero_rose_enabled(11, 16, 1));
    chk_false("17 个要素不画", pk_aero_rose_enabled(11, 17, 1));
    /* 一个 VOR 都没有时判定结果无所谓，但不能崩/不能反 */
    chk_true("没有 VOR 也返回 true（调用方按类型再筛）",
             pk_aero_rose_enabled(12, 0, 0));
}

static void test_lod_airport_pass(void)
{
    pk_aero_lod_t z4 = pk_aero_lod_for_zoom(4);   /* need_icao, min_rwy 8000 */
    chk_true("z4 大场过",  pk_aero_lod_airport_pass(&z4, 1, 1, 11000, true));
    chk_false("z4 无码不过", pk_aero_lod_airport_pass(&z4, 1, 1, 11000, false));
    chk_false("z4 短跑道不过", pk_aero_lod_airport_pass(&z4, 1, 1, 4000, true));
    /* 管制机场豁免跑道长度：管制本身就说明它有量 */
    chk_true("z4 管制豁免长度", pk_aero_lod_airport_pass(&z4, 1, 2, 4000, true));
    /* 直升机坪(2)/水上(3)/超轻(4)/滑翔(5)/军用(6) 在有长度门槛的档里一律不画 */
    chk_false("z4 直升机坪不过", pk_aero_lod_airport_pass(&z4, 2, 2, 11000, true));

    pk_aero_lod_t z12 = pk_aero_lod_for_zoom(12);
    chk_true("z12 直升机坪过", pk_aero_lod_airport_pass(&z12, 2, 0, 0, false));
    chk_true("z12 无码小场过", pk_aero_lod_airport_pass(&z12, 1, 1, 0, false));

    pk_aero_lod_t off = {0};
    chk_false("limit=0 全不画", pk_aero_lod_airport_pass(&off, 1, 2, 11000, true));
}

static void test_lod_navaid_fix_pass(void)
{
    pk_aero_lod_t z7 = pk_aero_lod_for_zoom(7);
    chk_true("z7 VOR 过",     pk_aero_lod_navaid_pass(&z7, 1));
    chk_true("z7 VORTAC 过",  pk_aero_lod_navaid_pass(&z7, 3));
    chk_false("z7 NDB 不过",  pk_aero_lod_navaid_pass(&z7, 4));
    chk_false("z7 DME 不过",  pk_aero_lod_navaid_pass(&z7, 6));

    pk_aero_lod_t z10 = pk_aero_lod_for_zoom(10);
    chk_true("z10 NDB 过",    pk_aero_lod_navaid_pass(&z10, 4));
    chk_true("z10 TACAN 过",  pk_aero_lod_navaid_pass(&z10, 7));
    chk_false("z10 未知类型不过", pk_aero_lod_navaid_pass(&z10, 0));
    chk_false("z10 越界类型不过", pk_aero_lod_navaid_pass(&z10, 8));

    chk_true("z10 enroute FIX 过", pk_aero_lod_fix_pass(&z10, 1));
    chk_true("z10 both FIX 过",    pk_aero_lod_fix_pass(&z10, 3));
    chk_false("z10 终端 FIX 不过", pk_aero_lod_fix_pass(&z10, 2));
    chk_false("z10 未知 scope 不过", pk_aero_lod_fix_pass(&z10, 0));

    pk_aero_lod_t z12 = pk_aero_lod_for_zoom(12);
    chk_true("z12 终端 FIX 过",  pk_aero_lod_fix_pass(&z12, 2));
    chk_true("z12 未知 scope 过", pk_aero_lod_fix_pass(&z12, 0));
}

/* ── 2. Web Mercator ─────────────────────────────────────────────── */

/* map_page.c:110 lonlat_to_world 的参考复刻（TILE_PX=256）。这份不是"再实现
 * 一次"，它就是对拍基准：pk_aero_lonlat_to_world 必须与地图底图用的那套
 * 数学逐值一致，否则叠加层整体偏移。 */
static void ref_lonlat_to_world(double lon, double lat, uint8_t z,
                                double *wx, double *wy)
{
    if (lat >  85.0511) lat =  85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double n = (double)(1u << z) * 256.0;
    double latrad = lat * M_PI / 180.0;
    *wx = (lon + 180.0) / 360.0 * n;
    *wy = (0.5 - log(tan(M_PI / 4.0 + latrad / 2.0)) / (2.0 * M_PI)) * n;
}

static void test_mercator(void)
{
    /* z0 原点：(0,0) 落在 128,128 */
    double wx, wy;
    pk_aero_lonlat_to_world(0.0, 0.0, 0, &wx, &wy);
    chk_dbl("z0 赤道本初 x", wx, 128.0, 1e-9);
    chk_dbl("z0 赤道本初 y", wy, 128.0, 1e-9);

    /* 经度线性：z10 上 +180° 正好半个世界 */
    double wx0, wy0, wx1, wy1;
    pk_aero_lonlat_to_world(-180.0, 0.0, 10, &wx0, &wy0);
    pk_aero_lonlat_to_world(0.0, 0.0, 10, &wx1, &wy1);
    chk_dbl("z10 半个世界宽", wx1 - wx0, 256.0 * 1024.0 / 2.0, 1e-6);

    /* 与 map_page 参考实现逐值对拍（含高纬、南半球、日界线附近） */
    static const struct { double lon, lat; uint8_t z; } pts[] = {
        { 113.2988,  23.3924, 12 },   /* ZGGG */
        { 139.7811,  35.5533, 10 },   /* RJTT */
        { -122.375,  37.6189,  9 },   /* KSFO */
        { 179.9,    -41.0,     7 },
        { -179.9,    64.0,     5 },
        {   0.0,     84.9,    12 },
        {  10.0,    -84.9,     3 },
    };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
        double a, b, c, d;
        pk_aero_lonlat_to_world(pts[i].lon, pts[i].lat, pts[i].z, &a, &b);
        ref_lonlat_to_world(pts[i].lon, pts[i].lat, pts[i].z, &c, &d);
        chk_dbl("对拍 map_page wx", a, c, 1e-9);
        chk_dbl("对拍 map_page wy", b, d, 1e-9);
    }

    /* ±85.0511 钳位：超出的纬度必须与边界值同结果，不能算出 inf/NaN */
    double cl_x, cl_y, ed_x, ed_y;
    pk_aero_lonlat_to_world(0.0, 89.9, 8, &cl_x, &cl_y);
    pk_aero_lonlat_to_world(0.0, 85.0511, 8, &ed_x, &ed_y);
    chk_dbl("北极钳位", cl_y, ed_y, 1e-9);
    pk_aero_lonlat_to_world(0.0, -89.9, 8, &cl_x, &cl_y);
    pk_aero_lonlat_to_world(0.0, -85.0511, 8, &ed_x, &ed_y);
    chk_dbl("南极钳位", cl_y, ed_y, 1e-9);
    chk_true("钳位后仍是有限值", isfinite(cl_y));
}

/* ── 3. 标签避让 ─────────────────────────────────────────────────── */
static void test_label_place(void)
{
    pk_aero_rect_t out;
    const int sx = 400, sy = 240, r = 7, lw = 40, lh = 12;

    /* 池空：走第一顺位 bottom */
    chk_true("空池命中", pk_aero_label_place(sx, sy, r, lw, lh, NULL, 0, &out));
    chk_true("空池落在符号下方", out.y0 > sy + r);
    chk_int("宽度含左右 padding", out.x1 - out.x0, lw + 4);
    chk_int("高度=字高", out.y1 - out.y0, lh);

    /* bottom 被完全占死 → 退到 right */
    pk_aero_rect_t occ1 = { sx - 60, sy + r, sx + 60, sy + r + 40 };
    chk_true("bottom 被占仍能放", pk_aero_label_place(sx, sy, r, lw, lh, &occ1, 1, &out));
    chk_true("退到右侧", out.x0 > sx + r);
    chk_int("右侧与占位零重叠", rect_overlap_area(&out, &occ1), 0);

    /* bottom + right 都占死 → left */
    pk_aero_rect_t occ2[2] = {
        { sx - 60, sy + r, sx + 60, sy + r + 40 },
        { sx + r,  sy - 40, sx + 200, sy + 40 },
    };
    chk_true("bottom+right 被占仍能放",
             pk_aero_label_place(sx, sy, r, lw, lh, occ2, 2, &out));
    chk_true("退到左侧", out.x1 < sx - r);

    /* 四向全占死 → 隐藏整个标签 */
    pk_aero_rect_t occ_all = { sx - 300, sy - 300, sx + 300, sy + 300 };
    chk_false("四向全撞则隐藏",
              pk_aero_label_place(sx, sy, r, lw, lh, &occ_all, 1, &out));

    /* 边缘：所有候选都只有一点点重叠（<50%）时仍要画出来，取最小的那个。
     * 造一条只压住 bottom 候选一角的窄条 + 三条把其余三向压满的。 */
    /* 三条封住 right/left/top 的挡板都只到 y=sy+r，不许探进 bottom 那条带，
     * 否则 bottom 候选的重叠会被它们撑过 50% —— 那测的就不是"取最小"了。 */
    pk_aero_rect_t occ_mix[4] = {
        { sx - 2,   sy + r + 3, sx + 2,   sy + r + 3 + lh },   /* bottom 压 4px 宽 */
        { sx + r,   sy - 300,   sx + 300, sy + r },            /* right 全压 */
        { sx - 300, sy - 300,   sx - r,   sy + r },            /* left 全压 */
        { sx - 300, sy - 300,   sx + 300, sy - r },            /* top 全压 */
    };
    chk_true("小重叠仍画", pk_aero_label_place(sx, sy, r, lw, lh, occ_mix, 4, &out));
    chk_true("选的是 bottom", out.y0 > sy + r);
}

/* ── 4. 机场名截断 ───────────────────────────────────────────────── */

static void chk_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
        g_fail++;
    }
}

/* 每次都灌满哨兵再调用：既验内容，也验没写越界（dst_sz 之后必须还是 '#'）。 */
static void ell(char *dst, size_t dst_sz, const char *src)
{
    memset(dst, '#', 64);
    dst[63] = '\0';
    name_ellipsize(dst, dst_sz, src);
    if (dst[dst_sz] != '#') { printf("FAIL 写越界 dst_sz=%zu\n", dst_sz); g_fail++; }
}

static void test_name_ellipsize(void)
{
    char b[64];

    /* 短名原样，不加省略号 */
    ell(b, 22, "Guangzhou Baiyun");
    chk_str("短名不变", b, "Guangzhou Baiyun");

    /* 词边界截断：预算 21，第 21 字节落在 "Chuangxin" 中间，退到上一个完整词 */
    ell(b, 22, "Shenyang Shenfu Gaige Chuangxin Shifanqu Heliport");
    chk_str("词边界截断", b, "Shenyang Shenfu...");
    chk_true("截断后不超预算", strlen(b) <= 21);

    /* 同一个名字在 label==3 档（预算 14）只剩第一个词 */
    ell(b, 15, "Shenyang Shenfu Gaige Chuangxin Heliport");
    chk_str("窄预算只剩首词", b, "Shenyang...");

    /* 首词本身就超预算 → 硬切 + 省略号（keep = cap-3 = 11） */
    ell(b, 15, "Chuangxinshifanquzhongxin Heliport");
    chk_str("首词超长硬切", b, "Chuangxinsh...");
    /* 整串一个词、没有空格，同样走硬切 */
    ell(b, 15, "Abcdefghijklmnopqrstuvwxyz");
    chk_str("单词串硬切", b, "Abcdefghijk...");

    /* 边界长度：恰好等于上限 / 上限-1 都原样；上限+1 才截 */
    ell(b, 11, "0123456789");            /* cap=10，长度 10 */
    chk_str("恰好等于上限", b, "0123456789");
    ell(b, 11, "012345678");             /* 上限 -1 */
    chk_str("上限减一", b, "012345678");
    ell(b, 11, "01234567890");           /* 上限 +1，无空格 → 硬切到 7 */
    chk_str("上限加一即截", b, "0123456...");
    chk_int("硬切后正好占满预算", (int)strlen(b), 10);

    /* 空串 / NULL */
    ell(b, 22, "");
    chk_str("空串", b, "");
    ell(b, 22, NULL);
    chk_str("NULL 源", b, "");

    /* 末尾空格：装得下时右修掉，不留悬空空格 */
    ell(b, 22, "Beijing Capital   ");
    chk_str("尾空格修掉", b, "Beijing Capital");
    /* 截断点左边的空格也不留，省略号必须紧贴词尾 */
    ell(b, 22, "Beijing   Capital   International Airport");
    chk_str("词界左侧空格不留", b, "Beijing   Capital...");

    /* 预算小到放不下 "..." → 给空串，不能吐半截省略号 */
    ell(b, 3, "Anything");
    chk_str("预算放不下省略号", b, "");
    ell(b, 1, "Anything");
    chk_str("只够一个 NUL", b, "");

    /* 生产预算：两档的最坏情况标签都要塞得进 put_label 那条 txt[64]，
     * 且宽度不超屏宽 1/3（26 字节 × PK_AA_XS 10px + 4px padding = 264px）。 */
    chk_true("label==3 档预算", NAME_CAP_ELEV == 13);
    chk_true("label==2 档预算", NAME_CAP_PLAIN == 21);
    chk_true("带标高档不超屏宽 1/3", 4 + 1 + NAME_CAP_ELEV + 1 + 7 <= 26);
    chk_true("代码+名称档不超屏宽 1/3", 4 + 1 + NAME_CAP_PLAIN <= 26);
    /* 预算都吃不满快照 name[28] 的 27 字节——这就是"不加大缓冲区"的依据 */
    chk_true("预算 < 快照存量", NAME_CAP_PLAIN < 27 && NAME_CAP_ELEV < 27);

    /* 多字节安全：源里混进 UTF-8 时硬切不许切出半个序列（切一半会渲成 '?'）。
     * "汉" = E6 B1 89，cap=14 → keep=11 正好落在第 3 个汉字中间。 */
    ell(b, 15, "汉汉汉汉汉汉");
    chk_int("硬切退回字符起始", (int)strlen(b), 9 + 3);   /* 3 个完整汉字 + "..." */
}

int main(void)
{
    test_lod_table();
    test_label_mode_density();
    test_rose_enabled();
    test_lod_airport_pass();
    test_lod_navaid_fix_pass();
    test_mercator();
    test_label_place();
    test_name_ellipsize();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
