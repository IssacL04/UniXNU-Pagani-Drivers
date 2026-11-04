#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Add the iOS flashlight plug-in to a copy of a packed Data seed, and its CDHash to a copy of the owner trust bundle.

  add-pagani-torch-data.py [--plugin DIR] --trust BUNDLE [--trust-out BUNDLE] [--original-trust BUNDLE] [--replace] BASE.dbrdisk OUTPUT.dbrdisk

The plug-in (DIR/H6ISP.mediacapture and DIR/trust.json from build-pagani-torch-plugin.py; without --plugin it is built now next to OUTPUT) goes
to the Data volume's ngpu/mediacapture, that is /private/var/ngpu/mediacapture/H6ISP.mediacapture, which the kernel shows over
/System/Library/MediaCapture with phone-torch-overlay.patch and the boot arguments pagani-torch=1 pagani-torch-ios=1. mediaserverd, a
platform process, only maps the plug-in's code when its CDHash is trusted. The trust bundle handling is PaganiAudio's add-pagani-audio-data.py
(one owner module, the original Apple modules compared byte for byte and never changed), imported from it. BASE and the base bundle are
not modified. --replace drops the CDHash of the plug-in BASE already carries. Writes OUTPUT, the trust bundle (default <OUTPUT
stem>-owner-trust.bundle) and OUTPUT with .json. Runs on macOS (hdiutil, codesign). The image then takes
  --data-ramdisk OUTPUT --owner-trust-cache <trust bundle> --kernel-arg pagani-torch=1 --kernel-arg pagani-torch-ios=1"""
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
spec = importlib.util.spec_from_file_location('add_pagani_audio_data', SOURCE.parent / 'PaganiAudio/scripts/add-pagani-audio-data.py')
audio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio)
packed_ramdisk = audio.packed_ramdisk
SEED_DIR = 'ngpu/mediacapture'
NAME = 'H6ISP.mediacapture'


def fail(message):
    raise SystemExit('add-pagani-torch-data: ' + message)


def plugin(directory, output):
    if directory is None:
        directory = output.with_suffix('.plugin')
        subprocess.run([sys.executable, '-B', str(SOURCE / 'scripts/build-pagani-torch-plugin.py'), '--out', str(directory)], check=True)
    file, trust_file = directory / NAME, directory / 'trust.json'
    if not file.is_file() or not trust_file.is_file():
        fail(f'{directory} has no {NAME} and trust.json (scripts/build-pagani-torch-plugin.py --out {directory})')
    trust = json.loads(trust_file.read_text())
    audio.run('codesign', '--verify', '--strict', str(file))
    if audio.cdhash_of(file) != trust.get('cdhash'):
        fail(f'{trust_file} does not hold the CDHash of {file}; rebuild the plug-in')
    return file, trust


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--plugin', type=Path)
    parser.add_argument('--trust', type=Path, required=True)
    parser.add_argument('--trust-out', type=Path)
    parser.add_argument('--original-trust', type=Path, default=audio.DEFAULT_ORIGINAL)
    parser.add_argument('--replace', action='store_true', help="drop the CDHash of the plug-in BASE carries (read from it)")
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    base, output = args.base.resolve(), args.output.resolve()
    trust_out = (args.trust_out or output.with_name(output.stem + '-owner-trust.bundle')).resolve()
    for path in (output, output.with_suffix('.json'), trust_out):
        if path.exists():
            fail('Refuse to overwrite ' + str(path))
    if not args.trust.is_file():
        fail(f'no trust bundle {args.trust}')
    original = args.original_trust if args.original_trust and args.original_trust.is_file() else None
    output.parent.mkdir(parents=True, exist_ok=True)
    file, plugin_trust = plugin(args.plugin.resolve() if args.plugin else None, output)
    logical, extents = packed_ramdisk.regions(base.read_bytes())
    report = dict(base=str(base), base_bytes=logical, base_sha256=packed_ramdisk.inspect(base)[0]['sha256'],
                  plugin=dict(path='/private/var/' + SEED_DIR + '/' + NAME, **plugin_trust))
    drop = []
    with tempfile.TemporaryDirectory(prefix='torch-seed-', dir=str(output.parent)) as scratch:
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
            if not (mount / 'mobile').is_dir():
                fail('The Data seed has no /mobile; wrong base image?')
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
            usage = subprocess.run(['df', '-k', str(mount)], capture_output=True, text=True).stdout.splitlines()[-1].split()
            report['used_kib'], report['available_kib'] = int(usage[2]), int(usage[3])
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True, capture_output=True)
        if raw.open('rb').read(4096)[32:36] != b'NXSB':
            fail('The edited image is no APFS container any more')
        trust_bytes, trust_report = audio.add_trust(args.trust.resolve(), original, plugin_trust['cdhash'], drop)
        report['output'] = packed_ramdisk.pack(raw, output)
    trust_out.write_bytes(trust_bytes)
    report['trust'] = dict(output=str(trust_out), **trust_report)
    report['image_arguments'] = ['--data-ramdisk', str(output), '--owner-trust-cache', str(trust_out), '--kernel-arg', 'pagani-torch=1',
                                 '--kernel-arg', 'pagani-torch-ios=1']
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {output} with {NAME} (CDHash {plugin_trust["cdhash"]}), trust {trust_out}')


if __name__ == '__main__':
    main()
