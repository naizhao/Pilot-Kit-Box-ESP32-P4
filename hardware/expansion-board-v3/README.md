# Expansion Board V3.8 Design (Archived)

<p align="center">
  <img src="render/top.png" alt="Expansion board V3 top side" width="760">
</p>

> **This board is superseded by [Expansion Board V4](../expansion-board-v4/).**
> V3.8 is the final maintenance design with the QPL9547 bias-supply correction
> and the GNSS antenna-bias ECO. BNO085 remains at 0° on V3.8; firmware must use
> the V3 coordinate transform instead of V4's 90° transform.
> The fabricated V3.2 batch remains useful for firmware debugging and antenna
> research, but requires the documented Gate rework for correct active-antenna bias.

| Document | Contents | Lang |
|---|---|---|
| [`PINMAP.md`](PINMAP.md) | Authoritative pin and net map for the V3.8 design | EN / [中文](PINMAP-zh_CN.md) |
| [`ASSEMBLY.md`](ASSEMBLY.md) | Hand-assembly placement list | EN / [中文](ASSEMBLY-zh_CN.md) |
| [`CHECKLIST.md`](CHECKLIST.md) | Per-reference BOM checklist (also [.xlsx](CHECKLIST.xlsx) / [.pdf](CHECKLIST.pdf)) | EN / [中文](CHECKLIST-zh_CN.md) |
| [`BASEBOARD_REF.md`](BASEBOARD_REF.md) | Mechanical reference for the Waveshare carrier | EN / [中文](BASEBOARD_REF-zh_CN.md) |

Engineering working notes (board bring-up records, antenna studies, layout
constraints, purchasing history) are kept out of the public tree.

---

# 扩展板 V3.8 设计（已归档）

<p align="center">
  <img src="render/top.png" alt="扩展板 V3 顶面渲染" width="760">
</p>

> **本板已被[扩展板 V4](../expansion-board-v4/) 取代。**
> V3.8 是包含 QPL9547 偏置供电修正和 GNSS 有源天线偏置 ECO 的最终维护设计。
> V3.8 的 BNO085 保持 0°，固件必须使用 V3 坐标变换，不能套用 V4 的 90° 变换。已经打样的 V3.2
> 仍可用于固件调试与天线研究，但必须按内部返修记录修改 Gate 才能正确给有源天线供电。

| 文档 | 内容 | 语言 |
|---|---|---|
| [`PINMAP.md`](PINMAP.md) | V3.8 设计权威引脚/网络映射 | [EN](PINMAP.md) / 中文 |
| [`ASSEMBLY.md`](ASSEMBLY.md) | 手工贴片点位清单 | [EN](ASSEMBLY.md) / 中文 |
| [`CHECKLIST.md`](CHECKLIST.md) | 按位号贴片核对表（另附 [.xlsx](CHECKLIST.xlsx) / [.pdf](CHECKLIST.pdf)） | [EN](CHECKLIST.md) / 中文 |
| [`BASEBOARD_REF.md`](BASEBOARD_REF.md) | 微雪载板机械参数基准 | [EN](BASEBOARD_REF.md) / 中文 |

工程过程记录（bring-up 记录、天线研究、布线约束、采购历史）不进公开目录。
