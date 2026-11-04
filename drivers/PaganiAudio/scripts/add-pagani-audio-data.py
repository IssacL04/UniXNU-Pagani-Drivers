#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Add the PaganiAudio audio server plug-in to a copy of a packed Data seed, and its CDHash to a copy of the owner trust bundle.

  add-pagani-audio-data.py [--plugin DIR] [--trust BUNDLE] [--trust-out BUNDLE] [--original-trust BUNDLE]
                           [--replace] [--drop-cdhash HEX ...] BASE.dbrdisk OUTPUT.dbrdisk

The plug-in (DIR/PaganiAudio.driver with DIR/trust.json from build-pagani-audio-hal.py; without --plugin it is built now into
OUTPUT's directory as <OUTPUT stem>.hal) goes to the Data volume's ngpu/audio, that is /private/var/ngpu/audio/PaganiAudio.driver,
which the kernel shows over /System/Library/Audio/Plug-Ins/HAL with pagani-audio-addon-mount.patch and boot-arg pagani-audio=1. The
audio server (audiomxd, a platform process) only maps the plug-in's code when its CDHash is trusted: the trust bundle (default the A830
user space's owner-trust.bundle: one owner module, then both original Apple modules) is copied to --trust-out (default
<OUTPUT stem>-owner-trust.bundle) with the plug-in's CDHash added to its owner module, kept in ascending order and checked as DarwinBoot's
ValidTrustCaches checks it. The original Apple modules are compared byte for byte with --original-trust (default the build-63 input, its
sha256 checked) and never changed. BASE and the base bundle are not modified; the volume UUID stays the same. Writes OUTPUT, the trust
bundle and OUTPUT with .json (what was done). Runs on macOS (hdiutil, codesign). The image then takes
  --data-ramdisk OUTPUT --owner-trust-cache <trust bundle> --kernel-arg pagani-audio=1

A rebuilt plug-in replaces the one a seed already carries (the base seeds of the current phone seeds were lost on 2026-10-31): pass that
seed as BASE, its trust bundle as --trust and --replace. The old PaganiAudio.driver in BASE is replaced, and its CDHash, read from the
bundle inside BASE itself (codesign), leaves the owner module, so the trust bundle keeps no stale plug-in hash; --drop-cdhash removes
others (a hash that is not there is refused, so a typo does not pass silently). Every other owner hash and the Apple modules stay.
BASE.json is not trusted for it: a seed made by another step after this script (the Find My seed of 2026-10-31, whose report's
plugin.cdhash is the Metal plug-in's) would otherwise lose that plug-in's trust. When BASE.json is this script's report of the audio
plug-in, its CDHash must be the one found in the seed."""
import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

sys.dont_write_bytecode = True
TOP = Path('/Users/xiaochoumao/unixnu-13t')
sys.path.insert(0, str(TOP / 'UniXNU/drivers/DarwinBoot/Test'))
import packed_ramdisk  # noqa: E402

SCRIPTS = Path(__file__).resolve().parent
DEFAULT_TRUST = TOP / 'build-userspace/tree/build/adreno-metal-plugin/owner-trust.bundle'
DEFAULT_ORIGINAL = TOP / '作者产物/build-63/original-static-system.bundle'
ORIGINAL_SHA256 = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'
AUDIO = 'ngpu/audio'
BUNDLE = 'PaganiAudio.driver'


def fail(message):
    raise SystemExit('add-pagani-audio-data: ' + message)


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True)


def cdhash_of(bundle):
    detail = run('codesign', '--display', '--verbose=4', str(bundle)).stderr
    match = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    if not match:
        fail(f'{bundle}: no CDHash in its signature')
    return match.group(1)


def plugin(directory, output):
    if directory is None:
        directory = output.with_name(output.stem + '.hal')
        if directory.exists():
            fail(f'Refuse to overwrite {directory} (pass --plugin to use it)')
        subprocess.run([sys.executable, '-B', str(SCRIPTS / 'build-pagani-audio-hal.py'), '--out', str(directory)], check=True)
    bundle, trust_file = directory / BUNDLE, directory / 'trust.json'
    if not (bundle / 'PaganiAudio').is_file() or not trust_file.is_file():
        fail(f'{directory} has no {BUNDLE} and trust.json (scripts/build-pagani-audio-hal.py --out {directory})')
    trust = json.loads(trust_file.read_text())
    run('codesign', '--verify', '--strict', str(bundle))
    if cdhash_of(bundle) != trust.get('cdhash'):
        fail(f'{trust_file} does not hold the CDHash of {bundle}; rebuild the plug-in')
    return bundle, trust


# ---- trust bundles (make-owner-trust-bundle.py format; DarwinBoot.c ValidTrustCaches) ------------------------------------------------
def modules_of(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128 or len(data) < 4 + 4 * count:
        fail('not a trust bundle')
    starts = list(struct.unpack_from(f'<{count}I', data, 4))
    ends = starts[1:] + [len(data)]
    return [data[start:end] for start, end in zip(starts, ends)]


def bundle_of(modules):
    cursor, starts = 4 + 4 * len(modules), []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    return struct.pack(f'<{1 + len(modules)}I', len(modules), *starts) + b''.join(modules)


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


def entries_of(module):
    count = struct.unpack_from('<I', module, 20)[0]
    return [module[24 + 24 * i:48 + 24 * i] for i in range(count)]


def add_trust(base, original, cdhash, drop=()):
    data = base.read_bytes()
    if not valid_like_the_loader(data):
        fail(f'{base} is not a trust bundle the boot loader accepts')
    modules = modules_of(data)
    apple = []
    if original:
        blob = original.read_bytes()
        if original == DEFAULT_ORIGINAL and hashlib.sha256(blob).hexdigest() != ORIGINAL_SHA256:
            fail(f'{original} is not the unchanged original static/System trust bundle')
        apple = modules_of(blob)
    # The owner module is the first one that is not an original Apple module (make-owner-trust-bundle.py puts it first); a bundle of
    # Apple modules only gets a new owner module in front.
    owner = next((i for i, module in enumerate(modules) if module not in apple), None)
    if apple and [m for m in modules if m in apple] != apple:
        fail(f'{base} does not carry the original Apple modules unchanged')
    entries = entries_of(modules[owner]) if owner is not None else []
    hashes = {entry[:20]: entry[20:] for entry in entries}
    added = bytes.fromhex(cdhash)
    dropped = []
    for old in sorted({bytes.fromhex(h) for h in drop} - {added}):
        if old not in hashes:
            fail(f'{base} has no owner CDHash {old.hex()} to drop')
        del hashes[old]
        dropped.append(old.hex())
    already = added in hashes
    hashes.setdefault(added, bytes([2, 0, 0, 0]))   # hash type 2 (SHA-256, truncated), no flags, no constraint category
    ordered = sorted(hashes)
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.unixnu.pagani.audio:' + ''.join(h.hex() for h in ordered))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(ordered)) + b''.join(h + hashes[h] for h in ordered)
    if owner is None:
        modules = [own, *modules]
    else:
        modules[owner] = own
    result = bundle_of(modules)
    if not valid_like_the_loader(result):
        fail('the new trust bundle would be refused by the boot loader')
    return result, dict(base=str(base), base_sha256=hashlib.sha256(data).hexdigest(), already_present=already,
                        dropped=dropped, owner_hashes=[h.hex() for h in ordered], apple_modules_unchanged=bool(apple),
                        sha256=hashlib.sha256(result).hexdigest())


# ---- the seed (make-phone-seed.py's mount and copy) ----------------------------------------------------------------------------------
def stage(mount, bundle):
    if not (mount / 'mobile').is_dir():
        fail('The Data seed has no /mobile; wrong base image?')
    target = mount / AUDIO / BUNDLE
    replaced = target.exists()
    if replaced:
        shutil.rmtree(target)
    (mount / AUDIO).mkdir(parents=True, exist_ok=True)
    for directory in (mount / 'ngpu', mount / AUDIO):
        os.chmod(directory, 0o755)
    shutil.copytree(bundle, target, symlinks=False)
    for path in [target, *target.rglob('*')]:
        os.chmod(path, 0o755 if path.is_dir() or path.name == 'PaganiAudio' else 0o644)
    run('codesign', '--verify', '--strict', str(target))
    return sorted(str(p.relative_to(mount / AUDIO)) for p in target.rglob('*') if p.is_file()), replaced


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plugin', type=Path)
    parser.add_argument('--trust', type=Path, default=DEFAULT_TRUST)
    parser.add_argument('--trust-out', type=Path)
    parser.add_argument('--original-trust', type=Path, default=DEFAULT_ORIGINAL)
    parser.add_argument('--replace', action='store_true', help="drop the CDHash of the PaganiAudio.driver BASE carries (read from it)")
    parser.add_argument('--drop-cdhash', action='append', default=[], metavar='HEX')
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    base, output = args.base.resolve(), args.output.resolve()
    trust_out = (args.trust_out or output.with_name(output.stem + '-owner-trust.bundle')).resolve()
    for path in (output, output.with_suffix('.json'), trust_out):
        if path.exists():
            fail('Refuse to overwrite ' + str(path))
    if not args.trust.is_file():
        fail(f'no trust bundle {args.trust} (build-userspace-a830.sh step 6 makes it)')
    original = args.original_trust if args.original_trust and args.original_trust.is_file() else None
    drop = []
    for value in args.drop_cdhash:
        if not re.fullmatch(r'[0-9a-f]{40}', value):
            fail(f'--drop-cdhash {value}: not a 40-digit lower-case hex CDHash')
        drop.append(value)
    output.parent.mkdir(parents=True, exist_ok=True)
    bundle, plugin_trust = plugin(args.plugin.resolve() if args.plugin else None, output)
    blob = base.read_bytes()
    logical, extents = packed_ramdisk.regions(blob)
    report = dict(base=str(base), base_bytes=logical, base_sha256=packed_ramdisk.inspect(base)[0]['sha256'],
                  plugin=dict(path='/private/var/' + AUDIO + '/' + BUNDLE, **plugin_trust))
    with tempfile.TemporaryDirectory(prefix='audio-seed-', dir=str(output.parent)) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        mount = Path(scratch) / 'mount'
        mount.mkdir()
        attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off',
             '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            if args.replace:
                # The plug-in BASE carries: its CDHash from its own signature (not from BASE.json, see above).
                present = mount / AUDIO / BUNDLE
                if not present.is_dir():
                    fail(f'--replace: {base} carries no {AUDIO}/{BUNDLE} to replace')
                replaced = cdhash_of(present)
                recorded = None
                try:
                    record = json.loads(base.with_suffix('.json').read_text()).get('plugin', {})
                    if isinstance(record, dict) and record.get('path') == report['plugin']['path']:
                        recorded = record.get('cdhash')
                except (OSError, ValueError):
                    pass
                if recorded and recorded != replaced:
                    fail(f'--replace: {base.with_suffix(".json")} records the audio plug-in {recorded}, the seed carries {replaced}')
                drop.append(replaced)
                report['replaced_plugin_cdhash'] = replaced
                report['replaced_plugin_recorded'] = bool(recorded)
            report['files'], report['replaced_in_seed'] = stage(mount, bundle)
            usage = subprocess.run(['df', '-k', str(mount)], capture_output=True, text=True).stdout.splitlines()[-1].split()
            report['used_kib'], report['available_kib'] = int(usage[2]), int(usage[3])
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True, capture_output=True)
        if raw.open('rb').read(4096)[32:36] != b'NXSB':
            fail('The edited image is no APFS container any more')
        # The trust before OUTPUT: a refusal (a hash to drop that the bundle lacks) leaves nothing behind.
        trust_bytes, trust_report = add_trust(args.trust.resolve(), original, plugin_trust['cdhash'], drop)
        report['output'] = packed_ramdisk.pack(raw, output)
    trust_out.write_bytes(trust_bytes)
    report['trust'] = dict(output=str(trust_out), **trust_report)
    report['image_arguments'] = ['--data-ramdisk', str(output), '--owner-trust-cache', str(trust_out), '--kernel-arg', 'pagani-audio=1']
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
