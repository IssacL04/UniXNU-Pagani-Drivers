#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Put the PaganiDiag daemon into a copy of an existing phone Data seed and its CDHash into a copy of the seed's owner trust bundle.

  replace-diag-in-seed.py --build DIR --trust BUNDLE [--kernel MACHO] [--no-budget] [--scratch DIR] SEED.dbrdisk OUTPUT.dbrdisk

The seeds of 2026-10-31 were made from base seeds that are gone (make-a830-layout.sh A830_PREBUILT_SEED), so make-phone-seed.py
--diag-daemon cannot be rerun; this does what 工具/replace-phone-wallpaper.py does for the wallpaper, and changes nothing else: same
volume UUID, same plug-ins, apps, preferences, sentinels. DIR is build-paganidiag.py's output (DIR/paganidiag must carry the CDHash of
DIR/trust.json); DIR/paganidiag.conf comes along when it is there.

In the copy of SEED: /private/var/factory_mount/paganidiag (0755) and paganidiag.conf (0644) are written, replacing what is there, and
the daemon's earlier place is cleared (/private/var/ngpu/bin/paganidiag and .conf, /private/var/ngpu/daemons/org.unixnu.pagani.diag.plist:
xpcproxy may not run a program from there, README.md "How it starts"). The seed is refused when its launch policy disables
com.apple.DumpPanic, in whose place the kernel starts the daemon. The files are read back.

OUTPUT-owner-trust.bundle is BUNDLE with the daemon's CDHash added and the CDHash of any daemon this replaced taken out; every other
owner entry and the Apple modules stay (make-diag-trust.py merge), so the Metal and PaganiAudio plug-ins stay trusted. Give it to the
image with A830_OWNER_TRUST.

Budget: as replace-phone-wallpaper.py, the seed, kernel and trust bundle must fit 95 % of the boot payload (--no-budget skips it).

Writes OUTPUT, OUTPUT-owner-trust.bundle and OUTPUT with .json, which carries plugin_cdhash and plugin of the seed's own .json and
diag_daemon (files, cdhash, host_job) for make-a830-layout.sh. Refuses to overwrite. Runs on macOS (hdiutil). Never touches the phone."""
import argparse
import hashlib
import importlib.util
import json
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
TOP = Path('/Users/xiaochoumao/unixnu-13t')
OLD_FILES = ('ngpu/bin/paganidiag', 'ngpu/bin/paganidiag.conf', 'ngpu/daemons/org.unixnu.pagani.diag.plist')
DAEMON = 'factory_mount/paganidiag'
CONFIG = 'factory_mount/paganidiag.conf'
LAUNCH_POLICY = 'db/com.apple.xpc.launchd/disabled.plist'
HOST_JOB = 'com.apple.DumpPanic'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def cdhash_of(path):
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(path)], capture_output=True, text=True).stderr
    found = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    return found.group(1) if found else None


def stage(mount, build, cdhash):
    """The seed's copy, mounted at `mount`: the old place cleared, the daemon and its configuration in. Returns the report."""
    if not (mount / 'mobile').is_dir() or not (mount / 'ngpu').is_dir():
        sys.exit('The seed has no /mobile or /ngpu; not a phone Data seed?')
    policy = mount / LAUNCH_POLICY
    if policy.is_file() and plistlib.loads(policy.read_bytes()).get(HOST_JOB) is True:
        sys.exit(f'The seed disables {HOST_JOB}, in whose place the kernel starts the daemon')
    old = sorted({cdhash_of(mount / name) for name in (DAEMON, OLD_FILES[0]) if (mount / name).is_file()} - {None})
    removed = []
    for name in OLD_FILES:
        if (mount / name).is_file():
            (mount / name).unlink()
            removed.append('/private/var/' + name)
    directory = (mount / DAEMON).parent
    directory.mkdir(exist_ok=True)
    directory.chmod(0o755)
    (mount / CONFIG).unlink(missing_ok=True)
    files = {}
    for source, relative, mode in ((build / 'paganidiag', DAEMON, 0o755), (build / 'paganidiag.conf', CONFIG, 0o644)):
        if source.is_file():
            target = mount / relative
            shutil.copyfile(source, target)
            target.chmod(mode)
            if target.read_bytes() != source.read_bytes():
                sys.exit(f'/private/var/{relative} reads back different')
            files['/private/var/' + relative] = f'{mode:o}'
    if cdhash_of(mount / DAEMON) != cdhash:
        sys.exit(f'/private/var/{DAEMON} does not carry {cdhash}')
    return dict(files=files, cdhash=cdhash, host_job=HOST_JOB, removed=removed, replaced_cdhashes=old)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--trust', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, default=TOP / 'UniXNU/out-gpu/oneplus13t/ipados-17.7.11-21H461/oneplus13t/kernel/kernel.macho')
    parser.add_argument('--no-budget', action='store_true')
    parser.add_argument('--scratch', type=Path, help='directory for the 2 GiB sparse working copy (default: beside OUTPUT)')
    parser.add_argument('seed', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    wallpaper = load('replace_phone_wallpaper', TOP / '工具/replace-phone-wallpaper.py')
    trust_tool = load('make_diag_trust', HERE / 'make-diag-trust.py')
    packed_ramdisk = wallpaper.packed_ramdisk
    trust_out = args.output.with_name(args.output.stem + '-owner-trust.bundle')
    for path in (args.output, trust_out, args.output.with_suffix('.json')):
        if path.exists():
            sys.exit(f'Refuse to overwrite {path}')
    if not args.trust.is_file():
        sys.exit(f'No trust bundle {args.trust}')
    if not args.no_budget and not args.kernel.is_file():
        sys.exit(f'No kernel {args.kernel} for the payload budget (--kernel, or --no-budget)')
    cdhash = trust_tool.daemon_cdhash(args.build)
    report = dict(seed=str(args.seed), seed_sha256=packed_ramdisk.inspect(args.seed)[0]['sha256'], build=str(args.build))
    lineage = args.seed.with_suffix('.json')
    if lineage.is_file():
        report['seed_report'] = json.loads(lineage.read_text())
        # what make-a830-layout.sh and the plug-in tools read from a seed's .json stays at the top level
        for key in ('plugin_cdhash', 'plugin'):
            if key in report['seed_report']:
                report[key] = report['seed_report'][key]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='diag-seed-', dir=str(args.scratch or args.output.parent)) as scratch:
        raw, mount = Path(scratch) / 'seed.raw', Path(scratch) / 'mount'
        report['container_bytes'] = wallpaper.unpack(args.seed, raw)
        mount.mkdir()
        device = wallpaper.attach(raw, mount, readonly=False)
        try:
            report['diag_daemon'] = stage(mount, args.build, cdhash)
            usage = subprocess.run(['df', '-k', str(mount)], capture_output=True, text=True).stdout.splitlines()[-1].split()
            report['used_kib'], report['available_kib'] = int(usage[2]), int(usage[3])
        finally:
            wallpaper.detach(device)
        if raw.open('rb').read(4096)[32:36] != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        report['output'] = packed_ramdisk.pack(raw, args.output)
    bundle, trust = trust_tool.merge(args.trust.read_bytes(), cdhash, report['diag_daemon']['replaced_cdhashes'])
    if not args.no_budget:
        report['payload'] = wallpaper.payload_budget(args.output.stat().st_size, args.kernel, args.trust)
        report['payload']['headroom'] -= len(bundle) - args.trust.stat().st_size
        if report['payload']['headroom'] < 0:
            args.output.unlink()
            sys.exit(f'The seed no longer fits the boot payload: {report["payload"]}')
    trust_out.write_bytes(bundle)
    report['owner_trust'] = dict(source=str(args.trust), copy=str(trust_out), **trust)
    if hashlib.sha256(trust_out.read_bytes()).hexdigest() != trust['sha256'] or bytes.fromhex(cdhash) not in trust_out.read_bytes():
        sys.exit(f'{trust_out} reads back different')
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    shown = {key: value for key, value in report.items() if key != 'seed_report'}
    shown['owner_trust'] = {key: value for key, value in trust.items() if key not in ('base_hashes', 'owner_hashes')}
    print(json.dumps(shown, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
