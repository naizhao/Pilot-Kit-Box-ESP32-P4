#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从嘉立创 EDA 专业版（EasyEDA Pro / LCEDA Pro）导出的 PCB JSON 里，
抠出 1090MHz 板载 IFA 天线的完整铜箔几何 + π 匹配网络拓扑 + 禁铜区，
输出成可直接喂给 KiCad pcbnew Python API 的数据。

历史背景曾声称该天线“41mm且已调好”，但V2实测约1408MHz，已证伪。本脚本只负责
提取旧几何供审计，**不得把提取结果直接当作1090MHz定版尺寸**。

── 文件格式（实测，非假设）─────────────────────────────────────────────
EasyEDA Pro 的 .json 不是标准 JSON，而是**逐行**的
    {"type":"XXX","ticket":N,"id":"..."}||{ ...body... }|
DOCHEAD 行标记一个文档的开始（docType: PCB / FOOTPRINT / SYMBOL / DEVICE ...）。
PCB 文档里 **不含**封装图形，只有 COMPONENT（放置点）+ ATTR(key=Footprint) 引用
封装 uuid；封装本体在工程包 project/*.epro2（zip）内的 "*.epru"（同样逐行格式，
多文档串联）里。

坐标单位 = mil（1 mil = 0.0254 mm）。板框反推验证见 verify_unit()。
y 轴向上为正（值越大越靠板子上边）；KiCad 的 y 向下为正，故导出时 y 取反。

图元 body 关键字段：
  PAD   : centerX/centerY/num/layerId/defaultPad{padType,width,height}
  POLY  : layerId/width/path=[x1,y1,"L",x2,y2,...]  → 走线段（NORMAL）/板框（BOARD_OUTLINE）
  LINE  : layerId/width/startX,startY,endX,endY/netName → 板级走线
  FILL  : layerId/path=[["R",x,y,w,h,ang,?]] 或 [["CIRCLE",x,y,r]] → 填充
  REGION: layerId/prohibitType[]/path → 禁止区（regionType=PROHIBIT）
  POUR  : 铺铜边界；POURED: 铺铜实际灌出的多边形（单位 = 0.254mm = 10mil，另算）
层号：1=顶层 2=底层 3=顶丝印 4=底丝印 11=板框 12=多层(Multi=所有铜层) 13=文档层
"""

import json
import os
import zipfile

MIL = 0.0254            # 1 mil -> mm
POURED_UNIT = 0.254     # POURED 记录用的单位 -> mm（实测 = 10 mil，见 check_pour_clipped）

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PCB_43 = os.path.join(ROOT, "docs/jlc/lcd-4.3in/pcb/waveshare-esp32-p4-wifi6-touch-lcd-4.3.json")
PCB_24 = os.path.join(ROOT, "docs/jlc/lcd-2.4in-8pin/pcb/lcd-2.4in-8pin.json")
EPRO2 = os.path.join(ROOT, "docs/jlc/lcd-2.4in-8pin/project/Pilot-Kit-Box.epro2")

ANT_DESIGNATOR = "1090_MHz_IFA_ANT"
PI_DESIGNATORS = ("Z_SER", "Z_SH1", "Z_SH2")
IPEX_DESIGNATOR = "IFA_ANT_IPEX"


# ─────────────────────────── 解析层 ───────────────────────────
def parse_lines(text):
    """把 EasyEDA Pro 的逐行文本解析成 [(header_dict, body_dict), ...]"""
    out = []
    for ln in text.splitlines():
        if "||" not in ln:
            continue
        hs, bs = ln.split("||", 1)
        try:
            head = json.loads(hs)
            body = json.loads(bs.rstrip("|"))
        except json.JSONDecodeError:
            continue
        out.append((head, body))
    return out


def parse_docs(text):
    """按 DOCHEAD 切分成多个文档：{uuid: {'head':..., 'items':[(head,body)...]}}"""
    docs, cur = {}, None
    for head, body in parse_lines(text):
        if head.get("type") == "DOCHEAD":
            cur = {"head": body, "items": []}
            docs[body.get("uuid")] = cur
        elif cur is not None:
            cur["items"].append((head, body))
    return docs


def load_pcb(path):
    return parse_lines(open(path, encoding="utf-8").read())


def load_footprint_lib(epro2_path):
    """epro2 是 zip，内含 '<工程名>.epru'（封装/符号/器件库，多文档串联）"""
    with zipfile.ZipFile(epro2_path) as z:
        name = [n for n in z.namelist() if n.endswith(".epru")][0]
        text = z.read(name).decode("utf-8")
    return parse_docs(text)


# ─────────────────────────── 工具 ───────────────────────────
def mm(v):
    return v * MIL


def f3(v):
    return round(v + 0.0, 3)


def items_of(pcb, *types):
    return [(h, b) for h, b in pcb if h.get("type") in types]


def designators(pcb):
    """parentId -> Designator 文本"""
    out = {}
    for h, b in items_of(pcb, "ATTR"):
        if b.get("key") == "Designator":
            out[b.get("parentId")] = b.get("value")
    return out


def footprint_of(pcb):
    """parentId -> 封装 uuid"""
    out = {}
    for h, b in items_of(pcb, "ATTR"):
        if b.get("key") == "Footprint":
            out[b.get("parentId")] = b.get("value")
    return out


def components(pcb):
    return {h["id"]: b for h, b in items_of(pcb, "COMPONENT")}


def pad_nets(pcb):
    """componentId -> {padNum: net}"""
    out = {}
    for h, b in items_of(pcb, "PAD_NET"):
        cid, num = json.loads(h["id"])[1:3]
        out.setdefault(cid, {})[num] = b.get("padNet")
    return out


def board_outline(pcb):
    for h, b in items_of(pcb, "POLY"):
        if b.get("polyType") == "BOARD_OUTLINE":
            return b["path"]
    return None


def poly_segments(path):
    """POLY 的 path=[x1,y1,'L',x2,y2,...] → [((x1,y1),(x2,y2)), ...]"""
    pts, i = [], 0
    while i < len(path):
        v = path[i]
        if v == "L":
            i += 1
            continue
        if v == "ARC":          # 本天线未用到，保留守卫
            raise NotImplementedError("POLY path 含 ARC，需扩展解析")
        pts.append((path[i], path[i + 1]))
        i += 2
    return list(zip(pts[:-1], pts[1:]))


def rect_of(path_entry):
    """['R', x, y, w, h, ang, ?] → (xmin, xmax, ymin, ymax)  —— y 为顶边，向下延伸 h"""
    assert path_entry[0] == "R", path_entry
    x, y, w, h = path_entry[1:5]
    return x, x + w, y - h, y


# ─────────────────────────── 校验 ───────────────────────────
def verify_unit(pcb, label):
    path = board_outline(pcb)
    x0, x1, y0, y1 = rect_of(path)
    w, h = x1 - x0, y1 - y0
    print(f"[单位验证] {label}")
    print(f"  板框 POLY(layerId=11,polyType=BOARD_OUTLINE).path = {path}")
    print(f"  原始宽高 = {w} x {h}")
    print(f"  按 mil 换算 = {f3(mm(w))} x {f3(mm(h))} mm  ← 整数毫米，单位确认为 mil")
    print(f"  板框范围(mil): x[{x0}, {x1}]  y[{y0}, {y1}]")
    return (x0, x1, y0, y1)


def check_pour_clipped(pcb, keepout_y_bottom_mil):
    """用 POURED（实际灌铜多边形）的 y 上界，验证禁铜区确实切掉了铺铜。"""
    print("[铺铜验证] POURED 实际灌出的多边形（单位 0.254mm = 10mil）")
    for h, b in items_of(pcb, "POURED"):
        for pf in b.get("pourFill", []):
            ys = []
            for sub in pf["path"]:
                i = 0
                while i < len(sub):
                    if sub[i] == "L":
                        i += 1
                        continue
                    if sub[i] == "ARC":
                        i += 2
                        continue
                    ys.append(sub[i + 1])
                    i += 2
            if ys:
                top_mm = max(ys) * POURED_UNIT
                print(f"  {h['id']}: 灌铜 y 上界 = {max(ys)} → {f3(top_mm)} mm"
                      f"   （禁铜区下沿 = {f3(mm(keepout_y_bottom_mil))} mm）")
            break


# ─────────────────────────── 主体提取 ───────────────────────────
def extract(pcb_path, lib, label):
    pcb = load_pcb(pcb_path)
    print("=" * 78)
    print(f"### {label}")
    print(f"### {pcb_path}")
    print("=" * 78)
    board = verify_unit(pcb, label)
    print()

    desig = designators(pcb)
    fps = footprint_of(pcb)
    comps = components(pcb)
    nets = pad_nets(pcb)

    by_name = {v: k for k, v in desig.items()}
    ant_id = by_name[ANT_DESIGNATOR]
    ant = comps[ant_id]
    fp_uuid = fps[ant_id]
    print(f"[器件] {ANT_DESIGNATOR}  componentId={ant_id}")
    print(f"  COMPONENT.x/y/angle = ({ant['x']}, {ant['y']}) mil, angle={ant['angle']}°")
    print(f"  ATTR(key=Footprint).value = {fp_uuid}")
    if fp_uuid not in lib:
        print("  !! 封装 uuid 不在库里，无法取几何")
        return None
    fp = lib[fp_uuid]
    title = next((b.get("title") for h, b in fp["items"] if h.get("type") == "META"), "?")
    print(f"  封装 META.title = {title}")
    print()

    pads = [b for h, b in fp["items"] if h.get("type") == "PAD"]
    pads.sort(key=lambda p: p["num"])
    polys = [b for h, b in fp["items"] if h.get("type") == "POLY"]

    # 馈点 = pad "1"（PAD_NET 里接 π 网络那一侧）
    feed = next(p for p in pads if p["num"] == "1")
    fx, fy = feed["centerX"], feed["centerY"]
    print(f"[馈点] PAD num=1 中心 = ({fx}, {fy}) mil（封装原点坐标系）"
          f" → 绝对 ({ant['x'] + fx}, {ant['y'] + fy}) mil")
    print("  以下所有局部坐标 = 封装坐标 - 馈点坐标（馈点为原点）")
    print()

    print("[焊盘]（layerId 1=顶层, 12=多层）")
    print(f"  {'num':>3} {'net':>8} {'层':>3} {'类型':>7} "
          f"{'局部x(mm)':>10} {'局部y(mm)':>10} {'尺寸(mm)':>16} {'孔':>10}")
    for p in pads:
        d = p["defaultPad"]
        hole = p.get("hole")
        hs = "-" if not hole else f"{f3(mm(hole['width']))}"
        print(f"  {p['num']:>3} {nets[ant_id].get(p['num'], ''):>8} {p['layerId']:>3} "
              f"{d['padType']:>7} {f3(mm(p['centerX'] - fx)):>10} {f3(mm(p['centerY'] - fy)):>10} "
              f"{f3(mm(d['width']))} x {f3(mm(d['height'])):<8} {hs:>10}")
    print()

    print("[铜箔走线段] POLY（封装内，layerId=1 顶层）")
    print(f"  {'#':>2} {'层':>3} {'线宽(mm)':>9} "
          f"{'x1(mm)':>10} {'y1(mm)':>10} {'x2(mm)':>10} {'y2(mm)':>10}  {'长度(mm)':>9}")
    segs = []
    for i, pl in enumerate(polys, 1):
        w = pl["width"]
        for (ax, ay), (bx, by) in poly_segments(pl["path"]):
            lx1, ly1 = mm(ax - fx), mm(ay - fy)
            lx2, ly2 = mm(bx - fx), mm(by - fy)
            ln = ((lx2 - lx1) ** 2 + (ly2 - ly1) ** 2) ** 0.5
            segs.append((pl["layerId"], mm(w), lx1, ly1, lx2, ly2))
            print(f"  {i:>2} {pl['layerId']:>3} {f3(mm(w)):>9} "
                  f"{f3(lx1):>10} {f3(ly1):>10} {f3(lx2):>10} {f3(ly2):>10}  {f3(ln):>9}")
    print()

    # ── 铜箔外接矩形（含线宽端帽 + 焊盘）──
    xs, ys = [], []
    for pl in polys:
        hw = pl["width"] / 2.0
        for (ax, ay), (bx, by) in poly_segments(pl["path"]):
            for px, py in ((ax, ay), (bx, by)):
                xs += [px - hw, px + hw]
                ys += [py - hw, py + hw]
    for p in pads:
        d = p["defaultPad"]
        xs += [p["centerX"] - d["width"] / 2, p["centerX"] + d["width"] / 2]
        ys += [p["centerY"] - d["height"] / 2, p["centerY"] + d["height"] / 2]
    bx0, bx1, by0, by1 = min(xs), max(xs), min(ys), max(ys)
    print("[铜箔外接矩形]（含走线端帽半线宽 + 焊盘）")
    print(f"  封装局部(mil): x[{f3(bx0)}, {f3(bx1)}]  y[{f3(by0)}, {f3(by1)}]")
    print(f"  馈点局部(mm) : x[{f3(mm(bx0 - fx))}, {f3(mm(bx1 - fx))}]  "
          f"y[{f3(mm(by0 - fy))}, {f3(mm(by1 - fy))}]")
    print(f"  尺寸         : {f3(mm(bx1 - bx0))} x {f3(mm(by1 - by0))} mm")
    abs_bx0, abs_bx1 = ant["x"] + bx0, ant["x"] + bx1
    abs_by0, abs_by1 = ant["y"] + by0, ant["y"] + by1
    print(f"  板上绝对(mil): x[{f3(abs_bx0)}, {f3(abs_bx1)}]  y[{f3(abs_by0)}, {f3(abs_by1)}]")
    print(f"  距板上边沿   : {f3(mm(board[3] - abs_by1))} mm  （板框上沿 y={board[3]} mil）")
    print()

    # ── 禁铜区 ──
    print("[禁铜区 / PROHIBIT REGION]（整板范围内全部列出，自行判断哪些属于天线）")
    regions = items_of(pcb, "REGION")
    if not regions:
        print("  原工程未画禁铜区")
    for h, b in regions:
        print(f"  id={h['id']} layerId={b['layerId']} prohibitType={b['prohibitType']} "
              f"regionType={b.get('regionType')}")
        p0 = b["path"][0]
        if p0[0] == "R":
            rx0, rx1, ry0, ry1 = rect_of(p0)
            print(f"    矩形 mil: x[{rx0}, {rx1}] y[{ry0}, {ry1}]"
                  f"  → {f3(mm(rx1 - rx0))} x {f3(mm(ry1 - ry0))} mm")
            print(f"    馈点局部 mm: x[{f3(mm(rx0 - abs_x(ant, fx)))}, "
                  f"{f3(mm(rx1 - abs_x(ant, fx)))}] "
                  f"y[{f3(mm(ry0 - abs_y(ant, fy)))}, {f3(mm(ry1 - abs_y(ant, fy)))}]")
            covers = not (rx1 < abs_bx0 or rx0 > abs_bx1 or ry1 < abs_by0 or ry0 > abs_by1)
            print(f"    与天线铜箔重叠: {'是' if covers else '否'}")
        else:
            pts = []
            i = 0
            while i < len(p0):
                if p0[i] in ("L", "ARC"):
                    i += 1 if p0[i] == "L" else 2
                    continue
                pts.append((p0[i], p0[i + 1]))
                i += 2
            xs2 = [p[0] for p in pts]
            ys2 = [p[1] for p in pts]
            print(f"    多边形 {len(pts)} 点, bbox mil: x[{min(xs2)}, {max(xs2)}] "
                  f"y[{min(ys2)}, {max(ys2)}]")
            print("    顶点(mil): " + " ".join(f"({p[0]},{p[1]})" for p in pts))
    print()

    # ── π 匹配网络 ──
    print("[π 型匹配网络]")
    print(f"  {'位号':>14} {'x,y(mil)':>16} {'角度':>5} {'Value':>6} {'封装':>18}  引脚→网络")
    ipex_id = by_name.get(IPEX_DESIGNATOR)
    for name in (IPEX_DESIGNATOR,) + PI_DESIGNATORS + (ANT_DESIGNATOR,):
        cid = by_name.get(name)
        if cid is None:
            print(f"  {name:>14}  —— 本板不存在")
            continue
        c = comps[cid]
        fpu = fps.get(cid, "")
        ftitle = "?"
        if fpu in lib:
            ftitle = next((b.get("title") for h, b in lib[fpu]["items"]
                           if h.get("type") == "META"), "?")
        pn = ", ".join(f"{k}={v}" for k, v in sorted(nets.get(cid, {}).items()))
        print(f"  {name:>14} {c['x']},{c['y']:>8} {c['angle']:>5} "
              f"{str(c['attrs'].get('Value', '-')):>6} {ftitle:>18}  {pn}")
    print()

    # ── 天线焊盘外接的板级走线 / 过孔 ──
    afx, afy = ant["x"] + fx, ant["y"] + fy
    print("[天线焊盘外接的板级走线 / 过孔]（局部坐标以馈点为原点）")
    for h, b in items_of(pcb, "LINE"):
        pts = [(b["startX"], b["startY"]), (b["endX"], b["endY"])]
        if not any(abs(px - afx) < 900 and abs(py - afy) < 400 for px, py in pts):
            continue
        if b.get("netName") not in ("$1N124", "GND"):
            continue
        print(f"  LINE net={b['netName']:>7} layer={b['layerId']} w={f3(mm(b['width']))}mm "
              f"({f3(mm(pts[0][0] - afx))}, {f3(mm(pts[0][1] - afy))}) → "
              f"({f3(mm(pts[1][0] - afx))}, {f3(mm(pts[1][1] - afy))})")
    for h, b in items_of(pcb, "VIA"):
        vx, vy = b["centerX"], b["centerY"]
        if abs(vx - afx) < 400 and abs(vy - afy) < 300:
            print(f"  VIA  net={b['netName']:>7} ({f3(mm(vx - afx))}, {f3(mm(vy - afy))}) "
                  f"外径 {f3(mm(b['viaDiameter']))}mm 孔 {f3(mm(b['holeDiameter']))}mm")
    print()

    # ── 占用面积小结 ──
    ko = [b for h, b in regions
          if b["path"][0][0] == "R" and not (
              rect_of(b["path"][0])[1] < abs_bx0 or rect_of(b["path"][0])[0] > abs_bx1 or
              rect_of(b["path"][0])[3] < abs_by0 or rect_of(b["path"][0])[2] > abs_by1)]
    print("[占用面积小结]")
    print(f"  铜箔 bbox            : {f3(mm(bx1 - bx0))} x {f3(mm(by1 - by0))} mm "
          f"= {f3(mm(bx1 - bx0) * mm(by1 - by0))} mm²")
    if ko:
        ky_top = max(rect_of(b["path"][0])[3] for b in ko)
        ky_bot = min(rect_of(b["path"][0])[2] for b in ko)
        y_lo, y_hi = min(abs_by0, ky_bot), max(abs_by1, ky_top)
        print(f"  与铜箔重叠的禁铜区   : y[{f3(mm(ky_bot - afy))}, {f3(mm(ky_top - afy))}] mm（馈点局部）")
        print(f"  铜箔 ∪ 禁铜区(纵向)  : {f3(mm(y_hi - y_lo))} mm 高")
        print(f"  按天线 x 跨度保留区  : {f3(mm(bx1 - bx0))} x {f3(mm(y_hi - y_lo))} mm "
              f"= {f3(mm(bx1 - bx0) * mm(y_hi - y_lo))} mm²")
    print()

    return {
        "feed_abs": (ant["x"] + fx, ant["y"] + fy),
        "segs": segs,
        "bbox_local_mm": (mm(bx0 - fx), mm(bx1 - fx), mm(by0 - fy), mm(by1 - fy)),
        "polys": polys, "pads": pads, "fx": fx, "fy": fy,
        "board": board, "abs_bbox": (abs_bx0, abs_bx1, abs_by0, abs_by1),
        "regions": regions, "pcb": pcb,
    }


def abs_x(comp, fx):
    return comp["x"] + fx


def abs_y(comp, fy):
    return comp["y"] + fy


def kicad_dump(res):
    """按 KiCad pcbnew 约定输出：单位 mm，y 轴取反（EDA y 向上 / KiCad y 向下）。"""
    print("=" * 78)
    print("### KiCad pcbnew 重建数据（馈点为原点，y 已取反 → KiCad 屏幕坐标）")
    print("=" * 78)
    print("# layer: F.Cu ；width 单位 mm")
    print("IFA_SEGMENTS_MM = [")
    for layer, w, x1, y1, x2, y2 in res["segs"]:
        print(f"    dict(start=({f3(x1)}, {f3(-y1)}), end=({f3(x2)}, {f3(-y2)}), "
              f"width={f3(w)}, layer='F.Cu'),")
    print("]")
    fx, fy = res["fx"], res["fy"]
    print("IFA_PADS_MM = [")
    for p in res["pads"]:
        d = p["defaultPad"]
        print(f"    dict(num='{p['num']}', at=({f3(mm(p['centerX'] - fx))}, "
              f"{f3(-mm(p['centerY'] - fy))}), size=({f3(mm(d['width']))}, "
              f"{f3(mm(d['height']))}), shape='{d['padType']}', layer_id={p['layerId']}),")
    print("]")
    x0, x1, y0, y1 = res["bbox_local_mm"]
    print(f"IFA_COPPER_BBOX_MM = dict(x_min={f3(x0)}, x_max={f3(x1)}, "
          f"y_min={f3(-y1)}, y_max={f3(-y0)}, w={f3(x1 - x0)}, h={f3(y1 - y0)})")
    print()


def main():
    lib = load_footprint_lib(EPRO2)
    print(f"[封装库] {EPRO2}")
    kinds = {}
    for u, d in lib.items():
        kinds[d["head"].get("docType")] = kinds.get(d["head"].get("docType"), 0) + 1
    print(f"  内含文档: {kinds}")
    print()

    r43 = extract(PCB_43, lib, "4.3 寸板（主基准）")
    check_pour_clipped(r43["pcb"], 2377.7835)
    print()
    kicad_dump(r43)

    r24 = extract(PCB_24, lib, "2.4 寸板（交叉验证）")

    print("=" * 78)
    print("### 两板铜箔几何一致性比对")
    print("=" * 78)
    same = [(f3(a), f3(b)) for a, b in zip(
        [v for s in r43["segs"] for v in s[1:]],
        [v for s in r24["segs"] for v in s[1:]])]
    ok = all(abs(a - b) < 1e-6 for a, b in same)
    print(f"  走线段（线宽 + 馈点局部坐标）逐值比对: {'完全一致' if ok else '不一致 !!'}")
    print(f"  4.3 寸铜箔 bbox(mm) = {tuple(f3(v) for v in r43['bbox_local_mm'])}")
    print(f"  2.4 寸铜箔 bbox(mm) = {tuple(f3(v) for v in r24['bbox_local_mm'])}")


if __name__ == "__main__":
    main()
