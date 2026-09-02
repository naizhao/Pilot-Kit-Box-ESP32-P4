#!/usr/bin/env python3
"""原理图生成器：从官方/项目符号库提取符号原文，按数据表摆放并用全局标签联网。

用法: gen_sch.py <sheet名>   （sheet 定义在 sheets_*.py 中注册）

原则：
- 符号定义从库文件原文提取（不重绘），嵌入 lib_symbols 段
- 引脚连接点由程序解析（pin 的 (at x y rot)，实例坐标 + 符号坐标 Y 取反）
- 每个引脚必须显式给网络：网络名 / "NC"（打 no_connect 标记）
- 断言：库中解析出的引脚集合 == 数据表给出的引脚集合，缺一多一都报错
"""
import os
import re
import sys
import uuid as uuidlib

KICAD_SYM_DIR = os.path.expanduser(
    "~/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")
PROJ = os.path.join(os.path.dirname(__file__), "..", "kicad")
PROJ_SYM = os.path.join(PROJ, "expansion-board-v3.kicad_sym")
ROOT_UUID = "10000000-0000-4000-8000-000000000001"

# ============ 无源件封装：非射频位置用 0603 ============
# 用户手上有 0603 电阻/电容本，原型阶段直接取用，省采购也方便手调。
# 判据：**任一引脚接触下列真射频网络的元件保留 0402**，其余一律 0603。
# 用"接触射频网络"而不是逐个点名位号——点名必漏，而且偏置扼流/天线口 ESD/匹配电容
# 这些有硬指标的料本来就都挂在射频网上，会被自动保住。
# 保留 0402 的理由（实算）：0603 焊盘约 0.85mm 宽 vs 50Ω 线 0.34mm，对地寄生
# 约 0.26pF（0402 约 0.11pF），1090MHz 下单件回损 −27dB vs −35dB，射频链串 8 个件会累加。
# 隔直位置上两者插损都 <0.01dB，所以非射频位置换 0603 没有代价。
RF_NETS = {
    # 1090 接收链（天线 → 开关 → LNA → SAW → LNA → SAW → 检波）
    "ANT1090_EXT", "ANT1090_IFA", "SW1_J1", "SW1_J2", "SW1_J3",
    "LNA1_IN", "LNA1_OUT", "SAW1_IN", "SAW1_OUT",
    "LNA2_IN", "LNA2_OUT", "SAW2_IN", "SAW2_OUT", "DET_IN", "DET_INLO",
    # 978 UAT（CC1312R 差分口 → lattice balun → 天线）
    "SUBG_RFP", "SUBG_RFN", "SUBG_RXTX", "SUBG_N3", "SUBG_N4", "SUBG_N5", "ANT_978",
    # GNSS 1575MHz（内置 patch / 外接 SMA 切换 → 模块）
    "ANT_GNSS_EXT", "ANT_GNSS_INT", "GNSS_INT_FEED", "GNSS_EXT_FEED", "GNSS_RF_IN",
    # 射频供电轨：去耦电容是射频回流路径的一部分
    "3V3_RF",
}
# 受控阻抗的那一部分：3V3_RF 是供电轨，归 POWER 类走 0.5mm、可以走内层，
# 不受"必须留在 F.Cu、不许打过孔"这条约束。这个减法以前只写在 gen_pcb.py 里，
# check_route.py 手推了一遍就推错了（把 3V3_RF 的 14 段内层走线报成"射频跑出 F.Cu"）。
# 单一来源放这里，谁都别再自己推。
RF50_NETS = RF_NETS - {"3V3_RF"}

# 名字像射频但不走射频的，明确排除（防止以后有人按名字正则误加）：
# ANT_SEL_*   = 开关的直流控制脚
# GNSS_*_FUSE = 偏置Tee 的直流侧（扼流另一端在 RF 网上，元件仍会被保住）
# RF_DET_OUT  = 对数检波器视频输出，带宽约 10MHz
FP_0402_TO_0603 = {
    "Capacitor_SMD:C_0402_1005Metric": "Capacitor_SMD:C_0603_1608Metric",
    "Resistor_SMD:R_0402_1005Metric": "Resistor_SMD:R_0603_1608Metric",
}
# C21/C37/C38 于 2026-09-02 一并纳入：它们是射频/检波器的本地去耦，
# 0402 才塞得进引脚旁边。原有的 C82-C86 一个都不能掉，掉了就是回归。
# 10 元素 = V3 原有的 C82-C86 + 从 V4 回灌的 R52/R53/C21/C37/C38。
# R52/R53 是 BNO085 ENV 总线上拉，紧贴 U4 引脚；C21/C37/C38 是射频/检波器的
# 本地去耦。这些位置 0603 塞不进去，也会挤占右侧 QSPI 扇出通道。
# 原有的 C82-C86 一个都不能掉，掉了就是回归。
LOCAL_0402_REFS = {"R52", "R53", "C21", "C37", "C38",
                   "C82", "C83", "C84", "C85", "C86"}


def resolve_fp(ref, footprint, nets):
    """非射频位置的 0402 电阻/电容改用 0603。电感/二极管不动——
    它们的选型靠 SRF/结电容等具体参数，本子里的通货不满足，换封装没有意义。"""
    if footprint not in FP_0402_TO_0603:
        return footprint
    # RP2040 的 VREG_VIN / USB_VDD / ADC_AVDD 与补充 DVDD 去耦必须贴近引脚；
    # V3 又是单面装配，0603 会挤占右侧 QSPI 扇出通道，明确保留 0402。
    if ref in LOCAL_0402_REFS:
        return footprint
    if any(n in RF_NETS for n in nets.values()):
        return footprint
    return FP_0402_TO_0603[footprint]

SHEET_UUIDS = {  # 与根图 sheet 实例一致
    "power": "20000000-0000-4000-8000-000000000001",
    "mcu": "20000000-0000-4000-8000-000000000002",
    "rf1090": "20000000-0000-4000-8000-000000000003",
    "subghz": "20000000-0000-4000-8000-000000000004",
    "sensors": "20000000-0000-4000-8000-000000000005",
    "interface": "20000000-0000-4000-8000-000000000006",
}


def _lib_path(lib):
    if lib == "expansion-board-v3":
        return PROJ_SYM
    return os.path.join(KICAD_SYM_DIR, lib + ".kicad_sym")


def _extract_block(text, header_pos):
    """从 header_pos（指向 '(' ）提取配平括号块。"""
    depth = 0
    i = header_pos
    in_str = False
    while i < len(text):
        c = text[i]
        if c == '"' and text[i - 1] != '\\':
            in_str = not in_str
        elif not in_str:
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return text[header_pos:i + 1]
        i += 1
    raise ValueError("括号不配平")


_sym_cache = {}


def get_symbol(lib, name):
    key = (lib, name)
    if key in _sym_cache:
        return _sym_cache[key]
    text = open(_lib_path(lib)).read()
    m = re.search(r'\(symbol\s+"' + re.escape(name) + r'"', text)
    assert m, f"库 {lib} 中找不到符号 {name}"
    block = _extract_block(text, m.start())
    # 派生符号（extends）：取父符号块整体改名，实现展平
    em = re.search(r'\(extends\s+"([^"]+)"\)', block)
    if em:
        parent = em.group(1)
        pm2 = re.search(r'\(symbol\s+"' + re.escape(parent) + r'"', text)
        assert pm2, f"{name} extends {parent}，但父符号未找到"
        block = _extract_block(text, pm2.start()).replace(f'(symbol "{parent}', f'(symbol "{name}')
    # 提取引脚：(pin TYPE line (at X Y ROT) ... (number "N" ...)
    pins = {}
    for pm in re.finditer(r'\(pin\s+(\w+)\s+\w+\s*\n?\s*\(at\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+(\d+)\)', block):
        seg = block[pm.start():pm.start() + 600]
        nm = re.search(r'\(name\s+"([^"]*)"', seg)
        num = re.search(r'\(number\s+"([^"]*)"', seg)
        assert num, f"{name}: pin 无编号"
        pins[num.group(1)] = {
            "type": pm.group(1),
            "x": float(pm.group(2)), "y": float(pm.group(3)),
            "rot": int(pm.group(4)),
            "name": nm.group(1) if nm else "",
        }
    # 重命名为 Lib:Name（仅最外层）
    block = block.replace(f'(symbol "{name}"', f'(symbol "{lib}:{name}"', 1)
    _sym_cache[key] = (block, pins)
    return _sym_cache[key]


def _uid():
    return str(uuidlib.uuid4())


class Sheet:
    def __init__(self, sheet_key, title):
        self.key = sheet_key
        self.uuid = SHEET_UUIDS[sheet_key]
        self.title = title
        self.libs = {}      # "Lib:Name" -> block
        self.body = []      # symbol instances / labels / no_connects
        self.refs = set()

    def place(self, ref, lib, name, x, y, nets, value=None, footprint="", rot=0, mirror=None,
              dnp=None):
        """nets: {pin号或pin名: 网络名 | "NC"}，必须覆盖符号全部引脚。"""
        assert ref not in self.refs, f"位号重复 {ref}"
        assert re.match(r'^#?[A-Za-z]+\d+$', ref), f"位号格式非法（须字母+数字结尾）: {ref}"
        self.refs.add(ref)
        block, pins = get_symbol(lib, name)
        lid = f"{lib}:{name}"
        self.libs[lid] = block
        # 网络映射：优先按 pin 号，其次按 pin 名
        by_name = {}
        for num, p in pins.items():
            if p["name"] and p["name"] != "~":
                by_name.setdefault(p["name"], []).append(num)
        resolved = {}
        for k, net in nets.items():
            if k in pins:
                resolved[k] = net
            elif k in by_name:
                for num in by_name[k]:
                    resolved[num] = net
            else:
                raise AssertionError(f"{ref}({name}): 找不到引脚 {k}；可用: 号{sorted(pins)} 名{sorted(by_name)}")
        missing = set(pins) - set(resolved)
        assert not missing, f"{ref}({name}): 引脚未给网络: {sorted(missing, key=lambda s: (len(s), s))}"
        assert rot in (0, 90, 180, 270)
        footprint = resolve_fp(ref, footprint, nets)
        if dnp is None:
            dnp = bool(re.search(r"\bDNP\b", value or name, re.IGNORECASE))

        props = [
            ("Reference", ref, x, y - 2.54, False),
            ("Value", value or name, x, y + 2.54, False),
            ("Footprint", footprint, x, y + 5.08, True),
            ("Datasheet", "~", x, y + 7.62, True),
        ]
        s = [f'\t(symbol\n\t\t(lib_id "{lid}")\n\t\t(at {x:g} {y:g} {rot})\n']
        if mirror:
            s.append(f'\t\t(mirror {mirror})\n')
        s.append('\t\t(unit 1)\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n'
                 f'\t\t(dnp {"yes" if dnp else "no"})\n')
        s.append(f'\t\t(uuid "{_uid()}")\n')
        for k, v, px, py, hide in props:
            h = "\n\t\t\t\t(hide yes)" if hide else ""
            s.append(f'\t\t(property "{k}" "{v}"\n\t\t\t(at {px:g} {py:g} 0)\n'
                     f'\t\t\t(effects (font (size 1.27 1.27)){h})\n\t\t)\n')
        for num in pins:
            s.append(f'\t\t(pin "{num}"\n\t\t\t(uuid "{_uid()}")\n\t\t)\n')
        s.append(f'\t\t(instances\n\t\t\t(project "expansion-board-v3"\n'
                 f'\t\t\t\t(path "/{ROOT_UUID}/{self.uuid}"\n'
                 f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')
        self.body.append("".join(s))

        # 引脚 → 全局标签 / no_connect
        for num, net in resolved.items():
            p = pins[num]
            ax, ay, arot = self._pin_abs(x, y, rot, mirror, p)
            if net == "NC":
                self.body.append(f'\t(no_connect\n\t\t(at {ax:g} {ay:g})\n\t\t(uuid "{_uid()}")\n\t)\n')
            elif net is not None:
                lx, ly = ax, ay
                # 标签文字一律从引脚点向外延伸；不加引出线（凭空导线会与同线上他处锚点相交）
                # 竖直引脚原映射 {90:270,270:90} 文字穿过本体 → 反向
                la = {0: 180, 180: 0, 90: 90, 270: 270}[arot]
                just = "right" if la in (180, 90) else "left"
                self.body.append(
                    f'\t(global_label "{net}"\n\t\t(shape passive)\n\t\t(at {lx:g} {ly:g} {la})\n'
                    f'\t\t(effects (font (size 1.27 1.27)) (justify {just}))\n'
                    f'\t\t(uuid "{_uid()}")\n\t)\n')

    @staticmethod
    def _pin_abs(X, Y, rot, mirror, p):
        px, py = p["x"], -p["y"]  # 符号 Y 轴取反
        pr = p["rot"]
        if mirror == "x":
            py = -py
            pr = (360 - pr) % 360 if pr in (90, 270) else pr
        if mirror == "y":
            px = -px
            pr = (180 - pr) % 360 if pr in (0, 180) else pr
        for _ in range(rot // 90):
            px, py = py, -px      # 逆时针 90°（KiCad 实例旋转为逆时针）
            pr = (pr + 90) % 360
        return X + px, Y + py, pr

    def write(self):
        path = os.path.join(PROJ, self.key + ".kicad_sch")
        out = ['(kicad_sch\n\t(version 20231120)\n\t(generator "gen_sch.py")\n'
               '\t(generator_version "8.0")\n'
               f'\t(uuid "{self.uuid}")\n\t(paper "A3")\n'
               f'\t(title_block\n\t\t(title "{self.title}")\n\t\t(date "2026-08-01")\n'
               '\t\t(rev "V1")\n\t\t(company "Pilot Kit")\n\t)\n']
        out.append('\t(lib_symbols\n')
        for lid in sorted(self.libs):
            b = self.libs[lid]
            out.append('\t\t' + b.replace('\n', '\n\t\t') + '\n')
        out.append('\t)\n')
        out.extend(self.body)
        out.append('\t(embedded_fonts no)\n)\n')
        with open(path, "w") as f:
            f.write("".join(out))
        print(f"OK: {self.key}.kicad_sch  symbols={len(self.refs)}")
