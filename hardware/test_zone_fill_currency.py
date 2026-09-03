#!/usr/bin/env python3
"""冻结「存盘的覆铜必须是最新的」这条契约。

## 这份测试是用一块差点投出去的错板换来的

2026-09-04：IFA 主臂由 49.0→50.0mm，禁铜区右沿跟着从 124.422 移到 125.422。
但**已经填好的铜不会跟着新几何退**——它停在旧边界上，于是
`x=124.422..125.422` 这条 1mm 宽的铜留在了新禁铜区里，正压在天线
**开路端外侧**（整根天线电压最高、对邻近金属最敏感的一头）。

根因：`integrate_ifa_v3.py` / `integrate_ifa_v4.py` 换 ANT1 封装后没有重灌覆铜。

## 为什么当时一整套自动检查全绿

    kicad-cli pcb drc --refill-zones   ← 在内存里重填再检查 → 看到的是"填对的板子"
    kicad-cli pcb export gerbers       ← 导出的是文件里存着的旧填充

**「DRC 0 违例」和「Gerber 是错的」不矛盾，可以同时成立。**
而 `gerber.sh` 正是这个顺序。焊盘落位检查（716/716 命中）只验"焊盘有没有对应
Flash"，铺铜多出来一块它不管。所以一屋子绿勾照样交出错文件。

## 这份测试测的是什么

不针对天线，也不针对某个禁铜区——直接测**通用的那一层**：
把板子读进来，记下每个覆铜当前的填充面积，在内存里重灌一次，再比。
**面积对不上 = 盘上存的填充已经过期**，不管是谁改了什么导致的。

这样任何"改了几何但忘了重灌"都会被拦下，而不只是这一次的天线。

## 运行方式

需要 `pcbnew`，用 KiCad 自带的 python 跑：

    ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/\\
        Versions/Current/bin/python3 -m unittest hardware.test_zone_fill_currency

普通 `/usr/bin/python3` 下会整体跳过（而不是假装通过）。
"""

import os
import shutil
import tempfile
import unittest

try:
    import pcbnew
except ImportError:                                    # pragma: no cover
    pcbnew = None

ROOT = os.path.dirname(os.path.abspath(__file__))
BOARDS = {
    "v3": os.path.join(ROOT, "expansion-board-v3", "kicad",
                       "expansion-board-v3.kicad_pcb"),
    "v4": os.path.join(ROOT, "expansion-board-v4", "kicad",
                       "expansion-board-v4.kicad_pcb"),
}

# 重灌前后允许的相对面积差。填充算法本身是确定的，这里只留浮点余量。
TOL = 1e-6


def fill_areas(board):
    """→ {(zone序号, 层): 填充面积}，只看真覆铜，跳过 rule area。"""
    out = {}
    for i, z in enumerate(board.Zones()):
        if z.GetIsRuleArea():
            continue
        for layer in z.GetLayerSet().Seq():
            out[(i, layer)] = z.CalculateFilledArea()
    return out


def stale_zones(pcb_path):
    """把板子重灌一遍，返回面积对不上的覆铜。空列表 = 存盘填充是最新的。"""
    board = pcbnew.LoadBoard(pcb_path)
    before = fill_areas(board)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    after = fill_areas(board)
    bad = []
    for key, a0 in before.items():
        a1 = after.get(key, 0.0)
        denom = max(abs(a0), abs(a1), 1.0)
        if abs(a1 - a0) / denom > TOL:
            bad.append((key, a0, a1))
    return bad


@unittest.skipIf(pcbnew is None, "需要 KiCad 的 python（pcbnew）")
class ZoneFillCurrencyTest(unittest.TestCase):

    def test_stored_zone_fills_are_current(self):
        """两块板存盘的覆铜必须已经是按当前几何填的。

        失败通常意味着：有人改了封装 / 禁铜区 / 板框 / 走线之后直接 Save，
        没有 `pcbnew.ZONE_FILLER(board).Fill(board.Zones())`。
        **这种板子 DRC 照样能全绿**，但导出的 Gerber 是旧铜。
        """
        for name, path in BOARDS.items():
            with self.subTest(board=name):
                self.assertTrue(os.path.isfile(path), f"找不到 {path}")
                bad = stale_zones(path)
                self.assertEqual(
                    bad, [],
                    f"{name} 有 {len(bad)} 处覆铜填充已过期——改了几何却没重灌。"
                    f"修法：在 Save() 前加 ZONE_FILLER().Fill()。前 3 处：{bad[:3]}")

    def test_no_copper_inside_any_keepout(self):
        """禁铜区里不许有铺铜——**全铜层**，不是只看 F.Cu。

        我第一次修完只验了 F.Cu 就宣布好了。天线禁铜区是全 6 铜层的，
        只验顶层会漏掉 In1/In3/In4。

        ⚠️ **判据必须是"网格点严格落在禁铜区内且被铜覆盖"，不能拿铺铜的顶点判。**
        我第一版就是拿顶点判的，结果 v3/v4 全红——命中点全在 71.422、57.762
        这些**禁铜区边界坐标**上。大面铺铜本来就贴着禁铜区边界走，顶点落在
        边界线上是正常形态，不是违规。当时已有 6 层 Gerber 采样全 0 和渲染图
        两条强证据说板子是干净的，新判据一上来就与之矛盾——**先怀疑判据**。
        """
        step = pcbnew.FromMM(0.3)
        margin = pcbnew.FromMM(0.12)       # 内缩，避开贴边的正常铺铜
        for name, path in BOARDS.items():
            with self.subTest(board=name):
                board = pcbnew.LoadBoard(path)
                keepouts = [(f.GetReference(), z)
                            for f in board.GetFootprints()
                            for z in f.Zones() if z.GetIsRuleArea()]
                self.assertTrue(keepouts, f"{name} 一个 rule area 都没有，可疑")
                bad, sampled = [], 0
                for ref, ko in keepouts:
                    outline = ko.Outline()
                    bb = ko.GetBoundingBox()
                    y = bb.GetTop() + margin
                    while y < bb.GetBottom() - margin:
                        x = bb.GetLeft() + margin
                        while x < bb.GetRight() - margin:
                            pt = pcbnew.VECTOR2I(x, y)
                            if outline.Contains(pt, -1):
                                sampled += 1
                                for z in board.Zones():
                                    if z.GetIsRuleArea():
                                        continue
                                    hit = next(
                                        (lay for lay in z.GetLayerSet().Seq()
                                         if ko.IsOnLayer(lay)
                                         and z.HitTestFilledArea(lay, pt, 0)),
                                        None)
                                    if hit is not None:
                                        bad.append((ref,
                                                    board.GetLayerName(hit),
                                                    round(pcbnew.ToMM(x), 2),
                                                    round(pcbnew.ToMM(y), 2)))
                                        break
                            x += step
                        y += step
                self.assertTrue(sampled > 100,
                                f"{name} 只采到 {sampled} 个点，判据没真正跑起来")
                self.assertEqual(
                    bad, [],
                    f"{name} 在 {sampled} 个采样点中有 {len(bad)} 个落在铺铜上"
                    f"（多半是改了禁铜区没重灌）：{bad[:5]}")


@unittest.skipIf(pcbnew is None, "需要 KiCad 的 python（pcbnew）")
class TopKeepoutBandTest(unittest.TestCase):
    """顶部禁铜带必须**通长贯穿整板**，中间不许有缝。

    🔴 2026-09-04 事故的真正根因，比"忘了重灌覆铜"更靠前一层。

    这条带其实是**三块拼起来**的：

        板级 ANT1090_short_end_keepout   板左沿 → ANT1 禁铜区左沿
        ANT1 封装自带的禁铜区             跟着天线几何走
        板级 ANT1090_open_end_keepout    ANT1 禁铜区右沿 → 板右沿

    两块板级区的坐标在 `gen_pcb.py` 里**写死**（注释还写着"改天线位置要一起改"），
    只有中间那块跟着天线走。把主臂由 53.5 缩到 50.0mm 之后，中间那块缩了
    3.5mm，两边纹丝不动 → 带子上开了个 **3.5mm 的洞**，GND 铺铜灌进去，
    正落在倒 F 天线**开路端外侧**（电场最强、对邻近金属最敏感的一头）。

    参考设计（4.3 原板）那条禁铜带是**横跨整块板**的矩形（99.941×5.697mm，
    从板左边一路通到板右边）。所以判据不是"天线周围留 2mm"——
    **是整条带从板左沿到板右沿连续**。

    我当时因为只截天线附近的图，反复确认"天线没问题"，直到罩哥截了整块板。
    """

    BOARD_L, BOARD_R = 50.000, 150.000

    def test_top_keepout_band_spans_the_whole_board_width(self):
        for name, path in BOARDS.items():
            with self.subTest(board=name):
                board = pcbnew.LoadBoard(path)
                segs = []
                for z in board.Zones():
                    if not z.GetIsRuleArea():
                        continue
                    if not str(z.GetZoneName()).startswith("ANT1090"):
                        continue
                    bb = z.GetBoundingBox()
                    segs.append((pcbnew.ToMM(bb.GetLeft()),
                                 pcbnew.ToMM(bb.GetRight())))
                ant = next(f for f in board.GetFootprints()
                           if f.GetReference() == "ANT1")
                ko = max((z for z in ant.Zones() if z.GetIsRuleArea()),
                         key=lambda z: z.GetBoundingBox().GetWidth())
                kb = ko.GetBoundingBox()
                segs.append((pcbnew.ToMM(kb.GetLeft()),
                             pcbnew.ToMM(kb.GetRight())))
                segs.sort()

                self.assertAlmostEqual(
                    segs[0][0], self.BOARD_L, places=3,
                    msg=f"{name} 禁铜带没有顶到板左沿: {segs}")
                self.assertAlmostEqual(
                    segs[-1][1], self.BOARD_R, places=3,
                    msg=f"{name} 禁铜带没有顶到板右沿: {segs}")
                gaps = [(round(a[1], 3), round(b[0], 3))
                        for a, b in zip(segs, segs[1:]) if b[0] - a[1] > 1e-6]
                self.assertEqual(
                    gaps, [],
                    f"{name} 顶部禁铜带断开于 {gaps} —— 铺铜会灌进这个缝，"
                    f"正落在天线开路端外侧。改天线长度时两块板级禁铜区必须跟着走。")


@unittest.skipIf(pcbnew is None, "需要 KiCad 的 python（pcbnew）")
class CurrencyCheckSelfTest(unittest.TestCase):
    """🔴 让这份测试自己证明它抓得住——**新检查必须先拿坏样本验红**。

    写完一条断言就宣布它有效是没有依据的。这里现造一块"改了几何没重灌"的板，
    如果 `stale_zones()` 对它也返回空，说明上面那条契约是摆设。
    """

    def test_the_keepout_check_catches_copper_left_behind(self):
        """禁铜区检查也必须拿坏样本验红，否则它可能只是"永远绿"。

        造法与真实事故同形：把天线连同禁铜区挪 1mm、**不重灌**就存盘，
        原地留下的铜就会落进新禁铜区——正是罩哥 2026-09-04 一眼看见的那条。
        """
        src = BOARDS["v4"]
        tmp = tempfile.mkdtemp(prefix="pkb_keepout_")
        try:
            dst = os.path.join(tmp, "stale.kicad_pcb")
            shutil.copy2(src, dst)
            board = pcbnew.LoadBoard(dst)
            ant = next(f for f in board.GetFootprints()
                       if f.GetReference() == "ANT1")
            ant.Move(pcbnew.VECTOR2I(pcbnew.FromMM(1.5), 0))
            board.Save(dst)                      # ← 故意不 Fill

            saved = dict(BOARDS)
            try:
                BOARDS.clear()
                BOARDS["stale"] = dst
                with self.assertRaises(AssertionError):
                    ZoneFillCurrencyTest(
                        "test_no_copper_inside_any_keepout"
                    ).test_no_copper_inside_any_keepout()
            finally:
                BOARDS.clear()
                BOARDS.update(saved)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_check_catches_a_deliberately_stale_fill(self):
        src = BOARDS["v4"]
        tmp = tempfile.mkdtemp(prefix="pkb_stalefill_")
        try:
            dst = os.path.join(tmp, "stale.kicad_pcb")
            shutil.copy2(src, dst)
            board = pcbnew.LoadBoard(dst)
            ant = next((f for f in board.GetFootprints()
                        if f.GetReference() == "ANT1"), None)
            self.assertIsNotNone(ant, "样本板里没有 ANT1")
            # 把天线连同它自带的禁铜区整体挪 1mm —— 正是 2026-09-04 那次的形态：
            # 几何变了，但**不重灌**就直接存盘。
            ant.Move(pcbnew.VECTOR2I(pcbnew.FromMM(1.0), 0))
            board.Save(dst)                      # ← 故意不 Fill

            bad = stale_zones(dst)
            self.assertTrue(
                bad,
                "构造了一块几何改过却没重灌的板，契约却认为它是最新的"
                "——说明 test_stored_zone_fills_are_current 拦不住真问题")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
