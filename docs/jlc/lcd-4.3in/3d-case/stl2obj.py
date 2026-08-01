#!/usr/bin/env python3
"""STL → OBJ 转换（无第三方依赖，支持 ASCII / 二进制 STL）。

用途：OpenSCAD 只能导出 STL/OFF/AMF/3MF，嘉立创 3D 打印下单只收
STEP / OBJ，用本脚本把 STL 转成 OBJ（单位毫米，网格与 STL 完全一致）。

用法：python3 stl2obj.py in.stl out.obj
"""
import struct
import sys


def read_stl(path):
    """返回三角形列表 [((x,y,z),(x,y,z),(x,y,z)), ...]"""
    raw = open(path, "rb").read()
    tris = []
    if raw[:5] == b"solid" and b"facet" in raw[:1000]:
        floats = []
        for line in raw.decode("ascii", "ignore").splitlines():
            parts = line.split()
            if parts and parts[0] == "vertex":
                floats.append(tuple(float(v) for v in parts[1:4]))
        assert len(floats) % 3 == 0, "ASCII STL vertex 数不是 3 的倍数"
        tris = [tuple(floats[i:i + 3]) for i in range(0, len(floats), 3)]
    else:
        (n,) = struct.unpack_from("<I", raw, 80)
        assert len(raw) >= 84 + n * 50, "二进制 STL 长度与三角形数不符"
        for i in range(n):
            off = 84 + i * 50 + 12          # 跳过法线
            v = struct.unpack_from("<9f", raw, off)
            tris.append(((v[0], v[1], v[2]), (v[3], v[4], v[5]), (v[6], v[7], v[8])))
    assert tris, "STL 中没有三角形"
    return tris


def write_obj(path, tris):
    idx, verts, faces = {}, [], []
    for tri in tris:
        face = []
        for v in tri:
            key = (round(v[0], 5), round(v[1], 5), round(v[2], 5))
            if key not in idx:
                idx[key] = len(verts) + 1
                verts.append(key)
            face.append(idx[key])
        faces.append(face)
    with open(path, "w") as f:
        f.write("# converted from STL, unit: mm\n")
        for v in verts:
            f.write(f"v {v[0]} {v[1]} {v[2]}\n")
        for a, b, c in faces:
            f.write(f"f {a} {b} {c}\n")
    assert len(verts) > 3 and len(faces) > 3, "OBJ 输出异常"
    return len(verts), len(faces)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("用法: python3 stl2obj.py in.stl out.obj")
    nv, nf = write_obj(sys.argv[2], read_stl(sys.argv[1]))
    print(f"{sys.argv[2]}: {nv} 顶点 / {nf} 三角形")
