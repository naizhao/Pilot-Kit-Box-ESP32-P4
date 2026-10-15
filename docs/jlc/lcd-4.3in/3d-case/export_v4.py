#!/usr/bin/env python3
"""更新 v4 最终打印件和预览；过程文件仅存放在系统临时目录。"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from export_a1mini import validate_mesh
from stl2obj import read_stl


def run_checked(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    log = result.stdout + result.stderr
    if result.returncode or 'ERROR:' in log or 'WARNING:' in log:
        raise RuntimeError(f'导出失败：\n{log}')
    return result


def main():
    here = Path(__file__).resolve().parent
    default_output = (here / 'v4').resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=default_output,
                        help='默认更新现有 v4 交付目录；自定义目录必须尚不存在')
    parser.add_argument('--gps-mode', choices=['ceramic', 'patch', 'none'], default='ceramic')
    parser.add_argument('--ceramic-height', type=float, choices=[4.6, 6.6], default=6.6)
    parser.add_argument('--openscad', default='/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD')
    parser.add_argument('--admesh', default='/opt/homebrew/bin/admesh')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() and output != default_output:
        parser.error(f'目标已存在，请指定新目录：{output}')
    if output == default_output and args.gps_mode != 'ceramic' and (output / 'gps-tray.stl').exists():
        parser.error('现有 v4 目录是陶瓷版；其他GPS模式请指定新目录，避免混入旧托盘')
    source = here / 'pilot-kit-box-43-base-v4.scad'
    subprocess.run([args.openscad, '--version'], capture_output=True, text=True, check=True)
    subprocess.run([args.admesh, '--version'], capture_output=True, check=True)
    with tempfile.TemporaryDirectory(prefix='case-v4-export-') as temporary:
        staging = Path(temporary)
        source_copy = staging / source.name
        shutil.copyfile(source, source_copy)
        shutil.copyfile(here / 'v4/README.md', staging / 'README.md')
        names = export_parts(args, source_copy, staging)
        output.mkdir(parents=True, exist_ok=True)
        for name in names + ['README.md']:
            shutil.copyfile(staging / name, output / name)
    print(f'已更新：{output}（仅最终 STL、预览及说明）', flush=True)


def export_parts(args, source_copy, output):
    common = [f'gps_mode="{args.gps_mode}"', f'gps_ceramic_h={args.ceramic_height}']
    presets = {'case-body': 'box', 'top-cap': 'cap', 'power-plungers': 'plungers'}
    if args.gps_mode == 'ceramic':
        presets['gps-tray'] = 'gps_tray'
    names = []
    for name, part in presets.items():
        stl = output / f'{name}.stl'
        definitions = common + [f'part="{part}"', 'n_plungers=4']
        command = [args.openscad, '--backend=Manifold', '--hardwarnings',
                   '--export-format', 'binstl', '-o', str(stl)]
        for definition in definitions:
            command.extend(['-D', definition])
        run_checked(command + [str(source_copy)])
        components = 4 if part == 'plungers' else 1
        validate_mesh(read_stl(stl), expected_components=components)
        run_checked([args.admesh, '--normal-values', f'--write-binary-stl={stl}', str(stl)])
        info = validate_mesh(read_stl(stl), expected_components=components)
        if abs(info['bounds_mm'][0][2]) > 0.001:
            raise ValueError(f'{name} 未平放在 Z=0：{info["bounds_mm"]}')
        names.append(f'{name}.stl')
        print(f'{name}: {info["size_mm"]} mm，{components} 个闭合实体', flush=True)
    views = {'assembly': 'assembly', 'exploded': 'exploded'}
    if args.gps_mode != 'none':
        views['gps-section'] = 'gps_section'
    for name, part in views.items():
        camera = '0,0,0,90,0,90,150' if part == 'gps_section' else '0,0,0,55,0,145,280'
        command = [args.openscad, '--backend=Manifold', '--hardwarnings',
                   '-o', str(output / f'{name}.png'), '--imgsize=1500,1100',
                   '--autocenter', '--viewall', '--projection=o',
                   f'--camera={camera}', '--colorscheme=Tomorrow']
        if part == 'gps_section':
            command.append('--render')
        for definition in common + [f'part="{part}"']:
            command.extend(['-D', definition])
        run_checked(command + [str(source_copy)])
        names.append(f'{name}.png')
    return names


if __name__ == '__main__':
    main()
