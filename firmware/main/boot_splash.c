/*
 * boot_splash.c — render the Pilot Kit boot logo on a rounded white card.
 *
 * The 160×160 RGB565 logo blob is embedded into the firmware by
 * CMakeLists.txt:
 *
 *     idf_component_register(
 *         ...
 *         EMBED_FILES "pk_logo.rgb565"
 *     )
 *
 * which produces two linker-defined symbols giving the start/end of
 * the data in flash. We declare them via `asm()` labels (no C
 * preprocessor name mangling) and read them as raw bytes; cast to
 * uint16_t* when blitting since the binary is pre-packed in little-
 * endian RGB565 by tools/png_to_rgb565.py.
 *
 * Layout (320 × 240 landscape):
 *
 *   y =   0   ╔═══════════════════════════════════╗
 *             ║                                    ║
 *             ║                                    ║
 *   y =  50   ║              ╭────────╮            ║  ← rounded white card,
 *             ║              │ [LOGO] │            ║    100×100, r=8
 *             ║              │ 80×80  │            ║
 *             ║              ╰────────╯            ║
 *   y = 162   ║         PILOT KIT BOX              ║  scale-2 title
 *   y = 186   ║       Booting abc1234 ...          ║  scale-1 git hash
 *   y = 198   ║     Built May 21 2026 12:34:56     ║  scale-1 build stamp
 *   y = 210   ║         ESP-IDF v6.0.1             ║  scale-1 IDF version
 *   y = 228   ║        (C) 2026 Pilot Kit          ║  scale-1 copyright footer
 *   y = 240   ╚═══════════════════════════════════╝
 *
 * The 160×160 source blob is downsampled 2:1 (nearest-neighbour) to
 * 80×80 on screen — the logo file in flash stays unchanged, only the
 * blit reads every other source pixel.
 *
 * 上面这张图是 320×240 那一版的排版，早已过时（当前是 800×480，尺寸与坐标
 * 以下面 CARD_SIZE 那一段的注释为准）。
 *
 * 没有动画，但**不是**只画一次：app_main 每走完一步开机流程就调一次
 * pk_boot_splash_progress()，那里重画整屏并推屏，屏上只有 logo 下方那条进度条
 * 在变（为什么需要它，见 boot_splash.h）。PFD 任务一起来，下一帧就把这一屏
 * 整个盖掉，不需要显式收场。
 */

#include "boot_splash.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_idf_version.h"

#include "config_demo.h"
#include "display.h"
#include "i18n.h"
#include "logo_blob.h"
#include "pfd_aa_text.h"
#include "pfd_aa_font.h"

extern const uint8_t pk_logo_start[] asm("_binary_pk_logo_rgb565_start");
extern const uint8_t pk_logo_end[]   asm("_binary_pk_logo_rgb565_end");

#define PK_LOGO_SRC_W       160                /* flash 里的源图尺寸 */
#define PK_LOGO_SRC_H       160

/* ── 布局（800×480）────────────────────────────────────────────
 *
 * 整块内容垂直居中。图标比 2.4″ 那版大一倍有余（80 → 176）——屏幕物理尺寸
 * 只有信用卡大小，但像素多了 5 倍，沿用旧尺寸会让开机画面显得空旷而寒酸。
 *
 * 图标画法与「关于」页一致：完整图案 + 内边距 + 圆角，不裁源图。旧版用
 * LOGO_SRC_CROP=24 裁掉 SVG 空白好让图案占满 80×80 的小卡片，在这个尺寸上
 * 会把六边形外框整个切掉。
 */
#define CARD_SIZE           176
#define CARD_RADIUS         (CARD_SIZE * 22 / 100)   /* app icon 惯例 */
/* 内边距取 8，于是内容区正好 176-16 = 160 = 源图尺寸，**1:1 显示不缩放**。
 *
 * 这不是凑巧凑出来的数：源图 160 px 被缩到 144 时，最近邻采样会直接丢掉
 * 像素，量角器那圈细刻度就断成虚线——实测反馈是「四周有点破」。放大同样
 * 会糊。只要卡片尺寸容得下，1:1 永远是最锐利的选择。 */
#define CARD_PAD            8
#define CARD_X              ((PK_DISPLAY_W - CARD_SIZE) / 2)

/*
 * 卡片的纵向起点有两档，因为进度条要占掉标题下方 46 px（阶段名 26 + 间距 8
 * + 条 12）：
 *
 *   PLAIN 64 —— 还没有人报过进度时的原始版面，一个像素没动。
 *   PROG  24 —— 报过进度之后整块内容上移 40 px，给那一带腾地方。下界由演示
 *               模式那条红色横幅钉死：横幅贴底，上沿在 y=432，三行构建信息
 *               必须在它之前收完（实测收在 423）。
 *
 * 两档之间的 40 px 跳变在真机上看不见：app_main 的第一次调用就是
 * progress(START, 0, 3)（见 boot_splash.h 的「调用次序」），PLAIN 版面根本
 * 不会被 flush 出去。留着它是为了 pk_boot_splash_render() 单独被调时（模拟器
 * 的 PK_SIM_PAGE=splash、既有基线图）版面与从前一致。
 */
#define CARD_Y_PLAIN        64
#define CARD_Y_PROG         24

#define TITLE_GAP           30                        /* 卡片 → 标题 */

/* 三行构建信息用 S 档而不是 M：它们是「极次要」的——开机时真正要确认的是
 * 版本号，日期与 IDF 版本只在排障时才看。行距也收到 2 px：三行同属一组，
 * 松开反而读成三件独立的事。
 *
 * 不用 XS（spec 允许的最小档）是因为那一档只存了 0x20..0x3F，没有字母，
 * 而这三行全是 "Booting"、"Built"、"ESP-IDF" 这样的词。 */
#define INFO_GAP            20                        /* 标题 → 信息行 */
#define INFO_LINE_GAP       2

/* ── 进度条 ───────────────────────────────────────────────────
 *
 * 宽度取 440：比最长的一行构建信息（"Built Jul 28 2026 21:40:00" 实测 390 px）
 * 略宽，于是它是这一屏最宽的元素，整块内容有了一条明确的左右边界；再宽就开始
 * 跟 800 px 的屏边打架，再窄就被构建信息顶出去。
 *
 * 高度 12 + 2 px 边框，比校准向导那条（400×24）矮一半：那一页进度条是主角，
 * 这里主角是 logo，进度条只是"还在动"的证据。
 */
#define PROG_BAR_W          440
#define PROG_BAR_H          12
#define PROG_BAR_X          ((PK_DISPLAY_W - PROG_BAR_W) / 2)
#define PROG_GAP_TITLE      14                        /* 标题   → 阶段名 */
#define PROG_GAP_LABEL       8                        /* 阶段名 → 进度条 */
#define PROG_GAP_INFO       14                        /* 进度条 → 信息行 */

/* ── 配色 ─────────────────────────────────────────────────────── */
#define BG_COLOR             pk_rgb565( 12,  12,  16)
#define CARD_COLOR           pk_rgb565(255, 255, 255)
#define TITLE_COLOR          pk_rgb565(240, 240, 240)
/* 构建信息压到接近背景的灰蓝：字号已经降无可降（S 是带字母的最小档，XS 只
 * 存了数字和符号），只能靠对比度把它们推到视觉后景。开机时真正要看的是产品名
 * 与版本号，日期和 IDF 版本属于「需要时才找得到」的层级。 */
#define VERSION_COLOR        pk_rgb565( 96, 102, 122)
/* 演示模式横幅。红底白字，与运行时那枚徽标同色系——两处是同一件事的两种呈现，
 * 换了颜色用户就得重新学一遍"红色代表什么"。 */
#define DEMO_BANNER_COLOR    pk_rgb565(208,  24,  32)
#define DEMO_TEXT_COLOR      pk_rgb565(255, 255, 255)
/* 进度条。
 *
 * 填充色取 pk_ui_nav.c 的 COL_FAB(0x2E6DF0)——全项目的「主操作色」，也正是
 * logo 里那道蓝。开机画面本来只有黑白灰加一条演示红，这里不新造第四种色相：
 * 唯一的彩色元素来自品牌本身。
 * 空槽色取校准向导的 COL_BAR_BG(30,38,50)，两条进度条同款。
 * 阶段名取 pk_ui_nav.c 的 COL_TXT(0xE2ECF8)——比构建信息亮、比产品名暗，
 * 层级正好卡在两者之间。 */
#define PROG_FILL_COLOR      pk_rgb565( 46, 109, 240)
#define PROG_TRACK_COLOR     pk_rgb565( 30,  38,  50)
#define PROG_TEXT_COLOR      pk_rgb565(226, 236, 248)

/* ── 进度状态 ─────────────────────────────────────────────────
 *
 * 只被 app_main 单线程写、被同一线程的 render 读（PFD 任务起来之前 splash
 * 没有第二个读者），所以不用加锁。
 *
 * 阶段名拷进来而不是存指针：现在的调用方喂的是 pk_i18n_text() 的返回值（指向
 * 常量表，活得比谁都久），但接口不该把这个前提写进契约——下一个人拿栈上的
 * snprintf 缓冲来调是完全合理的写法。48 字节装得下所有阶段名（最长的
 * "Starting up" 11 字节 / "地图数据" 12 字节）。 */
static char s_prog_label[48];
static int  s_prog_done  = -1;     /* <0 = 还没有人报过进度 */
static int  s_prog_total = 1;

static bool prog_active(void)
{
    return s_prog_done >= 0;
}

/* 版面按「报没报过进度」两档取值，见 CARD_Y_PLAIN / CARD_Y_PROG 的说明。 */
static int layout_card_y(void)
{
    return prog_active() ? CARD_Y_PROG : CARD_Y_PLAIN;
}

static int layout_title_y(void)
{
    return layout_card_y() + CARD_SIZE + TITLE_GAP;
}

static int layout_prog_label_y(void)
{
    return layout_title_y() + PK_AA_L_H + PROG_GAP_TITLE;
}

static int layout_prog_bar_y(void)
{
    return layout_prog_label_y() + PK_AA_M_H + PROG_GAP_LABEL;
}

static int layout_info_y(void)
{
    if (!prog_active()) return layout_title_y() + PK_AA_L_H + INFO_GAP;
    return layout_prog_bar_y() + PROG_BAR_H + PROG_GAP_INFO;
}

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

/* 曾经这里有个 put_pixel()。圆角改成按覆盖率混合（见下面 corner_cov）之后，
 * 边缘那一圈不再是"画或不画"而是 blend，最后一个调用点也就没了；留着只会换来
 * 一条 -Wunused-function。 */

/* Filled rounded rectangle. Drawn as three rects (left strip, middle
 * body, right strip) for the straight portion + 4 quarter-circle
 * arcs at the corners. r should be < min(w,h)/2; we don't bother
 * validating since callers are file-local. */

/*
 * 圆角的覆盖率，0..255。
 *
 * 原本返回 bool，边缘只能「整个像素画或整个不画」，斜着切过去就是一圈台阶
 * ——实测反馈是「四周有点破」。改成按到圆心的距离给出覆盖率，边缘那一圈
 * 与背景混合，锯齿就化掉了。
 */
static uint8_t rounded_coverage(int col, int row, int size, int r)
{
    int dx = 0, dy = 0;
    if (col < r)              dx = r - col;
    else if (col >= size - r) dx = col - (size - r - 1);
    if (row < r)              dy = r - row;
    else if (row >= size - r) dy = row - (size - r - 1);
    if (dx == 0 || dy == 0) return 255;

    /* 到角圆心的距离，定点算，避免浮点。 */
    const int d2 = dx * dx + dy * dy;
    const int rin = r - 1, rout = r + 1;
    if (d2 <= rin * rin)  return 255;
    if (d2 >= rout * rout) return 0;

    /* 落在 1 px 的过渡带里：按距离线性插值。 */
    int d = 0;
    while ((d + 1) * (d + 1) <= d2) ++d;      /* 整数平方根 */
    const int t = (rout - d) * 255 / (rout - rin);
    return (uint8_t)(t < 0 ? 0 : (t > 255 ? 255 : t));
}

/* 把一个像素按覆盖率混到背景上。 */
static void blend_over_bg(uint16_t *dst, uint16_t src, uint8_t a)
{
    if (a == 0) return;
    if (a == 255) { *dst = src; return; }

    const uint16_t s16 = (uint16_t)((src >> 8) | (src << 8));
    const uint16_t d16 = (uint16_t)((*dst >> 8) | (*dst << 8));
    const int sr = (s16 >> 11) & 0x1F, sg = (s16 >> 5) & 0x3F, sb = s16 & 0x1F;
    const int dr = (d16 >> 11) & 0x1F, dg = (d16 >> 5) & 0x3F, db = d16 & 0x1F;
    const int ia = 255 - a;
    const int r = (sr * a + dr * ia + 127) / 255;
    const int g = (sg * a + dg * ia + 127) / 255;
    const int b = (sb * a + db * ia + 127) / 255;
    const uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
    *dst = (uint16_t)((v >> 8) | (v << 8));
}

/*
 * 画图标：白色圆角底 + 居中的完整图案。与 about_page.c 的 draw_logo() 同构，
 * 两处显示的是同一张图、同一种取景，改一处要记得改另一处。
 */
static void draw_icon(uint16_t *fb, int x, int y, int size)
{
    int sw = 0, sh = 0;
    const uint16_t *src = pk_logo_bitmap(&sw, &sh);
    const int inner = size - 2 * CARD_PAD;

    for (int row = 0; row < size; ++row) {
        const int yy = y + row;
        if (yy < 0 || yy >= PK_DISPLAY_H) continue;
        uint16_t *dst = fb + yy * PK_DISPLAY_W;

        for (int col = 0; col < size; ++col) {
            const int xx = x + col;
            if (xx < 0 || xx >= PK_DISPLAY_W) continue;
            const uint8_t cov = rounded_coverage(col, row, size, CARD_RADIUS);
            if (cov == 0) continue;

            const int ix = col - CARD_PAD;
            const int iy = row - CARD_PAD;
            uint16_t px;
            if (src == NULL || ix < 0 || iy < 0 || ix >= inner || iy >= inner) {
                px = CARD_COLOR;
            } else {
                /* blob 与 framebuffer 的字节序约定不同，见 display.h 的
                 * pk_rgb565()。真机已验证：转换后颜色正确。 */
                const uint16_t v = src[(iy * sh / inner) * sw + (ix * sw / inner)];
                px = (uint16_t)((v >> 8) | (v << 8));
            }
            blend_over_bg(&dst[xx], px, cov);
        }
    }
}

/*
 * 进度条一带：阶段名（左）+ 计数（右）+ 条。
 *
 * 两行文字**不居中**：阶段名钉在条的左端、计数钉在右端。居中的话每换一个
 * 阶段整行都会左右跳，眼睛得重新找位置——而这条进度条存在的全部意义就是让人
 * 一眼看出"还在动、动到哪了"。
 */
static void draw_progress(uint16_t *fb)
{
    const int label_y = layout_prog_label_y();
    const int bar_y   = layout_prog_bar_y();

    if (s_prog_label[0] != '\0') {
        /* 宽度这里用不着（左对齐），但中文阶段名必须走 pk_aa_puts 的 UTF-8
         * 路径，不能按字节数排版——同 DEMO 横幅那处的教训。 */
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   PROG_BAR_X, label_y, s_prog_label, PROG_TEXT_COLOR,
                   PK_AA_M);
    }

    /* 计数右对齐。位数不设上限：3 步是当前的用法，真要报"第 12 个瓦片包"
     * 也只是这行变长，右端不动。 */
    {
        char count[24];
        snprintf(count, sizeof(count), "%d / %d", s_prog_done, s_prog_total);
        const int w = pk_aa_text_width(count, PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   PROG_BAR_X + PROG_BAR_W - w, label_y, count,
                   PROG_TEXT_COLOR, PK_AA_M);
    }

    /* 边框也吃填充色——照抄 cal_wizard.c 的 draw_progress_bar()：只染填充的
     * 话 done=0 那一帧是一条纯深灰，看上去像进度条坏了，而那恰恰是用户开机
     * 看到的第一帧。 */
    fill_rect(fb, PROG_BAR_X, bar_y,
              PROG_BAR_X + PROG_BAR_W, bar_y + PROG_BAR_H, PROG_FILL_COLOR);
    fill_rect(fb, PROG_BAR_X + 2, bar_y + 2,
              PROG_BAR_X + PROG_BAR_W - 2, bar_y + PROG_BAR_H - 2,
              PROG_TRACK_COLOR);

    const int filled = (PROG_BAR_W - 4) * s_prog_done / s_prog_total;
    if (filled > 0) {
        fill_rect(fb, PROG_BAR_X + 2, bar_y + 2,
                  PROG_BAR_X + 2 + filled, bar_y + PROG_BAR_H - 2,
                  PROG_FILL_COLOR);
    }
}

void pk_boot_splash_render(uint16_t *fb)
{
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, BG_COLOR);

    draw_icon(fb, CARD_X, layout_card_y(), CARD_SIZE);

    /* 产品名。用 M 档——它是这一屏唯一的主角，S 档在 93 mm 宽的屏上撑不住。 */
    {
        static const char kTitle[] = "PILOT KIT BOX";
        const int w = (int)(sizeof(kTitle) - 1) * pk_aa_cell_w(PK_AA_L);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, layout_title_y(), kTitle,
                   TITLE_COLOR, PK_AA_L);
    }

    /* 进度条只在 app_main 报过进度之后才有；没报过时这一带不留白，整块内容
     * 按原版面居中（见 CARD_Y_PLAIN）。 */
    if (prog_active()) draw_progress(fb);

    /*
     * 三行信息，全部取自 app descriptor 与 IDF 编译期宏——开机画面与二进制
     * 天然一致，不需要人工同步。
     */
    const esp_app_desc_t *app = esp_app_get_description();
    char line[64];
    int y = layout_info_y();

    if (app) snprintf(line, sizeof(line), "Booting %.32s ...", app->version);
    else     snprintf(line, sizeof(line), "Booting ...");
    {
        const int w = (int)strlen(line) * pk_aa_cell_w(PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, y, line, VERSION_COLOR, PK_AA_M);
        y += PK_AA_M_H + INFO_LINE_GAP;
    }

    if (app) snprintf(line, sizeof(line), "Built %.16s %.8s", app->date, app->time);
    else     snprintf(line, sizeof(line), "Built %s %s", __DATE__, __TIME__);
    {
        const int w = (int)strlen(line) * pk_aa_cell_w(PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, y, line, VERSION_COLOR, PK_AA_M);
        y += PK_AA_M_H + INFO_LINE_GAP;
    }

    snprintf(line, sizeof(line), "ESP-IDF v%d.%d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
    {
        const int w = (int)strlen(line) * pk_aa_cell_w(PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, y, line, VERSION_COLOR, PK_AA_M);
    }

    /*
     * 演示模式横幅。
     *
     * 为什么开机画面要单独画一遍：演示模式**跨重启存活**（存在 NVS，否则每次
     * 上电都要重开，便利性就没了），而运行时那枚常驻徽标画在 LVGL 控件层——
     * splash 早于 LVGL 初始化，控件层此刻还不存在，盖不到这一屏。于是「上一次
     * 谁把它打开了」这件事，如果不在这里说一句，用户重新上电后要等到 PFD 起来
     * 才知道。开机这几秒恰恰是人最认真看屏幕的时候。
     *
     * 满宽红条而不是一枚小徽标：这一屏没有别的内容跟它抢注意力，能做多醒目就
     * 做多醒目。位置在三行构建信息之下、屏底之上那片空白里（实测末行结束于
     * y≈424，屏高 480）。
     */
    if (pk_demo_enabled()) {
        const int bh = 40;
        const int by = PK_DISPLAY_H - bh - 8;
        fill_rect(fb, 0, by, PK_DISPLAY_W, by + bh, DEMO_BANNER_COLOR);
        const char *msg = pk_i18n_text(PK_TR_DEMO_SPLASH);
        /* 宽度走 pk_aa_text_width：这一句是中文，按 strlen×cell 算出来的是字节
         * 数，横幅会整体左偏三分之一。 */
        const int w = pk_aa_text_width(msg, PK_AA_M);
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                   (PK_DISPLAY_W - w) / 2, by + (bh - PK_AA_M_H) / 2,
                   msg, DEMO_TEXT_COLOR, PK_AA_M);
    }
}

void pk_boot_splash_progress(const char *label, int done, int total)
{
    /* 钳位而不是断言：这是开机路径，参数算错了也要继续把画面画出去，不能因为
     * 一个显示用的计数把整机拦在这儿。 */
    if (total < 1)     total = 1;
    if (done  < 0)     done  = 0;
    if (done  > total) done  = total;

    s_prog_done  = done;
    s_prog_total = total;
    snprintf(s_prog_label, sizeof(s_prog_label), "%s", label ? label : "");

    /* 取 framebuffer + 重画 + 推屏，调用方一行搞定。
     *
     * 整屏重画（而不是只擦进度条那一带）是刻意的：这几个调用点之间隔着秒级的
     * 阻塞动作，一屏 800×480 的 fill + 一次 full flush 相比之下可以忽略，而
     * 局部重绘要额外背上"底下那一版画的是什么"的假设——版面一改就悄悄错位。 */
    uint16_t *fb = pk_display_framebuffer();
    if (fb == NULL) return;   /* 点屏失败，app_main 那边已经记过日志 */
    pk_boot_splash_render(fb);
    (void)pk_display_flush_full();
}
