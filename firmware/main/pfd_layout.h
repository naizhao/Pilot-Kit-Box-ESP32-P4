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
#define PFD_HDGBOX_W       120      /* 航向数字框                      */
#define PFD_HDGBOX_H        40

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
