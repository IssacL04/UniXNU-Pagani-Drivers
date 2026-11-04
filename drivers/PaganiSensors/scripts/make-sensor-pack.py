#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Packs what the ADSP's sensor protection domain reads into the one read-only file PaganiSensors serves it from (SensorsPack.h), out of
the morning's Android dump (build-userspace/a830-inputs/sensors/ssc-facts.md appendix B). The pack goes into the RAM Data seed, at
/private/var/ngpu/sensors/pack.bin (make-phone-seed.py --sensor-pack). It holds the phone's own calibration: local only, never in a repository.

  make-sensor-pack.py --dump DIR --out pack.bin [--libs] [--library-path 'a;b'] [--add /device/path=FILE ...] [--list]

From DIR, all optional but at least one must be there:
  vendor-etc-sensors.tar   vendor/etc/sensors/...: sns_reg_config, config/*.json, json.lst
  persist-sensors.tar      mnt/vendor/persist/sensors/...: the registry Android generated, with this phone's calibration
  dsp-libs.tar             vendor/lib/rfsa/adsp, vendor/dsp (only with --libs: they are large, the payload has few MB to spare; the
                           boot log's "[sensors] rpc missing ..." lines say which the sensor PD really asks for: --add those one by one)
  soc0.txt                 name=value lines of /sys/devices/soc0 (appendix B.2), each served as /sys/devices/soc0/NAME
--library-path is Android's ADSP_LIBRARY_PATH (from the sensors daemon's environ, appendix B.2), kept as /.pagani/library_path.

Archives are only read, never extracted; a member with an unsafe name is refused."""
import argparse
import struct
import sys
import tarfile
from pathlib import Path, PurePosixPath

MAX_ENTRIES = 8192
MAX_NAMES = 0x100000
WARN_BYTES = 6 << 20


def fail(message):
    raise SystemExit('make-sensor-pack: ' + message)


def clean(name):
    """A tar member's name as an absolute device path; refuses what could escape."""
    path = PurePosixPath('/' + name.lstrip('./'))
    if '..' in path.parts or any(c < ' ' or c == '\x7f' for c in str(path)):
        fail(f'unsafe member name {name!r}')
    return str(path)


def add_tar(files, tar_path, skipped):
    with tarfile.open(tar_path) as tar:
        members = {m.name.lstrip('./'): m for m in tar.getmembers()}
        for name, member in members.items():
            target = member
            for _ in range(8):                               # a link inside the archive is served as what it points to
                if target is None or not (target.issym() or target.islnk()):
                    break
                base = PurePosixPath('/' + name).parent if target.issym() else PurePosixPath('/')
                link = target.linkname
                resolved = PurePosixPath(link) if link.startswith('/') else base / link
                parts = []
                for part in resolved.parts[1:]:
                    if part == '..':
                        parts and parts.pop()
                    elif part != '.':
                        parts.append(part)
                target = members.get('/'.join(parts))
            if target is None or target.issym() or target.islnk():
                skipped.append(f'{name} -> {member.linkname} (outside the archive)')
                continue
            if target.isfile():
                files[clean(name)] = tar.extractfile(target).read()


def add_soc0(files, path):
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, separator, value = line.partition('=')
        if not separator or '/' in name or not name:
            fail(f'{path}:{number}: not name=value')
        files['/sys/devices/soc0/' + name] = (value + '\n').encode()


def pack(files):
    """The bytes of a pack (SensorsPack.h) of {absolute path: bytes}; every parent directory listed."""
    directories = {'/'}
    for path in files:
        parent = PurePosixPath(path).parent
        while str(parent) != '/':
            directories.add(str(parent))
            parent = parent.parent
    clash = directories & set(files)
    if clash:
        fail('both a file and a directory: ' + ', '.join(sorted(clash)))
    names = sorted(directories | set(files), key=lambda text: text.encode())
    if len(names) > MAX_ENTRIES:
        fail(f'{len(names)} entries, the kernel takes {MAX_ENTRIES}')
    area, offsets = b'', {}
    for name in names:
        encoded = name.encode()
        if len(encoded) >= 256:
            fail(f'path too long: {name}')
        offsets[name] = len(area)
        area += encoded + b'\0'
    if len(area) > MAX_NAMES:
        fail(f'{len(area)} bytes of names, the kernel takes {MAX_NAMES}')
    at = 32 + 24 * len(names) + len(area)
    table, data = b'', []
    for name in names:
        if name in directories:
            table += struct.pack('<QQII', 0, 0, offsets[name], 1)
        else:
            body = files[name]
            table += struct.pack('<QQII', at, len(body), offsets[name], 0)
            data.append(body)
            at += len(body)
    head = table + area
    header = b'SSCPACK1' + struct.pack('<IIQII', len(names), len(area), at, sum(head) & 0xffffffff, 0)
    return header + head + b''.join(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dump', type=Path, required=True, help='the directory of the Android dump')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--libs', action='store_true', help='also dsp-libs.tar')
    parser.add_argument('--library-path', help="Android's ADSP_LIBRARY_PATH")
    parser.add_argument('--add', action='append', default=[], metavar='DEVICE=FILE', help='one more file, by its device path')
    parser.add_argument('--list', action='store_true', help='print what went in')
    args = parser.parse_args()
    files, skipped = {}, []
    wanted = ['vendor-etc-sensors.tar', 'persist-sensors.tar'] + (['dsp-libs.tar'] if args.libs else [])
    found = [name for name in wanted if (args.dump / name).is_file()]
    for name in found:
        add_tar(files, args.dump / name, skipped)
    if (args.dump / 'soc0.txt').is_file():
        add_soc0(files, args.dump / 'soc0.txt')
    for item in args.add:
        device, separator, source = item.partition('=')
        if not separator or not device.startswith('/'):
            fail(f'--add {item}: DEVICE=FILE with an absolute device path')
        files[device] = Path(source).read_bytes()
    if args.library_path:
        files['/.pagani/library_path'] = args.library_path.encode()
    if not files:
        fail(f'nothing to pack in {args.dump}')
    for needed in ('/vendor/etc/sensors/sns_reg_config',):
        if needed not in files:
            print(f'make-sensor-pack: warning: no {needed}: the sensor PD will not find its registry configuration', file=sys.stderr)
    blob = pack(files)
    args.out.write_bytes(blob)
    if args.list:
        for name in sorted(files):
            print(f'{len(files[name]):9} {name}')
    for line in skipped:
        print('make-sensor-pack: skipped ' + line, file=sys.stderr)
    if len(blob) > WARN_BYTES:
        print(f'make-sensor-pack: warning: {len(blob)} bytes: the 192 MiB payload has only a few MB to spare', file=sys.stderr)
    print(f'{args.out}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
