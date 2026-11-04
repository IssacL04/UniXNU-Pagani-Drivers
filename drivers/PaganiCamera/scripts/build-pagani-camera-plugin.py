#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""把 H6ISP.mediacapture 编成带相机测试图的版本：PaganiTorch 的 ios/TorchPlugin.m 原样，加 ios/CamProvider.m、ios/CamPattern.c。ad hoc 签名，4 KiB 页。

  build-pagani-camera-plugin.py [--out DIR]

写 DIR/H6ISP.mediacapture 和 DIR/trust.json（属主信任包要的 CDHash）。DIR 默认是 UniXNU-Pagani-Drivers 下的 build/pagani-camera-plugin（Git 忽略）。
手电筒的 scripts/build-pagani-torch-plugin.py 不动，它编出来的是没有这些代码的版本；同一路径只能放一个文件，种子里放哪个由
scripts/add-pagani-camera-data.py 决定。

检查：iOS 17.0 的 arm64 MH_DYLIB，install name 是 CMCapture 打开的路径；不导出任何 CaptureDeviceCreate（导出了厂商类就会相信有默认设备，TorchIos.h）；
两个构造函数都在；只链系统库；CMIOExtension 的类不做静态引用（SDK 把它们标成 iOS 不可用，而且 CoreMediaIO 由 CMCapture 带进进程）；TorchPlugin.h
的名字等于 TorchIos.h 的；签名验证通过，页大小 4096，标识 org.unixnu.pagani.camera。"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
TORCH = SOURCE.parent / 'PaganiTorch'
DEFAULT_OUT = SOURCE.parents[1] / 'build/pagani-camera-plugin'
IDENTIFIER = 'org.unixnu.pagani.camera'
NAME = 'H6ISP.mediacapture'
INSTALL_NAME = '/System/Library/MediaCapture/' + NAME
SOURCES = [TORCH / 'ios/TorchPlugin.m', SOURCE / 'ios/CamProvider.m', SOURCE / 'ios/CamPattern.c']
FRAMEWORKS = ['Foundation', 'CoreFoundation', 'CoreMedia', 'CoreVideo', 'IOKit']
PAIRS = (('PGT_PLUGIN_SERVICE', 'PGT_IOS_SERVICE'), ('PGT_PLUGIN_PROCESS', 'PGT_IOS_PROCESS'), ('PGT_PLUGIN_SELECTOR', 'PGT_IOS_SELECTOR_SET'))
FLAGS = ['-O2', '-g0', '-Wall', '-Wextra', '-Werror', '-fno-objc-arc', '-fvisibility=hidden', '-dynamiclib', '-Wl,-no_warn_inits']


def fail(message):
    raise SystemExit('build-pagani-camera-plugin: ' + message)


def define(text, name):
    found = re.search(r'#define\s+' + name + r'\s+(\S+)', text)
    if not found:
        fail(f'{name} is not defined')
    return found.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    plugin_h, ios_h = (TORCH / 'ios/TorchPlugin.h').read_text(), (TORCH / 'TorchIos.h').read_text()
    for ours, theirs in PAIRS:
        if define(plugin_h, ours) != define(ios_h, theirs):
            fail(f'{ours} differs from TorchIos.h {theirs}')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True, capture_output=True, text=True).stdout.strip()
    target = out / NAME
    target.unlink(missing_ok=True)
    frameworks = [flag for name in FRAMEWORKS for flag in ('-framework', name)]
    result = subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-I', str(TORCH / 'ios'), '-I', str(SOURCE / 'ios'), *FLAGS,
                             '-install_name', INSTALL_NAME, *frameworks, '-lobjc', *map(str, SOURCES), '-o', str(target)], capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    if result.returncode or 'warning:' in output:
        fail(output[-6000:])
    data = target.read_bytes()
    magic, cpu, subtype, kind = struct.unpack_from('<4I', data)
    if (magic, cpu, subtype & 0xffffff, kind) != (0xfeedfacf, 0x100000c, 0, 6):
        fail('not an arm64 MH_DYLIB')
    exported = subprocess.run(['xcrun', 'nm', '-gU', str(target)], capture_output=True, text=True).stdout
    if 'CaptureDeviceCreate' in exported:
        fail('the plug-in must not export a CaptureDeviceCreate function')
    undefined = subprocess.run(['xcrun', 'nm', '-u', str(target)], check=True, capture_output=True, text=True).stdout
    if '_OBJC_CLASS_$_CMIO' in undefined:
        fail('CMIOExtension classes must be looked up at run time')
    load = subprocess.run(['xcrun', 'otool', '-l', str(target)], check=True, capture_output=True, text=True).stdout
    if INSTALL_NAME not in load or 'minos 17.0' not in load:
        fail('wrong install name or minimum OS')
    initializers = subprocess.run(['xcrun', 'otool', '-s', '__DATA_CONST', '__mod_init_func', str(target)], capture_output=True, text=True).stdout
    if '__mod_init_func' not in initializers and 'pcam_plugin_load' not in subprocess.run(['xcrun', 'nm', str(target)], capture_output=True, text=True).stdout:
        fail('no constructor')
    linked = subprocess.run(['xcrun', 'otool', '-L', str(target)], check=True, capture_output=True, text=True).stdout
    outside = [line.split()[0] for line in linked.splitlines()[1:] if not line.split()[0].startswith(('/System/', '/usr/lib/'))]
    if outside:
        fail('links outside the system: ' + ', '.join(outside))
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', IDENTIFIER, str(target)], check=True, capture_output=True)
    subprocess.run(['codesign', '--verify', '--strict', str(target)], check=True)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(target)], capture_output=True, text=True, check=True).stderr
    cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    page = re.search(r'^Page size=(\d+)$', detail, re.M)
    ident = re.search(r'^Identifier=(.+)$', detail, re.M)
    if not cdhash or not page or page.group(1) != '4096' or not ident or ident.group(1) != IDENTIFIER:
        fail('the signature is not the one asked for:\n' + detail)
    (out / 'trust.json').write_text(json.dumps({
        'cdhash': cdhash.group(1), 'identifier': IDENTIFIER, 'page_size': 4096, 'install_name': INSTALL_NAME,
        'executable_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        'source_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in SOURCES}}, indent=2) + '\n')
    print(f'PASS: {target} CDHash {cdhash.group(1)} (arm64 MH_DYLIB, iOS 17.0, {len(data)} bytes, nothing exported for the vendor)')


if __name__ == '__main__':
    main()
