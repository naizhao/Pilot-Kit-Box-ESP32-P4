/*
 * map_page.c — SD 离线地图页。本机居中跟随，PMTiles 栅格底图 + ADS-B 目标
 * 叠加。设计依据 docs/superpowers/specs/2026-08-01-sd-offline-map-design.md。
 *
 * 数据获取照 traffic_page.c 的 PFD 分支：own_ship 取位置、
 * aircraft_state_snapshot 取目标。绘制原语（pk_pfd_draw_aircraft /
 * pk_aa_puts / pk_pfd_darken_rect …）照抄 traffic_page.c / pfd.c 的用法，
 * 不发明新的绘制抽象层。
 *
 * 与 traffic_page 的关键差异：traffic 是"本机为原点的极坐标"，本页是
 * "地理坐标 → Web Mercator 世界像素 → 屏幕像素"的平移+缩放视口，且整数
 * zoom（0..12，无小数级连续缩放——单指触摸硬件不支持捏合，spec 已把这条
 * 手势砍掉，缩放只有 +/− 两档步进，见 touch_gt911.c 顶部注释）。
 *
 * 渲染顺序严格按 spec：底图 blit（含缺瓦片占位）→ ADS-B 目标 → 本机符号 →
 * 比例尺 + 署名 → 最后是页头/按钮等 UI 铬层。
 *
 * north-up / heading-up（2026-08-03 加）：朝向设置复用 config_traffic.h 的
 * pk_map_orient_t（交通页与设置页"地图朝向"那一行已经在用同一个开关，本页
 * 直接读，不新建设置项）。north-up 时 map_rot_deg 恒为 0，底图走原来的
 * 轴对齐 blit（memcpy 级，帧率关键路径不碰三角函数）；heading-up 且拿到本机
 * 航向时，底图整体绕视口中心反向旋转 map_rot_deg，走 render_tiles_heading_up()
 * 的逐像素旋转扫描线（整数增量步进 + 按瓦片边界锁采样，不逐像素查表/浮点，
 * 耗时估算见该函数注释）。ADS-B 目标、本机符号、搜索 PIN、pk_aero_layer 的
 * 机场/导航台/FIX 位置都跟着同一个 map_rot_deg 旋转；比例尺/按钮/顶栏等铬层
 * 是屏幕坐标系的东西，不旋转。文字标签一律保持正立——旋转只动"画在哪"，不
 * 动"字形本身"。 */
#include "map_page.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"    /* EXT_RAM_BSS_ATTR —— 见 render 里两个大数组的注释 */
#include "esp_timer.h"
#include "sdkconfig.h"

#include "display.h"
#include "i18n.h"
#include "pfd_layout.h"
#include "pfd_statusbar.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_font.h"
#include "pfd_icon_font.h"

#include "aircraft_state.h"
#include "config_traffic.h"  /* pk_map_orient_get —— north-up/heading-up 开关，
                              * 与交通页/设置页"地图朝向"是同一个设置 */
#include "imu_task.h"   /* pk_imu_sample_get —— 本机符号旋转的 IMU 航向来源 */
#include "mag_var.h"    /* 磁->真 修正，见 own_heading_true_deg 注释 */
#include "own_ship.h"
#include "pk_aero_layer.h"
#include "pk_win.h"           /* pk_win_set_viewport（W1.5：视口格钉住）*/
#include "pk_callsign.h"    /* pk_callsign_display —— 呼号/ICAO 回退，三页共用 */
#include "pk_own_sampler.h"   /* pk_own_sampler_get_phase() —— 显著性跟随本机相位 */
#include "pk_sdcard.h"
#include "pk_tile_loader.h"
#include "pk_ui_nav.h"

/* ── 视口 / 常量 ─────────────────────────────────────────────────── */
#define MAP_TOP        PFD_BAR_BOT
#define MCX            (PK_DISPLAY_W / 2)
#define MCY            (MAP_TOP + (PK_DISPLAY_H - MAP_TOP) / 2)

#define MAP_ZOOM_MIN   0
#define MAP_ZOOM_MAX   12
#define MAP_ZOOM_DEFAULT 10

/* 底部 UI 铬层：先留出一条署名/比例尺条，按钮贴着它上方，避免互相压。 */
#define FOOTER_H       18
#define FOOTER_Y0      (PK_DISPLAY_H - FOOTER_H)

#define BTN_D          56
#define BTN_HIT_PAD    12
#define BTN_M          16
#define BTN_GAP_ABOVE_FOOTER  8
#define BTN_ZOUT_X     (PK_DISPLAY_W - BTN_M - BTN_D)
#define BTN_ZOUT_Y_DEF (FOOTER_Y0 - BTN_GAP_ABOVE_FOOTER - BTN_D)
#define BTN_ZIN_X      BTN_ZOUT_X
#define BTN_RECENTER_X BTN_M
#define BTN_RECENTER_Y_DEF (FOOTER_Y0 - BTN_GAP_ABOVE_FOOTER - BTN_D)

/* FAB 可拖动，固定坐标躲不开它——每帧按 FAB 当前占位动态避让：竖直方向
 * 与本列按钮堆相交时，把整堆抬到 FAB 上沿之上（顶到 MAP_TOP 为止）。
 * render 与 touch 用同一套结果，保证看见的位置就是点得中的位置。
 *
 * 2026-08-02：右列多了一枚「搜索」，压在 zoom-in 之上，整堆从两枚变三枚。
 * 它**跟着这一堆一起避让**而不是自成一列——避让逻辑只有一份，多开一列就得
 * 再抄一遍相交判定，而这一堆的总高 3×56+2×10 = 188，从 FOOTER_Y0−8 往上排
 * 到 y=266，离 MAP_TOP(48) 还远，摆得下。 */
#define BTN_SEARCH_X   BTN_ZOUT_X
/* 左列自下而上：朝向切换（常驻）→ 回中（仅手动平移时出现）。
 *
 * 朝向钮放最下面而不是回中——它与交通页左下角那枚是同一个功能、同一套图标，
 * 位置也该一致，用户在两页之间切换时手不用重新找。回中是条件出现的，摆在它
 * 上方，出现/消失不会把朝向钮挤走。 */
#define BTN_ORIENT_X   BTN_M
#define BTN_ORIENT_Y_DEF BTN_RECENTER_Y_DEF

/*
 * 「回结果列表」——收起的搜索/详情 sheet 的返回入口（见 map_page.h）。
 *
 * 位置：**左列最上方**，紧贴顶栏下沿。四周都是被占掉的：
 *   - 右列自下而上是 缩放− / 缩放+ / 放大镜，还带 FAB 避让；
 *   - 左列下方是 朝向 / 回中；
 *   - 顶栏 y<48 是状态图标；左下角 y≥462 是比例尺与署名。
 * 左上这块是整屏唯一一处既不与既有元素相撞、又在拇指自然落点上的空地。
 *
 * 为什么不放在 PIN 旁边：PIN 是"我刚查的就是这个"的唯一凭据，它周围必须留白。
 * 而且 PIN 会随视口移动、还会滚出屏外，把返回钮拴在它身上就是把一个常驻功能
 * 挂到一个会跑掉的锚点上。
 *
 * 视觉刻意与其余四枚按钮**完全一致**（同一个 draw_btn_plate、同一支浅色墨）：
 * 它是一枚普通的地图控件，不该和琥珀色的 PIN 抢焦点。
 *
 * 只在有收起的 sheet 时出现，其余时候一格地方都不占。
 */
#define BTN_SHEET_X    BTN_M
#define BTN_SHEET_Y_DEF (MAP_TOP + 10)

/* FAB 避让：只有一枚钮，用不着上面那套整堆搬迁——相交就挪到 FAB 下沿之下。
 * 往下挪而不是往上：上面就是顶栏，没有余量。FAB 最低只能到 PK_DISPLAY_H−56，
 * 那时它离左上角十万八千里，本分支根本不会进。 */
static int sheet_btn_y(void)
{
    int y = BTN_SHEET_Y_DEF;
    int fx, fy, fw, fh;
    if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh)) {
        const int gap = 10;
        const bool left_col = fx < BTN_SHEET_X + BTN_D + BTN_HIT_PAD;
        if (left_col && fy < y + BTN_D + gap && fy + fh > y - gap)
            y = fy + fh + gap;
    }
    return y;
}

static void btn_layout(int *search_y, int *zin_y, int *zout_y, int *recenter_y,
                       int *orient_y)
{
    int zo = BTN_ZOUT_Y_DEF, zi = BTN_ZOUT_Y_DEF - BTN_D - 10;
    int ori = BTN_ORIENT_Y_DEF, rc = BTN_ORIENT_Y_DEF - BTN_D - 10;
    int fx, fy, fw, fh;
    if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh)) {
        const int gap = 10;
        bool right_col = fx + fw > BTN_ZOUT_X - BTN_HIT_PAD;
        bool left_col  = fx < BTN_RECENTER_X + BTN_D + BTN_HIT_PAD;
        /* 相交判定要按**整堆**的上沿算（搜索钮在 zi 之上一格），否则 FAB 停在
         * 搜索钮那一格的高度时整堆纹丝不动，搜索钮正好被压住点不着。 */
        if (right_col && fy < zo + BTN_D + gap && fy + fh > zi - BTN_D - 10 - gap) {
            zo = fy - gap - BTN_D;            /* 堆底贴 FAB 上沿 */
            zi = zo - BTN_D - 10;
            if (zi - BTN_D - 10 < MAP_TOP + gap) {  /* 上方不够就翻到 FAB 下方 */
                zi = fy + fh + gap + BTN_D + 10;
                zo = zi + BTN_D + 10;
            }
        }
        /* 左列同样按**整堆**上沿算（回中在朝向之上一格），否则 FAB 停在回中
         * 那一格时整堆不动、回中被压住点不着——右列踩过一次的同一个坑。 */
        if (left_col && fy < ori + BTN_D + gap && fy + fh > rc - gap) {
            ori = fy - gap - BTN_D;
            rc  = ori - BTN_D - 10;
            if (rc < MAP_TOP + gap) {          /* 上方不够就整堆翻到 FAB 下方 */
                rc  = fy + fh + gap;
                ori = rc + BTN_D + 10;
            }
        }
    }
    if (search_y) *search_y = zi - BTN_D - 10;
    if (zin_y) *zin_y = zi;
    if (zout_y) *zout_y = zo;
    if (recenter_y) *recenter_y = rc;
    if (orient_y) *orient_y = ori;
}

#define TILE_PX        256

/* ── 状态（跨帧持久，单渲染任务，无需加锁）───────────────────────── */
static uint8_t  s_zoom        = MAP_ZOOM_DEFAULT;
/* 无 GPS fix 时的默认地图中心：ZGGG（广州白云）跑道区。开机没定位就把地图
 * 摆在这里，而不是停在 (0,0) 那个没意义的点（几内亚湾外海）。一旦拿到 fix
 * 且 s_follow=true，中心自动跳到本机位置（见 render 里 own_valid 分支）。 */
#define MAP_DEFAULT_CENTER_LAT  23.3924   /* ZGGG 跑道中线 */
#define MAP_DEFAULT_CENTER_LON  113.2989
static double   s_center_lat  = MAP_DEFAULT_CENTER_LAT;
static double   s_center_lon  = MAP_DEFAULT_CENTER_LON;
static bool     s_follow      = true;    /* true=本机居中跟随；false=手动平移 */
static bool     s_have_last_own = false; /* 是否曾经有过一次有效本机位置 */
static double   s_last_own_lat, s_last_own_lon;

static bool     s_press_active = false;  /* 正在拖动/按下 */
static int      s_press_lx, s_press_ly;  /* 上一帧触点，算增量用 */
static int      s_btn_down = -1;         /* 0=recenter 1=zoom-in 2=zoom-out
                                          * 3=search 4=orient 5=回结果列表，
                                          * -1=无 */
static bool     s_press_moved = false;   /* 本次按压有没有真的挪过视口 */

/* 搜索结果 PIN（见 map_page.h）。经纬度而不是屏幕坐标——视口一动，PIN 要
 * 跟着地面走，存屏幕坐标就成了贴在玻璃上的一个点。 */
static bool   s_pin_valid = false;
static double s_pin_lat, s_pin_lon;
static char   s_pin_label[8];

/* 上一帧 render() 用过的地图旋转角（north-up 恒 0；heading-up 拿到本机航向
 * 时=当前航向，拿不到时也是 0——见 render() 里 map_rot_deg 的计算）。
 * touch_up() 的命中测试要用**与刚画的那一帧完全一致**的投影参数（同
 * s_center_lat/lon 的道理，见 touch_up 头注释），旋转角不像中心点/zoom 那样
 * 是用户操作决定的持久状态，是每帧从实时航向算出来的，所以单独存一份供
 * touch_up 读，不重新走一遍航向解析（触摸回调没有 own/src，也不该现查）。 */
static float  s_map_rot_deg = 0.0f;

/* ── Web Mercator 世界像素 ↔ 经纬度（度）── */
static void lonlat_to_world(double lon, double lat, uint8_t z, double *wx, double *wy)
{
    if (lat >  85.0511) lat =  85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double n = (double)(1u << z) * (double)TILE_PX;
    double latrad = lat * M_PI / 180.0;
    *wx = (lon + 180.0) / 360.0 * n;
    *wy = (0.5 - log(tan(M_PI / 4.0 + latrad / 2.0)) / (2.0 * M_PI)) * n;
}

static void world_to_lonlat(double wx, double wy, uint8_t z, double *lon, double *lat)
{
    double n = (double)(1u << z) * (double)TILE_PX;
    *lon = wx / n * 360.0 - 180.0;
    double yfrac = wy / n;
    double latrad = atan(sinh(M_PI * (1.0 - 2.0 * yfrac)));
    *lat = latrad * 180.0 / M_PI;
}

/* 世界像素 → 屏幕像素，按 (cos_r,sin_r)=(cos,sin)(map_rot_deg) 把内容绕视口
 * 中心旋转。这是**正变换**（世界→屏幕），ADS-B 目标/本机符号/搜索 PIN 这些
 * "个位数量级"的点都走它——north-up 时 cos_r=1,sin_r=0 退化成原来的纯平移，
 * 不需要为这几十个点单独维护一条快路径。
 *
 * 与 pk_aero_layer.c 的 proj_screen() 是**同一套矩阵**（sx=dx·cos+dy·sin，
 * sy=dy·cos−dx·sin），两边分属两个文件是因为 pk_aero_layer 的视口宏
 * （AERO_MCX/AERO_MCY）是它自己抄的一份、不导出——公式必须逐字同步，任何一处
 * 改了另一处要跟着改，否则底图转了、叠加层没转，画面会整体错位。 */
static inline void world_to_screen_rot(double wx, double wy, double cwx, double cwy,
                                       double cos_r, double sin_r, int *sx, int *sy)
{
    const double dx = wx - cwx, dy = wy - cwy;
    *sx = MCX + (int)lround(dx * cos_r + dy * sin_r);
    *sy = MCY + (int)lround(dy * cos_r - dx * sin_r);
}

/*
 * heading-up 底图：逐屏幕行做**增量步进**的逆向采样，把 north-up 快路径换成
 * "旋转扫描线"，不用离屏画布+整体旋转的两趟方案（那个方案的第二趟要多一次
 * 800×432 的全屏拷贝+双线性/最近邻重采样，且离屏画布本身要一块 800×480×2B
 * ≈768KB 的缓冲区，即使放 PSRAM 也是一次额外的读写往返；逐像素方案只有一趟，
 * 没有这块开销）。
 *
 * 数学：设本函数把内容绕视口中心 (MCX,MCY) 反向旋转 map_rot_deg 度，屏幕
 * 偏移 (sdx,sdy)=(x-MCX,y-MCY) 对应的世界偏移是**逆旋转**：
 *     wdx = sdx·cos(H) − sdy·sin(H)
 *     wdy = sdx·sin(H) + sdy·cos(H)     (H = map_rot_deg，与 world_to_screen_rot
 *                                         的正变换互为转置——旋转矩阵是正交阵)
 * 固定一行（sdy 不变），wdx/wdy 随 sdx 每 +1 线性递增 cos(H)/sin(H)——这正是
 * "沿扫描线增量步进"：每列只需 wx+=cosH、wy+=sinH 两次加法，不必重算三角函数
 * 或重新做除法。
 *
 * 定点化：cosH/sinH 与累加器都转成 Q8 定点整数（乘 256）而不是每像素跑
 * double：
 *   - world 像素坐标在 zoom≤12 时最大 2^12×256=1,048,576，乘 256 后
 *     ≈2.68×10^8，落在 int32_t 范围内（上限 2.15×10^9），一次 32 位加法搞定，
 *     不需要 int64；
 *   - 单步误差 ≤0.5/256 世界像素，累加 800 步最坏漂移 ≈1.56/256 ≈ 0.006 像素，
 *     远小于 1 像素，肉眼与量测都看不出；
 *   - tx = wx_fp >> 16（Q8 再除以 256 定位到瓦片，等价再右移 8，合计 16）；
 *     lx = (wx_fp >> 8) & 0xFF（瓦片内局部坐标，256 对齐，移位+掩码代替取模）。
 *     两者都依赖"负数算术右移在这颗目标（RISC-V + gcc）上是符号扩展"这一条
 *     实现定义行为——本项目的 riscv32-esp-elf 工具链恒如此，是嵌入式/图形代码
 *     里公认可依赖的写法，这里落成注释而不是当成理所当然。
 *
 * 单像素成本（tile 命中、未跨瓦片边界的稳态路径）：2 次 int32 加法（步进）
 * + 2 次移位+掩码（tx/ty 与 lx/ly）+ 2 次整数比较（判断是否跨瓦片）+ 1 次
 * 数组下标读——十条上下的整数指令，个位数周期量级（不含 fb 写回与 cache miss）。
 * 跨瓦片边界时才多付一次 pk_tile_loader_lock_sample()（一次路由查找 + 一次
 * 哈希/线性查表 + 一把互斥量），一整行横跨的瓦片数最多 (800/256)+2 ≈ 5 张，
 * 432 行 × 5 ≈ 2160 次——量级与 north-up 路径每帧最多 ~15 次 try_blit() 调用
 * 不同，但每次的成本也低得多（不做 256×256 整块拷贝，只挂一个指针）。
 *
 * 与 north-up 的差异（已知取舍，未做的原因见下）：
 *   - 不做 ancestor 放大回退（try_blit_ancestor 那一套）：未命中的像素直接
 *     露出函数外层已经填过的背景色，不画缺瓦片网格。给每个缺失像素单独算
 *     "属于哪个 256 网格、网格内第几个格子"要重新引入按瓦片对齐的坐标系，
 *     增加的分支/查表成本不值得——旋转态本来就是新瓦片正在到达的过渡状态，
 *     真瓦片一到自然替换，与 north-up 的连续性诉求（overzoom 场景）不是
 *     同一件事。
 *   - 不做双线性插值：最近邻（整数截断）与 north-up 的 blit_tile_scaled 一致，
 *     没有引入新的视觉差异。
 */
static void render_tiles_heading_up(uint16_t *fb, double cwx, double cwy, uint8_t zoom,
                                    double cos_r, double sin_r, uint32_t now_ms)
{
    const int32_t ntiles = (int32_t)1 << zoom;
    const int32_t COS_FP = (int32_t)lround(cos_r * 256.0);
    const int32_t SIN_FP = (int32_t)lround(sin_r * 256.0);

    for (int y = MAP_TOP; y < PK_DISPLAY_H; y++) {
        const double sdy = (double)(y - MCY);
        const double wx0 = cwx + (double)(0 - MCX) * cos_r - sdy * sin_r;
        const double wy0 = cwy + (double)(0 - MCX) * sin_r + sdy * cos_r;
        int32_t wx_fp = (int32_t)lround(wx0 * 256.0);
        int32_t wy_fp = (int32_t)lround(wy0 * 256.0);

        int32_t cur_tx = INT32_MIN, cur_ty = INT32_MIN;
        const uint16_t *cur_data = NULL;
        uint32_t cur_shift = 0, cur_crop_x0 = 0, cur_crop_y0 = 0;
        bool cur_locked = false;

        for (int x = 0; x < PK_DISPLAY_W; x++, wx_fp += COS_FP, wy_fp += SIN_FP) {
            const int32_t tx = wx_fp >> 16;
            const int32_t ty = wy_fp >> 16;
            if (tx < 0 || tx >= ntiles || ty < 0 || ty >= ntiles) {
                if (cur_locked) { pk_tile_loader_unlock_sample(); cur_locked = false; }
                cur_tx = cur_ty = INT32_MIN;
                continue;   /* 背景色已在外层整屏填过，越界不用画 */
            }
            if (tx != cur_tx || ty != cur_ty) {
                if (cur_locked) { pk_tile_loader_unlock_sample(); cur_locked = false; }
                cur_tx = tx; cur_ty = ty;
                cur_locked = pk_tile_loader_lock_sample((uint8_t)zoom, (uint32_t)tx,
                                                        (uint32_t)ty, now_ms, &cur_data,
                                                        &cur_shift, &cur_crop_x0, &cur_crop_y0);
                if (!cur_locked) {
                    pk_map_route_result_t route;
                    if (pk_tile_loader_route((uint8_t)zoom, (uint32_t)tx, (uint32_t)ty, &route))
                        pk_tile_loader_request(&route);
                }
            }
            if (!cur_locked) continue;

            const uint32_t lx = ((uint32_t)(wx_fp >> 8)) & 0xFF;
            const uint32_t ly = ((uint32_t)(wy_fp >> 8)) & 0xFF;
            const uint32_t sx_in = cur_crop_x0 + (lx >> cur_shift);
            const uint32_t sy_in = cur_crop_y0 + (ly >> cur_shift);
            fb[y * PK_DISPLAY_W + x] = cur_data[sy_in * TILE_PX + sx_in];
        }
        if (cur_locked) pk_tile_loader_unlock_sample();
    }
}

/* ── 圆形按钮底（照抄 traffic_page.c 的 draw_btn_plate，本页自成一份：
 * 那份是 static，不跨文件导出，同一套视觉语言直接照抄写法）── */
static void draw_btn_plate(uint16_t *fb, int x, int y, bool down)
{
    const uint16_t face = down ? pk_rgb565( 62,  84, 112) : pk_rgb565( 22,  30,  42);
    const uint16_t edge = down ? pk_rgb565(210, 228, 245) : pk_rgb565(120, 145, 175);
    const int r = BTN_D / 2, cx = x + r, cy = y + r;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            const int px = cx + dx, py = cy + dy;
            if (px < 0 || px >= PK_DISPLAY_W || py < 0 || py >= PK_DISPLAY_H) continue;
            fb[py * PK_DISPLAY_W + px] = (d2 >= (r - 2) * (r - 2)) ? edge : face;
        }
    }
}

/* 放大镜图标：一个圆环 + 一根 45° 手柄。
 *
 * 为什么不用 pk_pfd_draw_arc_aa 画那个圆：它把 360° 按 1.5° 拆成 240 段
 * draw_line_aa，pk_aero_layer 就是这么从 8 FPS 掉到 5 FPS 的（见该文件的
 * 性能坑记录）。这里直接按 d² 判环带，一次双重循环 24×24 就画完。
 *
 * 也不用图标字体：pfd_icon_font 里没有放大镜，为一个 22 px 的图形去重跑
 * 字库生成器不划算，而这一个形状用几何原语就能画准。 */
static void draw_search_icon(uint16_t *fb, int cx, int cy)
{
    const uint16_t ink = pk_rgb565(225, 235, 248);
    /* 镜片中心略偏左上，给右下角的手柄让位——两者叠在正中的话，图标看着
     * 是个歪掉的圆而不是放大镜。 */
    const int ox = cx - 3, oy = cy - 3;
    const int r_out = 11, r_in = 8;
    for (int dy = -r_out; dy <= r_out; ++dy) {
        for (int dx = -r_out; dx <= r_out; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > r_out * r_out || d2 < r_in * r_in) continue;
            const int px = ox + dx, py = oy + dy;
            if (px < 0 || px >= PK_DISPLAY_W || py < 0 || py >= PK_DISPLAY_H) continue;
            fb[py * PK_DISPLAY_W + px] = ink;
        }
    }
    /* 手柄画三条平行线当作 3 px 线宽——draw_line_aa 有线宽参数，但这里是
     * 纯水平/对角的短线，三条整数线比 AA 更实，小尺寸下不糊。 */
    for (int k = -1; k <= 1; ++k)
        pk_pfd_draw_line(fb, ox + 7 + k, oy + 7, ox + 15 + k, oy + 15, ink);
}

/*
 * 「回结果列表」图标：一个左向尖角 + 三条列表横杠。
 *
 * 纯几何自绘，不用文字：任何字符都必须落在 ASCII 0x20–0x7F 内（屏上的 CJK
 * 字形是 i18n catalog 驱动的子集，加一句中文要改 catalog 再重跑字库生成器），
 * 而 "BACK"/"LIST" 这类英文缩写在 56 px 的圆盘里摆不下，摆得下也要先读再想。
 * 尖角 + 横杠是两个通用符号的叠加：「往回」+「一份列表」，扫一眼就知道点了
 * 会回到刚才那张单子。也不用图标字体——pfd_icon_font 里没有这个形状，为一个
 * 26×16 的图形重跑一次字库生成器不划算（同 draw_search_icon 的取舍）。
 */
static void draw_sheet_back_icon(uint16_t *fb, int cx, int cy)
{
    const uint16_t ink = pk_rgb565(225, 235, 248);

    /* 尖角：apex 在左，两条 8 px 的斜边。三条平行线当 3 px 线宽，同
     * draw_search_icon 的手柄——短斜线上整数线比 AA 更实，小尺寸不糊。 */
    const int ax = cx - 15, ay = cy;
    for (int k = -1; k <= 1; ++k) {
        pk_pfd_draw_line(fb, ax, ay + k, ax + 8, ay - 8 + k, ink);
        pk_pfd_draw_line(fb, ax, ay + k, ax + 8, ay + 8 + k, ink);
    }

    /* 列表横杠：三条 15 px 宽、3 px 高、间距 6 —— 总高 15，与尖角的 16 齐平，
     * 两者在竖直方向对得上，看着才像一个整体而不是两个图标挤在一起。 */
    for (int i = 0; i < 3; ++i) {
        const int y0 = cy - 7 + i * 6;
        pk_pfd_fill_rect(fb, cx - 1, y0, cx + 14, y0 + 3, ink);
    }
}

/* 搜索结果 PIN：泪滴形（圆头 + 尖脚落地）+ 白描边 + 代码标签。
 * 尖脚**正好落在目标经纬度上**，圆头在它上方——地图上的点位符号必须让人
 * 一眼知道"指的是哪个像素"，画成居中的圆就得靠猜。 */
static void draw_search_pin(uint16_t *fb, int sx, int sy, const char *label)
{
    const uint16_t body = pk_rgb565(255, 176,   0);   /* 琥珀：地图上没有第二处用它画符号 */
    const uint16_t edge = pk_rgb565(255, 255, 255);
    const uint16_t hole = pk_rgb565( 20,  16,   6);

    const int r = 11;                 /* 圆头半径 */
    const int cy = sy - 26;           /* 圆心离地 26 px，尖脚落在 (sx, sy) */

    /* 尖脚：从圆头下沿收到落点的实心三角。 */
    pk_pfd_draw_triangle(fb, sx - 7, cy + 6, sx + 7, cy + 6, sx, sy, edge);
    pk_pfd_draw_triangle(fb, sx - 5, cy + 5, sx + 5, cy + 5, sx, sy - 2, body);

    for (int dy = -r - 2; dy <= r + 2; ++dy) {
        for (int dx = -r - 2; dx <= r + 2; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > (r + 2) * (r + 2)) continue;
            const int px = sx + dx, py = cy + dy;
            if (px < 0 || px >= PK_DISPLAY_W || py < MAP_TOP || py >= PK_DISPLAY_H)
                continue;
            fb[py * PK_DISPLAY_W + px] =
                (d2 > r * r) ? edge : (d2 <= 4 * 4 ? hole : body);
        }
    }

    /* 代码标签压在 PIN 上方，带暗底衬——底图颜色不可控，不衬底就会有
     * 「白字压在雪地瓦片上」的读不出来（署名条踩过同一个坑）。 */
    if (label != NULL && label[0] != '\0') {
        const int lw = pk_aa_text_width(label, PK_AA_S);
        int lx = sx - lw / 2, ly = cy - r - 4 - PK_AA_S_H;
        if (lx < 2) lx = 2;
        if (lx + lw > PK_DISPLAY_W - 2) lx = PK_DISPLAY_W - 2 - lw;
        if (ly < MAP_TOP + 2) ly = MAP_TOP + 2;
        pk_pfd_darken_rect(fb, lx - 5, ly - 2, lx + lw + 5, ly + PK_AA_S_H + 2, 170);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, lx, ly, label, body, PK_AA_S);
    }
}

static bool hit_btn(int x, int y, int bx, int by)
{
    const int r = BTN_D / 2 + BTN_HIT_PAD;
    const int dx = x - (bx + BTN_D / 2);
    const int dy = y - (by + BTN_D / 2);
    return dx * dx + dy * dy <= r * r;
}

/* ── 缺瓦片占位：深色网格 + z/x/y 小字（FlightMate 同款思路，spec 错误态表）── */
static void draw_missing_tile(uint16_t *fb, int x0, int y0, uint8_t z, uint32_t tx, uint32_t ty)
{
    const uint16_t col_grid = pk_rgb565(40, 46, 56);
    const uint16_t col_bg   = pk_rgb565(18, 21, 26);
    int x1 = x0 + TILE_PX, y1 = y0 + TILE_PX;
    if (x0 < 0) x0 = 0;
    if (y0 < MAP_TOP) y0 = MAP_TOP;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    if (x0 >= x1 || y0 >= y1) return;
    pk_pfd_fill_rect(fb, x0, y0, x1, y1, col_bg);
    for (int gx = x0; gx < x1; gx += 32) pk_pfd_draw_line(fb, gx, y0, gx, y1 - 1, col_grid);
    for (int gy = y0; gy < y1; gy += 32) pk_pfd_draw_line(fb, x0, gy, x1 - 1, gy, col_grid);
    /* z 最大 12(<100)、x/y 在本项目 zoom 范围内最多 4 位十进制，40 字节绰绰
     * 有余——固定宽度上限让 gcc 的 -Wformat-truncation 静态分析满意
     * （它按 uint32_t 的理论最大 10 位数估算，24 字节会被判定"可能截断"）。 */
    char buf[40];
    snprintf(buf, sizeof(buf), "%u/%lu/%lu", (unsigned)z, (unsigned long)tx, (unsigned long)ty);
    if (x1 - x0 > 60 && y1 - y0 > 16)
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, x0 + 4, y0 + 4, buf,
                    pk_rgb565(90, 98, 110), 1);
}

/* ── 整页错误态：无 SD / 无 maps 目录 / 无有效包（同一症状，HINT 分因）── */
static void draw_no_data_state(uint16_t *fb, bool sd_mounted)
{
    const uint16_t col_bg    = pk_rgb565(7, 10, 16);
    const uint16_t col_amber = pk_rgb565(255, 176, 0);
    const uint16_t col_hint  = pk_rgb565(170, 182, 200);

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg);

    /* 没插卡 / 没瓦片包的占位态也是"地图页"，顶栏一样要有标题 + 设备状态组
     * ——SD 图标此时恰好是红色闪烁，与正文提示（"插卡"/"缺瓦片"）说的是
     * 同一件事，不冲突。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L, PK_UI_TITLE_Y,
              pk_i18n_text(PK_TR_MAP_TITLE), PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
    {
        pk_pfd_status_t stat = {0};
        pk_ui_topbar_status_collect(&stat);
        pk_ui_topbar_status_render(fb, &stat);
    }

    const char *title = pk_i18n_text(PK_TR_MAP_NO_DATA_TITLE);
    const char *hint  = pk_i18n_text(sd_mounted ? PK_TR_MAP_HINT_NO_PACK
                                                : PK_TR_MAP_HINT_NO_CARD);
    const int tw = pk_aa_text_width(title, PK_AA_L);
    const int hw = pk_aa_text_width(hint, PK_AA_S);
    const int cy = MAP_TOP + (PK_DISPLAY_H - MAP_TOP) / 2;

    pk_pfd_darken_rect(fb, PK_DISPLAY_W / 2 - (tw > hw ? tw : hw) / 2 - 16,
                       cy - PK_AA_L_H / 2 - 8,
                       PK_DISPLAY_W / 2 + (tw > hw ? tw : hw) / 2 + 16,
                       cy + PK_AA_L_H / 2 + PK_AA_S_H + 16, 190);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W / 2 - tw / 2,
              cy - PK_AA_L_H / 2, title, col_amber, PK_AA_L);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W / 2 - hw / 2,
              cy + PK_AA_L_H / 2 + 8, hint, col_hint, PK_AA_S);
}

/* 指北标识：一个小箭头 + "N"，箭头方向 = 真北当前的屏幕方位（north-up 时
 * map_rot_deg 恒 0，箭头恒指屏幕正上方；heading-up 时随 map_rot_deg 反向
 * 摆动）。north-up/heading-up 只用这一个 widget 就都覆盖了——箭头动不动本身
 * 就在说"地图有没有转"，比另外加一行 "HDG UP" 文案更直观，也不用碰 i18n：
 * "N" 是纯 ASCII，PK_AA 子集字库本来就有，不需要走 i18n_catalog 三件套。
 * 画在铬层（draw_chrome 最后调用，叠在所有地理内容之上），坐标固定、不旋转
 * ——它标的是"北在哪"，自己转了就失去意义了。 */
static void draw_north_ind(uint16_t *fb, int cx, int cy, float map_rot_deg)
{
    const uint16_t col = pk_rgb565(255, 200, 90);
    /* 真北的屏幕方位 = 真北在世界坐标系里的方位(0°) − map_rot_deg，与
     * pk_aero_layer.c 罗盘玫瑰/管制机场 tick 的推导是同一条公式。 */
    const float ang = -map_rot_deg * (float)M_PI / 180.0f;
    const float dx = sinf(ang), dy = -cosf(ang);
    const int r_tip = 8, r_tail = 3, half_w = 4;
    const int tipx  = cx + (int)lroundf(dx * r_tip),  tipy  = cy + (int)lroundf(dy * r_tip);
    const int tailx = cx - (int)lroundf(dx * r_tail), taily = cy - (int)lroundf(dy * r_tail);
    const int wx = (int)lroundf(-dy * half_w), wy = (int)lroundf(dx * half_w);
    pk_pfd_draw_triangle(fb, tipx, tipy, tailx + wx, taily + wy, tailx - wx, taily - wy, col);

    /* "N" 标在箭尖再往外一点，字形本身不跟着转（转了就读不出来）。 */
    const int lx = cx + (int)lroundf(dx * (r_tip + 8));
    const int ly = cy + (int)lroundf(dy * (r_tip + 8));
    const int lw = pk_aa_text_width("N", PK_AA_XS);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, lx - lw / 2, ly - PK_AA_XS_H / 2,
              "N", col, PK_AA_XS);
}

/* ── 顶栏 / 底部铬层 ── */
static void draw_chrome(uint16_t *fb, double meters_per_px, float map_rot_deg, size_t n_aircraft)
{
    draw_north_ind(fb, 34, MAP_TOP + 34, map_rot_deg);

    /* 顶栏底色：地图瓦片可能画到 y<MAP_TOP（视口边缘取整误差），用一块实底
     * 盖掉，与其它整屏页一致（页头永远是不透明的一条）。 */
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, MAP_TOP, pk_rgb565(7, 10, 16));
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L, PK_UI_TITLE_Y,
              pk_i18n_text(PK_TR_MAP_TITLE), PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);

    /* 卫星数 / ADS-B 目标数 / SD 卡状态等"设备状态组"，PFD/交通/列表同款，
     * 见 pk_ui_topbar_status_render 头注——四页画在同一个 x 上。 */
    {
        pk_pfd_status_t stat = { .aircraft_count = n_aircraft };
        pk_ui_topbar_status_collect(&stat);
        pk_ui_topbar_status_render(fb, &stat);
    }

    char zbuf[8];
    snprintf(zbuf, sizeof(zbuf), "Z%u", s_zoom);
    const int zw = pk_aa_text_width(zbuf, PK_UI_TITLE_SIZE);
    /* 右界走 pk_ui_topbar_content_right_limit：除了 DEMO 徽标，现在还要给
     * 常驻的设备状态组让位，否则 Z10 会被压在 SD 图标底下。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
              pk_ui_topbar_content_right_limit(PK_DISPLAY_W - 24) - zw, PK_UI_TITLE_Y,
              zbuf, pk_rgb565(205, 214, 228), PK_UI_TITLE_SIZE);

    /* 底部铬层：署名 + 比例尺，压一层暗底保证在任何底图颜色上可读——
     * FlightMate 把署名画成黑底黑字看不见的教训，这里必须是实际可见色。 */
    pk_pfd_darken_rect(fb, 0, FOOTER_Y0, PK_DISPLAY_W, PK_DISPLAY_H, 170);
    const char *attr = pk_i18n_text(PK_TR_MAP_ATTRIBUTION);
    const int aw = pk_aa_text_width(attr, PK_AA_XS);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W - 8 - aw,
              FOOTER_Y0 + (FOOTER_H - PK_AA_XS_H) / 2, attr,
              pk_rgb565(225, 230, 238), PK_AA_XS);

    /* 比例尺：从一组"整数好读"的 NM 值里挑一个像素长度落在 [50,150] 的。 */
    if (meters_per_px > 0.0) {
        static const double kNm[] = { 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000 };
        double nm = kNm[0];
        double bar_px = 0;
        for (size_t i = 0; i < sizeof(kNm) / sizeof(kNm[0]); i++) {
            double px = kNm[i] * 1852.0 / meters_per_px;
            nm = kNm[i];
            bar_px = px;
            if (px >= 50.0) break;
        }
        if (bar_px > 4 && bar_px < PK_DISPLAY_W / 2) {
            int bx0 = 8, by = FOOTER_Y0 + FOOTER_H / 2;
            pk_pfd_fill_rect(fb, bx0, by - 1, bx0 + (int)bar_px, by + 1,
                            pk_rgb565(225, 230, 238));
            char sbuf[16];
            if (nm < 1.0) snprintf(sbuf, sizeof(sbuf), "%.1fNM", nm);
            else          snprintf(sbuf, sizeof(sbuf), "%dNM", (int)nm);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, bx0 + (int)bar_px + 6,
                        by - 3, sbuf, pk_rgb565(225, 230, 238), 1);
        }
    }

    int search_y, zin_y, zout_y, rc_y, orient_y;
    btn_layout(&search_y, &zin_y, &zout_y, &rc_y, &orient_y);
    draw_btn_plate(fb, BTN_SEARCH_X, search_y, s_btn_down == 3);
    draw_search_icon(fb, BTN_SEARCH_X + BTN_D / 2, search_y + BTN_D / 2);
    draw_btn_plate(fb, BTN_ZIN_X, zin_y, s_btn_down == 1);
    draw_btn_plate(fb, BTN_ZOUT_X, zout_y, s_btn_down == 2);
    {
        const uint16_t ink = pk_rgb565(225, 235, 248);
        const int cw = pk_aa_cell_w(PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                  BTN_ZIN_X + (BTN_D - cw) / 2, zin_y + (BTN_D - PK_AA_L_H) / 2,
                  "+", ink, PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                  BTN_ZOUT_X + (BTN_D - cw) / 2, zout_y + (BTN_D - PK_AA_L_H) / 2,
                  "-", ink, PK_AA_L);
    }

    /* 朝向切换：图标而非文字，与交通页同一套。"HDG UP"/"N UP" 这类缩写要先读
     * 再想它什么意思；导航箭头与指南针是通用符号，扫一眼就懂。图标含义是
     * **当前朝向**（不是"点了会变成什么"），与交通页保持一致。 */
    draw_btn_plate(fb, BTN_ORIENT_X, orient_y, s_btn_down == 4);
    {
        const pk_icon_id_t oid = (pk_map_orient_get() == PK_MAP_HEADING_UP)
                                   ? PK_ICON_NAV_HDG : PK_ICON_NAV_NORTH;
        const uint8_t *oic = pk_icon_bitmap
                           + (size_t)oid * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
        pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        BTN_ORIENT_X + (BTN_D - PK_ICON_W) / 2,
                        orient_y + (BTN_D - PK_ICON_H) / 2,
                        oic, PK_ICON_W, PK_ICON_H, pk_rgb565(225, 235, 248));
    }

    /* 收起的 sheet 才画返回钮。没有就一格地方都不占——地图上每一枚常驻控件
     * 都在和地形抢像素。 */
    if (pk_map_page_sheet_collapsed()) {
        const int by = sheet_btn_y();
        draw_btn_plate(fb, BTN_SHEET_X, by, s_btn_down == 5);
        draw_sheet_back_icon(fb, BTN_SHEET_X + BTN_D / 2, by + BTN_D / 2);
    }

    if (!s_follow) {
        draw_btn_plate(fb, BTN_RECENTER_X, rc_y, s_btn_down == 0);
        const char *lbl = pk_i18n_text(PK_TR_MAP_RECENTER);
        const int lw = pk_aa_text_width(lbl, PK_AA_S);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                  BTN_RECENTER_X + (BTN_D - lw) / 2,
                  rc_y + (BTN_D - PK_AA_S_H) / 2,
                  lbl, pk_rgb565(225, 235, 248), PK_AA_S);
    }
}

/* ── 目标标签：简化版防遮挡——按距屏幕中心（约=本机）近→远的顺序占位，
 * 与已占用矩形相撞就只画符号、不画标签。不做 traffic_page 那套 14 槽位/
 * 帧间记忆的完整解法（那套是为雷达上密集扎堆调的），地图上目标本来就按
 * 地理位置分散，这条更轻量的规则已经能保证"标签之间不重叠"这条底线。 ── */
#define MAP_LBL_MAX  AIRCRAFT_TABLE_CAPACITY
/* 与航空叠加层共用同一个占位池，所以矩形类型也共用一个（定义在
 * pk_aero_layer.h）——两个图层各画各的标签、共享一份"这块已经被占了"。 */
typedef pk_aero_rect_t rect_t;

static bool rect_overlap(const rect_t *a, const rect_t *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

/* ── 渲染 ─────────────────────────────────────────────────────────── */
void pk_map_page_render(uint16_t *fb)
{
    const bool sd_mounted = pk_sdcard_is_mounted();
    if (!sd_mounted || pk_tile_loader_pack_count() == 0) {
        draw_no_data_state(fb, sd_mounted);
        return;
    }

    const uint16_t col_bg = pk_rgb565(18, 21, 26);
    pk_pfd_fill_rect(fb, 0, MAP_TOP, PK_DISPLAY_W, PK_DISPLAY_H, col_bg);

    int64_t now_us = esp_timer_get_time();
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    aircraft_t own = {0};
    pk_own_src_t src;
    bool own_valid = pk_own_ship_resolve(
        now_us, (int64_t)CONFIG_PK_OWN_STALE_AGE_MS * 1000LL, &own, &src);
    if (own_valid) {
        s_have_last_own = true;
        s_last_own_lat = own.lat;
        s_last_own_lon = own.lon;
    }

    if (s_follow && own_valid) {
        s_center_lat = own.lat;
        s_center_lon = own.lon;
    }

    double cwx, cwy;
    lonlat_to_world(s_center_lon, s_center_lat, s_zoom, &cwx, &cwy);

    /* 告知航空叠加层当前视图（只存值+置 dirty，查库在它自己的后台任务里）。 */
    pk_aero_layer_notify_view(s_center_lat, s_center_lon, s_zoom);

    /*
     * 地图旋转角。north-up 恒 0；heading-up 且拿到本机航向时 = 航向（否则也是
     * 0，见下）。这份计算原来贴在本机符号那一段（只为了那一个符号服务），
     * 现在提到最前面，因为**底图**也要用它——旋转与否决定接下来走 north-up
     * 快路径还是 render_tiles_heading_up()。
     *
     * own_valid==false 时 own_rot_deg 保持 0.0f，map_rot_deg 也就跟着是 0：
     * 丢失本机当前定位时地图自动退回 north-up（没有航向数据，"转成什么角度"
     * 无从谈起），与既有的"拿不到航向就朝北"降级逻辑是同一条原则，不是
     * 新增的特例。
     *
     * IMU 这一路要加磁偏角转真北（mag_var.h：东偏为正，真=磁+偏）——ADS-B
     * 地面航迹与 GPS track 本来就是真北，只有 IMU yaw 是磁北，直接拿磁航向
     * 当地图旋转角会让"正上方"偏掉一个磁偏角（国内 3~10°，高纬更多），而且
     * 屏幕上完全看不出错，这条换算不能省。
     */
    float own_rot_deg = 0.0f;
    if (own_valid) {
        pk_imu_sample_t imu;
        bool imu_ok = pk_imu_sample_get(&imu) && imu.valid;
        float hdg = 0.0f;
        pk_hdg_src_t hsrc = PK_HDG_SRC_NONE;
        if (pk_own_heading_resolve(own_valid, src, &own,
                                   imu_ok, imu_ok ? imu.yaw_deg : 0.0f,
                                   &hdg, &hsrc)) {
            if (hsrc == PK_HDG_SRC_IMU)
                hdg += pk_mag_var_lookup(own.lat, own.lon);
            own_rot_deg = hdg;
        }
    }
    const float map_rot_deg = (pk_map_orient_get() == PK_MAP_NORTH_UP) ? 0.0f : own_rot_deg;
    s_map_rot_deg = map_rot_deg;   /* touch_up() 命中测试要用同一份，见其声明处注释 */
    const double rot_rad = (double)map_rot_deg * M_PI / 180.0;
    const double cos_r = cos(rot_rad), sin_r = sin(rot_rad);

    /* 告知窗口模块当前视口范围（W1.5，2026-08-04）：把屏上可见的格钉进窗口
     * 不卸载，这样 pk_aero_layer 走 pk_win_nearest 时视口内的格一定在窗口里，
     * 不会因窗口只跟本机椭圆而漏掉用户拖到的远处。
     *
     * 取四角的 lon/lat min/max。屏幕角→世界坐标走 world_to_screen_rot 的逆
     * （绕视口中心反向旋转，与正变换互为转置，见 heading-up 扫描线那段注释）：
     *   wdx = sdx·cos − sdy·sin；wdy = sdx·sin + sdy·cos
     * 其中 sdx=±PK_DISPLAY_W/2、sdy=±(PK_DISPLAY_H−MAP_TOP)/2 是屏幕角相对
     * 视口中心 (MCX,MCY) 的偏移，视口中心对应世界 (cwx,cwy)。
     *   north-up（cos=1,sin=0）退化为轴对齐四角，bbox 精确；
     *   heading-up 时四角围成旋转可见矩形，其 lon/lat 外接框 ⊃ 实际可见区域，
     * 钉的格只会多不会少（墨卡托下 lon 只看 wx、lat 只看 wy，四角 min/max 即
     * 整条边的 min/max）。
     *   注：不能像旧写法那样只取对角两角 + 第三角——lat 只依赖 wy，对角两角
     * 的 wy 一北一南，但赋值时 min/max 名字写反，第三角校正又只修了 min_lat，
     * max_lat 永远卡在南边，bbox 退化成零纬度高度（host 回归见
     * firmware/test/test_pk_map_viewport_bbox.c，镜像本段几何）。 */
    {
        const double hw = (double)(PK_DISPLAY_W / 2);
        const double hh = (double)((PK_DISPLAY_H - MAP_TOP) / 2);
        double min_lat = 1e9, max_lat = -1e9, min_lon = 1e9, max_lon = -1e9;
        for (int ix = -1; ix <= 1; ix += 2) {
            for (int iy = -1; iy <= 1; iy += 2) {
                const double sdx = ix * hw, sdy = iy * hh;
                const double wdx = sdx * cos_r - sdy * sin_r;
                const double wdy = sdx * sin_r + sdy * cos_r;
                double lon, lat;
                world_to_lonlat(cwx + wdx, cwy + wdy, s_zoom, &lon, &lat);
                if (lat < min_lat) min_lat = lat;
                if (lat > max_lat) max_lat = lat;
                if (lon < min_lon) min_lon = lon;
                if (lon > max_lon) max_lon = lon;
            }
        }
        pk_win_set_viewport(min_lat, min_lon, max_lat, max_lon);
    }

    /* ── 底图：可见范围内的瓦片 blit（含缺瓦片占位）──
     * map_rot_deg==0（north-up，或 heading-up 但暂时没有航向数据）时走原来的
     * 轴对齐 blit（memcpy 级，逐瓦片而不是逐像素）；否则走旋转扫描线，见
     * render_tiles_heading_up() 的耗时与算法注释。 */
    const int32_t ntiles = (int32_t)1 << s_zoom;
    if (map_rot_deg == 0.0f) {
        double world_left  = cwx - MCX;
        double world_right = cwx + (PK_DISPLAY_W - MCX);
        double world_top    = cwy - (MCY - MAP_TOP);
        double world_bottom = cwy + (PK_DISPLAY_H - MCY);
        int32_t tx0 = (int32_t)floor(world_left  / TILE_PX);
        int32_t tx1 = (int32_t)floor(world_right / TILE_PX);
        int32_t ty0 = (int32_t)floor(world_top    / TILE_PX);
        int32_t ty1 = (int32_t)floor(world_bottom / TILE_PX);

        for (int32_t ty = ty0; ty <= ty1; ty++) {
            if (ty < 0 || ty >= ntiles) continue;
            for (int32_t tx = tx0; tx <= tx1; tx++) {
                if (tx < 0 || tx >= ntiles) continue;
                int dst_x0 = MCX + (int)lround((double)tx * TILE_PX - cwx);
                int dst_y0 = MCY + (int)lround((double)ty * TILE_PX - cwy);

                pk_map_route_result_t route;
                bool found = pk_tile_loader_route((uint8_t)s_zoom, (uint32_t)tx, (uint32_t)ty, &route);
                bool blitted = false, neg = false;
                if (found) {
                    blitted = pk_tile_loader_try_blit(&route, (uint32_t)tx, (uint32_t)ty,
                                                      fb, dst_x0, dst_y0, now_ms, &neg);
                    if (!blitted && !neg) pk_tile_loader_request(&route);
                }
                if (!blitted) {
                    /* 真瓦片还没到：先拿缓存里的上级瓦片放大顶上，画面不留空洞；
                     * 一级都找不到才退回网格占位。 */
                    if (!pk_tile_loader_try_blit_ancestor((uint8_t)s_zoom, (uint32_t)tx,
                                                          (uint32_t)ty, fb, dst_x0, dst_y0,
                                                          now_ms, 4))
                        draw_missing_tile(fb, dst_x0, dst_y0, (uint8_t)s_zoom,
                                          (uint32_t)tx, (uint32_t)ty);
                }
            }
        }
    } else {
        render_tiles_heading_up(fb, cwx, cwy, s_zoom, cos_r, sin_r, now_ms);
    }

    /* 越级放大提示：视口中心那格如果是 overzoom，弱化提示一下（spec §交互）。 */
    {
        pk_map_route_result_t center_route;
        int32_t ctx = (int32_t)floor(cwx / TILE_PX), cty = (int32_t)floor(cwy / TILE_PX);
        if (ctx >= 0 && ctx < ntiles && cty >= 0 && cty < ntiles &&
            pk_tile_loader_route((uint8_t)s_zoom, (uint32_t)ctx, (uint32_t)cty, &center_route) &&
            center_route.scale > 1) {
            char buf[24];
            snprintf(buf, sizeof(buf), pk_i18n_text(PK_TR_MAP_OVERZOOM_FMT),
                    (int)center_route.scale);
            const int w = pk_aa_text_width(buf, PK_AA_XS);
            pk_pfd_darken_rect(fb, PK_DISPLAY_W / 2 - w / 2 - 6, MAP_TOP + 4,
                               PK_DISPLAY_W / 2 + w / 2 + 6, MAP_TOP + 4 + PK_AA_XS_H + 6, 150);
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_DISPLAY_W / 2 - w / 2,
                      MAP_TOP + 7, buf, pk_rgb565(255, 176, 0), PK_AA_XS);
        }
    }

    /* ── ADS-B 目标叠加（own_valid 时才有意义算相对位置；无本机位置也照样
     * 能把目标画在地图上——目标的经纬度与本机是否已知无关）── */
    /*
     * 本页两块大静态数组（快照工作区 7168 B + 标签占位池 4112 B）**全部放
     * PSRAM**，让 map_page 在内部 .bss 里的占用降到 0。
     *
     * 起因是一次很不直观的链接期观察（2026-08-02）：本项目开启了
     * -Wl,--enable-non-contiguous-regions，.dram0.bss 会被拆到高低两段内部
     * RAM 里，而**哪个目标文件落在哪一段是按链接顺序贪心决定的**。给 libmain.a
     * 加一个新目标文件（search_page.c.obj，自身内部 .bss 只有 26 B）就足以让
     * map_page.c.obj 整个 11280 B 从高段翻到低段，把"调度器启动前可用内部堆"
     * 从 66160 B 压到 64384 B —— 低于 check_early_heap.py 的 66000 B 门槛，
     * 开机会在 vApplicationGetTimerTaskMemory 断言上 boot loop。
     *
     * 也就是说：只要这两块还在内部 .bss 里，本页就是一颗随链接顺序抖动的地雷，
     * 下一个往 main 加文件的人会莫名其妙地把机器搞成 boot loop。挪进 PSRAM
     * 之后这条依赖彻底消失（实测余量回到 66160 B 之上）。
     *
     * 访问模式支持这么做：s_scratch 每帧被 aircraft_state_snapshot 整块写一次
     * 再线性读一遍；s_occ 是几十条矩形的顺序比较。都没有随机小访存，PSRAM
     * 带宽绰绰有余，且 pfd.c:119 对同一个快照数组早就是这么做的。
     */
    static EXT_RAM_BSS_ATTR aircraft_t s_scratch[AIRCRAFT_TABLE_CAPACITY];
    size_t n = aircraft_state_snapshot(s_scratch, AIRCRAFT_TABLE_CAPACITY,
                                       now_us, AIRCRAFT_STALE_AGE_US);
    static EXT_RAM_BSS_ATTR rect_t s_occ[MAP_LBL_MAX + 1 + PK_AERO_LAYER_OCC_MAX];
    int nocc = 0;
    /* 航空叠加层分两趟夹住 ADS-B：符号垫在飞机之下，标签排在呼号之后
     * （交通信息优先于机场位置，见 pk_aero_layer.h）。 */
    pk_aero_layer_render_symbols(fb, s_center_lat, s_center_lon, s_zoom, map_rot_deg);
    const uint16_t col_ac  = pk_rgb565(0, 210, 235);
    const uint16_t col_lbl = pk_rgb565(207, 211, 220);
    /*
     * 地面目标独立色相（阶段 4d）：土黄偏黄绿，不进威胁色板（本机没有气压
     * 高度，参与威胁配色本身就是编造信息，见 pfd_draw.h pk_pfd_draw_aircraft_outline
     * 头注）。色值是量出来的，不是随手取的——底图 aviation-dark 样式的道路
     * 是 (150,115,74)（HSV 32°/51%/59%，实测像素，见 sim/capture.py
     * ui-4.3-map-ground），威胁琥珀是 (255,176,0)（35°）；两者都挤在
     * 30°~40° 的橙区。选 (150,190,70)（80°/63%/75%）偏黄绿而非偏橙，与
     * 道路拉开约 48°、与威胁琥珀拉开约 45°，公路密集区实测仍能一眼分辨
     * （见截图 ui-4.3-map-ground.png 肉眼核对结论）。再加一圈深色描边把
     * 符号从路网线条上"抬起来"，双保险。 */
    const uint16_t COL_GROUND      = pk_rgb565(150, 190, 70);
    const uint16_t COL_GROUND_HALO = pk_rgb565(22, 20, 8);

    /*
     * 显著性跟随本机相位（阶段 4d，spec：安全性而非美观——本机在地面时地面
     * 交通才是主要碰撞风险，在空中时反过来）。压暗不隐藏：五边进近时跑道
     * 上的目标正是要看的，隐藏=误导性缺失。
     *
     * 数值曾是 45%，2026-08-04 真机实测改成 75%：本机在地面看空中目标
     * "基本不可见"。深色底图上**亮度就是可见度**，拿亮度当"次要"的编码通道
     * 本身选错了——45% 把青色 (0,210,235) 压到 (0,94,105)，标签那边还要再
     * 叠一层 darken_rect(120)，两层下来就没了。次要目标看不见 = 这个功能在
     * 制造它本想避免的风险。75% 只做"能察觉的差别"，不做"退到背景里"；
     * 主次的强区分交给形状（空中实心 / 地面空心剪影）而不是亮度。
     *
     * 相位 unknown（开机瞬间/IMU 未接/GPS 丢失/状态机没判出来）时两侧都不
     * 压暗：宁可信息多，也不能因为状态机猜错方向而在错误时刻把该看的目标
     * 压暗——那比不做这个功能更危险，见 pk_own_sampler_get_phase() 头注。
     */
    const uint8_t MAP_SALIENCY_DIM_PCT = 75;
    const pk_flight_phase_t own_phase = pk_own_sampler_get_phase();
    const bool own_on_ground = pk_flight_phase_is_ground_family(own_phase);
    const bool own_airborne  = (own_phase == PK_PHASE_AIRBORNE);

    for (size_t i = 0; i < n && i < MAP_LBL_MAX; i++) {
        aircraft_t *a = &s_scratch[i];
        if (!a->have_position) continue;
        if (own_valid && own.icao24 != 0 && a->icao24 == own.icao24) continue;
        double wx, wy;
        lonlat_to_world(a->lon, a->lat, s_zoom, &wx, &wy);
        int sx, sy;
        world_to_screen_rot(wx, wy, cwx, cwy, cos_r, sin_r, &sx, &sy);
        if (sx < -20 || sx > PK_DISPLAY_W + 20 || sy < MAP_TOP - 20 || sy > PK_DISPLAY_H + 20)
            continue;

        uint8_t sal_pct = 100;
        if (own_on_ground && !a->on_ground) sal_pct = MAP_SALIENCY_DIM_PCT; /* 本机在地面：压暗空中目标 */
        else if (own_airborne && a->on_ground) sal_pct = MAP_SALIENCY_DIM_PCT; /* 本机在空中：压暗地面目标 */

        /* 目标航向是绝对方位（真北基准），地图转了 map_rot_deg 之后画在屏幕上
         * 的角度要跟着减，否则目标机头会指向错误方向——这条与本机符号、
         * pk_aero_layer 的罗盘玫瑰/tick 是同一条公式（screen 方位 = 真方位
         * − map_rot_deg）。航向未知时维持既有降级：画成朝屏幕正上方，这是
         * "不知道"的既有约定，不随地图旋转变化。 */
        const float rot = a->have_velocity ? (float)a->heading_deg - map_rot_deg : 0.0f;
        /* 地面目标画空心剪影——与空中实心目标一眼可辨（阶段 4c，见
         * pfd_draw.h pk_pfd_draw_aircraft_outline 头注）。阶段 4d 起颜色也
         * 独立成一套（COL_GROUND），不再借用空中目标的青色。 */
        if (a->on_ground) {
            const uint16_t gcol = pk_pfd_scale_rgb565(COL_GROUND, sal_pct);
            const uint16_t hcol = pk_pfd_scale_rgb565(COL_GROUND_HALO, sal_pct);
            pk_pfd_draw_aircraft_outline(fb, sx, sy, rot, 11, hcol);
            pk_pfd_draw_aircraft_outline(fb, sx, sy, rot, 9, gcol);
        } else {
            const uint16_t acol = pk_pfd_scale_rgb565(col_ac, sal_pct);
            pk_pfd_draw_aircraft(fb, sx, sy, rot, 9, acol);
        }

        char cs[10];
        pk_callsign_display(a->have_callsign, a->callsign, a->icao24,
                            cs, sizeof(cs));

        const int lw = pk_aa_text_width(cs, PK_AA_XS);
        rect_t r = { sx + 10, sy - PK_AA_XS_H / 2 - 1, sx + 10 + lw + 2, sy + PK_AA_XS_H / 2 + 1 };
        bool clash = false;
        for (int j = 0; j < nocc && !clash; j++) if (rect_overlap(&r, &s_occ[j])) clash = true;
        if (!clash) {
            if (nocc < (int)(sizeof(s_occ) / sizeof(s_occ[0]))) s_occ[nocc++] = r;
            pk_pfd_darken_rect(fb, r.x0 - 2, r.y0, r.x1, r.y1, 120);
            /* 标签颜色跟着符号一起压暗——符号暗、标签亮的组合看着像"标签更
             * 重要"，与显著性想传达的信息相反。 */
            pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, sx + 10, sy - PK_AA_XS_H / 2,
                      cs, pk_pfd_scale_rgb565(col_lbl, sal_pct), PK_AA_XS);
        }
    }
    pk_aero_layer_render_labels(fb, s_center_lat, s_center_lon, s_zoom,
                                s_occ, &nocc, (int)(sizeof(s_occ) / sizeof(s_occ[0])),
                                map_rot_deg);

    /* ── 本机航迹线（飞行段，GPS 驱动，不依赖 ADS-B 绑定）──────────────
     * 从 pk_own_sampler 的 ring 取全量点，按相位自适应降采样挑点画（飞行 15s/
     * 地面 60s），避免长轨迹每帧重画上千段拖帧。用 pk_pfd_draw_line（整数
     * Bresenham），禁用 AA 版——AA 画线在地图热路径上是性能陷阱（见 :362 注释）。
     * 轨迹画在本机符号之下、ADS-B 目标之上。 */
    {
        /* Garmin 风格航迹紫：高饱和品红紫，在深色底图上醒目，与 ADS-B 亮青
         * (0,210,235)、FIX 偏蓝紫 (160,110,235) 色相区分。Garmin Pilot/G3X 的
         * 飞行轨迹即用此色系。 */
        const uint16_t TRAIL_COL = pk_rgb565(220, 40, 210);
        uint32_t tn, tstart;
        const pk_own_trail_point_t *tr = pk_own_sampler_get_trail(&tn, &tstart);
        if (tr && tn >= 2) {
            int prev_sx = -1, prev_sy = -1;
            uint32_t last_drawn_ts = 0;
            /* 可视区外扩 4px（3px 粗线 + 1px 余量），线段两端都在外扩框外且
             * 不穿过它时整段跳过——pk_pfd_draw_line 虽逐点裁剪（put_pixel 有
             * 边界检查），但屏外长线段会白跑完整条 Bresenham 循环，地图缩小时
             * 大部分轨迹在屏外，这是掉帧主因。 */
            const int clip_x0 = -4, clip_x1 = PK_DISPLAY_W + 4;
            const int clip_y0 = -4, clip_y1 = PK_DISPLAY_H + 4;
            for (uint32_t i = 0; i < tn; i++) {
                /* 循环缓冲：从最老点 tstart 起按下标掩码绕一圈。ring 满回绕后线性
                 * 遍历会把最新点与最老点连成一条横穿地图的直线（CAP=2048≈34min
                 * 1Hz 写满）。降采样跳过未到间隔的中间点（首尾点必画，轨迹连到
                 * 当前位置）；continue 在投影之前——跳过的点不做昂贵的 lonlat_to_world。 */
                const pk_own_trail_point_t *p =
                    &tr[(tstart + i) & (PK_OWN_TRAIL_CAP - 1)];
                const bool ground = pk_flight_phase_is_ground_family(
                                        (pk_flight_phase_t)p->phase);
                const uint32_t interval = ground ? 60000u : 15000u;
                if (i > 0 && i < tn - 1 && p->ts_1k - last_drawn_ts < interval)
                    continue;
                double wx, wy;
                lonlat_to_world((double)p->lon_e7 / 1e7, (double)p->lat_e7 / 1e7,
                                s_zoom, &wx, &wy);
                int sx, sy;
                world_to_screen_rot(wx, wy, cwx, cwy, cos_r, sin_r, &sx, &sy);
                if (prev_sx >= 0) {
                    /* 屏外整段剔除：两端同在外扩框一侧（且线段不穿过）→ 跳过。
                     * 这是廉价的包围盒测试，不画屏外的 3 遍 draw_line。 */
                    const bool p_out_x = (prev_sx < clip_x0 && sx < clip_x0) ||
                                         (prev_sx >= clip_x1 && sx >= clip_x1);
                    const bool p_out_y = (prev_sy < clip_y0 && sy < clip_y0) ||
                                         (prev_sy >= clip_y1 && sy >= clip_y1);
                    if (!p_out_x && !p_out_y) {
                        /* 3 px 粗线：对角偏移画三遍（照 :386 手柄图标的粗线范例）。
                         * 不用 draw_line_aa 的 width 参数——AA 在地图热路径是性能陷阱。 */
                        for (int k = -1; k <= 1; ++k)
                            pk_pfd_draw_line(fb, prev_sx + k, prev_sy + k,
                                             sx + k, sy + k, TRAIL_COL);
                    }
                }
                prev_sx = sx; prev_sy = sy;
                last_drawn_ts = p->ts_1k;
            }
        }
    }


    /* ── 本机符号：跟随模式画在视口中心；手动平移模式画在它真实的地理投影位置
     * （可能滚出视口之外，此时自然不画——离开可见范围本来就不该出现）。
     * GPS 无 fix：灰显于上次已知位置；从未有过位置则整个不画（同 traffic 页的
     * "无本机符号"降级逻辑，画一个我不知道在哪的"我在这"就是说谎）。
     *
     * 图标旋转角 = own_rot_deg − map_rot_deg：north-up 时 map_rot_deg=0，与
     * 原来行为一致；heading-up 时两者相等，画出来恒为 0（机头指屏幕正上方）
     * ——这正是 heading-up 这个名字的字面意思。 ── */
    if (own_valid) {
        int ox, oy;
        if (s_follow) { ox = MCX; oy = MCY; }
        else {
            double owx, owy;
            lonlat_to_world(own.lon, own.lat, s_zoom, &owx, &owy);
            world_to_screen_rot(owx, owy, cwx, cwy, cos_r, sin_r, &ox, &oy);
        }
        if (ox >= 0 && ox < PK_DISPLAY_W && oy >= MAP_TOP && oy < PK_DISPLAY_H) {
            const uint8_t *ac = pk_icon_bitmap
                              + (size_t)PK_ICON_OWNSHIP * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
            pk_aa_blit_4bpp_rot(fb, PK_DISPLAY_W, PK_DISPLAY_H, ox, oy, ac, PK_ICON_W, PK_ICON_H,
                               own_rot_deg - map_rot_deg, pk_rgb565(255, 255, 255));
        }
    } else if (s_have_last_own) {
        double owx, owy;
        lonlat_to_world(s_last_own_lon, s_last_own_lat, s_zoom, &owx, &owy);
        int ox, oy;
        world_to_screen_rot(owx, owy, cwx, cwy, cos_r, sin_r, &ox, &oy);
        if (ox >= 0 && ox < PK_DISPLAY_W && oy >= MAP_TOP && oy < PK_DISPLAY_H) {
            const uint8_t *ac = pk_icon_bitmap
                              + (size_t)PK_ICON_OWNSHIP * (((size_t)PK_ICON_W * PK_ICON_H + 1) / 2);
            /* 陈旧位置：位置本身已经不可信，航向更不可信（可能是几分钟前的），
             * 保持朝北而不是画一个会误导的角度——本分支 own_valid 恒 false，
             * map_rot_deg 因此也恒为 0（见上面的计算），0.0f 与"指向真北"
             * 完全等价，不用额外写 -map_rot_deg。 */
            pk_aa_blit_4bpp_rot(fb, PK_DISPLAY_W, PK_DISPLAY_H, ox, oy, ac, PK_ICON_W, PK_ICON_H,
                               0.0f, pk_rgb565(140, 148, 158));
        }
    }

    /* ── 搜索 PIN：画在所有地理符号之上、UI 铬层之下。它是"我刚查的就是
     * 这个"的唯一凭据，被交通目标压住就白放了。 ── */
    if (s_pin_valid) {
        double pwx, pwy;
        lonlat_to_world(s_pin_lon, s_pin_lat, s_zoom, &pwx, &pwy);
        int px, py;
        world_to_screen_rot(pwx, pwy, cwx, cwy, cos_r, sin_r, &px, &py);
        /* 余量给足：PIN 主体在落点上方 37 px，标签还要再高一截，
         * 落点刚滚出下沿时上半截仍该露出来。 */
        if (px > -40 && px < PK_DISPLAY_W + 40 &&
            py > MAP_TOP - 8 && py < PK_DISPLAY_H + 60)
            draw_search_pin(fb, px, py, s_pin_label);
    }

    /* ── 比例尺基准：中心纬度的米/像素（Web Mercator 随纬度变形，用当前
     * 中心点的纬度算，跟屏幕中心那一列最准）── */
    double mpp = 156543.03392 * cos(s_center_lat * M_PI / 180.0) / (double)(1u << s_zoom);

    /* ── 拔卡提示留给 pk_tile_loader.c 的 toast（pfd.c 统一叠加），本页
     * 不重复画——两处都画会互相压。 ── */

    draw_chrome(fb, mpp, map_rot_deg, n);
}

/* ── 触摸 ─────────────────────────────────────────────────────────── */
bool pk_map_page_touch(int x, int y)
{
    /* FAB 是浮在页面之上的 LVGL 控件，落在它身上的按下必须**不吃**、返回 false
     * 让给 LVGL——touch_gt911 的分发是 `eaten = (mode==MAP && 本函数())` 的短路
     * 契约，本函数以前无论点哪儿都返回 true，等于把 FAB 的点击全吞了（2026-08-01
     * 实测：点过缩放后发现 dock FAB 点不动）。
     *
     * 只在"还没拿到这次按压"时让路：已经在拖地图的过程中手指划过 FAB，那次
     * 拖动仍归本页面，不能中途易主。 */
    if (!s_press_active) {
        int fx, fy, fw, fh;
        if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh) &&
            x >= fx && x < fx + fw && y >= fy && y < fy + fh)
            return false;
    }

    if (!s_press_active) {
        int search_y, zin_y, zout_y, rc_y, orient_y;
        btn_layout(&search_y, &zin_y, &zout_y, &rc_y, &orient_y);
        /* 命中按钮同样要把这次按压标记为 active：触摸驱动在手指按住期间会
         * 持续上报，不置位的话每一帧都重新走一遍这里——一次点击涨好几级
         * zoom（2026-08-01 实测）。s_btn_down 之后充当"本次按压已归属
         * 某个按钮"的凭据，下面的重复上报据此直接吃掉。 */
        /* 搜索钮。与下面几个一样必须置 s_press_active——触摸驱动在手指按住
         * 期间持续上报，不置位就会每帧重开一次搜索页（历史 bug 8cf64ec 的
         * 教训，那次是一次点击涨好几级 zoom）。 */
        if (hit_btn(x, y, BTN_SEARCH_X, search_y)) {
            s_btn_down = 3;
            s_press_active = true;
            pk_map_page_on_search();
            return true;
        }
        /*
         * 「回结果列表」。判在最前面是因为它离顶栏最近，与别的按钮不相交，
         * 次序其实无所谓；置 s_press_active 的理由与上面逐字相同（触摸驱动
         * 按住期间持续上报，不置位就会每帧恢复一次）。
         *
         * **不做右滑手势**：本页的拖动就是平移地图，一次右滑与"我要往东看"
         * 逐帧同形，只能靠起手位置或速度阈值去猜。猜错的代价是不对称的——
         * 猜成返回会把用户正在看的视口整个换掉，而多点一下按钮只是多点一下。
         * 二级页面那套 LV_DIR_RIGHT 在这里用不上：那些页底下没有一个会吃掉
         * 全屏拖动的画布。
         */
        if (pk_map_page_sheet_collapsed() &&
            hit_btn(x, y, BTN_SHEET_X, sheet_btn_y())) {
            s_btn_down = 5;
            s_press_active = true;
            pk_map_page_on_sheet_restore();
            return true;
        }
        if (hit_btn(x, y, BTN_ZIN_X, zin_y)) {
            s_btn_down = 1;
            if (s_zoom < MAP_ZOOM_MAX) { s_zoom++; pk_tile_loader_bump_view(); }
            s_press_active = true;
            return true;
        }
        if (hit_btn(x, y, BTN_ZOUT_X, zout_y)) {
            s_btn_down = 2;
            if (s_zoom > MAP_ZOOM_MIN) { s_zoom--; pk_tile_loader_bump_view(); }
            s_press_active = true;
            return true;
        }
        /* 朝向切换：在两种投影间来回切，与交通页左下角那枚同一行为
         * （traffic_page.c 的 BTN_ORI 分支）。 */
        if (hit_btn(x, y, BTN_ORIENT_X, orient_y)) {
            s_btn_down = 4;
            pk_map_orient_set(pk_map_orient_get() == PK_MAP_HEADING_UP
                                  ? PK_MAP_NORTH_UP : PK_MAP_HEADING_UP);
            return true;
        }
        if (!s_follow && hit_btn(x, y, BTN_RECENTER_X, rc_y)) {
            s_btn_down = 0;
            s_follow = true;
            pk_tile_loader_bump_view();
            s_press_active = true;
            return true;
        }
        s_press_active = true;
        s_press_moved  = false;
        s_press_lx = x;
        s_press_ly = y;
        return true;
    }

    /* 本次按压归某个按钮所有：一次按压只算一次，手指赖在按钮上也不拖地图。
     * 松手由 pk_map_page_touch_up() 清 s_btn_down/s_press_active。 */
    if (s_btn_down >= 0) return true;

    const int dx = x - s_press_lx;
    const int dy = y - s_press_ly;
    if (dx != 0 || dy != 0) {
        double wx, wy;
        lonlat_to_world(s_center_lon, s_center_lat, s_zoom, &wx, &wy);
        wx -= dx;
        wy -= dy;
        world_to_lonlat(wx, wy, s_zoom, &s_center_lon, &s_center_lat);
        s_follow = false;
        s_press_moved = true;
        s_press_lx = x;
        s_press_ly = y;
    }
    return true;
}

void pk_map_page_touch_up(void)
{
    /*
     * 一次「落在地图上、没按到按钮、也没拖动过」的点击有两种去处：
     *
     *   ① 点中了某个航空要素   → 机场进详情页，导航台/FIX 落一枚 PIN；
     *   ② 点在空白处           → 清掉 PIN。
     *
     * 判定顺序不能反：先问命中，没命中才当空白。判据里的"没拖动过"是必须的
     * ——平移途中手指自然会停一下，把那当成点击就成了"一拖就弹详情"。
     *
     * 命中测试放在 touch_up 而不是 touch：**按压归属完全不受影响**。按下那
     * 一刻这里照旧返回 true（地图区归本页），FAB / 三枚按钮 / 拖动平移的分支
     * 一行都没动——map_page.c 那两处历史 bug（FAB 被吞、按住每帧重复触发）
     * 的闸门（s_press_active / s_btn_down）都在 touch() 里，这里够不着。
     */
    const bool tap = s_press_active && s_btn_down < 0 && !s_press_moved;
    if (tap) {
        pk_aero_layer_hit_t hit;
        /* 投影参数（含旋转角 s_map_rot_deg）用本页当前的视图状态，与刚画完
         * 的那一帧完全一致——看得见的就是点得中的。 */
        if (pk_aero_layer_hit_test(s_press_lx, s_press_ly, s_center_lat,
                                   s_center_lon, s_zoom, s_map_rot_deg, &hit)) {
            if (hit.kind == PK_AERO_LAYER_KIND_AIRPORT) {
                pk_map_page_on_apt_detail(hit.idx);
            } else {
                /* 导航台 / FIX 没有详情可看（数据包里只有 ident+坐标+频率），
                 * 但"这个三角是什么"是个真实的问题——落一枚带 ident 的 PIN
                 * 就地回答它，与搜索结果点导航台落到的是同一个终态。 */
                pk_map_page_set_pin(hit.lat, hit.lon, hit.code);
            }
        } else {
            /* PIN 的清除时机必须是用户能想到的动作，而不是某个定时器：定时
             * 消失会让人以为"刚才那个机场没找到"。另一条清除路径是再搜一次
             * （set_pin 直接覆盖）。 */
            s_pin_valid = false;
        }
    }
    s_press_active = false;
    s_press_moved  = false;
    s_btn_down = -1;
}

/* ── 搜索页的两个入口（见 map_page.h）───────────────────────────── */

void pk_map_page_goto(double lat, double lon, int zoom)
{
    if (zoom < MAP_ZOOM_MIN) zoom = MAP_ZOOM_MIN;
    if (zoom > MAP_ZOOM_MAX) zoom = MAP_ZOOM_MAX;
    s_zoom       = (uint8_t)zoom;
    s_center_lat = lat;
    s_center_lon = lon;
    /* 关跟随是这个函数的重点：不关的话，下一帧本机位置一到就把视口拽回去，
     * 用户看到的是"跳过去又弹回来"。回中钮此时会重新出现，是回去的路。 */
    s_follow     = false;
    pk_tile_loader_bump_view();
    pk_aero_layer_notify_view(s_center_lat, s_center_lon, s_zoom);
}

void pk_map_page_set_pin(double lat, double lon, const char *label)
{
    s_pin_lat   = lat;
    s_pin_lon   = lon;
    s_pin_valid = true;
    snprintf(s_pin_label, sizeof(s_pin_label), "%s", label ? label : "");
}

void pk_map_page_clear_pin(void)
{
    s_pin_valid = false;
}

/* 弱符号默认实现：模拟器与 host 单测不必把搜索页链进来（同 pk_ui_nav.c 的
 * 各个 on_* 回调）。固件侧由 pfd.c 提供强符号。 */
__attribute__((weak)) void pk_map_page_on_search(void)
{
}

#ifdef PK_SIM_BUILD
void pk_map_page_sim_tap_sheet_back(void)
{
    const int by = sheet_btn_y();
    pk_map_page_touch(BTN_SHEET_X + BTN_D / 2, by + BTN_D / 2);
    pk_map_page_touch_up();
}
#endif

/* 默认「没有收起的 sheet」：不链模态栈的构建里那枚返回钮就从不出现，
 * 而不是画一枚点了没反应的按钮。 */
__attribute__((weak)) bool pk_map_page_sheet_collapsed(void)
{
    return false;
}

__attribute__((weak)) void pk_map_page_on_sheet_restore(void)
{
}

__attribute__((weak)) void pk_map_page_on_apt_detail(uint32_t apt_idx)
{
    (void)apt_idx;
}
