#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""用 UniXNU 构建树里真实的 XNU 编译记录生成 build/pagani-camera/libPaganiCamera.a（一加 13T 相机阶段 2，PaganiCamera）。

作为 PaganiCamera 组件的构建步骤运行（"{component}/scripts/build-pagani-camera.py"，工作目录是构建树），排在 Adreno830 组件之后：PaganiCamera 调用 libAdreno830.a 里的
SM8750 平台层（Sm8750Rpmh），自己不再编译。--append-to ARCHIVE（设备配方传 build/adreno830/libAdreno830.a）把目标文件也加进那个强制加载的归档，不改链接设置；会检查归档
定义了需要的平台符号，也不重复定义这里的强符号。

归档里是可移植核心（CamBringup.c、CamCci.c、CamPmic.c）和内核胶水 PaganiCamera.cpp、PaganiCameraIos.cpp。PaganiCameraIos.cpp 的类初始化从 __mod_init_func 摘掉并导出，
由 pagani_camera_ios_publish 晚注册（PaganiTorchIos.cpp 的做法）；别的目标文件不许带静态初始化。零警告，每个目标文件都核对是 iOS 17.0 的 ARM64 MH_OBJECT，内核代码不用浮点。

  build-pagani-camera.py [--tree TREE] [--source DIR] [--adreno830 DIR] [--out DIR] [--append-to A]

--out 默认 TREE/build/pagani-camera；检查构建传别的目录，构建树原样不动（编译记录只读）。"""
import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
# 编译记录的回放、审计和符号解析和蓝牙、手电筒同一份，从它们的脚本取，不抄第三遍。
bt_spec = importlib.util.spec_from_file_location('build_pagani_bt', SOURCE.parent / 'PaganiBt/scripts/build-pagani-bt.py')
bt = importlib.util.module_from_spec(bt_spec)
bt_spec.loader.exec_module(bt)
torch_spec = importlib.util.spec_from_file_location('build_pagani_torch', SOURCE.parent / 'PaganiTorch/scripts/build-pagani-torch.py')
torch = importlib.util.module_from_spec(torch_spec)
torch_spec.loader.exec_module(torch)

C_SOURCES = ['CamBringup.c', 'CamCci.c', 'CamPmic.c']
CXX_SOURCES = ['PaganiCamera.cpp']
STAGED = 'PaganiCameraIos.cpp'
HEADERS = ['PaganiCamera.h', 'PaganiCameraBoard.h', 'CamBringup.h', 'CamCci.h', 'CamPmic.h', 'CamUtil.h', 'ios/CamProvider.h']
INITIALIZER = '__GLOBAL__sub_I_PaganiCameraIos.cpp'
REQUIRED = ['_pagani_camera_start', '_pagani_camera_status', '_pagani_camera_ios_publish', '_pcam_run', '_pcam_write_allowed', '_pcam_vrm_allowed', '_pcam_cci_read16',
            '_pcam_pmic_rail', INITIALIZER]
WEAK = ['_boot_progress_trace', '_pagani_panel_probe_quiet']
PLATFORM = ['_Sm8750CmdDbCopy', '_Sm8750CmdDbVrm', '_Sm8750CmdDbArc', '_Sm8750RpmhArcLevel', '_Sm8750RpmhWriteActive']


def fail(message):
    raise SystemExit('build-pagani-camera: ' + message)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiCamera sources (default: this directory)')
    parser.add_argument('--adreno830', type=Path, help='Adreno830 sources (default: TREE/drivers/Adreno830)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-camera)')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / bt.RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({bt.RECORD_DIR}); run the UniXNU kernel step first')
    missing = [name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    adreno = (args.adreno830 or tree / 'drivers/Adreno830').resolve()
    platform = adreno / 'platform/sm8750'
    if not (platform / 'Sm8750Rpmh.h').is_file():
        fail(f'{platform}/Sm8750Rpmh.h missing; PaganiCamera depends on the Adreno830 component (its SM8750 platform layer)')
    out = (args.out or tree / 'build/pagani-camera').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = bt.load_record(tree, bt.CXX_RECORD), bt.load_record(tree, bt.C_RECORD)
    extra = ['-I', str(source), '-I', str(platform), '-mgeneral-regs-only'] + bt.WARNINGS
    objects = [bt.compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.cam.o'), extra) for name in C_SOURCES]
    objects += [bt.compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.cam.o'), extra) for name in CXX_SOURCES]
    for obj in objects:
        if '__mod_init_func' in bt.section_names(obj):
            fail(f'{obj.name} has a static initializer; only {STAGED} may (registered late by pagani_camera_ios_publish)')
    service = bt.compile_one(cxx_record, source / STAGED, out / 'obj/PaganiCameraIos.cam.o', extra)
    late = out / 'obj/PaganiCameraIos.cam.late.o'
    late.unlink(missing_ok=True)
    subprocess.run([torch.tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=' + INITIALIZER, str(service), str(late)], check=True)
    if torch.text(service) != torch.text(late) or any(sect == '__mod_init_func' for _, sect in torch.sections(late)):
        fail('staging the class initializer changed its instructions or kept __mod_init_func')
    objects.append(late)
    archive = out / 'libPaganiCamera.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    defined, undefined = bt.symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    problems += [f'{name} must come from libAdreno830.a, not be compiled here' for name in PLATFORM if name in defined]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS] + [Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': bt.sha(archive), 'objects': [obj.name for obj in objects],
              'source_sha256': {p.name: bt.sha(p) for p in listed}, 'platform': str(platform), 'undefined': sorted(undefined)}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list "component:Adreno830" (and its board recipe) before PaganiCamera')
        target_defined, _ = bt.symbols([target])
        absent = [name for name in PLATFORM if target_defined.get(name) != 'strong']
        if absent:
            fail(f'{target} lacks the SM8750 platform symbols: ' + ', '.join(absent))
        clash = [name for name, kind in defined.items() if kind == 'strong' and target_defined.get(name) == 'strong']
        if clash:
            fail(f'strong symbols already in {target}: ' + ', '.join(sorted(clash)))
        subprocess.run(['xcrun', 'ar', 'rcs', str(target), *map(str, objects)], check=True)
        twice = {name: m for name, m in bt.strong_per_member(target).items() if len(m) > 1}
        if twice:
            fail(f'{target}: symbols defined by more than one member: ' + ', '.join(sorted(twice)))
        report['appended_to'] = str(target)
        report['appended_sha256'] = bt.sha(target)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    where = f' and appended to {args.append_to}' if args.append_to else ''
    print(f'PASS: {len(objects)} objects in {archive}{where} ({len(REQUIRED)} entry points, {len(WEAK)} weak defaults, '
          f'{len([n for n in undefined if n.startswith("_Sm8750")])} platform imports)')


if __name__ == '__main__':
    main()
