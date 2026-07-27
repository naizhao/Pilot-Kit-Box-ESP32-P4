/*
 * pfd_layout.h — PFD 各区块的布局基准，按面板分辨率分档。
 *
 * 为什么需要它
 * ------------
 * 原先每个绘制模块（pfd_attitude.c / pfd_tape.c / pfd_hsi.c …）在文件
 * 顶部各自 #define 一套坐标，全部是照着 320×240 手算出来的绝对值。
 * 迁移到 800×480 时这些值一个都不能用 —— 高度带会落在画面中间、HSI
 * 圆心偏左、俯仰刻度挤在左上角。
 *
 * 这里把它们收拢成单一来源：两套分辨率各自给出明确基准，再派生出
 * 各模块要用的坐标。模块只引用本文件的宏，不再自己算。
 *
 * 为什么不用统一公式推导两套布局
 * ------------------------------
 * 因为两者的构图模型不同，硬套公式反而模糊：
 *
 *   320×240（现役）—— 重叠式：HSI 是个半圆，圆心压在屏幕底边之外
 *     (CY = H)，只露出上半部分，并且在 y 方向与两侧 tape 重叠
 *     （tape 到 y=168，HSI 从 y=138 起），靠 x 不冲突来共存。
 *     小屏上这是必要的妥协。
 *
 *   800×480（新屏）—— 分区式：底部 140 px 完整划给 HSI/雷达区，
 *     与 tape 在 y 上不再重叠。依据 docs/ux/box-4.3-ux-spec.md §5.1。
 *
 * 所以两套各自列明，只把真正同构的部分（水平中线、tape 左右位置、
 * HSI 圆心 x/y）做成共用派生。
 */
#pragma once

#include "display.h"

/* ══════════════════════════════════════════════════════════════
 * 分档基准
 * ══════════════════════════════════════════════════════════════ */
#if PK_DISPLAY_W >= 800

/* ---- 4.3" / 5" 800×480（分区式）---------------------------------
 * 依据 docs/ux/box-4.3-ux-spec.md §5.1：
 *   顶栏 48 ｜ 主区 292（速度带 100 + 姿态 600 + 高度带 100）｜ HSI 140
 */
#define PFD_BAR_BOT         48      /* 状态栏下沿 = 主区上沿           */
#define PFD_TAPE_W         100      /* 左右两条 tape 各自的宽度        */
#define PFD_TAPE_TOP        48
#define PFD_TAPE_BOT       298      /* tape 刻度带下沿                 */
#define PFD_METRIC_TOP     300      /* 速度带下方 km/h + mph 换算区    */
#define PFD_METRIC_BOT     340
#define PFD_HSI_TOP        340      /* 底部 HSI/雷达区上沿             */
#define PFD_HSI_R          150      /* 半圆半径（圆心在底边外）        */
#define PFD_ATT_H          292      /* 姿态仪竖直跨度                  */
#define PFD_BANK_ARC_CY    250      /* 坡度刻度弧圆心 y                */
#define PFD_BANK_ARC_R     215      /* 坡度刻度弧半径                  */
#define PFD_PIXELS_PER_DEG   7      /* 俯仰 1° 对应像素（320 屏为 3）  */
/* 航向框按 M 档（cell 24×40）实测尺寸开：4 字符「018°」= 96 px，
 * 加左右 5 px 内边距与 1 px 边框。原本的 120×40 是照 M 档预估的，但里面
 * 填的还是 48 px 宽的 cockpit 字，白白空出 72 px。 */
#define PFD_HDGBOX_W       108      /* 航向数字框                      */
#define PFD_HDGBOX_H        48

#else

/* ---- 2.4" 320×240（现役，重叠式）------------------------------
 * 保持与迁移前逐像素一致，这些值是历史上手工调出来的，勿动。
 */
#define PFD_BAR_BOT         18
#define PFD_TAPE_W          64
#define PFD_TAPE_TOP        18
#define PFD_TAPE_BOT       168
#define PFD_METRIC_TOP     170
#define PFD_METRIC_BOT     208
#define PFD_HSI_TOP        138
#define PFD_HSI_R           65
#define PFD_ATT_H          160
#define PFD_BANK_ARC_CY    130
#define PFD_BANK_ARC_R     100
#define PFD_PIXELS_PER_DEG   3
#define PFD_HDGBOX_W        54
#define PFD_HDGBOX_H        18

#endif

/* ══════════════════════════════════════════════════════════════
 * 字号档位
 *
 * 位图字体：scale N → 视觉 5N×7N px，cell 6N×8N px。
 * 依据 docs/ux/box-4.3-ux-spec.md §2 的字号阶梯换算（4.3" 屏 8.54 px/mm）：
 *
 *   scale 2 → 视觉 14 px = 1.64 mm   小屏够用，新屏偏小
 *   scale 3 → 视觉 21 px = 2.46 mm   ≈ 规格 S 级 2.5 mm
 *   scale 4 → 视觉 28 px = 3.28 mm   ≈ 规格 M 级 3.0 mm
 *   scale 6 → 视觉 42 px = 4.92 mm   ≈ 规格 XL 级 5.0 mm
 *
 * 规格中"低于 2.1 mm 一律禁止"，故新屏最小档为 scale 3。
 * ══════════════════════════════════════════════════════════════ */
#if PK_DISPLAY_W >= 800
#define PFD_FS_BAR          3       /* 状态栏                        */
#define PFD_BAR_TEXT_Y      9       /* 30 px cell 在 48 px 状态栏内垂直居中 */
#define PFD_BAR_MARGIN_L   12
#define PFD_BAR_MARGIN_R   16
#define PFD_BAR_GAP_LABEL   8       /* 标签与其数值之间              */
#define PFD_BAR_GAP_WORD   16       /* 独立词组之间                  */
#else
#define PFD_FS_BAR          2
#define PFD_BAR_TEXT_Y      1
#define PFD_BAR_MARGIN_L    6
#define PFD_BAR_MARGIN_R    8
#define PFD_BAR_GAP_LABEL   4
#define PFD_BAR_GAP_WORD    8
#endif

/* ══════════════════════════════════════════════════════════════
 * 共用派生量（两套分辨率同构，无需分档）
 * ══════════════════════════════════════════════════════════════ */

/* 水平中线。姿态仪、坡度弧、HSI、航向框都以它对齐。 */
#define PFD_CX              (PK_DISPLAY_W / 2)

/* 左侧速度带 */
#define PFD_SPD_X0          0
#define PFD_SPD_X1          PFD_TAPE_W

/* 右侧高度带 —— 贴右边缘，这是原先 256/320 硬编码的真正含义 */
#define PFD_ALT_X0          (PK_DISPLAY_W - PFD_TAPE_W)
#define PFD_ALT_X1          PK_DISPLAY_W

/* 姿态仪：左右铺满，上沿接状态栏 */
#define PFD_ATT_LEFT        0
#define PFD_ATT_TOP         PFD_BAR_BOT

/* 坡度刻度弧圆心 x 恒在中线 */
#define PFD_BANK_ARC_CX     PFD_CX

/* HSI：圆心落在屏幕底边（CY = H），只露出上半圆 */
#define PFD_HSI_BOT         PK_DISPLAY_H
#define PFD_HSI_CX          PFD_CX
#define PFD_HSI_CY          PK_DISPLAY_H

/* 航向数字框：以中线居中，坐落于 HSI 上方 */
#define PFD_HDGBOX_X0       (PFD_CX - PFD_HDGBOX_W / 2)
#define PFD_HDGBOX_X1       (PFD_HDGBOX_X0 + PFD_HDGBOX_W)
#define PFD_HDGBOX_Y0       (PFD_HSI_TOP + 6)
#define PFD_HDGBOX_Y1       (PFD_HDGBOX_Y0 + PFD_HDGBOX_H)

/* ── 右下三行信息框 / 左下本机来源徽标（见 pfd_infobox.c）────────
 *
 * 320 的历史值是 x[256,320) y 170/190/210、行高 18、徽标 x[0,88] y[210,232]。
 *
 * 800 这一档目前**放不下**：spec §5.1 给高度带下方只留了 300…340 共 40 px，
 * 而三行 S 档（cell 30 px）+ 行距需要 96 px。这里先按元素齐全的实际需求给
 * 值（向上占用 tape 尾段），把冲突显性化 —— 垂直空间怎么分配是阶段 4a 第二
 * 步要整体定的事，不在这里偷偷压字号糊弄过去。 */
#if PK_DISPLAY_W >= 800
#define PFD_IB_X0           PFD_ALT_X0
#define PFD_IB_TOP          244
#define PFD_IB_ROW_H         30
#define PFD_IB_ROW_GAP        2
#define PFD_IB_PAD            4
#define PFD_BADGE_W         160
#define PFD_BADGE_H          34
#define PFD_BADGE_Y0        (PK_DISPLAY_H - PFD_BADGE_H - 4)
#define PFD_BADGE_PAD         4
#else
#define PFD_IB_X0           256
#define PFD_IB_TOP          170
#define PFD_IB_ROW_H         18
#define PFD_IB_ROW_GAP        2
#define PFD_IB_PAD            2
#define PFD_BADGE_W          88
#define PFD_BADGE_H          22
#define PFD_BADGE_Y0        210
#define PFD_BADGE_PAD         2
#endif
#define PFD_BADGE_Y1        (PFD_BADGE_Y0 + PFD_BADGE_H)
