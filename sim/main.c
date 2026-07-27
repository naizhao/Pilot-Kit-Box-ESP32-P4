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
 *     ← →      手动步进 roll，观察极限姿态
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

#include "display.h"
#include "pfd_attitude.h"
#include "pfd_hsi.h"
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

static void mock_fill(const sim_state_t *st,
                      pk_pfd_imu_t *imu, pk_pfd_hsi_t *hsi,
                      pk_pfd_alt_tape_t *alt, pk_pfd_speed_tape_t *spd,
                      pk_pfd_status_t *stat)
{
    const float t = st->t;

    /* 三个周期互质，避免动作同步显得假 */
    imu->valid     = true;
    imu->roll_deg  = 25.0f * sinf(t * 0.37f) + st->roll_bias;
    imu->pitch_deg = 10.0f * sinf(t * 0.23f);

    hsi->imu_valid = true;
    hsi->yaw_deg   = fmodf(t * 6.0f, 360.0f);

    alt->valid       = true;
    alt->altitude_ft = 23225 + (int)(1200.0f * sinf(t * 0.11f));

    spd->valid           = true;
    spd->ground_speed_kt = 378 + (int)(60.0f * sinf(t * 0.17f));

    stat->imu_valid      = true;
    stat->yaw_deg        = hsi->yaw_deg;
    stat->aircraft_count = 6;
    stat->gps_have_fix   = true;
    stat->gps_sats       = 17;
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
    static uint16_t fb[PK_DISPLAY_W * PK_DISPLAY_H];
    sim_state_t st = { .t = at_sec, .roll_bias = 0.0f, .paused = true };

    pk_pfd_imu_t imu; pk_pfd_hsi_t hsi; pk_pfd_alt_tape_t alt;
    pk_pfd_speed_tape_t spd; pk_pfd_status_t stat;
    mock_fill(&st, &imu, &hsi, &alt, &spd, &stat);

    pk_pfd_attitude_render(fb, &imu);
    pk_pfd_statusbar_render(fb, &stat);
    pk_pfd_alt_tape_render(fb, &alt);
    pk_pfd_speed_tape_render(fb, &spd);
    pk_pfd_hsi_render(fb, &hsi);

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(
        0, PK_DISPLAY_W, PK_DISPLAY_H, 16, SDL_PIXELFORMAT_RGB565);
    if (!s) { fprintf(stderr, "surface: %s\n", SDL_GetError()); return 1; }

    uint16_t *dst = (uint16_t *)s->pixels;
    const int stride = s->pitch / (int)sizeof(uint16_t);
    for (int y = 0; y < PK_DISPLAY_H; ++y)
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            const uint16_t v = fb[(size_t)y * PK_DISPLAY_W + x];
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

    /* 和真机一样的单缓冲：绘制模块直接往这块写 */
    static uint16_t fb[PK_DISPLAY_W * PK_DISPLAY_H];

    sim_state_t st = { .t = 0.0f, .roll_bias = 0.0f, .paused = false };
    int  shot_seq  = 0;
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
                case SDLK_s:     save_bmp(fb, shot_seq++);     break;
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

        fb_to_texture(fb, tex);
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
