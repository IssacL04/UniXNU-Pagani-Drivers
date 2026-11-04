#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""用 UniXNU 构建树里真实的 XNU 编译记录生成 build/pagani-bt/libPaganiBt.a（一加 13T 蓝牙，PaganiBt）。

作为 PaganiBt 组件的构建步骤运行（"{component}/scripts/build-pagani-bt.py"，工作目录是构建树），排在 Adreno830 组件之后：PaganiBt 调用
libAdreno830.a 里的 SM8750 平台层（Sm8750Rpmh），自己不再编译。和 PaganiWifi、PaganiHaptics 一样，--append-to ARCHIVE（设备配方传
build/adreno830/libAdreno830.a）把目标文件也加进那个强制加载的归档，不改链接设置；会检查归档定义了 PaganiBt 需要的平台符号，
也不重复定义 PaganiBt 的强符号。

归档里是可移植核心（BtTlv.c、BtH4.c、BtIbs.c、BtGeni.c、BtPmic.c、BtFat.c、BtFirmware.c、BtDownload.c、BtBringup.c、BtBridge.c、BtStandIn.c）和
内核胶水 PaganiBt.cpp、PaganiBtHost.cpp；任何目标文件都不许带静态初始化（胶水不注册 IOKit 类）。零警告（-Wall -Wextra -Werror，和其他 Pagani 驱动同一策略），
每个目标文件都核对是 iOS 17.0 的 ARM64 MH_OBJECT，内核代码不用浮点（-mgeneral-regs-only）。

  build-pagani-bt.py [--tree TREE] [--source DIR] [--adreno830 DIR] [--out DIR] [--append-to A]

--out 默认 TREE/build/pagani-bt；检查构建传别的目录，构建树原样不动（编译记录只读）。--append-to 传 libAdreno830.a 的副本时只检查，
不碰树里的那份。"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
RECORD_DIR = Path('build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837')
CXX_RECORD = RECORD_DIR / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = RECORD_DIR / 'bsd/DEVELOPMENT/bsd_init.o.json'
C_SOURCES = ['BtTlv.c', 'BtH4.c', 'BtIbs.c', 'BtGeni.c', 'BtPmic.c', 'BtFat.c', 'BtFirmware.c', 'BtDownload.c', 'BtBringup.c', 'BtBridge.c',
             'BtStandIn.c']
CXX_SOURCES = ['PaganiBt.cpp', 'PaganiBtHost.cpp']
HEADERS = ['PaganiBt.h', 'PaganiBtBoard.h', 'BtTlv.h', 'BtH4.h', 'BtIbs.h', 'BtGeni.h', 'BtPmic.h', 'BtFat.h', 'FatVolume.h', 'FatVolume.c',
           'BtFirmware.h', 'BtDownload.h', 'BtBringup.h', 'BtBridge.h', 'BtStandIn.h', 'BtUtil.h', 'PaganiBtHost.h']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
REQUIRED = ['_pagani_bt_start', '_pagani_bt_status', '_pb_dl_boot', '_pb_geni_write', '_pb_pmic_bt_en', '_pagani_bt_fat_mount', '_pb_bridge_step',
            '_pagani_bt_host_publish']
# 内核可能没有的提供者（BootProgress、NativeUfs）和共享锁字：弱定义，真的那个优先。
WEAK = ['_boot_progress_trace', '_native_ufs_part_find', '_native_ufs_part_read', '_native_ufs_part_status', '_sm8750_gcc_vote_owner',
        '_pagani_panel_probe_quiet']
# libAdreno830.a 必须定义、这个归档不能定义的。
PLATFORM = ['_Sm8750CmdDbCopy', '_Sm8750CmdDbVrm', '_Sm8750RpmhWriteActive']


def fail(message):
    raise SystemExit('build-pagani-bt: ' + message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_record(tree, relative):
    path = tree / relative
    if not path.is_file():
        fail(f'{path} missing; the XNU bootstrap pass (XNU_BOOTSTRAP=1) leaves the compile records this build replays')
    return json.loads(path.read_text().strip().rstrip(','))


def command(record, source, obj, extra):
    """把记录改成另一个源文件和目标文件：保留 ABI 选项，去掉它的警告和依赖选项。"""
    arguments, result, i = record['arguments'], [], 0
    while i < len(arguments):
        value = arguments[i]
        if value == record['file']:
            result.append(str(source))
        elif value == '-o':
            result += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ', '-MT', '-MQ'):
            i += 1
        elif value == '-Xclang' and i + 1 < len(arguments) and arguments[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP', '-MMD'):
            result.append(value)
        i += 1
    return result + extra


def audit(obj):
    """ARM64 MH_OBJECT，iOS，最低 17.0。"""
    data = obj.read_bytes()
    magic, cpu, _, kind, count, _ = struct.unpack_from('<6I', data)
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c, 1):
        fail(f'{obj}: not an ARM64 Mach-O object')
    offset, version = 32, None
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            version = struct.unpack_from('<II', data, offset + 8)
        offset += size
    if version != (2, 0x110000):
        fail(f'{obj}: LC_BUILD_VERSION {version} is not iOS 17.0')


def compile_one(record, source, obj, extra):
    obj.parent.mkdir(parents=True, exist_ok=True)
    obj.unlink(missing_ok=True)
    # /usr/bin/python3 是 xcrun 垫片，会带上 macOS 的 SDKROOT，盖过记录里的 iOS SDK
    environment = {key: value for key, value in os.environ.items() if key != 'SDKROOT'}
    result = subprocess.run(command(record, source, obj, extra), cwd=record['directory'], capture_output=True, text=True, env=environment)
    output = (result.stdout + result.stderr).strip()
    if result.returncode:
        fail(f'{source}:\n{output[-6000:]}')
    if 'warning:' in output:
        fail(f'{source} compiled with warnings:\n{output[-6000:]}')
    audit(obj)
    return obj


def section_names(obj):
    data = obj.read_bytes()
    count = struct.unpack_from('<I', data, 16)[0]
    offset, names = 32, []
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            names += [data[offset + 72 + 80 * k:offset + 72 + 80 * k + 16].rstrip(b'\0').decode() for k in range(nsects)]
        offset += size
    return names


def symbols(paths):
    """nm -m 给出的 {名字: 'weak'|'strong'} 定义集合和未定义集合。"""
    output = subprocess.run(['xcrun', 'nm', '-m', *map(str, paths)], check=True, capture_output=True, text=True).stdout
    defined, undefined = {}, set()
    for line in output.splitlines():
        match = re.search(r'\(undefined\) (?:weak )?external (?:\[[^\]]*\] )*(\S+)', line)
        if match:
            undefined.add(match.group(1))
            continue
        match = re.search(r'\((?:__\w+,[^)]+|absolute|common)\) (weak )?(?:private )?external (?:\[[^\]]*\] )*'
                          r'(?:automatically hidden )?(\S+)', line)
        if match:
            name = match.group(2)
            kind = 'weak' if match.group(1) else 'strong'
            defined[name] = 'strong' if defined.get(name) == 'strong' else kind
    return defined, undefined - set(defined)


def strong_per_member(archive):
    output = subprocess.run(['xcrun', 'nm', '-m', str(archive)], check=True, capture_output=True, text=True).stdout
    member, owners = None, {}
    for line in output.splitlines():
        header = re.match(r'^.*\((.+\.o)\):$', line)
        if header:
            member = header.group(1)
            continue
        match = re.search(r'\((?:__\w+,[^)]+|absolute)\) (weak )?(?:private )?external (?:\[[^\]]*\] )*'
                          r'(?:automatically hidden )?(\S+)', line)
        if match and not match.group(1):
            owners.setdefault(match.group(2), set()).add(member)
    return owners


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiBt sources (default: this directory)')
    parser.add_argument('--adreno830', type=Path, help='Adreno830 sources (default: TREE/drivers/Adreno830)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-bt)')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({RECORD_DIR}); run the UniXNU kernel step first')
    missing = [name for name in C_SOURCES + CXX_SOURCES + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    adreno = (args.adreno830 or tree / 'drivers/Adreno830').resolve()
    platform = adreno / 'platform/sm8750'
    if not (platform / 'Sm8750Rpmh.h').is_file():
        fail(f'{platform}/Sm8750Rpmh.h missing; PaganiBt depends on the Adreno830 component (its SM8750 platform layer)')
    out = (args.out or tree / 'build/pagani-bt').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(platform), '-mgeneral-regs-only'] + WARNINGS
    objects = [compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.bt.o'), extra) for name in C_SOURCES]
    objects += [compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.bt.o'), extra) for name in CXX_SOURCES]
    for obj in objects:
        if '__mod_init_func' in section_names(obj):
            fail(f'{obj.name} has a static initializer')
    archive = out / 'libPaganiBt.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    defined, undefined = symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    problems += [f'{name} must come from libAdreno830.a, not be compiled here' for name in PLATFORM if name in defined]
    problems += [f'{name} is defined by FatVolume under its own name' for name in ('_fat_mount', '_fat_open', '_fat_read') if name in defined]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + HEADERS] + [Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [obj.name for obj in objects],
              'source_sha256': {p.name: sha(p) for p in listed}, 'platform': str(platform), 'undefined': sorted(undefined)}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list "component:Adreno830" (and its board recipe) before PaganiBt')
        target_defined, _ = symbols([target])
        absent = [name for name in PLATFORM if target_defined.get(name) != 'strong']
        if absent:
            fail(f'{target} lacks the SM8750 platform symbols: ' + ', '.join(absent))
        clash = [name for name, kind in defined.items() if kind == 'strong' and target_defined.get(name) == 'strong']
        if clash:
            fail(f'strong symbols already in {target}: ' + ', '.join(sorted(clash)))
        subprocess.run(['xcrun', 'ar', 'rcs', str(target), *map(str, objects)], check=True)
        twice = {name: m for name, m in strong_per_member(target).items() if len(m) > 1}
        if twice:
            fail(f'{target}: symbols defined by more than one member: ' + ', '.join(sorted(twice)))
        report['appended_to'] = str(target)
        report['appended_sha256'] = sha(target)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    where = f' and appended to {args.append_to}' if args.append_to else ''
    print(f'PASS: {len(objects)} objects in {archive}{where} ({len(REQUIRED)} entry points, {len(WEAK)} weak defaults, '
          f'{len([n for n in undefined if n.startswith("_Sm8750")])} platform imports)')


if __name__ == '__main__':
    main()
