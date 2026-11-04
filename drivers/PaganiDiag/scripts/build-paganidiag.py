#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the PaganiDiag daemon (PaganiDiag.mm, DiagCore.cpp) for iOS 17 arm64, signed ad hoc with 4 KiB pages and its entitlements.

  build-paganidiag.py [--out DIR]

Writes DIR/paganidiag and DIR/trust.json (the CDHash for the owner trust cache with the facts checked below). Default DIR:
build/pagani-diag of the UniXNU-Pagani-Drivers checkout (ignored by Git). Built like adrenoprobed (build-userspace
scripts/build-adreno-metal-plugin.py): clang for arm64-apple-ios17.0, -Wall -Wextra -Werror, stripped, codesign --pagesize 4096. Checks:
a thin arm64 (subtype 0) MH_EXECUTE for iOS 17.0, a verified ad hoc signature with 4 KiB pages and the identifier
org.unixnu.pagani.diag, the signed entitlements equal to paganidiag.entitlements."""
import argparse
import hashlib
import json
import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
DEFAULT_OUT = SOURCE.parents[1] / 'build/pagani-diag'
IDENTIFIER = 'org.unixnu.pagani.diag'
DAEMON_PATH = '/private/var/factory_mount/paganidiag'    # PAGANI_DIAG_DAEMON of kernel/pagani_diag.h
ENTITLEMENTS = {
    'com.apple.private.coreservices.canmaplsdatabase': True,
    'com.apple.private.logging.admin': True, 'com.apple.private.logging.diagnostic': True, 'com.apple.private.logging.stream': True,
    'com.apple.private.mobileinstall.allowedSPI': ['LookupSystemAppState', 'GetSystemAppMigrationStatus'],
    'com.apple.private.security.no-container': True, 'keychain-access-groups': ['org.unixnu.pagani.diag'],
}


def fail(message):
    raise SystemExit('build-paganidiag: ' + message)


def macho_facts(path):
    """(filetype, cputype, cpusubtype, (platform, minos)) of a thin 64-bit Mach-O."""
    data = path.read_bytes()
    magic, cpu, subtype, filetype, count, _ = struct.unpack_from('<6I', data)
    if magic != 0xfeedfacf:
        fail(f'{path}: not a thin 64-bit Mach-O')
    offset, version = 32, None
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:                     # LC_BUILD_VERSION
            version = struct.unpack_from('<II', data, offset + 8)
        offset += size
    return filetype, cpu, subtype & 0xffffff, version


def signature(path):
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(path)], capture_output=True, text=True, check=True).stderr
    facts = {key: re.search(rf'^{key}=(\S+)$', detail, re.M) for key in ('CDHash', 'Page size', 'Identifier', 'Signature')}
    return {key: match.group(1) if match else None for key, match in facts.items()}, detail


def signed_entitlements(path):
    data = subprocess.run(['codesign', '-d', '--entitlements', ':-', str(path)], capture_output=True, check=True).stdout
    return plistlib.loads(data) if data else {}


def build(out):
    if plistlib.loads((SOURCE / 'paganidiag.entitlements').read_bytes()) != ENTITLEMENTS:
        fail('paganidiag.entitlements is not the intended set')
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True, capture_output=True, text=True).stdout.strip()
    out.mkdir(parents=True, exist_ok=True)
    daemon = out / 'paganidiag'
    daemon.unlink(missing_ok=True)
    result = subprocess.run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-std=c++17', '-O2', '-g0', '-Wall',
                             '-Wextra', '-Werror', '-fstack-protector-strong', '-fobjc-arc', '-I', str(SOURCE),
                             str(SOURCE / 'DiagCore.cpp'), '-x', 'objective-c++', str(SOURCE / 'PaganiDiag.mm'),
                             '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'OSLog', '-o', str(daemon)],
                            capture_output=True, text=True)
    if result.returncode or 'warning:' in result.stderr:
        fail('compile:\n' + (result.stdout + result.stderr)[-6000:])
    subprocess.run(['strip', '-x', str(daemon)], check=True)
    filetype, cpu, subtype, version = macho_facts(daemon)
    if (filetype, cpu, subtype, version) != (2, 0x100000c, 0, (2, 0x110000)):
        fail(f'{daemon}: filetype {filetype} cpu {cpu:#x}/{subtype} build version {version}, not an arm64 iOS 17.0 executable')
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', IDENTIFIER, '--entitlements',
                    str(SOURCE / 'paganidiag.entitlements'), str(daemon)], check=True, capture_output=True)
    subprocess.run(['codesign', '--verify', '--strict', str(daemon)], check=True)
    facts, detail = signature(daemon)
    if facts['Page size'] != '4096' or facts['Identifier'] != IDENTIFIER or facts['Signature'] != 'adhoc' or not facts['CDHash'] \
            or not re.fullmatch(r'[0-9a-f]{40}', facts['CDHash']):
        fail('unexpected signature:\n' + detail)
    if signed_entitlements(daemon) != ENTITLEMENTS:
        fail('the signed entitlement set differs from paganidiag.entitlements')
    os_mode = daemon.stat().st_mode & 0o777
    if os_mode & 0o111 != 0o111:
        daemon.chmod(0o755)
    if f'"{DAEMON_PATH}"' not in (SOURCE / 'kernel/pagani_diag.h').read_text():
        fail(f'{DAEMON_PATH} is not the kernel\'s PAGANI_DIAG_DAEMON')
    sources = {name: hashlib.sha256((SOURCE / name).read_bytes()).hexdigest()
               for name in ('DiagCore.h', 'DiagCore.cpp', 'PaganiDiag.mm', 'paganidiag.entitlements')}
    trust = {'cdhash': facts['CDHash'], 'identifier': IDENTIFIER, 'page_size': 4096, 'path': DAEMON_PATH,
             'executable_sha256': hashlib.sha256(daemon.read_bytes()).hexdigest(), 'executable_bytes': daemon.stat().st_size,
             'entitlements': ENTITLEMENTS, 'sources_sha256': sources,
             'purpose': 'PaganiDiag diagnostic daemon; trusted only through an extra owner trust module'}
    (out / 'trust.json').write_text(json.dumps(trust, indent=2) + '\n')
    return daemon, trust


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    daemon, trust = build(args.out.resolve())
    print(f'PASS: {daemon} CDHash {trust["cdhash"]} ({trust["executable_bytes"]} bytes, arm64 iOS 17.0, 4 KiB pages, entitlements as intended)')


if __name__ == '__main__':
    sys.exit(main())
