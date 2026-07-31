/*
 * main.c — Pilot Kit Box PFD 的 PC 模拟器（SDL2）。
 *
 * 为什么存在
 * ----------
 * 4.3" 触摸屏迁移期间，UI 布局要反复调整。每改一行就烧一次板子太慢，
 * 而固件里的 PFD 绘制模块恰好都是纯函数 —— 签名统一为
 *
 *     void pk_pfd_xxx_render(uint16_t *fb, const pk_pfd_xxx_t *data);
 *
 * 只吃数据、只写 framebuffer，除了 esp_attr.h 里几个段属性宏之外不碰
 * 任何 ESP-IDF API。于是把同一批 .c 文件拉到 PC 上编译，喂 mock 数据，
 * 把 framebuffer 丢进 SDL 窗口，就能实时看到和真机逐像素一致的画面。
 *
 * 这里刻意不引入 LVGL：第一步只验证「现有绘制代码在 PC 上跑得对」，
 * 把变量降到最少。LVGL 控件层是下一步的事。
 *
 * 用法
 * ----
 *     cd sim && cmake -B build && cmake --build build && ./build/pkbox_sim
 *
 *     ESC / Q   退出
 *     空格      暂停 / 继续动画（方便盯住某一帧看细节）
 *     S         把当前帧存成 BMP，用于和设计稿逐像素比对
 *     T         切换一条 toast 提示（验证它压在 dock / FAB 之上）
 *     B         进出二级页面（验证返回栏与 FAB 变 ←）
 *     ← →      手动步进 roll，观察极限姿态
 *
 * 「极端无数据」开关
 * ------------------
 * 改完 UI 除了压最长文本/目标扎堆/数值极值，**还要顺手截一轮空态**——用户
 * 第一次开机、或外设一个都没接时看到的就是它，而那一侧此前从没被系统截过，
 * 交通页那句「无本机位置」被本机符号压住整整几个版本没人发现。
 *
 *     PK_SIM_EMPTY=1        总开关：以下全部打开（= 出厂开机、外设全没接）
 *
 *     PK_SIM_NO_IMU=1       BNO085 没接（姿态/航向/调平同时失效）
 *     PK_SIM_NO_OWN=1       没有本机位置（GPS 未定位且未绑定 ADS-B 本机）
 *     PK_SIM_NO_BARO=1      BMP388 没接
 *     PK_SIM_NO_GPS=1       GPS 模块没接 / 无定位
 *     PK_SIM_NO_TRAFFIC=1   一架 ADS-B 目标都没收到
 *     PK_SIM_TFC_FAR=1      收到了目标但全部在量程外（与上一条观感同、成因异）
 *     PK_SIM_TFC_BARE=1     目标只有位置，呼号/高度/速度全缺（各列降级）
 *     PK_SIM_NO_APPDESC=1   读不到 app 描述符（关于页版本显示 "?"）
 *     PK_SIM_DIAG_OK=1      反向开关：把诊断页整体切成「一切正常」
 *
 * 版面位置类（与空态常配合用）：
 *     PK_SIM_DIAG_SCROLL / PK_SIM_SET_SCROLL / PK_SIM_ABOUT_SCROLL=<px>
 *     PK_SIM_DIAG_DETAIL=<0..11>   直接进某个子系统的详情页
 *
 * 常用组合已经登记在 capture.py 的 SCENES 里（empty-4.3-* 那一组），
 * 跑 `python3 sim/capture.py --only empty` 一次出齐。
 */
/* 用 <SDL.h> 而非 <SDL2/SDL.h>：sdl2-config --cflags 给出的是
 * -I<prefix>/include/SDL2，头文件已在搜索路径根部。这也是 SDL 官方
 * 配合 sdl2-config 时的推荐写法，Linux/macOS 通用。 */
#include <SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv_backend.h"
#include "pk_ui_nav.h"
#include "about_page.h"
#include "adsb_list.h"
#include "config_devname.h"   /* PK_DEVNAME_MAX_LEN —— 键盘页的上限跟真机同一个数 */
#include "diag_page.h"
#include "keyboard_page.h"
#include "settings_page.h"
#include "boot_splash.h"
#include "traffic_page.h"

#include "display.h"
#include "i18n.h"
#include "pfd_attitude.h"
#include "mock_runtime.h"
#include "pfd_hsi.h"
#include "pfd_hsi_traffic.h"
#include "pfd_infobox.h"
#include "pfd_speed_tape.h"
#include "pfd_statusbar.h"
#include "pfd_tape.h"

/* 窗口放大倍数。真机 320×240 在 Retina 上太小看不清细节，放大 3 倍
 * 观察；改到 800×480 之后可以调成 1~2。 */
#define ZOOM 3

/* 模拟器主循环频率。真机 PFD 约 30 FPS，这里也按 30 跑，好让动画
 * 观感和真机一致。 */
#define TARGET_FPS 30

/* ------------------------------------------------------------------ */
/* mock 数据：让画面动起来，才能看出刻度、指针、滚动是否正确           */
/* ------------------------------------------------------------------ */
typedef struct {
    float t;             /* 累计秒数 */
    float roll_bias;     /* ← → 手动叠加的 roll，用于逼出极限姿态 */
    bool  paused;
} sim_state_t;

/*
 * 「这一路数据在不在」的判定，与 mock_runtime / page_stub 用同一个总开关。
 *
 * 此前这个函数把每个 valid 位**恒填 true**：于是 PK_SIM_EMPTY 下别的页面都
 * 空了，PFD 仍是一屏姿态齐全、速度高度俱在的假象，各仪表的降级显示一次都没
 * 被截到过——而产品负责人手上那台盒子（IMU/气压/GPS/SDR 全没接）开机第一眼
 * 看到的就是 PFD。数据源按真机接线对上：
 *   姿态 / 航向      ← BNO085   (PK_SIM_NO_IMU)
 *   地速 / 本机高度  ← ADS-B 绑定或 GPS (PK_SIM_NO_OWN)
 *   气压高度 / 升降率 ← BMP388   (PK_SIM_NO_BARO)
 *   星数 / 定位      ← ATGM336H (PK_SIM_NO_GPS)
 */
typedef struct {
    bool no_imu, no_own, no_gps, no_baro, no_traffic, empty;
} sim_lack_t;

static sim_lack_t sim_lack(void)
{
    sim_lack_t l = {
        .no_imu     = pk_sim_flag("PK_SIM_NO_IMU"),
        .no_own     = pk_sim_flag("PK_SIM_NO_OWN"),
        .no_gps     = pk_sim_flag("PK_SIM_NO_GPS"),
        .no_baro    = pk_sim_flag("PK_SIM_NO_BARO"),
        .no_traffic = pk_sim_flag("PK_SIM_NO_TRAFFIC"),
        /* 总开关本身也当一路用：录制、蓝牙连接、超温这几项不是「传感器缺数据」
         * 而是「什么都还没发生」，出厂开机一律是灭的。 */
        .empty      = pk_sim_flag("PK_SIM_EMPTY"),
    };
    return l;
}

static void mock_fill(const sim_state_t *st,
                      pk_pfd_imu_t *imu, pk_pfd_hsi_t *hsi,
                      pk_pfd_alt_tape_t *alt, pk_pfd_speed_tape_t *spd,
                      pk_pfd_status_t *stat)
{
    const float t = st->t;
    const sim_lack_t k = sim_lack();

    /* 三个周期互质，避免动作同步显得假 */
    imu->valid     = !k.no_imu;
    imu->roll_deg  = 25.0f * sinf(t * 0.37f) + st->roll_bias;
    imu->pitch_deg = 10.0f * sinf(t * 0.23f);

    hsi->imu_valid = !k.no_imu;
    hsi->yaw_deg   = fmodf(t * 6.0f, 360.0f);

    alt->valid       = !k.no_own;
    alt->altitude_ft = 23225 + (int)(1200.0f * sinf(t * 0.11f));

    spd->valid           = !k.no_own;
    spd->ground_speed_kt = 378 + (int)(60.0f * sinf(t * 0.17f));

    stat->imu_valid      = !k.no_imu;
    stat->yaw_deg        = hsi->yaw_deg;
    stat->aircraft_count = k.no_traffic ? 0 : 6;
    stat->gps_have_fix   = !k.no_gps;
    stat->gps_sats       = k.no_gps ? 0 : 17;
    /* 满载：所有状态位同时点亮并取最坏宽度，用于验证顶栏是否溢出。 */
    stat->rec_active     = !k.empty;
    stat->ble_connected  = !k.empty;
    stat->batt_valid     = true;   /* 电池在板上，没有「没接」这一说 */
    /* 电量图标有七档刻度 + 低电告警 + 充电动画，逐档验证需要能改电量而
     * 不重编。走环境变量而非命令行参数：--shot 已占用位置参数，且这类
     * "临时拨一个值看看"的旋钮以后还会有别的。 */
    const char *batt_env = getenv("PK_SIM_BATT");
    stat->batt_pct       = batt_env ? (uint8_t)atoi(batt_env) : 100;
    stat->batt_charging  = getenv("PK_SIM_CHARGING") != NULL;
    stat->uptime_ms      = (uint32_t)(t * 1000.0f);

    /* 把本帧姿态推给运行时桩，交通目标才会随航向绕罗盘转。 */
    pk_mock_update(hsi->yaw_deg, alt->altitude_ft);
    stat->temp_warn      = !k.empty;
    stat->temp_c         = k.empty ? 42 : 78;
}

/*
 * 右下信息框与左下速度框。
 *
 * 抽成函数是因为 headless 与交互两条路径本来各抄了一份**一模一样**的初始化，
 * 于是给它们加降级开关就得改两处、漏一处就是两种画面。
 */
static void mock_fill_boxes(pk_pfd_infobox_t *ib, pk_pfd_leftbox_t *lb,
                            const pk_pfd_alt_tape_t *alt,
                            const pk_pfd_speed_tape_t *spd)
{
    const sim_lack_t k = sim_lack();

    memset(ib, 0, sizeof(*ib));
    ib->baro_valid   = !k.no_baro;
    ib->baro_alt_ft  = alt->altitude_ft - 120;
    ib->alt_valid    = !k.no_own;
    ib->alt_ft       = alt->altitude_ft;
    /* 升降率两个来源：ADS-B 报文里的 VS，或气压高度的微分。两条都断了才没有。 */
    ib->vs_valid     = !(k.no_own && k.no_baro);
    ib->vs_fpm       = -640;
    ib->vs_from_adsb = !k.no_own;

    memset(lb, 0, sizeof(*lb));
    lb->speed_valid = !k.no_own;
    lb->kmh = (int)(spd->ground_speed_kt * 1.852f + 0.5f);
    lb->src = PK_PFD_SRC_ADSB;
    /* 呼号只有绑定了本机才有。没绑就留空串——不是留上一次的 "CES2158"，
     * 那会让「没有本机」这一态看起来像绑着一架幽灵飞机。 */
    if (!k.no_own) snprintf(lb->label, sizeof(lb->label), "CES2158");
}

/* ------------------------------------------------------------------ */
/* framebuffer → SDL 纹理                                              */
/* ------------------------------------------------------------------ */
/*
 * 固件里的 pk_rgb565() 把像素预先 swap 成大端，因为 ST7789 的 SPI 线上
 * 就是那个字节序。PC 这边是小端，直接喂给 SDL 会红蓝错乱，所以拷贝时
 * 换回来。每帧 O(W×H)，320×240 才 76 K 次，PC 上可以忽略。
 */
static void fb_to_texture(const uint16_t *fb, SDL_Texture *tex)
{
    void *pixels = NULL;
    int   pitch  = 0;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) return;

    uint16_t *dst = (uint16_t *)pixels;
    const int stride = pitch / (int)sizeof(uint16_t);
    for (int y = 0; y < PK_DISPLAY_H; ++y) {
        const uint16_t *src_row = fb + (size_t)y * PK_DISPLAY_W;
        uint16_t       *dst_row = dst + (size_t)y * stride;
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            const uint16_t v = src_row[x];
            dst_row[x] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
    SDL_UnlockTexture(tex);
}

static void save_bmp(const uint16_t *fb, int seq)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(
        0, PK_DISPLAY_W, PK_DISPLAY_H, 16, SDL_PIXELFORMAT_RGB565);
    if (!s) return;
    uint16_t *dst = (uint16_t *)s->pixels;
    const int stride = s->pitch / (int)sizeof(uint16_t);
    for (int y = 0; y < PK_DISPLAY_H; ++y)
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            const uint16_t v = fb[(size_t)y * PK_DISPLAY_W + x];
            dst[(size_t)y * stride + x] = (uint16_t)((v >> 8) | (v << 8));
        }
    char path[64];
    snprintf(path, sizeof(path), "frame_%03d.bmp", seq);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    printf("saved %s (%dx%d)\n", path, PK_DISPLAY_W, PK_DISPLAY_H);
}

/*
 * headless 截图模式：--shot <秒> [输出名]
 *
 * 不开窗口，直接把「动画进行到第 N 秒」那一帧渲染出来存成 BMP。
 * 用途是 CI 回归和肉眼比对 —— 改完布局跑一条命令就能拿到图，
 * 不用人守在窗口前按 S。
 */
static int run_headless(float at_sec, const char *out)
{
    /* 经 LVGL 走一遍，而不是直接看 PFD 写的那块缓冲：截图要反映的是**合成
     * 之后**的画面，将来叠上 FAB / dock 才不会漏掉图层间的相互影响。 */
    uint16_t *fb = pk_sim_lv_init();
    pk_ui_nav_init();
    /* 截图时展开 dock：它默认收起，否则评审看不到这一屏最占地方的状态。 */
    /* PK_SIM_FAB=left 把 FAB 吸到左缘，用来核对 dock 是否跟着反向铺开。 */
    {
        const char *side = getenv("PK_SIM_FAB");
        if (side && side[0] == 'l') pk_ui_nav_set_fab_side(true);
    }
    if (getenv("PK_SIM_DOCK")) pk_ui_nav_set_dock_open(true);
    if (getenv("PK_SIM_TOAST")) pk_ui_nav_toast("已绑定本机", false);
    /* PK_SIM_SUB=1 进入二级页，核对返回栏与 FAB 图标是否都切到「←」。 */
    if (getenv("PK_SIM_SUB")) pk_ui_nav_set_subpage(true, "诊断");
    /*
     * 诊断详情页自带 backbar，不必再手动加 PK_SIM_SUB。
     *
     * 真机上 pk_diag_page_touch_up() 点开卡片时就顺手调了 set_subpage()，两者
     * 是同一个动作的两半；模拟器这边靠环境变量直接跳进详情，漏掉后半截，截图
     * 里 backbar 那条就是空的——顶部三行的次序错了整整四轮没人看出来，正是因
     * 为截图上根本没有 backbar。这里补齐，让截图和真机是同一件事。
     *
     * parent_title 与 diag_page.c 走**同一个词条**而不是抄一份英文字面量：
     * 抄下来的那份不会跟着 PK_SIM_LANG 切，于是中文截图上顶着一条英文
     * backbar，量出来的宽度也就不是中文真机的宽度——而中文比英文短得多，
     * 正是要靠截图确认的那个差异。
     */
    {
        const char *pg = getenv("PK_SIM_PAGE");
        if (getenv("PK_SIM_DIAG_DETAIL") && pg && strcmp(pg, "diag") == 0)
            pk_ui_nav_set_subpage(true, pk_i18n_text(PK_TR_DIAG_TITLE));
    }
    sim_state_t st = { .t = at_sec, .roll_bias = 0.0f, .paused = true };

    pk_pfd_imu_t imu; pk_pfd_hsi_t hsi; pk_pfd_alt_tape_t alt;
    pk_pfd_speed_tape_t spd; pk_pfd_status_t stat;
    mock_fill(&st, &imu, &hsi, &alt, &spd, &stat);

    /* PK_SIM_PAGE=<name> 渲染整页视图而不是 PFD——那几页正在从 2.4″ 迁到
     * 800×480，需要能截图比对。名字与 dock 页签一一对应。 */
    const char *page = getenv("PK_SIM_PAGE");
    if (page != NULL && page[0] != '\0') {
        if (strcmp(page, "about") == 0) {
            pk_about_page_render(fb);
        } else if (strcmp(page, "splash") == 0) {
            pk_boot_splash_render(fb);
        } else if (strcmp(page, "traffic") == 0) {
            pk_traffic_page_render(fb);
        } else if (strcmp(page, "list") == 0) {
            pk_adsb_list_render(fb);
        } else if (strcmp(page, "diag") == 0) {
            pk_diag_page_render(fb);
        } else if (strcmp(page, "settings") == 0) {
            pk_settings_page_render(fb);

        } else if (strcmp(page, "keyboard") == 0) {
            /* 键盘编辑器。真机上它由设置页那一行点开（pk_settings_apply
             * 的 case 8），模拟器直接调 open() —— 这与诊断详情页那边用
             * PK_SIM_DIAG_DETAIL 跳进去是同一个套路。
             *
             * PK_SIM_KBD=<初值> 摆不同的输入态：不给 = 空输入，
             * 给满 PK_DEVNAME_MAX_LEN 个字符 = 计数器转警示色的那一态。
             * 上限跟着真机的宏走，不写字面量：写死过一次 10，上限一改
             * 模拟器就在演一个真机上不存在的键盘。 */
            pk_keyboard_page_open(PK_TR_SETTINGS_DEVNAME,
                                  getenv("PK_SIM_KBD"), PK_DEVNAME_MAX_LEN);
            pk_keyboard_page_render(fb);
        } else {
            fprintf(stderr, "未知的 PK_SIM_PAGE=%s\n", page);
            return 2;
        }
        goto page_done;
    }

    pk_pfd_attitude_render(fb, &imu);
    pk_pfd_statusbar_render(fb, &stat);
    pk_pfd_alt_tape_render(fb, &alt);
    pk_pfd_speed_tape_render(fb, &spd);
    pk_pfd_hsi_render(fb, &hsi);
    pk_pfd_hsi_traffic_render(fb);   /* 罗盘外圈的交通目标，顺序同 pfd.c */
    {
        pk_pfd_infobox_t ib;
        pk_pfd_leftbox_t lb;
        mock_fill_boxes(&ib, &lb, &alt, &spd);
        pk_pfd_infobox_render(fb, &ib);
        pk_pfd_leftbox_render(fb, &lb);
    }

page_done:;   /* 空语句：C17 里标签后面不能直接跟声明（-Wc23-extensions） */
    /* 多合成几帧：首帧 LVGL 只画屏幕底色，canvas 要到第二帧才落到 s_screen；
     * 而 dock 的滑出动画有 180 ms，两帧远不够，截出来会是它还在屏外的样子。
     * 12 帧 ≈ 400 ms，动画收敛且离 5 s 自动收起还远。 */
    const uint16_t *shot = NULL;
    /* PK_SIM_FRAMES 可加长合成帧数，用来验证「5 s 无操作自动收起」这类
     * 靠定时器触发的行为——默认 12 帧只够动画收敛。 */
    const char *nf = getenv("PK_SIM_FRAMES");
    const int frames = nf ? atoi(nf) : 12;
    for (int i = 0; i < frames; ++i) shot = pk_sim_lv_render(33);

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(
        0, PK_DISPLAY_W, PK_DISPLAY_H, 16, SDL_PIXELFORMAT_RGB565);
    if (!s) { fprintf(stderr, "surface: %s\n", SDL_GetError()); return 1; }

    uint16_t *dst = (uint16_t *)s->pixels;
    const int stride = s->pitch / (int)sizeof(uint16_t);
    for (int y = 0; y < PK_DISPLAY_H; ++y)
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            const uint16_t v = shot[(size_t)y * PK_DISPLAY_W + x];
            dst[(size_t)y * stride + x] = (uint16_t)((v >> 8) | (v << 8));
        }

    const int rc = SDL_SaveBMP(s, out);
    SDL_FreeSurface(s);
    if (rc != 0) { fprintf(stderr, "SaveBMP: %s\n", SDL_GetError()); return 1; }

    printf("%s  %dx%d  t=%.1fs  roll=%+.1f pitch=%+.1f yaw=%.0f alt=%d gs=%d\n",
           out, PK_DISPLAY_W, PK_DISPLAY_H, at_sec,
           (double)imu.roll_deg, (double)imu.pitch_deg, (double)hsi.yaw_deg,
           alt.altitude_ft, spd.ground_speed_kt);
    return 0;
}

int main(int argc, char **argv)
{
    /* headless 分支要在 SDL_Init(VIDEO) 之前判掉：存 BMP 只需要
     * surface，不需要视频后端，这样在没有显示器的环境也能跑。 */
    if (argc >= 3 && strcmp(argv[1], "--shot") == 0) {
        const float at  = (float)atof(argv[2]);
        const char *out = (argc >= 4) ? argv[3] : "shot.bmp";
        return run_headless(at, out);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    char title[128];
    snprintf(title, sizeof(title),
             "Pilot Kit Box — PFD sim  %dx%d  (zoom %dx)",
             PK_DISPLAY_W, PK_DISPLAY_H, ZOOM);

    SDL_Window *win = SDL_CreateWindow(
        title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        PK_DISPLAY_W * ZOOM, PK_DISPLAY_H * ZOOM, SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }

    /* 保持像素硬边缘 —— PFD 的刻度线只有 1 px 宽，插值会糊成一团，
     * 那样就看不出真机上的实际观感了。 */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(ren, PK_DISPLAY_W, PK_DISPLAY_H);

    SDL_Texture *tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
        PK_DISPLAY_W, PK_DISPLAY_H);
    if (!tex) { fprintf(stderr, "CreateTexture: %s\n", SDL_GetError()); return 1; }

    /* PFD 写入的是 LVGL canvas 的缓冲；窗口上显示的是 LVGL 合成之后的结果。
     * 两者分开，将来叠上 FAB / dock 才看得出图层间的相互影响。 */
    uint16_t *fb = pk_sim_lv_init();
    pk_sim_lv_attach_mouse(ZOOM);   /* 鼠标当触摸用，验证 FAB / dock 的交互 */
    pk_ui_nav_init();
    const uint16_t *screen = fb;

    sim_state_t st = { .t = 0.0f, .roll_bias = 0.0f, .paused = false };
    int  shot_seq  = 0;
    bool toast_on  = false;
    bool sub_on    = false;
    bool running   = true;

    const uint32_t frame_ms = 1000 / TARGET_FPS;
    uint32_t last = SDL_GetTicks();

    printf("PFD sim running — ESC/Q 退出 · 空格 暂停 · S 截图 · ←→ 调 roll\n");

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:     running = false;              break;
                case SDLK_SPACE: st.paused = !st.paused;       break;
                case SDLK_s:     save_bmp(screen, shot_seq++);  break;
                /* T 键弹一次 toast：验证它压在 dock / FAB 之上。真机上由
                 * 调平、绑定本机等动作触发。 */
                /* B 键进出二级页：核对三条退路都能返回。 */
                case SDLK_b:
                    sub_on = !sub_on;
                    pk_ui_nav_set_subpage(sub_on, "诊断");
                    break;
                case SDLK_t:
                    toast_on = !toast_on;
                    pk_ui_nav_toast(toast_on ? "已保存" : NULL, false);
                    break;
                case SDLK_LEFT:  st.roll_bias -= 5.0f;         break;
                case SDLK_RIGHT: st.roll_bias += 5.0f;         break;
                default: break;
                }
            }
        }

        const uint32_t now = SDL_GetTicks();
        const float dt = (float)(now - last) / 1000.0f;
        last = now;
        if (!st.paused) st.t += dt;

        /* ---- 与固件 pfd.c 完全相同的渲染顺序 ---- */
        pk_pfd_imu_t        imu;
        pk_pfd_hsi_t        hsi;
        pk_pfd_alt_tape_t   alt;
        pk_pfd_speed_tape_t spd;
        pk_pfd_status_t     stat;
        mock_fill(&st, &imu, &hsi, &alt, &spd, &stat);

        pk_pfd_attitude_render(fb, &imu);    /* 姿态仪铺满作为背景 */
        pk_pfd_statusbar_render(fb, &stat);
        pk_pfd_alt_tape_render(fb, &alt);
        pk_pfd_speed_tape_render(fb, &spd);
        pk_pfd_hsi_render(fb, &hsi);
    pk_pfd_hsi_traffic_render(fb);   /* 罗盘外圈的交通目标，顺序同 pfd.c */
    {
        pk_pfd_infobox_t ib;
        pk_pfd_leftbox_t lb;
        mock_fill_boxes(&ib, &lb, &alt, &spd);
        pk_pfd_infobox_render(fb, &ib);
        pk_pfd_leftbox_render(fb, &lb);
    }

        /* 让 LVGL 把 canvas（以及将来的控件）合成到最终画面。 */
        screen = pk_sim_lv_render((uint32_t)(dt * 1000.0f));
        fb_to_texture(screen, tex);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        const uint32_t spent = SDL_GetTicks() - now;
        if (spent < frame_ms) SDL_Delay(frame_ms - spent);
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
