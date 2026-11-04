#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""A copy of an owner trust bundle whose owner module also trusts the PaganiDiag daemon.

  make-diag-trust.py [--drop CDHASH]... BASE.bundle OUTPUT.bundle BUILD_DIR

BUILD_DIR is build-paganidiag.py's output: its trust.json names the CDHash, and BUILD_DIR/paganidiag must carry that CDHash (checked
with codesign). The bundle format is make-owner-trust-bundle.py's: one owner module (version 2), then the original Apple modules, which
are compared byte for byte with the build-63 static/System bundle and never changed; the owner module keeps every entry of BASE, less
the --drop ones (a daemon the seed no longer has), and gains the daemon's (SHA-256 truncated, no flags, no constraint category), hashes
strictly ascending. The result is checked as the boot loader checks it (DarwinBoot.c ValidTrustCaches). Same method as
next2/metal-for-apps make-owner-trust-superset.py. Writes OUTPUT and OUTPUT.json; refuses to overwrite. replace-diag-in-seed.py uses
merge() directly."""
import hashlib
import json
import re
import struct
import subprocess
import sys
import uuid
from pathlib import Path

ORIGINAL = Path('/Users/xiaochoumao/unixnu-13t/作者产物/build-63/original-static-system.bundle')
ORIGINAL_SHA256 = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'


def fail(message):
    raise SystemExit('make-diag-trust: ' + message)


def modules_of(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128 or len(data) < 4 + 4 * count:
        fail('not a trust bundle')
    starts = list(struct.unpack_from('<%dI' % count, data, 4))
    return [data[a:b] for a, b in zip(starts, starts[1:] + [len(data)])]


def bundle_of(modules):
    cursor, starts = 4 + 4 * len(modules), []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    return struct.pack('<%dI' % (1 + len(modules)), len(modules), *starts) + b''.join(modules)


def valid_like_the_loader(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from('<I', data, 4 + 4 * index)[0]
        end = struct.unpack_from('<I', data, 8 + 4 * index)[0] if index + 1 < count else len(data)
        if start < 4 + 4 * count or start & 3 or end > len(data) or end - start < 24 or struct.unpack_from('<I', data, start)[0] != 2:
            return False
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= data[at + 20] <= 4 or (entry and data[at - 24:at - 4] >= data[at:at + 20]):
                return False
    return True


def owner_hashes(data):
    owner = modules_of(data)[0]
    return [owner[24 + 24 * i:44 + 24 * i].hex() for i in range(struct.unpack_from('<I', owner, 20)[0])]


def daemon_cdhash(build):
    trust = json.loads((build / 'trust.json').read_text())
    cdhash = trust.get('cdhash', '')
    if not re.fullmatch(r'[0-9a-f]{40}', cdhash):
        fail(f'{build}/trust.json has no CDHash')
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(build / 'paganidiag')], capture_output=True, text=True).stderr
    actual = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    if not actual or actual.group(1) != cdhash:
        fail(f'{build}/paganidiag does not carry the CDHash of trust.json ({cdhash})')
    return cdhash


def merge(data, cdhash, drop=()):
    """The bundle `data` with `cdhash` in its owner module and the `drop` hashes out of it, and a report."""
    if not valid_like_the_loader(data):
        fail('the base is not a trust bundle the boot loader accepts')
    original = ORIGINAL.read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        fail('the original static/System trust bundle changed')
    apple = modules_of(original)
    modules = modules_of(data)
    if len(modules) != 1 + len(apple) or modules[1:] != apple:
        fail('the base is not one owner module followed by the unchanged Apple modules')
    owner = modules[0]
    entries = {owner[24 + 24 * i:44 + 24 * i]: owner[44 + 24 * i:48 + 24 * i] for i in range(struct.unpack_from('<I', owner, 20)[0])}
    before = sorted(h.hex() for h in entries)
    dropped = sorted(h for h in drop if h != cdhash and entries.pop(bytes.fromhex(h), None) is not None)
    entries.setdefault(bytes.fromhex(cdhash), bytes([2, 0, 0, 0]))     # SHA-256 truncated, no flags, no constraint category
    ordered = sorted(entries)
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.adreno-metal:' + ''.join(h.hex() for h in ordered))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(ordered)) + b''.join(h + entries[h] for h in ordered)
    result = bundle_of([own, *apple])
    if not valid_like_the_loader(result) or modules_of(result)[1:] != apple:
        fail('the new trust bundle would be refused by the boot loader')
    if set(owner_hashes(result)) != (set(before) - set(dropped)) | {cdhash}:
        fail('the owner module lost or gained an unexpected entry')
    report = dict(base_sha256=hashlib.sha256(data).hexdigest(), base_hashes=before, added=[cdhash], dropped=dropped,
                  owner_hashes=[h.hex() for h in ordered], apple_modules_unchanged=True, sha256=hashlib.sha256(result).hexdigest(),
                  bytes=len(result))
    return result, report


def main():
    arguments = sys.argv[1:]
    drop = []
    while len(arguments) >= 2 and arguments[0] == '--drop':
        drop.append(arguments[1])
        arguments = arguments[2:]
    if len(arguments) != 3 or not all(re.fullmatch(r'[0-9a-f]{40}', h) for h in drop):
        sys.exit(__doc__)
    base, output, build = Path(arguments[0]), Path(arguments[1]), Path(arguments[2])
    if output.exists():
        fail(f'refuse to overwrite {output}')
    cdhash = daemon_cdhash(build)
    result, report = merge(base.read_bytes(), cdhash, drop)
    output.write_bytes(result)
    report = dict(base=str(base), daemon=str(build / 'paganidiag'), **report)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {output} ({len(result)} bytes, sha256 {report["sha256"][:16]}): {len(report["base_hashes"])} owner entries'
          f'{f", less {len(report["dropped"])}" if report["dropped"] else ""} + paganidiag {cdhash}')


if __name__ == '__main__':
    main()
