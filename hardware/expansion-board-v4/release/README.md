# V4.7 当前制造输出入口

**当前审阅用制造包已刷新（C99=100nF，C100 VREF 去耦已完成布局布线）。** 工程包内 PCB 与当前板逐字节一致；原理图与本次 PCB 的元件及引脚网络一致，BOM/CPL 和装配文档也已刷新。

**用户已接受本轮样板保留 Y4 的3.3V供电，不再因此要求改板；回板验证启动、频率及输出幅度。U22 VREF 已通过 C100 并联100nF，见[当前电气复查结论](../README.md#airband-reference-clock--v47-recheck)。**

- [Gerber/钻孔包](expansion-board-v4-gerber-JLC-V4.7-20260913.zip)
- [配套 KiCad 源包](expansion-board-v4-kicad-V4.7-20260913.zip)

板上的主电源键是 **SW3**（TS24CA 侧按，接 SY6970 `/QON`）。它是纯硬件的：
按住 1.25–2.25s 退 ship mode 开机，BUS 未插时按住 12–18s 触发 BATFET 复位。
**不经过固件**，所以不需要任何 GPIO 参与。

## 目录说明

| 目录 | 是什么 |
|---|---|
| `smt/` | 当前正式 BOM / CPL |
| `*-V4.x-*.zip` | 历代正式包；当前快照为本文件顶部两个 V4.7；Y4按上述样板取舍保留，C100已完成 |

## 本次快照校验

PCB SHA-256：`ca864819fb793e71249baea4ce074de5ae6f6283081e03722331adbf11ebda04`。

- `expansion-board-v4-gerber-JLC-V4.7-20260913.zip`：`441b602f64c57a9273e06518e18eafa9fa827cfe4e2ecaa567ff16ebe1bdd27b`
- `expansion-board-v4-kicad-V4.7-20260913.zip`：`4df41d3684f97794e09535566d6c620b6c1143589cdc03702d38460f97045e2e`

较早的同名 V4.7 两包已保留在 `archive/20260913-before-c99-100nF/`，不可用于新订单。

本次排除后续 GNSS 备用供电（C100/C101/D7/D8/J11/U23 及 U7.6 改网）。工作区两张后续原理图未覆盖；复核本次打板请使用上述 KiCad 源包。C100 添加前的同名两包已保存在 `archive/20260913-before-c102/`，不可用于新订单。
