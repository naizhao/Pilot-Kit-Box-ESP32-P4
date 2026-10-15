#!/usr/bin/env python3
"""导出 A1 mini 用 STL；新产物放在独立目录，不覆盖历史 v2 文件。

python3 export_a1mini.py --output /absolute/path/to/new-output
"""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess

from stl2obj import read_stl


def validate_mesh(triangles, expected_components=1):
    """用三角面共享边检查闭合、绕序、连通数和实际打印包围盒。"""
    edges = defaultdict(list)
    parents = list(range(len(triangles)))
    volume = 0.0
    vertices = []

    def root(i):
        while parents[i] != i:
            parents[i] = parents[parents[i]]
            i = parents[i]
        return i

    for i, tri in enumerate(triangles):
        if not all(math.isfinite(value) for point in tri for value in point):
            raise ValueError('网格含非有限坐标')
        a, b, c = [tuple(round(v, 6) for v in point) for point in tri]
        vertices.extend((a, b, c))
        ab = [b[k] - a[k] for k in range(3)]
        ac = [c[k] - a[k] for k in range(3)]
        cross = (ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0])
        if sum(v*v for v in cross) < 1e-18:
            raise ValueError('网格含退化面')
        volume += sum(a[k] * cross[k] for k in range(3)) / 6
        for u, v in ((a, b), (b, c), (c, a)):
            edges[tuple(sorted((u, v)))].append((i, 1 if u < v else -1))
    if not vertices:
        raise ValueError('空网格')
    for uses in edges.values():
        if len(uses) != 2:
            raise ValueError('网格不闭合或有非流形边')
        if uses[0][1] == uses[1][1]:
            raise ValueError('网格绕序冲突')
        parents[root(uses[0][0])] = root(uses[1][0])
    components = len({root(i) for i in range(len(triangles))})
    if components != expected_components:
        raise ValueError(f'连通实体数 {components}，预期 {expected_components}')
    if volume <= 0:
        raise ValueError('网格体积非正或整体反面')
    bounds = [[min(v[k] for v in vertices) for k in range(3)],
              [max(v[k] for v in vertices) for k in range(3)]]
    size = [round(bounds[1][k] - bounds[0][k], 4) for k in range(3)]
    if any(v > 180 for v in size):
        raise ValueError(f'超过 A1 mini 180 mm 打印范围：{size}')
    return dict(triangles=len(triangles), components=components,
                volume_mm3=volume, size_mm=size, bounds_mm=bounds)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True,
                        help='新目录；已存在时拒绝覆盖')
    parser.add_argument('--openscad', default=shutil.which('openscad') or
                        '/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD')
    parser.add_argument('--admesh', default=shutil.which('admesh') or '/opt/homebrew/bin/admesh')
    parser.add_argument('--images', action='store_true', help='同时生成模型 PNG')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error(f'目标已存在，请指定新目录：{output}')
    source = Path(__file__).with_name('pilot-kit-box-43-base.scad')
    version = subprocess.run([args.openscad, '--version'], text=True,
                             capture_output=True, check=True)
    subprocess.run([args.admesh, '--version'], text=True, capture_output=True, check=True)
    output.mkdir(parents=True)
    presets = {
        'case-patch': ['part="box"', 'gps_mode="patch"'],
        'case-ceramic-2mm': ['part="box"', 'gps_mode="ceramic"', 'gps_ceramic_h=4.6'],
        'case-ceramic-4mm': ['part="box"', 'gps_mode="ceramic"', 'gps_ceramic_h=6.6'],
        'case-no-gps': ['part="box"', 'gps_mode="none"'],
        'power-plungers': ['part="plungers"', 'n_plungers=4'],
        'mount-plate': ['part="plate"'],
    }
    report = {'openscad': (version.stdout + version.stderr).strip(),
              'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'models': {}}
    for name, definitions in presets.items():
        stl = output / f'{name}.stl'
        command = [args.openscad, '--hardwarnings', '--export-format', 'binstl', '-o', str(stl)]
        for definition in definitions:
            command.extend(['-D', definition])
        result = subprocess.run(command + [str(source)], capture_output=True, text=True)
        log = result.stdout + result.stderr
        (output / f'{name}.log').write_text(log)
        if result.returncode or 'ERROR:' in log or 'WARNING:' in log:
            raise RuntimeError(f'{name} 导出失败，见 {output / (name + ".log")}')
        components = 4 if name == 'power-plungers' else 1
        validate_mesh(read_stl(stl), expected_components=components)
        # 沿用原流程校正 STL 存储法线；先检查真实三角面，避免修复器掩盖破面。
        normals = subprocess.run([args.admesh, '--normal-values', f'--write-binary-stl={stl}', str(stl)],
                                 capture_output=True, text=True, check=True)
        (output / f'{name}.admesh.log').write_text(normals.stdout + normals.stderr)
        info = validate_mesh(read_stl(stl), expected_components=components)
        info['definitions'] = definitions
        report['models'][name] = info
        print(f'{name}: {info["size_mm"]} mm，{info["components"]} 个闭合实体', flush=True)
        if args.images:
            command = [args.openscad, '--hardwarnings', '-o', str(output / f'{name}.png'),
                       '--imgsize=1400,1000', '--autocenter', '--viewall',
                       '--camera=0,0,0,55,0,215,250', '--colorscheme=Tomorrow']
            for definition in definitions:
                command.extend(['-D', definition])
            result = subprocess.run(command + [str(source)], capture_output=True, text=True)
            image_log = result.stdout + result.stderr
            if result.returncode or 'ERROR:' in image_log or 'WARNING:' in image_log:
                raise RuntimeError(f'{name} 图片导出失败：{image_log}')
    (output / 'mesh-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
