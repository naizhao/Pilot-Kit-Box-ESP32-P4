#!/usr/bin/env python3
"""⛔ 已废弃（2026-08-21，V3.5-r2）——不再被任何脚本调用，可以删。

r1 曾给短路脚加一条 0.508mm 水平引线、末端打过孔来"补"电感。r2 推翻了这个做法：
线宽突变本身就会引入寄生电感，正解是让 1.5mm 等宽的竖脚全程走在净空区内、
末端直接撞进 GND 铺铜（铺铜边界 = 净空边界 = 脚末端），不需要引线也不需要过孔。
文件留着只为记录这段弯路，以及 SWIG 代理失效那两个坑的实测结论。

IFA 短路脚接地过孔——r1 的唯一实现，gen_pcb.py 与 import_routes.py 曾调用这里。

## 为什么要单独一个模块

天线封装里放不了这个过孔：pad2 是 SMD，不穿层。可它又必须存在——
短路引线末端不打孔，短路脚就是**悬空**的，V3.5 那套"引线 + 隔离槽"整个失效。
而这种失效 DRC 未必报得出来：pad2 与 pad1 之间有 net_tie，网络上看不出断路。

放 gen_pcb 里会被 import_routes 清掉（它的语义是"板上布线 = 快照"，先清空再贴）；
放 import_routes 里则 gen_pcb 单独跑出来的板子是残的。两边各写一份坐标又正是
这块板栽过 5 次的「改了一处漏了另一处」。所以抽成一处，两边 import。

坐标来自 tools/ifa_geom.py（gen_ifa_footprint.py 生成），本文件不持有任何几何数字。
"""


def ant_pos_of(board, pcbnew):
    """读 ANT1 锚点，返回 (x, y) 纯数值或 None。

    ⚠️ 调用方若随后要 board.Remove()，必须**在 Remove 之前**调这个：
    Remove() 会让本进程里所有既有 SWIG 代理失效（import_routes.py:50、
    route_fix.py:26 同一个坑），之后再去 FindFootprintByReference 拿到的对象
    一碰就抛 'SwigPyObject has no attribute GetPosition'。纯数值不受影响。
    """
    ant = board.FindFootprintByReference("ANT1")
    if not ant:
        return None
    return (pcbnew.ToMM(ant.GetPosition().x), pcbnew.ToMM(ant.GetPosition().y))


def add_stub_via(board, pcbnew, netmap=None, via_dia=0.45, via_drill=0.3,
                 quiet=False, ant_pos=None, known_vias=()):
    """在 ANT1 短路引线末端放一个 GND 过孔。

    netmap    传 gen_pcb 的网络表可省一次查找；不传就从板上按名字找。
    ant_pos   ant_pos_of() 预读的坐标；清空过布线的场景必须传（见 ant_pos_of 的说明）。
    known_vias 板上已有过孔的 [(x,y), ...]，用于去重。

    ⚠️ 去重为什么要调用方喂数据、不自己遍历 board：Remove() 之后连 board 自己的
    容器访问都废了——board.GetTracks() 内部是 list(self.Tracks())，而 Tracks()
    返回的 SwigPyObject 已经不可迭代，抛 'object is not iterable'。
    只有**新建**的对象和纯数值不受影响。

    返回 (x, y) 或 None（板上没有 ANT1 时）。
    """
    from ifa_geom import STUB_VIA_LOCAL, KEEPOUT_LOCAL

    if ant_pos is None:
        ant_pos = ant_pos_of(board, pcbnew)
    if ant_pos is None:
        return None
    ax, ay = ant_pos
    vx, vy = ax + STUB_VIA_LOCAL[0], ay + STUB_VIA_LOCAL[1]

    # 过孔必须落在主禁铜带**外**：带内 route_rf.py 会判它违例，
    # 而且那里本来就是要留给辐射体的净空。
    kx0, ky0 = ax + KEEPOUT_LOCAL[0], ay + KEEPOUT_LOCAL[1]
    kx1, ky1 = ax + KEEPOUT_LOCAL[2], ay + KEEPOUT_LOCAL[3]
    assert not (kx0 <= vx <= kx1 and ky0 <= vy <= ky1), \
        f"短路脚过孔 ({vx:.3f},{vy:.3f}) 落在 IFA 禁铜带内，会被判违例"

    for kx, ky in known_vias:
        if abs(kx - vx) < 0.01 and abs(ky - vy) < 0.01:
            return (vx, vy)              # 已经有了（例如从 ROUTES.json 贴回来的）

    net = netmap["GND"] if netmap else board.FindNet("GND")
    assert net, "找不到 GND 网络"
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I_MM(round(vx, 4), round(vy, 4)))
    v.SetWidth(pcbnew.FromMM(via_dia))
    v.SetDrill(pcbnew.FromMM(via_drill))
    v.SetNet(net)
    board.Add(v)
    if not quiet:
        print(f"IFA 短路脚接地过孔: ({vx:.3f}, {vy:.3f}) GND Ø{via_dia}/{via_drill}")
    return (vx, vy)
