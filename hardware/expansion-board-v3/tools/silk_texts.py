#!/usr/bin/env python3
"""板级丝印文字的固化与复现——丝印的 ROUTES.json。

    PLACEMENT.py  → 元件在哪、位号在哪
    ROUTES.json   → 线怎么走
    SILK.json     → 板级文字与丝印图形（品牌、接口标注、极性线）  ← 本脚本
    三个都贴回，才是一块完整的板。

## 为什么必须有

`gen_pcb.py` 是 `pcbnew.NewBoard()`，从零重建时**只放元件/覆铜/板框**，板级文字
（gr_text）一条都不生成。之前用 `gen_brand_silk.py` 在流程里补 5 条品牌文字顶了
一阵，但 `gen_silk.py` 跑通之后板上有 **11 条**——多出来的 6 条是接口功能标注
（GNSS INT/EXT、1090 EXT、978 UAT、USB）和天线标注，那 5 条品牌根本盖不住。

更根本的问题是：**在流程里"重新生成"文字，和"固化人改过的文字"是两回事**。
手工调过丝印之后，任何"重新生成"都会把这些调整冲掉。所以改成跟布线一样
的做法——导出成数据、原样贴回。

## 与 gen_silk.py 的分工

  · `gen_silk.py`  离线排版工具：重排位号、生成标注。**改了布局之后手动跑一次**
  · 本脚本         流程步骤：把排好的结果原样复现，一个字都不重新计算

用法：
    silk_texts.py export     板 → tools/SILK.json
    silk_texts.py import     SILK.json → 板（幂等：先清空板级文字再贴）
"""
import json
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
OUT = os.environ.get("PK_SILK_SNAPSHOT") or os.path.join(T, "tools", "SILK.json")
MODE = sys.argv[1] if len(sys.argv) > 1 else "export"

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
LAYERS = ("F.SilkS", "B.SilkS", "F.Fab", "B.Fab", "Cmts.User", "Dwgs.User")
LID = {n: board.GetLayerID(n) for n in LAYERS}
LNAME = {v: k for k, v in LID.items()}


def is_text(d):
    return d.GetClass() in ("PCB_TEXT", "PCB_TEXTBOX")


def is_silk_shape(d):
    return d.GetClass() == "PCB_SHAPE" and d.GetLayer() in (LID["F.SilkS"], LID["B.SilkS"])


def point_record(point):
    return [round(mm(point.x), 4), round(mm(point.y), 4)]


if MODE == "export":
    items, graphics = [], []
    for d in board.GetDrawings():
        if is_silk_shape(d):
            assert d.GetShape() == pcbnew.SHAPE_T_SEGMENT, (
                f"暂不支持的板级丝印图形 {d.GetShape()}；必须先扩展快照格式，不能静默丢失"
            )
            graphics.append(dict(kind="segment", layer=LNAME[d.GetLayer()],
                                 start=point_record(d.GetStart()),
                                 end=point_record(d.GetEnd()),
                                 width=round(mm(d.GetWidth()), 4)))
            continue
        if not is_text(d):
            continue
        ly = LNAME.get(d.GetLayer())
        assert ly, f"文字在意料之外的层 {board.GetLayerName(d.GetLayer())}: {d.GetText()!r}"
        p = d.GetPosition()
        items.append(dict(text=d.GetText(), layer=ly,
                          x=round(mm(p.x), 4), y=round(mm(p.y), 4),
                          h=round(mm(d.GetTextHeight()), 4),
                          w=round(mm(d.GetTextWidth()), 4),
                          thick=round(mm(d.GetTextThickness()), 4),
                          angle=round(d.GetTextAngleDegrees(), 2),
                          mirror=bool(d.IsMirrored()),
                          just=int(d.GetHorizJustify())))
    # 稳定排序：git diff 才能看出"哪条变了"，而不是整片重排
    items.sort(key=lambda z: (z["layer"], z["y"], z["x"], z["text"]))
    graphics.sort(key=lambda z: (z["layer"], z["kind"], z["start"], z["end"], z["width"]))
    with open(OUT, "w") as f:
        json.dump({"_comment": "板级文字与丝印图形。由 silk_texts.py export 生成、import 贴回。"
                               "元件位号不在这里（那在 PLACEMENT.py）。",
                   "texts": items, "graphics": graphics}, f, ensure_ascii=False, indent=0)
        f.write("\n")
    print(f"导出 {len(items)} 条板级文字 / {len(graphics)} 个丝印图形 → {OUT}")
    for it in items:
        print(f"    [{it['layer']:12s}] ({it['x']:7.2f},{it['y']:7.2f}) h={it['h']:.2f} {it['text'][:38]!r}")

elif MODE == "import":
    data = json.load(open(OUT))
    old, old_graphics = 0, 0
    for d in list(board.GetDrawings()):
        if is_text(d):
            board.Remove(d)
            old += 1
        elif is_silk_shape(d):
            board.Remove(d)
            old_graphics += 1
    for it in data["texts"]:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(it["text"])
        t.SetPosition(pcbnew.VECTOR2I_MM(it["x"], it["y"]))
        t.SetLayer(LID[it["layer"]])
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(it["w"]), pcbnew.FromMM(it["h"])))
        t.SetTextThickness(pcbnew.FromMM(it["thick"]))
        t.SetTextAngleDegrees(it["angle"])
        t.SetMirrored(it["mirror"])
        t.SetHorizJustify(it["just"])
        board.Add(t)
    for it in data.get("graphics", []):
        assert it["kind"] == "segment", f"不支持的丝印图形类型: {it['kind']}"
        shape = pcbnew.PCB_SHAPE(board)
        shape.SetShape(pcbnew.SHAPE_T_SEGMENT)
        shape.SetStart(pcbnew.VECTOR2I_MM(*it["start"]))
        shape.SetEnd(pcbnew.VECTOR2I_MM(*it["end"]))
        shape.SetLayer(LID[it["layer"]])
        shape.SetWidth(pcbnew.FromMM(it["width"]))
        board.Add(shape)
    board.Save(PCB)
    print(f"清掉原有 {old} 条文字 / {old_graphics} 个丝印图形，贴回 "
          f"{len(data['texts'])} 条文字 / {len(data.get('graphics', []))} 个图形 → {PCB}")
else:
    sys.exit(f"未知模式 {MODE}，用 export 或 import")
