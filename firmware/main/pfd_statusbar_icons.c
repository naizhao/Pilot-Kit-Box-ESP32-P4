/*
 * pfd_statusbar_icons.c — 状态栏图标。
 *
 * 为什么要图标
 * ------------
 * 纯文字状态位缺少语义线索：孤零零一个「100%」看不出是电量还是亮度，
 * 「78°C」也看不出是芯片温度、更看不出它是不是异常。加一个电池轮廓、
 * 一个温度计，含义立刻自明。
 *
 * 为什么从手绘改成字形表
 * ----------------------
 * 初版是用绘图原语手工拼的（圆、三角、折线）。形状勉强能认，但在
 * 21 px 高度下蓝牙折线挤成一团、定位标记像个水滴——"像不像"这件事
 * 手绘几何拼不过专业图形设计。
 *
 * 改用 Material Symbols（Google，Apache-2.0），经 gen_pfd_icons.py 生成
 * 4bpp 字形表，与文字**共用同一条 alpha 混合路径**（pk_aa_blit_4bpp），
 * 保证二者边缘处理一致。
 */

#include "pfd_statusbar_icons.h"

#include <stddef.h>

#include "display.h"
#include "pfd_aa_text.h"
#include "pfd_icon_font.h"

/* 图标与后续文字之间的呼吸间隙。 */
#define ICON_GAP  2

/* 充电动画：CHG_1…CHG_6 → CHG_FULL 循环，每帧的驻留时长。
 *
 * 取 300 ms（整周期 2.1 s）而不是手机上常见的 200 ms：座舱里任何接近
 * 1 Hz 的动效都会被余光当成告警闪烁去抓一下注意力，而充电是最不需要
 * 被注意的状态。放慢到 2 s 一圈，看过去明显是"进度"而不是"警报"。 */
#define CHG_FRAME_MS   300
#define CHG_FRAMES     (PK_ICON_BATT_CHG_FULL - PK_ICON_BATT_CHG_1 + 1)

/* 电量 → 图标。
 *
 * battery_android 系列排成 alert → _0…_6 → full 九档连续刻度，形态本身
 * 即读数，不像此前的 battery_horiz_* 只有三档、让 20% 和 4% 渲染成同一
 * 个空壳。九档连号，故这里是一次线性映射，没有特判分支。
 *
 * 两端各有讲究：最低档 alert（电池内嵌感叹号）说的是"该处理了"，与 _0
 * 的"快空了"不是同一句话；最高档 full 是整块实心，_6 内部还留着一条黑，
 * 拿 _6 当 100% 会一直显示成"还差一格"，最容易被误当成故障。 */
static pk_icon_id_t batt_icon_for(const pk_bar_batt_t *b)
{
    if (b->charging) {
        uint32_t frame = (b->uptime_ms / CHG_FRAME_MS) % CHG_FRAMES;
        return (pk_icon_id_t)(PK_ICON_BATT_CHG_1 + frame);
    }
    unsigned step = ((unsigned)b->pct * 8u + 50u) / 100u;   /* 四舍五入到 0…8 */
    if (step > 8u) step = 8u;
    return (pk_icon_id_t)(PK_ICON_BATT_ALERT + step);
}

static pk_icon_id_t icon_id_for(pk_bar_icon_t kind, const pk_bar_batt_t *batt)
{
    switch (kind) {
    case PK_BAR_ICON_REC:  return PK_ICON_REC;
    case PK_BAR_ICON_WARN: return PK_ICON_WARN;
    case PK_BAR_ICON_TEMP: return PK_ICON_TEMP;
    case PK_BAR_ICON_SAT:  return PK_ICON_SAT;
    case PK_BAR_ICON_BLE:  return PK_ICON_BLE;
    case PK_BAR_ICON_SD:   return PK_ICON_SD;
    case PK_BAR_ICON_SD_ALERT: return PK_ICON_SD_ALERT;
    case PK_BAR_ICON_ADSB: return PK_ICON_ADSB;
    case PK_BAR_ICON_BATT: return batt ? batt_icon_for(batt) : PK_ICON_COUNT;
    default:               return PK_ICON_COUNT;      /* 无图标 */
    }
}

int pk_bar_icon_width(pk_bar_icon_t kind)
{
    return (kind == PK_BAR_ICON_NONE) ? 0 : PK_ICON_W + ICON_GAP;
}

int pk_bar_icon_draw(uint16_t *fb, int x, int y,
                     pk_bar_icon_t kind, const pk_bar_batt_t *batt,
                     uint16_t col)
{
    pk_icon_id_t id = icon_id_for(kind, batt);
    if (id >= PK_ICON_COUNT) return 0;

    /* 字重跟随文字设置，避免文字加粗而图标仍纤细的割裂感。 */
    const uint8_t *bitmap = pk_icon_bitmap
                          + (size_t)id * ((PK_ICON_W * PK_ICON_H) / 2);

    pk_aa_blit_4bpp(fb, PK_DISPLAY_W, PK_DISPLAY_H, x, y,
                    bitmap, PK_ICON_W, PK_ICON_H, col);

    return PK_ICON_W + ICON_GAP;
}
