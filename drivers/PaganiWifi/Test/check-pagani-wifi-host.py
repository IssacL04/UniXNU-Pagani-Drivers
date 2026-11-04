#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Host tests of PaganiWifi (the OnePlus 13T Wi-Fi), all with -Wall -Wextra -Werror and ASan/UBSan:

  tables     PaganiWifiTables.h consistency (the facts step's self test, the boot argument names)
  mhi_boot   the MHI boot state machine (ported from UniXNU-sheng with its own test) and peach's firmware layout (mhi_fw_layout)
  mhi_load   stage 5's own MHI boot against a boot ROM/loader model: buffers, BHIe table of peach's split image and of a single image,
             contexts before the loader and again after its reset, the 36-bit ranges, event ring acknowledgements, failures
  dma        the DMA window allocator (alignment on the bus address, the reserved MSI page, merging, a random run with a shadow map)
  pmic       rail readback and the sleep clock pin against an SPMI arbiter v7 model (writes only when owned, sheng's five writes)
  firmware   the firmware sources: modem_a as FAT16 (4096-byte sectors) and modem_b as FAT32 volumes made by mtools, holding synthetic
             peach files (fragmented), read through a NativeUfs block model, and a seed directory as /private/var/ngpu/firmware/wlan
  bringup    stages 1 to 4 against a simulated PCIe0 corner (GCC, TLMM, TCSR, PARF, PHY, DBI/iATU, the endpoint, RPMh, PMIC): order,
             values, the rail rules (S7I), every failure path, a survey that writes nothing
  apple80211 the Apple80211 request layouts PaganiWifiNet.cpp answers with
  net        PaganiWifiNet.cpp itself against stub kernel headers (Test/net_stub.h) and a model of the module: Settings, request by request, as IO80211.framework
             of 21H461 sends it: the scan, a wrong key, the join, the link's events, a join from a joined network, a leave, the radio off
  smmu       the endpoint's bypass stream through the Sm8750Platform kit's SmmuTakeover.h (the real header) next to the UFS stream
  rddm       the RAM dump reader against a synthetic BHIe table in a model of the DMA window (the link gone, an upload that never ends, tables and
             segments outside the window); its export is read back by the UFS log's own reader when 工具/gpu-test-loop is there
  loggate    which stack lines reach the boot log: the default as before the switch, level 7's bucket, the totals
  quiet      the kernel glue holds the UFS log's writer with its own bit of pagani_panel_probe_quiet (the word is shared bit by bit), and every stage's
             stretch is in place: the host model of WifiBringup.cpp cannot see PaganiWifi.cpp
  stack      with Ath12kWifi/deps (tools/fetch-sources.sh): Ath12kWifi/tools/sta-sim.sh, the station code against a fake access point (leave, join from a
             joined network, a wrong key), and Ath12kWifi/tools/peach-sim.sh, the whole stack under ASan against the simulated device,
             the peach entry's calibration pass in each way the firmware can behave after the mode OFF. Skipped, and said so, without deps

Needs cc/c++, mformat/mmd/mcopy/mdel (mtools). --sm8750-kit points at UniXNU-sm8750-common/kits/Sm8750Platform (default: the sibling
checkout). Host tests do not establish that anything works on the phone.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DRIVER = HERE.parent
DEFAULT_KIT = DRIVER.parents[2] / 'UniXNU-sm8750-common/kits/Sm8750Platform'
SAN = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror'] + SAN
CXXFLAGS = ['-std=c++17', '-Wall', '-Wextra', '-Werror'] + SAN


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def build(out, name, sources, cxx=False, extra=()):
    exe = out / name
    run(['c++' if cxx else 'cc', *(CXXFLAGS if cxx else CFLAGS), *extra, *map(str, sources), '-o', str(exe)])
    return exe


def check_output(exe, *args):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    text = (result.stdout + result.stderr).strip()
    print(text.splitlines()[-1] if text else '(no output)')
    if result.returncode or not text or not text.splitlines()[-1].startswith('PASS'):
        print(text)
        sys.exit(f'FAIL: {exe.name} {" ".join(map(str, args))}')


def firmware_files(rng):
    """The synthetic peach files (relative staging names -> bytes) and where each goes."""
    amss = bytearray(rng.randbytes(1536 * 1024 + 4321))
    amss[0:4] = b'\x7fELF'
    amss[0x80000:0x80004] = b'\x7fELF'
    fat = {
        'peach/amss20.bin': bytes(amss),
        'peach/bdwlan.elf': b'\x7fELF' + rng.randbytes(180 * 1024),
        'peach/bdwlan.b0e': b'\x7fELF' + rng.randbytes(181 * 1024),
        'peach/regdb.bin': rng.randbytes(25000),
        'peach/phy_ucode20.elf': b'\x7fELF' + rng.randbytes(300 * 1024),
        'peach/aux_ucode20.elf': b'\x7fELF' + rng.randbytes(64 * 1024),
        'peach/qdss_trace_config_v2.cfg': b'qdss v2\n' * 300,
        'peach/join.txt': b'NotFromThePartition\nnope\n',
        'tmel_peach_20.elf': b'\x7fELF' + rng.randbytes(40 * 1024),
    }
    seed = {
        'seed-join.txt': ('join.txt', b'HomeWiFi\ncorrect horse battery staple\n'),
        'seed-regdb.bin': ('odm/peach/regdb.bin', rng.randbytes(26000)),
        'seed-bdwlan.elf': ('odm/peach/bdwlan.elf', b'\x7fELF' + rng.randbytes(125 * 1024)),
        'seed-qdss_trace_config_perf_v2.cfg': ('peach/qdss_trace_config_perf_v2.cfg', b'qdss perf\n' * 200),
    }
    fat32 = {'fat32-amss20.bin': rng.randbytes(900 * 1024)}
    return fat, seed, fat32


def make_volume(tmp, name, mformat_args, size, files, rng):
    """files: {path inside the volume: bytes}; holes written first and half deleted, so later files are fragmented."""
    img = tmp / name
    with open(img, 'wb') as f:
        f.truncate(size)
    run(['mformat', '-i', str(img), *mformat_args, '::'], stdout=subprocess.DEVNULL)
    dirs = sorted({str(Path(p).parent) for p in files if str(Path(p).parent) != '.'})
    made = set()
    for d in dirs:
        parts = Path(d).parts
        for i in range(1, len(parts) + 1):
            sub = '/'.join(parts[:i])
            if sub not in made:
                run(['mmd', '-i', str(img), '::' + sub], stdout=subprocess.DEVNULL)
                made.add(sub)
    holes = []
    for i in range(8):
        hole = tmp / f'{name}.hole{i}'
        hole.write_bytes(rng.randbytes(30000))
        run(['mcopy', '-i', str(img), str(hole), f'::hole{i}.bin'])
        holes.append(f'::hole{i}.bin')
    for h in holes[::2]:
        run(['mdel', '-i', str(img), h])
    for path, data in files.items():
        src = tmp / (name + '.' + path.replace('/', '_'))
        src.write_bytes(data)
        run(['mcopy', '-i', str(img), str(src), '::' + path])
    return img


def test_firmware(out, tmp):
    exe = build(out, 'firmware', [HERE / 'firmware.c', DRIVER / 'WifiFirmware.c', DRIVER / 'WifiFat.c'])
    rng = random.Random(110)
    fat, seed, fat32 = firmware_files(rng)
    staging = tmp / 'staging'
    for name, data in list(fat.items()) + [(k, v[1]) for k, v in seed.items()] + list(fat32.items()):
        path = staging / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    a = make_volume(tmp, 'modem_a.img', ['-M', '4096', '-c', '4', '-T', '32768', '-h', '1', '-s', '32768'], 128 << 20,
                    {'image/' + k: v for k, v in fat.items()}, rng)
    b = make_volume(tmp, 'modem_b.img', ['-F', '-c', '1'], 48 << 20, {'image/amss20.bin': fat32['fat32-amss20.bin']}, rng)
    seed_dir = tmp / 'seed'
    for _, (relative, data) in seed.items():
        path = seed_dir / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    big = seed_dir / 'big/peach/amss20.bin'
    big.parent.mkdir(parents=True, exist_ok=True)
    with open(big, 'wb') as f:
        f.truncate(33 << 20)
    check_output(exe, a, b, staging, seed_dir)


def test_rddm(out, tmp):
    """The reader, and its export through the UFS log's reader (the real unpack of 工具/gpu-test-loop) when that checkout is beside this one."""
    blob = tmp / 'rddm-export.bin'
    check_output(build(out, 'rddm', [HERE / 'rddm.c', DRIVER / 'WifiRddm.c']), blob)
    tool = DRIVER.parents[2] / '工具/gpu-test-loop'
    if not (tool / 'ufslog_format.py').is_file():
        print('(the export not read back: no 工具/gpu-test-loop)')
        return
    sys.path.insert(0, str(tool))
    sys.dont_write_bytecode = True
    try:
        import ufslog_format as uf
    finally:
        sys.path.pop(0)
    manifest = uf.unpack_export(blob.read_bytes(), tmp / 'rddm')
    names = [entry['path'] for entry in manifest['files']]
    if manifest['directory'] != '/rddm' or names != ['table.bin', 'seg00.bin', 'seg01.bin', 'seg02.bin']:
        sys.exit(f'FAIL: the export reads back as {manifest["directory"]} {names}')
    print('PASS RDDM export: read back by ufslog_format, ' + ', '.join(names))


def net_includes(out):
    """One line headers for every kernel include of PaganiWifiNet.cpp, all of them Test/net_stub.h."""
    for header in re.findall(r'#include <([^>]+)>', (DRIVER / 'PaganiWifiNet.cpp').read_text()):
        if header != 'stdarg.h':
            path = out / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "net_stub.h"\n')


NET_FLAGS = ['-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable']


def test_net(out, tmp):
    includes = tmp / 'net-includes'
    net_includes(includes)
    check_output(build(out, 'net', [HERE / 'net.cpp'], cxx=True, extra=NET_FLAGS + ['-I', str(includes), '-I', str(HERE), '-I', str(DRIVER / 'Ath12kWifi/include'), '-I', str(DRIVER)]))


def test_quiet():
    """The word has other drivers' bits (0 PaganiPanel, 1 PaganiBattery, 2 PaganiBt): a plain store here would let go of their holds."""
    glue = (DRIVER / 'PaganiWifi.cpp').read_text()
    wanted = ('__atomic_fetch_or(&pagani_panel_probe_quiet, kQuietBit, __ATOMIC_RELEASE)', '__atomic_fetch_and(&pagani_panel_probe_quiet, ~kQuietBit, __ATOMIC_RELEASE)',
              'hwPdc, hwLog, hwQuiet};', 'gB.opt.crumbMs = crumbMs();')
    missing = [text for text in wanted if text not in glue]
    bit = re.search(r'kQuietBit = 1u << (\d+);', (DRIVER / 'WifiBringup.h').read_text())
    if missing or '__atomic_store_n(&pagani_panel_probe_quiet' in glue or not bit or int(bit.group(1)) != 3:
        sys.exit(f'FAIL quiet: {missing or "the bit of the word"}')
    # each stretch of the kernel side, in the order of the stages, each ended; the stack's by its own rule
    stretches = re.findall(r'guard\(gB, "(\w+)"\)', glue)
    if stretches != ['dma', 'bar0', 'chip', 'mhi', 'stack']:
        sys.exit(f'FAIL quiet: the stretches of PaganiWifi.cpp are {stretches}')
    for before, after in (('traceKv("soc hw version"', 'letGo(gB);'), ('chipPowerUp(argument(bootarg::kChipReset', 'letGo(gB);'), ('pw_mhi_load(&load', 'letGo(gB);')):
        if after not in glue[glue.index(before):glue.index(before) + 200]:
            sys.exit(f'FAIL quiet: nothing ends the stretch after {before}')
    if 'if (gB.held && (nw_wifi_stats.stage >= 3 || nowMs() >= holdEnds)) letGo(gB);' not in glue:
        sys.exit('FAIL quiet: the stack\'s stretch has no end')
    for function in ('int fail(', 'void watchFaults(const char *text)\n', 'void pHalt()'):
        at = glue.index(function)
        if 'letGo(gB);' not in glue[at:glue.index('\n}\n', at)]:
            sys.exit(f'FAIL quiet: {function} keeps the writer held')
    if 'if (gStopAfter != step) return false;\n    letGo(gB);' not in glue:
        sys.exit('FAIL quiet: a stop keeps the writer held')
    print('PASS quiet: the driver\'s own bit, five stretches on the kernel side, each ended')


def test_stack(tmp):
    module = DRIVER / 'Ath12kWifi'
    if not (module / 'deps/linux/drivers/net/wireless/ath/ath12k').is_dir():
        print('(the stack simulation skipped: no Ath12kWifi/deps)')
        return 0
    result = subprocess.run(['bash', str(module / 'tools/sta-sim.sh')], capture_output=True, text=True)
    text = (result.stdout + result.stderr).strip()
    if result.returncode or not text.endswith('all passed'):
        print(text)
        sys.exit(f'FAIL: {module}/tools/sta-sim.sh exit {result.returncode}')
    print('PASS station code: join, leave, a join from a joined network, a wrong key')
    result = subprocess.run(['bash', str(module / 'tools/peach-sim.sh'), str(tmp / 'stack')], capture_output=True, text=True)
    lines = [line for line in (result.stdout + result.stderr).splitlines() if line.startswith(('PASS', 'FAIL'))]
    print('\n'.join(lines))
    if result.returncode or not lines:
        sys.exit(f'FAIL: {module}/tools/peach-sim.sh exit {result.returncode}')
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sm8750-kit', type=Path, default=DEFAULT_KIT, help='Sm8750Platform kit (SmmuTakeover.h)')
    parser.add_argument('--keep', type=Path, help='build and test files here instead of a temporary directory')
    args = parser.parse_args()
    if not (args.sm8750_kit / 'SmmuTakeover.h').is_file():
        sys.exit(f'{args.sm8750_kit}/SmmuTakeover.h missing (use --sm8750-kit)')
    with tempfile.TemporaryDirectory(prefix='pagani-wifi-host-') as scratch:
        tmp = args.keep.resolve() if args.keep else Path(scratch)
        tmp.mkdir(parents=True, exist_ok=True)
        out = tmp / 'bin'
        out.mkdir(exist_ok=True)
        check_output(build(out, 'tables', [HERE / 'tables.cpp'], cxx=True, extra=['-Wshadow', '-Wconversion', '-Wno-sign-conversion']))
        check_output(build(out, 'mhi_boot', [HERE / 'mhi_boot.c', DRIVER / 'MhiBoot.c']))
        check_output(build(out, 'mhi_load', [HERE / 'mhi_load.c', DRIVER / 'WifiMhiLoad.c', DRIVER / 'MhiBoot.c', DRIVER / 'WifiDma.c']))
        check_output(build(out, 'dma', [HERE / 'dma.c', DRIVER / 'WifiDma.c']))
        check_output(build(out, 'pmic', [HERE / 'pmic.c', DRIVER / 'WifiPmic.c']))
        test_firmware(out, tmp)
        check_output(build(out, 'bringup', [HERE / 'bringup.cpp', DRIVER / 'WifiBringup.cpp'], cxx=True))
        check_output(build(out, 'apple80211', [HERE / 'apple80211.cpp'], cxx=True))
        test_net(out, tmp)
        check_output(build(out, 'smmu', [HERE / 'smmu.cpp'], cxx=True, extra=['-I', str(args.sm8750_kit)]))
        test_rddm(out, tmp)
        check_output(build(out, 'loggate', [HERE / 'loggate.c']))
        test_quiet()
        stack = test_stack(tmp)
    print(f'PASS: PaganiWifi host tests ({12 + 2 * stack} programs)')


if __name__ == '__main__':
    main()
