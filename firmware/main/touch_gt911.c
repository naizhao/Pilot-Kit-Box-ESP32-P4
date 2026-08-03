/*
 * touch_gt911.c — GT911 电容触摸 → LVGL 输入设备。
 *
 * 没有它，FAB、二级页面的三条退路全都点不动：pk_ui_nav.c 里那些
 * lv_indev_active() / lv_indev_get_vect() 取的是「当前正在上报的输入设备」，
 * 而在本文件出现之前，这台机器一个输入设备都没注册过。
 *
 * 硬件事实（docs/hardware/board_pinout-zh_CN.md §GT911，据实物原理图）
 * ------------------------------------------------------------------
 *   SDA GPIO7 / SCL GPIO8   与 BNO085、BMP388 共用 I²C0，走 pk_i2c0_bus_get()
 *   RESET GPIO23
 *   INT   GPIO2，**R35 默认不贴**
 *
 * INT 不贴带来两个后果，都不是可选项：
 *
 *   1. 不能用中断，只能轮询。这也是为什么下面走 LVGL 的定时读取而不是
 *      esp_lcd_touch_register_interrupt_callback()。
 *
 *   2. I²C 地址不确定。GT911 在复位释放的瞬间采样 INT 电平来决定自己是
 *      0x5D 还是 0x14，而 INT 悬空时采到什么取决于芯片内部下拉——**没有**
 *      可依赖的默认值。驱动组件也帮不上忙：它那段「拉 INT 选地址」的时序
 *      只在 int_gpio_num 有效时才走（esp_lcd_touch_gt911.c 第 101 行的条件），
 *      INT 传 NC 就直接跳到 else 分支，只做一次普通复位。
 *      所以这里先在总线上探两个地址，探到哪个用哪个。
 */
#include "touch_gt911.h"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"     /* 触摸自愈重试的退避计时 */
#include "lvgl.h"

#include "display.h"
#include "imu_task.h"      /* pk_i2c0_bus_get() */
#include "pk_i2c0_recover.h"  /* 总线恢复代数 —— 见 pk_touch_retry_after_bus_recovery() */
#include "about_page.h"
#include "adsb_list.h"
#include "cal_wizard.h"
#include "diag_page.h"
#include "apt_detail_page.h"
#include "keyboard_page.h"
#include "nav_grid_page.h"
#include "search_page.h"
#include "settings_page.h"
/* pk_ui_nav.h 已不需要：本文件唯一用到它的地方是 dock 展开时的整体让路，
 * 随 dock 一起删了。FAB 的事件走 LVGL 自己的通路，不经过这里。 */
#include "pk_touch_arbiter.h"
#include "traffic_page.h"
#include "map_page.h"
#include "ui_state.h"

static const char *TAG = "touch";

#define TOUCH_RST_GPIO      GPIO_NUM_23
#define TOUCH_PROBE_MS      50

/* GT911 报的是**面板原生**坐标系（竖屏 480×800），与固件的逻辑横屏无关。 */
#define TOUCH_NATIVE_W      480
#define TOUCH_NATIVE_H      800

static esp_lcd_touch_handle_t s_tp;

/* 这一次按压归谁：自绘页面还是 LVGL 控件。归属在按下那一刻定死，松手才作废。
 *
 * 取代原来那个 s_armed 布尔量。s_armed 只记「命中判定还没用掉」，没记「这次
 * 按压已经归了 LVGL」——于是按在 FAB 上的拖动，手指划进列表区时会被列表反手
 * 抢走。规则与踩坑过程见 pk_touch_arbiter.h。 */
static pk_touch_arbiter_t s_arb;

/*
 * 原生触摸坐标 → 逻辑屏坐标。
 *
 * display.c 每帧把 800×480 的逻辑 framebuffer 顺时针旋转 90° 送进 480×800 的
 * 面板（那里用 PPA 的 270° CCW 表达同一件事，见 display.c 的 rotation_angle）。
 * 触摸面板没有参与这次旋转，它报的仍是原生坐标，所以这里要把旋转**反过来**
 * 做一次，否则手指点左上角、光标落在右上角。
 *
 *   顺时针 90°：逻辑 (lx, ly) → 原生 (px, py) = (LH-1-ly, lx)
 *   反解：      lx = py,  ly = LH-1-px          （LH = 逻辑高 = 480）
 *
 * 自己算而不用 esp_lcd_touch 的 swap_xy/mirror 标志位：那三个开关的组合顺序
 * 藏在组件内部，出了偏差只能靠试；写成两行算式，对不对一眼就能看出来。
 */
static inline void native_to_logical(uint16_t px, uint16_t py, int *lx, int *ly)
{
    *lx = py;
    *ly = (PK_DISPLAY_H - 1) - px;

    if (*lx < 0) *lx = 0;
    if (*ly < 0) *ly = 0;
    if (*lx > PK_DISPLAY_W - 1) *lx = PK_DISPLAY_W - 1;
    if (*ly > PK_DISPLAY_H - 1) *ly = PK_DISPLAY_H - 1;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);

    /* 只取第一个触点：本机全部交互都是单点（拖 FAB、点页签、右滑返回、
     * 雷达量程 +/−）。曾经规划过双指捏合切量程，2026-07-29 从 spec 取消——
     * 量程只有三四档，用不上捏合的连续性，而它恰恰是戴手套、颠簸时最难
     * 做对的手势。所以这里不打算扩多点。 */
    /*
     * 取点用 esp_lcd_touch_get_data() 而不是 esp_lcd_touch_get_coordinates()。
     * 后者在 esp_lcd_touch 1.2.1 里已标 [[deprecated]]（见组件头
     * esp_lcd_touch.h:316），声明 2.0.0 移除，是本工程唯一的编译告警。
     *
     * 两者**坐标语义完全相同**，不是换算口径的改动：
     *   - 二者都直接调同一个 tp->get_xy()（esp_lcd_touch.c:73 与 :125），
     *     GT911 那侧就是把 tp->data.coords[i].x/y 原样抄出来
     *     （esp_lcd_touch_gt911.c 的 get_xy），仍是**面板原生**竖屏坐标；
     *   - 之后两条路径跑的是同一段 process_coordinates 回调与同一段
     *     mirror_x/mirror_y/swap_xy 软件校正。本工程 process_coordinates 为
     *     NULL、三个 flag 全 0（见 pk_touch_init 的 tp_cfg），两边都不动坐标。
     * 所以 native_to_logical() 那层旋转换算**不受影响**，一个数都不用改。
     *
     * 唯一要当心的是「没触摸」时的约定变了：旧 API 用返回值 false 表示没摸到；
     * 新 API 返回 ESP_OK 并把点数置 0，**且提前 return、不 memset 出参**
     * （esp_lcd_touch.c:127）。因此 cnt 必须先清零、且只有 cnt > 0 时才可以读
     * pt 里的坐标。GT911 的 get_xy 每次都会写 *point_num（无触摸即 0），所以
     * 「cnt > 0」与旧代码的「pressed && cnt > 0」判定完全等价。
     */
    esp_lcd_touch_point_data_t pt = { 0 };
    uint8_t  cnt = 0;

    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (esp_lcd_touch_get_data(s_tp, &pt, &cnt, 1) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (cnt > 0) {
        const uint16_t px = pt.x, py = pt.y;
        int lx, ly;
        native_to_logical(px, py, &lx, &ly);
        data->point.x = lx;
        data->point.y = ly;
        data->state   = LV_INDEV_STATE_PRESSED;

        /* 交通页在雷达上叠了几个自绘按钮（朝向 / 量程）。它们不是 LVGL 控件，
         * 命中判定只能在这里做——落在按钮上就把这一下吃掉，报成 RELEASED，
         * 否则手指同时会点到底下的 FAB。
         *
         * 只在按下的那一瞬间触发一次：命中判定归 s_arb 管（HITTEST 只在按压的
         * 第一帧出现），否则手指停在按钮上不动，每帧都会切一次朝向。 */
        /*
         * 2026-08-02：这里曾有一段「dock 展开时页面的自绘命中一律让路」——
         * dock 是浮在页面之上的 LVGL 控件、正好压在列表页的数据区上，而列表
         * 把**整个数据区**都当命中区，于是点 dock 页签的坐标先被列表吃掉。
         * dock 已由全屏导航网格取代，网格是自绘模态层，走下面 pk_ui_modal_top()
         * 那条正规的模态优先级，不再需要这条特例。
         */
        const pk_ui_mode_t m = pk_ui_get_mode();
        bool eaten = false;

        switch (pk_touch_arbiter_press(&s_arb)) {
        case PK_TOUCH_ACTION_HITTEST:
            /*
             * 模态层先问，且与 mode 无关。
             *
             * 键盘编辑器与搜索页是浮在**某一页**之上的整屏层（渲染那侧同样是
             * 这个次序，见 pfd.c）。底下那一页此刻在屏上根本看不见，它的命中表
             * 却还留着上一帧的几何——不挡掉的话，点键盘上的「Q」会被判成点中了
             * 设置页的某个分段控件。
             *
             * 原先这两条是写成 `m == PK_UI_MODE_SETTINGS && 键盘…` 的，等于把
             * 「键盘只可能从设置页打开」这个当时的事实钉进了分派表；搜索页从
             * 地图页打开，键盘又能从搜索页打开，那条 case 就再也覆盖不到。
             * 模态层的归属只该由它自己的 active() 决定，与当前是哪一页无关。
             *
             * 2026-08-02 加了机场详情页（第三层）之后，次序收进
             * pk_ui_modal_top()（apt_detail_page.h），与 pfd.c 共用同一个
             * 纯函数——"看得见的就是点得中的"从此不再依赖两处注释对齐。
             *
             * 导航网格排在最前，而 pfd.c 那侧它**不进**渲染的 if/else 链——
             * 这处不对称是有意的，不是漏改：网格是**半透明**覆盖层，底页要
             * 照常画完再叠上去（pfd.c 约 165 行那段说明了为什么：darken_rect
             * 就地压暗 + canvas 是单块常驻缓冲，只画网格不画底页两三帧就全黑）；
             * 而触摸没有"半透明"这回事，它盖在最上面，就该最先拿到触摸。
             */
            switch (pk_ui_modal_top(pk_nav_grid_page_active(),
                                    pk_keyboard_page_active(),
                                    pk_apt_detail_page_active(),
                                    pk_search_page_active())) {
            case PK_UI_MODAL_NAVGRID:
                eaten = pk_nav_grid_page_touch(lx, ly);
                break;
            case PK_UI_MODAL_KEYBOARD:
                eaten = pk_keyboard_page_touch(lx, ly);
                break;
            case PK_UI_MODAL_DETAIL:
                eaten = pk_apt_detail_page_touch(lx, ly);
                break;
            case PK_UI_MODAL_SEARCH:
                eaten = pk_search_page_touch(lx, ly);
                break;
            case PK_UI_MODAL_NONE:
            default:
                eaten = (m == PK_UI_MODE_TRAFFIC   && pk_traffic_page_touch(lx, ly))
                     || (m == PK_UI_MODE_MAP       && pk_map_page_touch(lx, ly))
                     || (m == PK_UI_MODE_ADSB_LIST && pk_adsb_list_touch(lx, ly))
                     || (m == PK_UI_MODE_DIAG      && pk_diag_page_touch(lx, ly))
                     || (m == PK_UI_MODE_SETTINGS  && pk_settings_page_touch(lx, ly))
                     || (m == PK_UI_MODE_ABOUT     && pk_about_page_touch(lx, ly))
                     /* 校准页只有「稍后再说」一个命中区，其余落点放行给
                      * LVGL（FAB → 导航网格那条退路要照常可用）。这一页在
                      * 4.3″ 板上没有别的退路：MODE 键已经没有了。 */
                     || (m == PK_UI_MODE_CAL_WIZARD && pk_cal_wizard_touch(lx, ly));
                break;
            }
            /* 归属就此定死，松手前不再回头问——这一行是「拖 FAB 拖到一半
             * 被列表抢走」那个 bug 的闸门，别改成每帧重判。 */
            pk_touch_arbiter_settle(&s_arb, eaten);
            break;

        case PK_TOUCH_ACTION_DRAG:
            /* 模态层同样排在最前，理由同上：这次按压归了模态层，后续帧就不能
             * 落到底下那一页的滚动上。键盘不滚动但仍然要吃掉（不吃就漏给
             * LVGL 或底下的页面），搜索页要滚。 */
            {
                const pk_ui_modal_t modal =
                    pk_ui_modal_top(pk_nav_grid_page_active(),
                                    pk_keyboard_page_active(),
                                    pk_apt_detail_page_active(),
                                    pk_search_page_active());
                /* 网格要滑动翻页，续帧必须给它。 */
                if (modal == PK_UI_MODAL_NAVGRID) {
                    eaten = pk_nav_grid_page_drag(lx, ly);
                    break;
                }
                if (modal == PK_UI_MODAL_KEYBOARD) { eaten = true; break; }
                if (modal == PK_UI_MODAL_DETAIL) {
                    eaten = pk_apt_detail_page_drag(lx, ly);
                    break;
                }
                if (modal == PK_UI_MODAL_SEARCH) {
                    eaten = pk_search_page_drag(lx, ly);
                    break;
                }
            }
            if (m == PK_UI_MODE_MAP) {
                /* 地图页没有独立的 drag() 入口——单指拖动平移的每一帧都重复调
                 * pk_map_page_touch()，由它内部的按下/续行状态机区分"新按下"
                 * 还是"接着上一次拖"（见 map_page.h 顶部注释）。 */
                eaten = pk_map_page_touch(lx, ly);
            } else if (m == PK_UI_MODE_DIAG) {
                eaten = pk_diag_page_drag(lx, ly);
            } else if (m == PK_UI_MODE_SETTINGS) {
                eaten = pk_settings_page_drag(lx, ly);
            } else if (m == PK_UI_MODE_ABOUT) {
                /* 关于页正文比屏高，同样要按住不放地连续滚动，判定与 diag/settings 一致。 */
                eaten = pk_about_page_drag(lx, ly);
            } else if (m == PK_UI_MODE_ADSB_LIST) {
                /* 按住不放的后续帧交给列表做滚动。表格的滑动必须是连续的，
                 * 只在按下那一瞬间取一次坐标是滚不起来的——这也是为什么这里
                 * 不能沿用交通页那种「一次按下只处理一次」的写法。 */
                eaten = pk_adsb_list_drag(lx, ly);
            }
            break;

        case PK_TOUCH_ACTION_YIELD:
            /* 这次按压在按下那一刻就归了 LVGL（比如落在 FAB 上）。手指之后
             * 划到哪儿都不关页面的事，eaten 保持 false，原样交给 LVGL。 */
            break;
        }

        if (eaten) data->state = LV_INDEV_STATE_RELEASED;
        /* 标定用：真机上点四角，核对原生与逻辑两组数是否符合上面的算式。
         * 若发现 X/Y 反了或某轴镜像，改 native_to_logical 一处即可。 */
        ESP_LOGD(TAG, "native(%u,%u) -> logical(%d,%d)", px, py, lx, ly);
    } else {
        /* 松手才重新装弹。
         *
         * 上一版把这行写在 pressed 分支**内部**（if (!pressed) s_armed = true;），
         * 那里 !pressed 恒假——于是第一次点击把闸门关上之后再也没机会恢复，
         * 所有自绘按钮从此全部失灵。 */
        pk_touch_arbiter_release(&s_arb);
        pk_nav_grid_page_touch_up();
        pk_traffic_page_touch_up();
        pk_map_page_touch_up();
        pk_adsb_list_touch_up();
        pk_diag_page_touch_up();
        pk_settings_page_touch_up();
        pk_keyboard_page_touch_up();
        pk_apt_detail_page_touch_up();
        pk_search_page_touch_up();
        pk_about_page_touch_up();
        pk_cal_wizard_touch_up();
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* 在总线上探地址。返回探到的地址，两个都不在则返回 0。 */
static uint8_t probe_addr(i2c_master_bus_handle_t bus)
{
    const uint8_t candidates[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,          /* 0x5D，INT 上电为低 */
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,   /* 0x14，INT 上电为高 */
    };
    for (size_t i = 0; i < sizeof(candidates); ++i) {
        if (i2c_master_probe(bus, candidates[i], TOUCH_PROBE_MS) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 found at 0x%02X", candidates[i]);
            return candidates[i];
        }
    }
    return 0;
}

esp_err_t pk_touch_init(void)
{
    i2c_master_bus_handle_t bus = pk_i2c0_bus_get();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready — touch disabled");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t addr = probe_addr(bus);
    if (addr == 0) {
        /* 不是致命错误：没有触摸设备照样能飞，PFD 该画还画。但必须喊出来，
         * 否则现象只是「屏幕点不动」，会被当成 UI 的 bug 查上半天。 */
        ESP_LOGE(TAG, "GT911 not responding at 0x5D or 0x14 — touch disabled");
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    /* 组件那个宏是给**旧** I2C 驱动写的，不含 scl_speed_hz；新驱动把它原样
     * 传给 i2c_master_bus_add_device()，0 会被判为非法参数。实测现象就是
     * 「GT911 found at 0x5D」之后紧跟一条 panel_io_i2c ESP_ERR_INVALID_ARG——
     * 芯片明明在，却建不出 io。
     *
     * 取 400 kHz 与总线上其余设备一致（imu_task.c 的 IMU_I2C_HZ、baro_task.c
     * 的 BARO_I2C_HZ 都是这个值），GT911 本身也支持到 400 kHz。 */
    io_cfg.scl_speed_hz = 400000;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io_i2c failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_NATIVE_W,
        .y_max = TOUCH_NATIVE_H,
        .rst_gpio_num = TOUCH_RST_GPIO,
        /* INT 走线未贴（R35），传 NC：组件会跳过「拉 INT 选地址」那段时序，
         * 只做一次普通复位——地址已经由上面探出来了。 */
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset     = 0,        /* RST 低有效 */
            .interrupt = 0,
        },
        /* 旋转在 native_to_logical() 里一次做完，这里全部保持原样。 */
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gt911 init failed: %s", esp_err_to_name(err));
        /* io 已经建出来了,不还回去的话每重试一次就漏一个 device handle
         * （2026-08-03 加了总线恢复后的重试才让这条路径可能跑不止一次）。 */
        (void)esp_lcd_panel_io_del(io);
        return err;
    }

    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) return ESP_ERR_NO_MEM;
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    ESP_LOGI(TAG, "GT911 ready: native %dx%d -> logical %dx%d (90 CCW)",
             TOUCH_NATIVE_W, TOUCH_NATIVE_H, PK_DISPLAY_W, PK_DISPLAY_H);
    return ESP_OK;
}

/* 最近一次已经据此重试过的总线恢复代数。 */
static uint32_t s_bus_gen_tried;

/* 独立重试的下一次到期时刻与当前退避间隔。 */
static int64_t  s_next_retry_us;
static int64_t  s_retry_backoff_us = PK_TOUCH_RETRY_FIRST_US;

void pk_touch_retry_tick(void)
{
    /* 已经成功过就一个指针比较,直接回。**不能**重跑:pk_touch_init() 末尾会
     * lv_indev_create(),跑第二遍就多一个输入设备。 */
    if (s_tp != NULL) return;

    const int64_t  now = esp_timer_get_time();
    const uint32_t gen = pk_i2c0_recover_generation();

    /* 两条独立的触发路径,缺一不可:
     *
     * ① 总线刚被救回来一轮 —— 立刻重试,不等退避。这是 2026-08-03 那次真机
     *    日志里的形态:GT911 初始化中途把 I²C0 拖塌,baro/imu 跟着挂,恢复模块
     *    把总线救回来之后,触摸得跟着补一刀。
     *
     * ② 到点了 —— 与总线恢复无关的**自愈**路径,这一条是 2026-08-04 补的。
     *    上机后出现"偶发拉不起触摸,重启才好":总线只在 GT911 初始化那一瞬间
     *    抖了一下、随后自己好了,baro 和 imu 全程正常,于是没有任何器件报告
     *    失败 → 不触发总线恢复 → 代数永远停在 0 → 只看代数的话这里每次都
     *    直接 return,触摸再也没有第二次机会。
     *
     *    触摸是这块板子唯一的输入手段(4.3″ 板没有实体键),拉不起来等于整机
     *    不可用、只能断电重启,所以宁可一直退避重试也不设次数上限。 */
    const bool bus_recovered = (gen != s_bus_gen_tried);
    const bool due           = (now >= s_next_retry_us);
    if (!bus_recovered && !due) return;

    s_bus_gen_tried = gen;

    if (bus_recovered) {
        ESP_LOGW(TAG, "I²C0 总线已复位(第 %lu 轮)— 重试触摸初始化",
                 (unsigned long)gen);
        s_retry_backoff_us = PK_TOUCH_RETRY_FIRST_US;   /* 换了条件,退避重来 */
    } else {
        ESP_LOGW(TAG, "触摸仍未就绪 — 自愈重试(下次间隔 %lld ms)",
                 (long long)(s_retry_backoff_us / 1000));
    }

    const esp_err_t err = pk_touch_init();
    if (err == ESP_OK) {
        s_retry_backoff_us = PK_TOUCH_RETRY_FIRST_US;
        return;
    }

    /* 失败:指数退避到封顶。pk_touch_init() 里探两个地址各 50 ms,失败一次
     * 最多占总线 100 ms,别让它每帧都来抢 I²C。 */
    s_next_retry_us    = now + s_retry_backoff_us;
    s_retry_backoff_us = (s_retry_backoff_us * 2 > PK_TOUCH_RETRY_MAX_US)
                       ? PK_TOUCH_RETRY_MAX_US
                       : s_retry_backoff_us * 2;
}
