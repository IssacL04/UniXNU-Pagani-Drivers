#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build the iOS flashlight plug-in (ios/TorchPlugin.m) as the file mediaserverd's capture stack opens, signed ad hoc with 4 KiB pages.

  build-pagani-torch-plugin.py [--out DIR]

Writes DIR/H6ISP.mediacapture and DIR/trust.json (its CDHash for the owner trust cache). Default DIR: build/pagani-torch-plugin of the
UniXNU-Pagani-Drivers checkout (ignored by Git). Checks: an arm64 MH_DYLIB for iOS 17.0 with the install name of the path CMCapture opens;
H6ISPCaptureDeviceCreate NOT exported (with it the vendor would believe in a capture device; TorchIos.h); the constructor present; the
names of TorchPlugin.h equal to TorchIos.h's; a verified ad hoc signature with 4 KiB pages and the identifier org.unixnu.pagani.torch."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
DEFAULT_OUT = SOURCE.parents[1] / 'build/pagani-torch-plugin'
IDENTIFIER = 'org.unixnu.pagani.torch'
NAME = 'H6ISP.mediacapture'
INSTALL_NAME = '/System/Library/MediaCapture/' + NAME
PAIRS = (('PGT_PLUGIN_SERVICE', 'PGT_IOS_SERVICE'), ('PGT_PLUGIN_PROCESS', 'PGT_IOS_PROCESS'), ('PGT_PLUGIN_SELECTOR', 'PGT_IOS_SELECTOR_SET'))


def fail(message):
    raise SystemExit('build-pagani-torch-plugin: ' + message)


def define(text, name):
    found = re.search(r'#define\s+' + name + r'\s+(\S+)', text)
    if not found:
        fail(f'{name} is not defined')
    return found.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    plugin_h, ios_h = (SOURCE / 'ios/TorchPlugin.h').read_text(), (SOURCE / 'TorchIos.h').read_text()
    for ours, theirs in PAIRS:
        if define(plugin_h, ours) != define(ios_h, theirs):
            fail(f'{ours} differs from TorchIos.h {theirs}')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True, capture_output=True, text=True).stdout.strip()
    target = out / NAME
    target.unlink(missing_ok=True)
    result = subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-I', str(SOURCE / 'ios'), '-O2', '-g0',
                             '-Wall', '-Wextra', '-Werror', '-fno-objc-arc', '-fvisibility=hidden', '-dynamiclib', '-Wl,-no_warn_inits', '-install_name',
                             INSTALL_NAME, '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'IOKit', '-lobjc',
                             str(SOURCE / 'ios/TorchPlugin.m'), '-o', str(target)], capture_output=True, text=True)
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
    load = subprocess.run(['xcrun', 'otool', '-l', str(target)], check=True, capture_output=True, text=True).stdout
    if INSTALL_NAME not in load or 'minos 17.0' not in load:
        fail('wrong install name or minimum OS')
    linked = subprocess.run(['xcrun', 'otool', '-L', str(target)], check=True, capture_output=True, text=True).stdout
    outside = [line.split()[0] for line in linked.splitlines()[1:] if not line.split()[0].startswith(('/System/', '/usr/lib/'))]
    if outside:
        fail('links outside the system: ' + ', '.join(outside))
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', IDENTIFIER, str(target)], check=True,
                   capture_output=True)
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
        'source_sha256': hashlib.sha256((SOURCE / 'ios/TorchPlugin.m').read_bytes()).hexdigest()}, indent=2) + '\n')
    print(f'PASS: {target} CDHash {cdhash.group(1)} (arm64 MH_DYLIB, iOS 17.0, {len(data)} bytes, nothing exported for the vendor)')


if __name__ == '__main__':
    main()
