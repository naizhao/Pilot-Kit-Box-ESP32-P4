# V4.8 当前制造输出入口

**当前审阅用制造包已刷新（C99=100nF，C100 VREF 去耦已完成布局布线）。** 工程包内 PCB 与当前板逐字节一致；原理图与本次 PCB 的元件及引脚网络一致，BOM/CPL 和装配文档也已刷新。

**用户已接受本轮样板保留 Y4 的3.3V供电，不再因此要求改板；回板验证启动、频率及输出幅度。U22 VREF 已通过 C100 并联100nF，见[当前电气复查结论](../README.md#airband-reference-clock--v47-recheck)。**

- [Gerber/钻孔包](expansion-board-v4-gerber-JLC-V4.8-20260913.zip)
- [配套 KiCad 源包](expansion-board-v4-kicad-V4.8-20260913.zip)

板上的主电源键是 **SW3**（TS24CA 侧按，接 SY6970 `/QON`）。它是纯硬件的：
按住 1.25–2.25s 退 ship mode 开机，BUS 未插时按住 12–18s 触发 BATFET 复位。
**不经过固件**，所以不需要任何 GPIO 参与。

## 目录说明

| 目录 | 是什么 |
|---|---|
| `smt/` | 当前正式 BOM / CPL |
| `*-V4.x-*.zip` | 历代正式包；当前快照为本文件顶部两个 V4.8；Y4按上述样板取舍保留，C100已完成 |

## 本次快照校验

PCB SHA-256：`74655acd6895ffe5478f9481f2f7ea388d9967645e3c7e3e35af626efd1e087d`。

- `expansion-board-v4-gerber-JLC-V4.8-20260913.zip`：`2fa4adeb1619cf09e6b71e7856c79a22b9a2cea30601af17522b6172d160974b`
- `expansion-board-v4-kicad-V4.8-20260913.zip`：`ae63add9c87044e546e79944ca0d9608c0663526ae893f5474ae57a11d7130aa`

较早的同名 V4.7 两包已保留在 `archive/20260913-before-c99-100nF/`，不可用于新订单。

**V4.8 已包含 GNSS 备份供电**：`U23` / `C101` / `C102` / `D7` / `R63` / `BT1`，且
`U7.pin6` 已由 `3V3_GNSS` 改接 `GNSS_VBAT`。这是 V4.8 相对 V4.7 的唯一功能性差异
——V4.7 刻意排除了这一组，两者不可混用。

`BT1`（Seiko MS621FE-FL11E）在 **B 面**，经济型 SMT 只贴一面，它不在任何 CPL 里，
**需手工焊接**。C100 添加前的同名两包已保存在 `archive/20260913-before-c102/`，
不可用于新订单。
