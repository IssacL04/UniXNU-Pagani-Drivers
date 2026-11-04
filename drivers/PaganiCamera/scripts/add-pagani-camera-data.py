#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""把带测试图的 H6ISP.mediacapture 放进打包 Data 种子的副本，把它的 CDHash 加进属主信任包的副本。BASE 和原信任包都不动。

  add-pagani-camera-data.py [--plugin DIR] --trust BUNDLE [--trust-out BUNDLE] [--original-trust BUNDLE] [--replace] [--test-pattern]
                            BASE.dbrdisk OUTPUT.dbrdisk

插件（DIR 里 build-pagani-camera-plugin.py 的产物；没有 --plugin 时现编，放在 OUTPUT 旁边）进 Data 卷的 ngpu/mediacapture，也就是
/private/var/ngpu/mediacapture/H6ISP.mediacapture，内核的 phone-torch-overlay.patch 把它叠到 /System/Library/MediaCapture。--test-pattern 再放一个空的
标记文件 PaganiCameraTestPattern：有它，插件才建提供者；没有它，插件里只有手电筒和厂商类那一处修正。--replace 换掉 BASE 里已有的插件，它的 CDHash 从
信任包里删掉（手电筒的种子走这条路）。信任包的处理是 PaganiAudio 的 add_trust（一个属主模块，苹果模块逐字节比对、绝不改），经手电筒的脚本取。

写 OUTPUT、信任包（默认 <OUTPUT 主名>-owner-trust.bundle）和 OUTPUT 的 .json。要 macOS（hdiutil、codesign）。这个脚本不部署任何东西，镜像再加：
  --data-ramdisk OUTPUT --owner-trust-cache <信任包> --kernel-arg pagani-torch=1 --kernel-arg pagani-torch-ios=1"""
import argparse
import importlib.util
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('add_pagani_torch_data', SOURCE.parent / 'PaganiTorch/scripts/add-pagani-torch-data.py')
torch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(torch)
audio, packed_ramdisk, SEED_DIR, NAME = torch.audio, torch.packed_ramdisk, torch.SEED_DIR, torch.NAME
MARKER = 'PaganiCameraTestPattern'     # ios/CamProvider.h PCAM_MARKER


def fail(message):
    raise SystemExit('add-pagani-camera-data: ' + message)


def plugin(directory, output):
    if directory is None:
        directory = output.with_suffix('.plugin')
        subprocess.run([sys.executable, '-B', str(SOURCE / 'scripts/build-pagani-camera-plugin.py'), '--out', str(directory)], check=True)
    return torch.plugin(directory, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plugin', type=Path)
    parser.add_argument('--trust', type=Path, required=True)
    parser.add_argument('--trust-out', type=Path)
    parser.add_argument('--original-trust', type=Path, default=audio.DEFAULT_ORIGINAL)
    parser.add_argument('--replace', action='store_true', help='drop the CDHash of the plug-in BASE carries (read from it)')
    parser.add_argument('--test-pattern', action='store_true', help='also place the marker that switches the camera test pattern on')
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    base, output = args.base.resolve(), args.output.resolve()
    trust_out = (args.trust_out or output.with_name(output.stem + '-owner-trust.bundle')).resolve()
    for path in (output, output.with_suffix('.json'), trust_out):
        if path.exists():
            fail('refuse to overwrite ' + str(path))
    if not args.trust.is_file():
        fail(f'no trust bundle {args.trust}')
    original = args.original_trust if args.original_trust and args.original_trust.is_file() else None
    output.parent.mkdir(parents=True, exist_ok=True)
    file, plugin_trust = plugin(args.plugin.resolve() if args.plugin else None, output)
    if plugin_trust.get('identifier') != 'org.unixnu.pagani.camera':
        fail(f'{file} is not the plug-in of build-pagani-camera-plugin.py')
    logical, extents = packed_ramdisk.regions(base.read_bytes())
    report = dict(base=str(base), base_bytes=logical, base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], test_pattern=args.test_pattern,
                  plugin=dict(path='/private/var/' + SEED_DIR + '/' + NAME, **plugin_trust))
    drop = []
    with tempfile.TemporaryDirectory(prefix='camera-seed-', dir=str(output.parent)) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        mount = Path(scratch) / 'mount'
        mount.mkdir()
        attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off',
                                                           '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            if not (mount / 'mobile').is_dir():
                fail('the Data seed has no /mobile; wrong base image?')
            target = mount / SEED_DIR / NAME
            if target.exists():
                if not args.replace:
                    fail(f'{base} already carries {SEED_DIR}/{NAME}; pass --replace')
                drop.append(audio.cdhash_of(target))
                report['replaced_plugin_cdhash'] = drop[0]
                target.unlink()
            (mount / SEED_DIR).mkdir(parents=True, exist_ok=True)
            for directory in (mount / 'ngpu', mount / SEED_DIR):
                os.chmod(directory, 0o755)
            shutil.copy2(file, target)
            os.chmod(target, 0o755)
            audio.run('codesign', '--verify', '--strict', str(target))
            marker = mount / SEED_DIR / MARKER
            marker.unlink(missing_ok=True)
            if args.test_pattern:
                marker.write_bytes(b'')
                os.chmod(marker, 0o644)
            usage = subprocess.run(['df', '-k', str(mount)], capture_output=True, text=True).stdout.splitlines()[-1].split()
            report['used_kib'], report['available_kib'] = int(usage[2]), int(usage[3])
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True, capture_output=True)
        if raw.open('rb').read(4096)[32:36] != b'NXSB':
            fail('the edited image is no APFS container any more')
        trust_bytes, trust_report = audio.add_trust(args.trust.resolve(), original, plugin_trust['cdhash'], drop)
        report['output'] = packed_ramdisk.pack(raw, output)
    trust_out.write_bytes(trust_bytes)
    report['trust'] = dict(output=str(trust_out), **trust_report)
    report['image_arguments'] = ['--data-ramdisk', str(output), '--owner-trust-cache', str(trust_out), '--kernel-arg', 'pagani-torch=1',
                                 '--kernel-arg', 'pagani-torch-ios=1']
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {output} with {NAME} (CDHash {plugin_trust["cdhash"]}, test pattern {"on" if args.test_pattern else "off"}), trust {trust_out}')


if __name__ == '__main__':
    main()
