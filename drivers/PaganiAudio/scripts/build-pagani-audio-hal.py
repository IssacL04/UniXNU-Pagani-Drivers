#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the PaganiAudio audio server plug-in (hal/PaganiAudioPlugIn.c) as an iOS driver bundle, signed ad hoc with 4 KiB pages.

  build-pagani-audio-hal.py [--out DIR]

Writes DIR/PaganiAudio.driver and DIR/trust.json (the executable's CDHash for the owner trust cache, with the bundle facts the checks
below established). Default DIR: build/pagani-audio-hal of the UniXNU-Pagani-Drivers checkout (ignored by Git). The iOS SDK has no
CoreAudio/AudioHardware.h; the macOS SDK's CoreAudio plug-in headers come first on the include path for the build only (the interface is
the same; UniXNU-sheng scripts/build-sheng-audio-hal.py does the same). Checks: an arm64 MH_BUNDLE for iOS 17.0 that exports the
factory function Info.plist names, a verified ad hoc signature with 4 KiB pages, and the identifier org.unixnu.pagani.audio."""
import argparse
import hashlib
import json
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
DEFAULT_OUT = SOURCE.parents[1] / 'build/pagani-audio-hal'
IDENTIFIER = 'org.unixnu.pagani.audio'
PLUGIN_TYPE = '443ABAB8-E7B3-491A-B985-BEB9187030DB'   # kAudioServerPlugInTypeUUID
HEADERS = ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h', 'AudioHardwareDeprecated.h')


def fail(message):
    raise SystemExit('build-pagani-audio-hal: ' + message)


def sdk(name):
    return subprocess.run(['xcrun', '--sdk', name, '--show-sdk-path'], check=True, capture_output=True, text=True).stdout.strip()


def macho_facts(path):
    """(filetype, cputype, (platform, minos)) of a thin 64-bit Mach-O."""
    data = path.read_bytes()
    magic, cpu, _, filetype, count, _ = struct.unpack_from('<6I', data)
    if magic != 0xfeedfacf:
        fail(f'{path}: not a thin 64-bit Mach-O')
    offset, version = 32, None
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            version = struct.unpack_from('<II', data, offset + 8)
        offset += size
    return filetype, cpu, version


def build(out):
    ios, mac = sdk('iphoneos'), sdk('macosx')
    info = plistlib.loads((SOURCE / 'hal/Info.plist').read_bytes())
    factories = info.get('CFPlugInFactories', {})
    if info.get('CFBundleIdentifier') != IDENTIFIER or info.get('CFBundleExecutable') != 'PaganiAudio' or len(factories) != 1:
        fail('hal/Info.plist does not describe the PaganiAudio bundle')
    factory, function = next(iter(factories.items()))
    if info.get('CFPlugInTypes') != {PLUGIN_TYPE: [factory]}:
        fail('hal/Info.plist: the factory must be registered for the audio server plug-in type')
    bundle = out / 'PaganiAudio.driver'
    shutil.rmtree(bundle, ignore_errors=True)
    bundle.mkdir(parents=True)
    executable = bundle / 'PaganiAudio'
    with tempfile.TemporaryDirectory(prefix='pagani-audio-hal-') as tmp:
        headers = Path(tmp) / 'CoreAudio'
        headers.mkdir()
        for name in HEADERS:
            header = Path(mac) / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name
            if header.is_file():
                (headers / name).symlink_to(header)
        result = subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-I', tmp, '-I', str(SOURCE),
                                 '-O2', '-g0', '-Wall', '-Wextra', '-Werror', '-bundle', '-fvisibility=hidden',
                                 '-framework', 'CoreFoundation', '-framework', 'IOKit', str(SOURCE / 'hal/PaganiAudioPlugIn.c'),
                                 '-o', str(executable)], capture_output=True, text=True)
        if result.returncode or 'warning:' in result.stderr:
            fail('compile:\n' + (result.stdout + result.stderr)[-4000:])
    filetype, cpu, version = macho_facts(executable)
    if (filetype, cpu, version) != (8, 0x100000c, (2, 0x110000)):
        fail(f'{executable}: filetype {filetype} cpu {cpu:#x} build version {version}, not an arm64 iOS 17.0 bundle')
    exported = subprocess.run(['xcrun', 'nm', '-gU', str(executable)], check=True, capture_output=True, text=True).stdout.split()
    if '_' + function not in exported:
        fail(f'{executable} does not export {function}, the factory Info.plist names')
    shutil.copy(SOURCE / 'hal/Info.plist', bundle / 'Info.plist')
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', IDENTIFIER, str(bundle)], check=True,
                   capture_output=True)
    subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(bundle)], capture_output=True, text=True, check=True).stderr
    cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    page = re.search(r'^Page size=(\d+)$', detail, re.M)
    ident = re.search(r'^Identifier=(\S+)$', detail, re.M)
    if not cdhash or not page or page.group(1) != '4096' or not ident or ident.group(1) != IDENTIFIER:
        fail('unexpected signature:\n' + detail)
    trust = {'cdhash': cdhash.group(1), 'identifier': IDENTIFIER, 'page_size': 4096, 'factory': factory, 'factory_function': function,
             'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
             'source_sha256': hashlib.sha256((SOURCE / 'hal/PaganiAudioPlugIn.c').read_bytes()).hexdigest(),
             'abi_sha256': hashlib.sha256((SOURCE / 'PaganiAudioAbi.h').read_bytes()).hexdigest()}
    (out / 'trust.json').write_text(json.dumps(trust, indent=2) + '\n')
    return bundle, trust


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    bundle, trust = build(out)
    print(f'PASS: {bundle} CDHash {trust["cdhash"]} (arm64 MH_BUNDLE, iOS 17.0, 4 KiB pages, {trust["factory_function"]} exported)')


if __name__ == '__main__':
    sys.exit(main())
