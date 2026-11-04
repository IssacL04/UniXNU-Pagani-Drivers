#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Host tests of PaganiSensors (the OnePlus 13T sensors for iOS), all with -Wall -Wextra -Werror and ASan/UBSan:

  wire     the protobuf encoder/decoder and the sensor client service's QMI/protobuf messages: the 16 vectors of
           build-userspace/a830-inputs/sensors/ssc-facts.md A.4, tolerance, refusals, round trips, a mutation fuzz
  fixed    the integer-only float conversions (a million random floats against the host's arithmetic), iOS units, axis maps
  orient   the device orientation from the accelerometer: states, hold, hysteresis, dead zones, noise, the whole sphere
  core     the sensor manager against a simulated sensor hub: late service, refusals, timeouts, restarts, demand, mode 2, budget,
           the orientation's accelerometer, repeated detection modes
  hid      the report descriptors (walked item by item), the personalities (also parsed by plistlib here), events, demand, the
           orientation service and its plug-in's methods, the MobileGestalt node plan
  rpc      the FastRPC reverse file service against a simulated DSP (its own reading of the public layouts: bring-up, every apps_std
           call, apps_mem, failures, the link, a fuzz of the DSP's blobs) and the file pack, with a pack from scripts/make-sensor-pack.py
  stack    the manager on PaganiAdsp's real IPC router client ports against a DSP-side router and hub
  qrtr     PaganiAdsp's router client ports and router hooks (../PaganiAdsp/Test/qrtr_client.c)
  weak     the weak defaults of PaganiAdsp.h's client API and of PaganiSensors.h
  adsp     PaganiAdsp's own host suite (its existing tests must still pass: --no-adsp-suite skips it)

--kernel TREE --kernelcache KC also runs scripts/build-pagani-sensors.py into a temporary directory (the zero-warning kernel compile with
the tree's records and the ABI qualification against the pinned image). Host tests do not establish that the sensor hub answers.
"""
import argparse
import io
import plistlib
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DRIVER = HERE.parent
ADSP = DRIVER.parent / 'PaganiAdsp'
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
CORE = ['SensorsPb.c', 'SensorsSsc.c', 'SensorsFixed.c', 'SensorsOrient.c', 'SensorsCore.c']
ADSP_PORTABLE = ['QcomSmem.c', 'QcomSmp2p.c', 'QcomGlink.c', 'QcomQrtr.c', 'QcomMdt.c', 'FatVolume.c', 'AdspFirmware.c', 'AdspCore.c',
                 'AdspQrtr.c']


def build(out, name, sources, extra=()):
    exe = out / name
    subprocess.run(['cc', *CFLAGS, *extra, *map(str, sources), '-o', str(exe)], check=True)
    return exe


def check_output(exe, *args):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    text = (result.stdout + result.stderr).strip()
    print(text)
    if result.returncode or not text.splitlines()[-1].startswith('PASS'):
        sys.exit(f'FAIL: {exe.name} {" ".join(map(str, args))}')
    return text


def personalities(path):
    """The personalities as IOKit reads them: an array of dictionaries (here through plistlib, a different XML parser)."""
    document = ('<?xml version="1.0" encoding="UTF-8"?><plist version="1.0">' + path.read_text() + '</plist>').encode()
    array = plistlib.loads(document)
    expected = [('Pagani Accelerometer', 0xff00, 3), ('Pagani Gyroscope', 0xff00, 9), ('Pagani Compass', 0xff00, 0x0a),
                ('Pagani Ambient Light', 0xff00, 4), ('Pagani Proximity', 0xff00, 8), ('Pagani Pressure', 0x20, 0x31)]
    if len(array) != len(expected):
        sys.exit('FAIL: personalities: not one dictionary per kind')
    for d, (product, page, usage) in zip(array, expected):
        ok = (d['IOClass'] == 'IOHIDEventDriver' and d['IOProviderClass'] == 'IOHIDInterface' and
              d['CFBundleIdentifier'] == 'com.apple.iokit.IOHIDFamily' and d['IOPropertyMatch'] == {'Product': product} and
              d['DeviceUsagePairs'] == [{'DeviceUsagePage': page, 'DeviceUsage': usage}] and d['PrimaryUsagePage'] == page and
              d['PrimaryUsage'] == usage and d['Built-In'] is True and d['IOProbeScore'] == 6000 and 'Transport' not in d and
              d.get('Placement') == (1 if product == 'Pagani Proximity' else None))
        if not ok:
            sys.exit(f'FAIL: personality {product}: {d}')
    print(f'PASS personalities through plistlib: {len(array)} dictionaries with their keys and types')


def member(tar, name, data=None, link=None):
    info = tarfile.TarInfo(name)
    if link is not None:
        info.type = tarfile.SYMTYPE
        info.linkname = link
        tar.addfile(info)
    else:
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))


def sample_dump(directory):
    """An Android dump as ssc-facts.md appendix B makes it: two tars (a link inside, a link that leaves), soc0.txt."""
    with tarfile.open(directory / 'vendor-etc-sensors.tar', 'w') as tar:
        member(tar, 'vendor/etc/sensors/sns_reg_config', b'version=1\n')
        member(tar, 'vendor/etc/sensors/config/a.json', b'{}')
        member(tar, 'vendor/etc/sensors/alias.json', link='config/a.json')
        member(tar, 'vendor/etc/sensors/escape', link='/etc/passwd')
    with tarfile.open(directory / 'persist-sensors.tar', 'w') as tar:
        member(tar, 'mnt/vendor/persist/sensors/registry/registry/sensors_registry', b'calib')
    (directory / 'soc0.txt').write_text('soc_id=618\nhw_platform=MTP\n')
    (directory / 'added').write_bytes(b'add')


def pack_tool(directory):
    tool = DRIVER / 'scripts/make-sensor-pack.py'
    sample_dump(directory)
    pack = directory / 'pack.bin'
    done = subprocess.run([sys.executable, str(tool), '--dump', str(directory), '--out', str(pack), '--library-path', '/vendor/dsp/sensors;/x',
                           '--add', f'/vendor/extra/added={directory / "added"}'], capture_output=True, text=True)
    if done.returncode or 'skipped vendor/etc/sensors/escape' not in done.stderr:
        sys.exit(f'FAIL: make-sensor-pack: {done.stdout}{done.stderr}')
    (directory / 'evil').mkdir()
    with tarfile.open(directory / 'evil/vendor-etc-sensors.tar', 'w') as tar:
        member(tar, 'vendor/../../etc/passwd', b'x')
    done = subprocess.run([sys.executable, str(tool), '--dump', str(directory / 'evil'), '--out', str(directory / 'evil.bin')],
                          capture_output=True, text=True)
    if not done.returncode or 'unsafe member name' not in done.stderr or (directory / 'evil.bin').exists():
        sys.exit(f'FAIL: make-sensor-pack took an unsafe name: {done.stdout}{done.stderr}')
    print('PASS make-sensor-pack: links, soc0, the additions, unsafe names refused')
    return pack


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--only', action='append', default=[])
    parser.add_argument('--no-adsp-suite', action='store_true', help='skip PaganiAdsp\'s own host suite')
    parser.add_argument('--kernel', type=Path, help='a UniXNU tree with compile records: also run the kernel build check')
    parser.add_argument('--kernelcache', type=Path, help='the pinned 21H461 kernelcache.decompressed (with --kernel)')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as t:
        out = Path(t)
        drv = lambda names: [DRIVER / n for n in names]
        adsp = lambda names: [ADSP / n for n in names]
        tests = {
            'wire': lambda: check_output(build(out, 'wire', [HERE / 'wire.c'] + drv(['SensorsPb.c', 'SensorsSsc.c']) + adsp(['QcomQrtr.c']))),
            'fixed': lambda: check_output(build(out, 'fixed', [HERE / 'fixed.c'] + drv(['SensorsFixed.c']), ['-lm'])),
            'orient': lambda: check_output(build(out, 'orient', [HERE / 'orient.c'] + drv(['SensorsOrient.c']), ['-lm'])),
            'core': lambda: check_output(build(out, 'core', [HERE / 'core.c'] + drv(CORE))),
            'hid': lambda: (check_output(build(out, 'hid', [HERE / 'hid.c'] + drv(['SensorsHid.c'])), out / 'personalities.xml'),
                            personalities(out / 'personalities.xml')),
            'rpc': lambda: check_output(build(out, 'rpc', [HERE / 'rpc.c'] + drv(['SensorsRpc.c', 'SensorsPack.c']), ['-D_DARWIN_C_SOURCE']),
                                        pack_tool(out)),
            'stack': lambda: check_output(build(out, 'stack', [HERE / 'stack.c'] + drv(CORE) + adsp(ADSP_PORTABLE))),
            'qrtr': lambda: check_output(build(out, 'qrtr_client', [ADSP / 'Test/qrtr_client.c'] + adsp(ADSP_PORTABLE))),
            'weak': lambda: check_output(build(out, 'weak', [HERE / 'weak.c'])),
        }
        if not args.no_adsp_suite:
            tests['adsp'] = lambda: subprocess.run([sys.executable, str(ADSP / 'Test/check-pagani-adsp-host.py')], check=True)
        if args.kernel:
            if not args.kernelcache:
                sys.exit('--kernel needs --kernelcache')
            tests['kernel'] = lambda: subprocess.run([sys.executable, str(DRIVER / 'scripts/build-pagani-sensors.py'), '--tree',
                                                      str(args.kernel), '--kernelcache', str(args.kernelcache), '--out',
                                                      str(out / 'kernel')], check=True)
        for name, test in tests.items():
            if args.only and name not in args.only:
                continue
            print(f'== {name}')
            test()
    print('PASS: PaganiSensors host tests')


if __name__ == '__main__':
    main()
