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
 *     T         切换一条 toast 提示（验证它压在 FAB / 菜单之上）
 *     B         进出二级页面（验证返回栏与 FAB 变 ←）
 *     M         开 / 关全屏导航网格（真机上点 FAB 打开的主菜单）
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
 * 演示模式
 * --------
 *     PK_SIM_DEMO=1         打开真机的「演示模式」标识（红色 DEMO 徽标 + 红框）
 *
 * 注意模拟器**本来就在**喂合成数据，这个开关只影响标识与顶栏让位，不影响数据
 * 本身。它存在的意义是：那两块标识画在控件层、压在所有页面之上，必须能逐页
 * 截图确认「每一页都在」——这是演示模式的安全底线，不能只靠肉眼在真机上翻页。
 *
 * 全屏导航网格（点 FAB 打开的主菜单，取代了原来的横向 dock）
 * ----------------------------------------------------------
 *     PK_SIM_MENU=1          打开菜单（第 1 页 7 项 + 1 格余量 + 动作条）
 *     PK_SIM_MENU_PAGE=<n>   打开并翻到第 n 页（0 起；=1 就是第 2 页那 3 项）
 *     PK_SIM_MENU_BRIGHT=1   打开并展开亮度快调 pop
 *     PK_SIM_MENU_LEVEL=<pct>   打开并停在「调平」长按进行中：橙色进度填充走到
 *                               pct%（0~100，UX 规格 §6 的 ③）
 *     PK_SIM_MENU_LEVEL_DONE=1  打开并停在调平完成的绿闪那一帧（同 §6 的 ④）
 *     PK_SIM_UI_MODE=<n>     当前在哪一页（pk_ui_mode_t 序号），决定哪一格
 *                            画选中框；默认 0=PFD，第 2 页可给 6=DIAG
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

#include "lvgl.h"          /* --drag-test 要直接问 screen 的滚动偏移 */
/* lv_obj_get_layer_type() 只在私有头里导出。测试要问的正是"这个对象会不会被
 * 判成 TRANSFORM layer"，而那是 LVGL 的内部概念，没有公开 API 能问——用私有头
 * 比在测试里照抄一份 calculate_layer_type() 的判定条件可靠（照抄的那份会随
 * LVGL 升级悄悄失真）。sim 的 include 路径已含 lvgl 的 src/。 */
#include "core/lv_obj_draw_private.h"

#include "lv_backend.h"
#include "pk_ui_nav.h"
#include "nav_grid_page.h"
#include "about_page.h"
#include "adsb_list.h"
#include "config_devname.h"   /* PK_DEVNAME_MAX_LEN —— 键盘页的上限跟真机同一个数 */
#include "diag_page.h"
#include "keyboard_page.h"
#include "apt_detail_page.h"
#include "search_page.h"
#include "settings_page.h"
#include "boot_splash.h"
#include "traffic_page.h"
#include "map_page.h"

#include "config_demo.h"
#include "demo_data.h"
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
    /* 动画时间。合成曲线全部搬到了 firmware/main/demo_data.c——真机的"演示模式"
     * 用的就是同一批数值，两边各写一份的话，模拟器上验过的版面在真机上未必压
     * 得到同样的极值。 */
    const int64_t anim_us = (int64_t)((double)t * 1000000.0);

    /* 三个周期互质，避免动作同步显得假 */
    imu->valid     = !k.no_imu;
    imu->roll_deg  = pk_demo_roll_deg(anim_us) + st->roll_bias;
    imu->pitch_deg = pk_demo_pitch_deg(anim_us);

    hsi->imu_valid = !k.no_imu;
    hsi->yaw_deg   = pk_demo_yaw_deg(anim_us);

    alt->valid       = !k.no_own;
    alt->altitude_ft = pk_demo_own_alt_ft(anim_us);

    spd->valid           = !k.no_own;
    spd->ground_speed_kt = pk_demo_own_gs_kt(anim_us);

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
    pk_mock_update(hsi->yaw_deg, alt->altitude_ft, anim_us);
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
     * 之后**的画面，叠上 FAB / toast / DEMO 标识才不会漏掉图层间的相互影响。 */
    uint16_t *fb = pk_sim_lv_init();
    pk_ui_nav_init();
    /* 演示模式标识。放在这里、在页面渲染**之前**，是为了让它像真机一样压在
     * 任何一页之上——真机由 pfd.c 每帧同步，模拟器一帧就够。 */
    pk_ui_nav_set_demo(pk_demo_enabled());
    /* PK_SIM_FAB=left 把 FAB 吸到左缘，用来核对各页的浮层避让两侧都对。 */
    {
        const char *side = getenv("PK_SIM_FAB");
        if (side && side[0] == 'l') pk_ui_nav_set_fab_side(true);
    }
    /*
     * 全屏导航网格 = 真机上点 FAB 打开的主菜单。三个开关都会把它打开，后两个
     * 再摆到具体的一态（第几页 / 亮度 pop 开着没）——摆法写在 nav_grid_page.c
     * 的截图钩子里，走的是与真机同一条 open()，同 search_page.c 的
     * sim_setup_once。
     *
     * 这里**没有** PK_SIM_FAB=left 的对应场景：网格是全屏的，不像 dock 那样
     * 锚在 FAB 上、随吸附边缘反向铺开，所以 ui-4.3-dock-left 那一格截图在这
     * 一版没有对应物，已从 capture.py 删除。
     */
    if (getenv("PK_SIM_MENU") || getenv("PK_SIM_MENU_PAGE") ||
        getenv("PK_SIM_MENU_BRIGHT") || getenv("PK_SIM_MENU_LEVEL") ||
        getenv("PK_SIM_MENU_LEVEL_DONE"))
        pk_nav_grid_page_open();
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
     * 800×480，需要能截图比对。名字与导航网格的项一一对应。 */
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

        } else if (strcmp(page, "map") == 0) {
            /*
             * PK_SIM_MAP_ZOOM_STEPS=<n>：地图页的 zoom 是内部静态状态
             * （map_page.c 里的 s_zoom），没有对外的"直接设值"入口——跟
             * PK_SIM_DIAG_SCROLL 那类"页面自己读环境变量"的桩不一样，因为
             * map_page.c 是不可改的固件源码。这里改用它已经导出的
             * pk_map_page_touch() 公开触摸接口，照真实用户点 +/− 按钮的
             * 路径走：n>0 连点 n 下缩小览过大（放大 n 级），n<0 连点 |n| 下
             * 缩小。按钮坐标照抄 map_page.c 的常量算式（FOOTER_H/BTN_D/
             * BTN_M/BTN_GAP_ABOVE_FOOTER/BTN_ZIN_Y 那几行），因为它们是
             * static #define，没有导出——固定在 800×480 面板上，PANEL 只有
             * 这一个有效取值（见 CMakeLists.txt 对 PANEL 的说明），不会漂。
             */
            {
                const char *zs = getenv("PK_SIM_MAP_ZOOM_STEPS");
                const int steps = zs ? atoi(zs) : 0;
                if (steps != 0) {
                    const int footer_y0 = PK_DISPLAY_H - 18;              /* FOOTER_H */
                    const int btn_zout_y = footer_y0 - 8 - 56;            /* -BTN_GAP_ABOVE_FOOTER -BTN_D */
                    const int btn_zin_y  = btn_zout_y - 56 - 10;          /* -BTN_D -10 */
                    const int btn_x      = PK_DISPLAY_W - 16 - 56;        /* -BTN_M -BTN_D */
                    const int cx = btn_x + 28;                            /* +BTN_D/2 */
                    const int cy_in  = btn_zin_y + 28;
                    const int cy_out = btn_zout_y + 28;
                    const int n = steps > 0 ? steps : -steps;
                    const int cy = steps > 0 ? cy_in : cy_out;
                    for (int i = 0; i < n; i++) {
                        pk_map_page_touch(cx, cy);
                        pk_map_page_touch_up();
                    }
                }
            }
            /*
             * PK_SIM_MAP_PIN=<lat>,<lon>[,<label>]：摆一枚搜索结果 PIN，
             * 用来核对它在各种底图颜色上都读得出来、且不被 ADS-B 目标压住。
             * 真机上这一步由搜索页点结果时调 pk_map_page_goto + set_pin 完成，
             * 这里直接调同两个公开接口，走的是同一条路径。
             */
            {
                const char *p = getenv("PK_SIM_MAP_PIN");
                if (p != NULL && p[0] != '\0') {
                    double plat = 0, plon = 0;
                    char label[16] = "";
                    if (sscanf(p, "%lf,%lf,%15s", &plat, &plon, label) >= 2) {
                        pk_map_page_set_pin(plat, plon, label);
                        pk_map_page_goto(plat, plon, 11);
                    }
                }
            }
            pk_map_page_render(fb);
            /*
             * PK_SIM_MAP_TAP=<x>,<y>：在地图上点一下，走**真机同一条**触摸
             * 路径（touch → touch_up），用来验"点机场符号进详情页"这条新链路
             * ——命中测试、快照里的记录下标、详情页取数三段一次跑通。
             * 没命中的话它就是"点空白清 PIN"，同样是真机行为。
             */
            {
                const char *t = getenv("PK_SIM_MAP_TAP");
                int tx = 0, ty = 0;
                if (t != NULL && sscanf(t, "%d,%d", &tx, &ty) == 2) {
                    pk_map_page_touch(tx, ty);
                    pk_map_page_touch_up();
                    if (pk_apt_detail_page_active())
                        pk_apt_detail_page_render(fb);   /* 分派次序同 pfd.c */
                    else
                        pk_map_page_render(fb);          /* PIN 变化要重画 */
                }
            }
        } else if (strcmp(page, "search") == 0) {
            /* 航空数据搜索页。真机上由地图页右侧那枚放大镜打开，这里直接调
             * open()——与诊断详情页用 PK_SIM_DIAG_DETAIL 跳进去是同一个套路。
             *
             * 数据要 PK_SIM_AERO=1 才有（同地图叠加层）；不给就能截到
             * 「数据库不可用」那一屏。摆哪一态由 search_page.c 的截图钩子读
             * 环境变量决定：PK_SIM_SEARCH / _HIST / _SCROLL，见那边的注释。 */
            pk_search_page_open();
            pk_search_page_render(fb);
        } else if (strcmp(page, "aptdetail") == 0) {
            /*
             * 机场详情页。真机上有两个入口（搜索结果点机场、地图上点机场
             * 符号），两条路最后都落到同一个 pk_apt_detail_page_open()，
             * 所以这里直接调它——同 PK_SIM_DIAG_DETAIL / search 那两处的套路。
             *
             *   PK_SIM_APT=<下标>      看哪个机场（桩表里 1=ZGGG 10 跑道
             *                          63 频率、2=ZGOW 有跑道无频率、
             *                          4=直升机坪两段皆空）
             *   PK_SIM_APT_FROM=search 摆成"从搜索进来"（关页要回搜索，
             *                          FAB 得继续藏着）
             *   PK_SIM_APT_SCROLL=<px> 滚到指定位置，好截到长列表的尾巴
             *
             * 数据同样要 PK_SIM_AERO=1；不给就能截到"机场数据不可用"那一屏。
             */
            {
                const char *f = getenv("PK_SIM_APT_FROM");
                const char *a = getenv("PK_SIM_APT");
                const bool from_search = (f != NULL && strcmp(f, "search") == 0);
                if (from_search) pk_search_page_open();
                pk_apt_detail_page_open(a ? (uint32_t)atoi(a) : 1u,
                                        from_search ? PK_APT_DETAIL_FROM_SEARCH
                                                    : PK_APT_DETAIL_FROM_MAP);
                const char *sc = getenv("PK_SIM_APT_SCROLL");
                if (sc != NULL) {
                    /* 滚动位置是页面的内部状态，没有 setter——照 map 那边用
                     * 公开触摸接口的做法，这里用一次真实的拖动把它滚下去：
                     * 先渲一帧让 s_content_h 有值（钳位要它），再拖。 */
                    const int px = atoi(sc);
                    pk_apt_detail_page_render(fb);
                    pk_apt_detail_page_touch(400, 400);
                    pk_apt_detail_page_drag(400, 400 - px);
                    pk_apt_detail_page_touch_cancel();
                }
            }
            pk_apt_detail_page_render(fb);
        } else if (strcmp(page, "keyboard") == 0) {
            /* 键盘编辑器。真机上它由设置页那一行点开（pk_settings_apply
             * 的 case 8），模拟器直接调 open() —— 这与诊断详情页那边用
             * PK_SIM_DIAG_DETAIL 跳进去是同一个套路。
             *
             * PK_SIM_KBD=<初值> 摆不同的输入态：不给 = 空输入，
             * 给满 PK_DEVNAME_MAX_LEN 个字符 = 计数器转警示色的那一态。
             * 上限跟着真机的宏走，不写字面量：写死过一次 10，上限一改
             * 模拟器就在演一个真机上不存在的键盘。 */
            pk_keyboard_page_open(pk_i18n_text(PK_TR_SETTINGS_DEVNAME),
                                  getenv("PK_SIM_KBD"), PK_DEVNAME_MAX_LEN,
                                  NULL, NULL);
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
    /* 导航网格是**半透明**覆盖层：底页照常画完再把它叠上去，次序与真机的
     * pfd.c 一致（它不进那条模态 if/else 链，理由见 nav_grid_page.h）。 */
    if (pk_nav_grid_page_active()) pk_nav_grid_page_render(fb);
    /* 多合成几帧：首帧 LVGL 只画屏幕底色，canvas 要到第二帧才落到 s_screen，
     * 两帧不够；FAB 的显隐与 toast 的排版也要一两帧才落位。
     * 12 帧 ≈ 400 ms，离网格 5 s 无操作自动收起还远。 */
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

/*
 * --drag-test：把 FAB 从右缘往左拖一趟，断言**屏幕自己没有跟着滚**。
 *
 * 这条测试对应罩哥真机实测的一个 bug：按住右侧 FAB 想拉到左边，结果整个屏幕
 * 的内容被一起拖走，下方还凭空冒出一条滚动条。
 *
 * 根因不在 FAB，在屏幕根对象：LVGL 里每个 lv_obj 默认都带 LV_OBJ_FLAG_SCROLLABLE
 * （lv_obj.c 的构造函数），screen 也不例外；而 pk_ui_nav.c 给 backbar /
 * toast / demo 那几个容器都显式摘掉了这个 flag，唯独 screen 漏了。FAB 自身是
 * lv_button（构造时已不可滚），但它保留着 SCROLL_CHAIN，于是 LVGL 的
 * lv_indev_find_scroll_obj() 沿父链一路找到 screen，认定"这个能滚"。
 *
 * screen 当年之所以真的滚得动，是因为横向 dock 收起时被停在屏外，滚动区永远
 * 比屏幕宽出一整个 dock。dock 已删，这个触发源随之消失，但断言留着：只要哪
 * 天又有人往 screen 上挂一个越出屏幕的子对象，这条就会立刻红。
 *
 * 为什么必须走 indev 而不是直接调 fab_event_cb：滚动判定跑在 indev 读取之后、
 * 控件事件之前，绕开 indev 就把要测的那一段跳过去了。
 *
 * 判据取 lv_obj_get_scroll_x/y(screen) 恒为 0——滚动条只是滚动的显示结果，
 * 盯着偏移量比盯着滚动条可靠。顺带把 FAB 是否被拖出屏外也一并断言了：那是
 * 同一段代码里的第二个洞（PRESSING 分支只夹了 ny，nx 没夹）。
 */
static int run_drag_test(void)
{
    pk_sim_lv_init();
    pk_ui_nav_init();
    pk_sim_lv_attach_script_pointer();

    lv_obj_t *scr = lv_screen_active();
    int fails = 0;

    /* 先空跑几帧让控件层落位（首帧 LVGL 只画底色）。 */
    for (int i = 0; i < 12; ++i) pk_sim_lv_render(33);

    int fx, fy, fw, fh;
    if (!pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh)) {
        fprintf(stderr, "FAB 不可见，测试无从谈起\n");
        return 2;
    }
    printf("FAB 起始位置 x=%d y=%d %dx%d\n", fx, fy, fw, fh);

    int px = fx + fw / 2;
    const int py = fy + fh / 2;

    /* 按住不动 ~660 ms：FAB 要 LONG_PRESSED（默认 400 ms）才进拖动态。 */
    for (int i = 0; i < 20; ++i) {
        pk_sim_lv_pointer_set(px, py, true);
        pk_sim_lv_render(33);
    }

    /*
     * 拖动态**不许**把 FAB 变成 TRANSFORM layer。
     *
     * 2026-08-02 真机「一长按 FAB 就死机」的根因：TRANSFORM layer 必须一次性拿到
     * 整块 buffer（SIMPLE layer 才能受 LV_DRAW_LAYER_SIMPLE_BUF_SIZE 限制分块），
     * 而真机 LVGL 池只有 CONFIG_LV_MEM_SIZE_KILOBYTES=64，分配不出来；失败后
     * lv_draw.c:506 只是 "Try later"，紧接着 lv_draw.c:302 在 LV_OS_NONE 下是裸忙等
     * `while(!dispatch_req);`——整个 LVGL 都在 pfd 一个任务里，没有第二个线程能
     * 释放内存或发请求，于是永久占满 CPU0，看门狗每 15 s 报一次 `CPU 0: pfd`。
     *
     * 判据直接问 lv_obj_get_layer_type()，**不去量 LVGL 堆用量**：layer buffer 是
     * 渲染那一瞬间分配、画完立刻释放的，隔几帧采样一次根本撞不上峰值——试过，
     * 把 transform_scale 加回去堆增量也只有 72 B，测试照样全绿（假绿）。
     *
     * 模拟器 LV_MEM_SIZE 是 8 MB，永远不会 OOM，所以这里只能查"会不会走上那条
     * 路"，查不了"内存够不够"。真机那口池子有多小，见上面那行 CONFIG。
     */
    {
        lv_obj_t *fab = NULL;
        const uint32_t nch = lv_obj_get_child_count(scr);
        for (uint32_t i = 0; i < nch; ++i) {
            lv_obj_t *c = lv_obj_get_child(scr, i);
            if (lv_obj_get_width(c) == fw && lv_obj_get_height(c) == fh) { fab = c; break; }
        }
        if (fab == NULL) {
            printf("  [FAIL] 找不到 FAB 对象，layer 类型没验成\n");
            fails++;
        } else if (lv_obj_get_layer_type(fab) == LV_LAYER_TYPE_TRANSFORM) {
            printf("  [FAIL] 拖动态把 FAB 变成了 TRANSFORM layer："
                   "真机 64 KB 池分配不出整块 buffer，pfd 任务会卡死在忙等里\n");
            fails++;
        } else {
            printf("拖动态 FAB layer 类型 = %d（0=NONE，安全）\n",
                   (int)lv_obj_get_layer_type(fab));
        }
    }

    /* 一路拖到屏幕最左边（手指最多到 x=0，GT911 的坐标被 native_to_logical
     * 夹在屏内）。每帧 20 px，中途逐帧检查，不能只看终点——滚动是过程量，
     * LVGL 松手时会把它弹回去，只验终点会漏掉。 */
    for (int step = 0; px > 0; ++step) {
        px -= 20;
        if (px < 0) px = 0;
        pk_sim_lv_pointer_set(px, py, true);
        pk_sim_lv_render(33);

        const int32_t sx = lv_obj_get_scroll_x(scr);
        const int32_t sy = lv_obj_get_scroll_y(scr);
        if (sx != 0 || sy != 0) {
            printf("  [FAIL] 第 %2d 步 指针 x=%4d：屏幕被滚动了 scroll=(%d,%d)\n",
                   step, px, (int)sx, (int)sy);
            fails++;
        }
        /* 这里**测不到** FAB 拖动中有没有滑出屏外：pk_ui_nav_fab_rect() 返回的
         * 是 fab_x() 算出的吸附位，不是 lv_obj 的实时坐标，拖动期间它恒等于
         * 左/右缘那两个值。曾经在这里写过一条 fx<0 的断言，看着绿，其实什么
         * 都没验——留这段话，免得下次又有人照着补一条同样无效的。 */
    }

    /* 松手，再跑几帧让吸附动画走完。 */
    pk_sim_lv_pointer_set(px, py, false);
    for (int i = 0; i < 12; ++i) pk_sim_lv_render(33);

    const int32_t sx = lv_obj_get_scroll_x(scr);
    const int32_t sy = lv_obj_get_scroll_y(scr);
    if (sx != 0 || sy != 0) {
        printf("  [FAIL] 松手后屏幕仍是滚动的 scroll=(%d,%d)\n", (int)sx, (int)sy);
        fails++;
    }

    if (pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh))
        printf("FAB 落点 x=%d y=%d（预期吸附到左缘）\n", fx, fy);

    /*
     * 各页面必须把 FAB 头上的触摸放行。
     *
     * 这些页面的命中区是自绘的，判定不经过 LVGL；哪个页面把 FAB 那块吃掉了，
     * LVGL 就一次按下都收不到，FAB 当场变成死钮——点不动也拖不动。列表页
     * 2026-08-02 正是这么坏的：内容右缘从 724 放到 784 之后 FAB 落进了命中区。
     *
     * 左右两个落点都要试：FAB 可以吸在任一侧，而页面的命中区左右并不对称。
     */
    for (int side = 0; side < 2; ++side) {
        pk_ui_nav_set_fab_side(side == 0);
        for (int i = 0; i < 4; ++i) pk_sim_lv_render(33);
        if (!pk_ui_nav_fab_rect(&fx, &fy, &fw, &fh)) continue;

        const int cx = fx + fw / 2, cy = fy + fh / 2;
        const char *where = (side == 0) ? "左缘" : "右缘";

        if (pk_adsb_list_touch(cx, cy)) {
            printf("  [FAIL] FAB 吸在%s (%d,%d)：列表页吃掉了它的按下\n",
                   where, cx, cy);
            fails++;
        }
        pk_adsb_list_touch_cancel();

        if (pk_traffic_page_touch(cx, cy)) {
            printf("  [FAIL] FAB 吸在%s (%d,%d)：交通页吃掉了它的按下\n",
                   where, cx, cy);
            fails++;
        }
        pk_traffic_page_touch_up();

        if (pk_map_page_touch(cx, cy)) {
            printf("  [FAIL] FAB 吸在%s (%d,%d)：地图页吃掉了它的按下\n",
                   where, cx, cy);
            fails++;
        }
        pk_map_page_touch_up();
    }

    printf(fails ? "\n拖动测试 FAIL（%d 处）\n" : "\n拖动测试 PASS\n", fails);
    return fails ? 1 : 0;
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

    /* 同样不需要视频后端：纯事件流 + 断言，无窗口。 */
    if (argc >= 2 && strcmp(argv[1], "--drag-test") == 0) return run_drag_test();

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
     * 两者分开，叠上 FAB / toast / DEMO 标识才看得出图层间的相互影响。 */
    uint16_t *fb = pk_sim_lv_init();
    pk_sim_lv_attach_mouse(ZOOM);   /* 鼠标当触摸用，验证 FAB 的点击与拖动 */
    pk_ui_nav_init();
    pk_ui_nav_set_demo(pk_demo_enabled());
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
                /* T 键弹一次 toast：验证它压在 FAB / 菜单之上。真机上由
                 * 调平、绑定本机等动作触发。 */
                /* B 键进出二级页：核对三条退路都能返回。 */
                case SDLK_b:
                    sub_on = !sub_on;
                    pk_ui_nav_set_subpage(sub_on, "诊断");
                    break;
                /*
                 * M 键开 / 关全屏导航网格。真机上它由点 FAB 触发，走
                 * pk_ui_nav_on_menu() 这个弱符号回调；模拟器不编译
                 * pk_ui_nav_host.c（那份强实现在那儿），所以这里用一个按键
                 * 代替，只为了让评审能在窗口里看到这一层压在各页之上的样子。
                 *
                 * **网格内部的触摸没有接进来**：真机那条路在 touch_gt911.c
                 * 里（模态优先级 + 触摸仲裁），模拟器没有那个文件。所以窗口
                 * 里只能看、不能点，再按一次 M 关掉。要验交互仍以真机为准，
                 * 要验版面用 capture.py 的 ui-4.3-menu* 四张。
                 */
                case SDLK_m:
                    if (pk_nav_grid_page_active()) pk_nav_grid_page_close();
                    else                           pk_nav_grid_page_open();
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

        /* 半透明覆盖层，叠在画完的底页之上，次序同真机的 pfd.c。 */
        if (pk_nav_grid_page_active()) pk_nav_grid_page_render(fb);

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

/*
 * map_page.c 里弱符号 pk_map_page_on_apt_detail 的模拟器强实现。
 *
 * 固件那份在 pk_ui_nav_host.c（"页面之间怎么跳"归导航宿主），模拟器没有那个
 * 文件，于是这里补一份**语义完全相同**的——不是另写一套逻辑，只是把同一个
 * 调用接上，好让 PK_SIM_MAP_TAP 走的是真机那条链路。
 */
void pk_map_page_on_apt_detail(uint32_t apt_idx)
{
    pk_apt_detail_page_open(apt_idx, PK_APT_DETAIL_FROM_MAP);
}
