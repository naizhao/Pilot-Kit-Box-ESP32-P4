# V4.7 当前制造输出入口

**当前审阅用制造包已刷新（C99=100nF，保留用户修订布局）。** 工程包与当前设计文件逐字节一致，BOM/CPL 和装配文档也已刷新。

**下单前仍需闭环 Y4 供电超推荐范围及 U22 VREF 并联100nF建议，见[当前电气复查结论](../README.md#airband-reference-clock--v47-recheck)。文件同步通过不等于电气指标已保证。**

- [Gerber/钻孔包](expansion-board-v4-gerber-JLC-V4.7-20260913.zip)
- [配套 KiCad 源包](expansion-board-v4-kicad-V4.7-20260913.zip)

板上的主电源键是 **SW3**（TS24CA 侧按，接 SY6970 `/QON`）。它是纯硬件的：
按住 1.25–2.25s 退 ship mode 开机，BUS 未插时按住 12–18s 触发 BATFET 复位。
**不经过固件**，所以不需要任何 GPIO 参与。

## R62 / CHG_QON_SENSE 候选：未采纳

`candidate-R62-20260913/` 是一版把 `/QON` 经 100k 引到 J1.8 的候选，让固件能感知按键。
**不要拿它下单**，也不要把它的 BOM/CPL 混进 `release/smt/`。2026-09-13 评审结论是不并入，
判据如下（全部来自手册原文，不是推测）：

1. **J1.8 = ESP32-P4 的 GPIO2**（`docs/hardware/board_pinout-zh_CN.md` 的 J3 上排映射表）。
2. SY6970 手册对 `/QON` 只写 `internally pulled up to maintain default high logic`，
   **没给上拉源，也没给阻值**。而 ship mode 下 BATFET 关断、SYS/REGN 均无电，`/QON`
   却必须保持高才能接受按键 —— 上拉只可能来自 **BAT 侧（3.0~4.35V）**。
3. 由此三条未验证风险：
   - **超压**：GPIO2 长期承受约 4.2V，而 P4 GPIO 绝对最大额定 ≈ VDD+0.3 = 3.6V。
     100k 是**限流不是钳位**，压不下来。
   - **失电反灌**：关机后 SYS=0、P4 无电，约 35µA 经 GPIO2 的 ESD 二极管灌进 P4 的
     3.3V 轨，把它抬到约 0.7V，P4 停在部分上电的非法状态。
   - **整机随机断电**：GPIO2 若在复位/下载/误配置下为低，经 100k 对 `/QON` 分压
     `VBAT×100k/(R_int+100k)`。R_int=100k → 2.1V 安全；R_int=500k → 0.7V，低于
     VIH(1.3V) 即判为低，持续 12–18s 触发 BATFET 复位。**刷固件通常超过 18s。**
4. R_int 手册没给 → 这个方案**无法从数据判活也无法判死，只能上实板测**。

另注：载板上 GPIO2 还有一个默认未贴的 R35 位可接 TP_INT
（`docs/hardware/board_pinout-zh_CN.md`），将来若贴上会与 `/QON` 打架。

也别想着绕开这根线用 I²C 读按键：SY6970 的寄存器里**没有任何位反映 `/QON` 引脚电平**，
只有 `BATFET_DIS`（控制）和 `BATFET_RST_EN`（使能）。引线是感知按键的唯一途径 ——
这恰恰说明它是纯增益功能，不是修复项。审计 §1.1 的原话也是「**若要**固件感知长按，
**再**并一个 GPIO」。

要重启这件事，先在实板上测出 `/QON` 的开路电压与 R_int，再据此设计；届时方案另议。

候选板与它的导出记录已移出本目录（2026-09-13 移入回收站），因为同名同版本号的
两套产物放在一起，迟早有人拿错包下单 —— 这件事当天已经发生过一次。完整的修复
报告与验证脚本保留在 `internal/qon-parity-repair/`（不进公开树）。

## 目录说明

| 目录 | 是什么 |
|---|---|
| `smt/` | 当前正式 BOM / CPL，**无 R62** |
| `*-V4.x-*.zip` | 历代正式包；当前快照为本文件顶部两个 V4.7；先处理上述待决项 |

## 本次快照校验

PCB SHA-256：`2e61961aac08d2289737d14d640c14d1f4c381ab087f9153b109cc709611bf98`。

- `expansion-board-v4-gerber-JLC-V4.7-20260913.zip`：`acb70e7250b175e67f538115f207e7b9c33c0944d967f1b5c3ea713c2d4d6a30`
- `expansion-board-v4-kicad-V4.7-20260913.zip`：`03fa3f31d8a03b763bc6ad39ce20ff88707352e36f4bb56e415be6e25d32c03b`

较早的同名 V4.7 两包已保留在 `archive/20260913-before-c99-100nF/`，不可用于新订单。
