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
/* 速度带下方 km/h + mph 换算区。
 *
 * spec §5.1 原本只给了 300…340 这 40 px，且默认宽度等于速度带的 100 px。
 * 两条都不够：S 档一行就 30 px，两行要 60 px；"672 km/h" 八个字符按 S 档
 * 是 136 px。而 6 px 的位图字低于 spec §2 的 18 px 硬下限，不能靠缩字解决。
 *
 * 往右下两个方向借空间：罗盘是半圆（x 285…515），左下这块本来就是空的，
 * 下沿 360 也还在罗盘顶（365）之上，谁都不挡。 */
#define PFD_METRIC_TOP     300
#define PFD_METRIC_BOT     360
#define PFD_METRIC_X1      160
#define PFD_HSI_TOP        340      /* 底部 HSI/雷达区上沿             */
/* 半圆半径（圆心在底边外）。
 *
 * 按 spec §5.1 给 HSI 的 140 px 反推：交通目标画在 R×1.215 的外圈上，
 * 要让**外圈**正好落在 HSI 区上沿，得 R = 140 / 1.215 ≈ 115。
 * 原值 150 会让罗盘本身就占到 y=330，外圈更是顶到 298，把航向框挤得没
 * 地方放 —— 这就是「罗盘过大」的由来。 */
#define PFD_HSI_R          115
#define PFD_ATT_H          292      /* 姿态仪竖直跨度                  */
/* 坡度弧半径按**物理尺寸**对齐 320，而不是按像素等比放大：
 * 320 屏 100 px @167 PPI = 15.2 mm，800 屏 217 PPI 下同样 15.2 mm = 130 px。
 * 原值 215 px 相当于 25 mm，大了 64%，弧顶落到 y=35 直接顶穿 48 px 的状态栏。
 * 圆心与地平线中心重合——它是坡度的旋转参考，偏一点整个姿态仪就是歪的。 */
/* 坡度弧的圆心**下移**到地平线中心以下 46 px。
 *
 * 弧顶被状态栏卡死在 y≈56，圆心若钉在地平线中心（194），半径最多 138 —— 弧
 * 就只有 240 px 宽，在 800 px 面板上显得局促，而且离俯仰梯度太近。把圆心往
 * 下挪，弧顶不动而半径可以做大，弧随之变宽变平：
 *
 *   圆心 240、半径 184 → 弧顶 56，±60° 处跨度 2×184×sin60° = 319 px（40%）
 *
 * G1000 把坡度指示放在屏幕中间 1/3…3/5 宽度内，40% 正在其中。弧不随 roll
 * 旋转（只有 chevron 沿弧走），所以圆心偏离地平线中心不影响读数正确性。 */
#define PFD_BANK_ARC_CY    (PFD_CY + 46)
#define PFD_BANK_ARC_R     184      /* 坡度刻度弧半径                  */
/* 俯仰 1° 对应像素（320 屏为 3）。
 *
 * 取 6 而非 7：航向框坐在姿态区下沿，姿态仪的有效高度实际只到框顶（约
 * 240 px）。7 px/° 时 ±20° 就要 280 px，负刻度几乎永远被挤出可视区——
 * 之前「负刻度不见了」除了裁剪写错，这也是一半原因。6 px/° 下 ±20° 占
 * 240 px，平飞与下俯时负刻度都在。 */
#define PFD_PIXELS_PER_DEG   6
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
#define PFD_METRIC_X1      PFD_TAPE_W
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

/* 地平线 / 姿态仪的几何中心。
 *
 * 800：取**姿态区**中心（48 + 292/2 = 194）。此前 pfd_attitude.c 里写的是
 *      (18 + PK_DISPLAY_H)/2 = 249 —— 18 是 320 的顶栏高度，PK_DISPLAY_H 是
 *      整屏高，两个数都不属于 800，于是地平线整体下沉了 55 px。
 * 320：重叠式布局，姿态仪就是整屏背景，沿用历史值 129。 */
#if PK_DISPLAY_W >= 800
#define PFD_CY              (PFD_ATT_TOP + PFD_ATT_H / 2)
#else
#define PFD_CY              ((18 + PK_DISPLAY_H) / 2)
#endif

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
/* 交通目标画在罗盘外圈上。偏移取半径的 ~21%（320 档即历史上的 R+14），
 * 换屏时外圈与罗盘的相对关系不变。放这里是因为航向框要避开它 —— 两个
 * 模块都要用，就不能只留在 pfd_hsi_traffic.c 里。 */
#define PFD_HSI_TRAFFIC_R   (PFD_HSI_R + PFD_HSI_R * 14 / 65)

#define PFD_HSI_BOT         PK_DISPLAY_H
#define PFD_HSI_CX          PFD_CX
#define PFD_HSI_CY          PK_DISPLAY_H

/* 航向数字框：以中线居中，坐落于 HSI 上方 */
#define PFD_HDGBOX_X0       (PFD_CX - PFD_HDGBOX_W / 2)
#define PFD_HDGBOX_X1       (PFD_HDGBOX_X0 + PFD_HDGBOX_W)
/* 航向框坐在罗盘**外侧**（正上方），框底贴着罗盘顶沿。
 *
 * 原来锚在 PFD_HSI_TOP + 6，那是 HSI 分区的上沿，而罗盘是个圆心在屏幕
 * 底边外的半圆，实际顶沿在 HSI_CY - HSI_R —— 两者不是一回事，于是框落进
 * 了罗盘内部，把刻度和数字压在身下。 */
/* 再让出标签的整个外推距离：相对高度标签沿径向朝外放，正前方（rel≈0）
 * 的那个会顶到外圈之上约「外推 16 + 半个 cell」的位置，只避开外圈本身
 * 还是会被框压住。 */
#define PFD_HDGBOX_Y1       (PFD_HSI_CY - PFD_HSI_TRAFFIC_R - 6 - 16 - 13)
#define PFD_HDGBOX_Y0       (PFD_HDGBOX_Y1 - PFD_HDGBOX_H)

/* ── 右下三行信息框 / 左下本机来源徽标（见 pfd_infobox.c）────────
 *
 * 320 的历史值是 x[256,320) y 170/190/210、行高 18、徽标 x[0,88] y[210,232]。
 *
 * 800 这一档目前**放不下**：spec §5.1 给高度带下方只留了 300…340 共 40 px，
 * 而三行 S 档（cell 30 px）+ 行距需要 96 px。这里先按元素齐全的实际需求给
 * 值（向上占用 tape 尾段），把冲突显性化 —— 垂直空间怎么分配是阶段 4a 第二
 * 步要整体定的事，不在这里偷偷压字号糊弄过去。 */
#if PK_DISPLAY_W >= 800
/* 三行信息框挪到罗盘**右侧**的空当里：罗盘是半圆，圆心在屏幕底边中点，
 * 半径 115，所以 x > 515 那片是空的。此前放在高度带正下方，直接压在 tape
 * 的刻度标签上（"23493ft" 叠着 "23500"），而且 100 px 带宽也装不下 119 px
 * 的数值串。放到这里两个问题一起解决，还不必去动 tape 的垂直空间。 */
#define PFD_IB_X0           600
/* 底边与左下角的来源徽标**对齐**：两块都贴着屏幕下沿，底不齐会在右下留出
 * 一条空档，看着像没画完。368 是当初随手给的，差 14 px。 */
#define PFD_IB_TOP          382
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
