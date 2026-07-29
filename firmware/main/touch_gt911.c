/*
 * touch_gt911.c — GT911 电容触摸 → LVGL 输入设备。
 *
 * 没有它，dock、FAB、二级页面的三条退路全都点不动：pk_ui_nav.c 里那些
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
#include "lvgl.h"

#include "display.h"
#include "imu_task.h"      /* pk_i2c0_bus_get() */
#include "adsb_list.h"
#include "diag_page.h"
#include "settings_page.h"
#include "pk_ui_nav.h"
#include "traffic_page.h"
#include "ui_state.h"

static const char *TAG = "touch";

#define TOUCH_RST_GPIO      GPIO_NUM_23
#define TOUCH_PROBE_MS      50

/* GT911 报的是**面板原生**坐标系（竖屏 480×800），与固件的逻辑横屏无关。 */
#define TOUCH_NATIVE_W      480
#define TOUCH_NATIVE_H      800

static esp_lcd_touch_handle_t s_tp;

/* 自绘按钮的「一次按下只触发一次」闸门，松手时重新装弹。 */
static bool s_armed = true;

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
    uint16_t px = 0, py = 0;
    uint8_t  cnt = 0;

    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    bool pressed = esp_lcd_touch_get_coordinates(s_tp, &px, &py, NULL, &cnt, 1);

    if (pressed && cnt > 0) {
        int lx, ly;
        native_to_logical(px, py, &lx, &ly);
        data->point.x = lx;
        data->point.y = ly;
        data->state   = LV_INDEV_STATE_PRESSED;

        /* 交通页在雷达上叠了几个自绘按钮（朝向 / 量程）。它们不是 LVGL 控件，
         * 命中判定只能在这里做——落在按钮上就把这一下吃掉，报成 RELEASED，
         * 否则手指同时会点到底下的 FAB。
         *
         * 只在按下的那一瞬间触发一次：用 s_armed 记住上一帧的按压状态，
         * 否则手指停在按钮上不动，每帧都会切一次朝向。 */
        /*
         * dock 展开时，页面的自绘命中一律让路。
         *
         * dock 是浮在页面之上的 LVGL 控件，覆盖屏幕中部——正好压在列表页的
         * 数据区上。而列表把**整个数据区**都当命中区（不像交通页只有三个小
         * 按钮），于是点 dock 页签的坐标先被列表吃掉，dock 永远收不到，
         * 表现就是「进了 list 就切不走页」。
         *
         * FAB 之所以不受影响，纯属巧合：它在 x >= 728，刚好落在列表内容区
         * 右缘（724）之外。这种靠坐标碰巧不重叠的"安全"不能依赖，所以这里
         * 按状态显式让路。
         */
        const pk_ui_mode_t m = pk_ui_get_mode();
        bool eaten = false;

        if (pk_ui_nav_dock_open()) {
            /* 让路期间把页面的手势状态**取消**掉（不是 touch_up）：否则
             * dock 展开前落在列表上的那次按下会被当成一次完整点击提交，
             * 手指还没松抽屉就自己开了。 */
            pk_adsb_list_touch_cancel();
            pk_diag_page_touch_cancel();
            pk_settings_page_touch_cancel();
        } else if (s_armed) {
            eaten = (m == PK_UI_MODE_TRAFFIC   && pk_traffic_page_touch(lx, ly))
                 || (m == PK_UI_MODE_ADSB_LIST && pk_adsb_list_touch(lx, ly))
                 || (m == PK_UI_MODE_DIAG      && pk_diag_page_touch(lx, ly))
                 || (m == PK_UI_MODE_SETTINGS  && pk_settings_page_touch(lx, ly));
            if (eaten) s_armed = false;
        } else if (m == PK_UI_MODE_DIAG) {
            eaten = pk_diag_page_drag(lx, ly);
        } else if (m == PK_UI_MODE_SETTINGS) {
            eaten = pk_settings_page_drag(lx, ly);
        } else if (m == PK_UI_MODE_ADSB_LIST) {
            /* 按住不放的后续帧交给列表做滚动。表格的滑动必须是连续的，
             * 只在按下那一瞬间取一次坐标是滚不起来的——这也是为什么这里
             * 不能沿用交通页那种「一次按下只处理一次」的写法。 */
            eaten = pk_adsb_list_drag(lx, ly);
        }

        if (eaten) data->state = LV_INDEV_STATE_RELEASED;
        /* 标定用：真机上点四角，核对原生与逻辑两组数是否符合上面的算式。
         * 若发现 X/Y 反了或某轴镜像，改 native_to_logical 一处即可。 */
        ESP_LOGD(TAG, "native(%u,%u) -> logical(%d,%d)", px, py, lx, ly);
    } else {
        /* 松手才重新装弹。
         *
         * 上一版把这行写在 pressed 分支**内部**（if (!pressed) s_armed = true;），
         * 那里 !pressed 恒假——于是第一次点击把 s_armed 置 false 之后再也没机会
         * 恢复，所有自绘按钮从此全部失灵。 */
        s_armed = true;
        pk_traffic_page_touch_up();
        pk_adsb_list_touch_up();
        pk_diag_page_touch_up();
        pk_settings_page_touch_up();
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
