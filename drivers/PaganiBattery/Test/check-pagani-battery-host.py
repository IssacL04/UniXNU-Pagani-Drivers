#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiBattery host checks: the portable sources compiled strictly, then each test with ASan/UBSan and -Werror.

  battmgr  BatteryBattmgr.c against scripted OPLUS and Qualcomm charger firmware (layouts, probes, writes and their refusals)
  gauge    BatteryGauge.c against an NFG8011B register model (units, sign, ranges, retries, absence, device type)
  i2c      BatteryI2c.c against a model of the GCC votes, the RCG, the TLMM pins and a GENI I2C engine with the gauge behind it
  spmi     BatterySpmi.c against a fake SPMI PMIC arbiter v7 (the charger PMIC's owners, TYPE/SUBTYPE, the log lines, the charger's state)
  usb      BatteryUsb.c against a model where reading a block without power fails the test (gates, order, SMMU table, line lengths)
  props    BatteryProps.c: the IOPMPowerSource keys, types and conditions; with the 21H461 system volume at hand, every key in powerd
  core     BatteryCore.c: the whole run with a simulated clock, gauge and battery manager (sources, guards, policy, log size, the charger PMIC)
  args     every boot argument PaganiBattery.cpp reads is in README.md's table and in PaganiBattery.h (and they name no other)
  decode   scripts/decode-usb-probe.py on the lines of a run of the usb test

  check-pagani-battery-host.py [--tree TREE] [--powerd FILE]   (--tree: also the kernel archive build, output in a temporary directory;
                                                                --powerd: 21H461 powerd, default the local system volume if there)

Host checks do not establish that the gauge answers or that the phone charges on the device."""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
CORE = ['BatteryBattmgr.c', 'BatteryGauge.c', 'BatteryI2c.c', 'BatterySpmi.c', 'BatteryUsb.c', 'BatteryCore.c', 'BatteryProps.c']
TESTS = {
    'battmgr': ['Test/battmgr.c', 'BatteryBattmgr.c'],
    'gauge': ['Test/gauge.c', 'BatteryGauge.c'],
    'i2c': ['Test/i2c.c', 'BatteryI2c.c', 'BatteryGauge.c'],
    'spmi': ['Test/spmi.c', 'BatterySpmi.c'],
    'usb': ['Test/usb.c', 'BatteryUsb.c', 'BatterySpmi.c'],
    'props': ['Test/props.c', 'BatteryProps.c'],
    'core': ['Test/core.c', 'BatteryCore.c', 'BatteryBattmgr.c', 'BatteryGauge.c', 'BatterySpmi.c'],
}
STRICT = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Wno-sign-conversion', '-Wcast-qual',
          '-Wstrict-prototypes', '-Wmissing-prototypes', '-O2']
SANITIZE = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}')
    return result.stdout


def check_arguments():
    """The boot arguments are an operator's only way to switch the driver's hardware steps off: every one the glue reads must be in
    README.md's table and PaganiBattery.h (pagani-battery-pmic, which starts the SPMI survey by default, was in neither)."""
    read = set(re.findall(r'(?:argument|PE_parse_boot_argn)\("(pagani-[a-z-]+)"', (SOURCE / 'PaganiBattery.cpp').read_text()))
    if 'pagani-battery-pmic' not in read or len(read) < 10:
        raise SystemExit(f'FAIL args: the boot argument reads were not found in PaganiBattery.cpp ({sorted(read)})')
    readme = (SOURCE / 'README.md').read_text().split('## Boot arguments', 1)[-1].split('\n## ', 1)[0]
    header = (SOURCE / 'PaganiBattery.h').read_text().split('#ifndef', 1)[0]
    problems = []
    for where, text in (('README.md', readme), ('PaganiBattery.h', header)):
        named = set(re.findall(r'(?<![a-z-])(pagani-[a-z]+(?:-[a-z]+)*)', text))
        problems += [f'{name} is read but not in {where}' for name in sorted(read - named)]
        stale = sorted(name for name in named - read if name.startswith('pagani-battery'))
        problems += [f'{where} names {name}, which is not read' for name in stale]
    if problems:
        raise SystemExit('FAIL args:\n  ' + '\n  '.join(problems))
    print(f'PASS args: {len(read)} boot arguments read, each in README.md and PaganiBattery.h')


def check_quiet():
    """The hold of the UFS log's writer is the one part of the USB probe's rule the glue carries: the model of Test/usb.c only sees the callback. A word of
    bits, shared with PaganiPanel (bit 0) and PaganiBt (bit 2): a plain store here would let go of their holds, and of the writer's rule with it."""
    glue = (SOURCE / 'PaganiBattery.cpp').read_text()
    bit = re.search(r'#define PBU_QUIET_BIT (\d+)u', (SOURCE / 'BatteryUsb.h').read_text())
    wanted = ('__atomic_fetch_or(&pagani_panel_probe_quiet, PBU_QUIET_BIT, __ATOMIC_RELEASE)', '__atomic_fetch_and(&pagani_panel_probe_quiet, ~PBU_QUIET_BIT, __ATOMIC_RELEASE)',
              'const PbuIo kUsbIo = {nullptr, usbRead, pmicRead, opTrace, usbQuiet};')
    missing = [text for text in wanted if text not in glue]
    if missing or '__atomic_store_n(&pagani_panel_probe_quiet' in glue or not bit or int(bit.group(1)) != 2:
        raise SystemExit(f'FAIL quiet: {missing or "the bit of the word"}')
    print('PASS quiet: the USB probe holds the writer with its bit of the word')


def check_decode(exe, tmp):
    """scripts/decode-usb-probe.py on the lines of a run of the usb test: the verdict, the numbers, and a run that stopped half way."""
    lines = run([str(exe), 'lines'])
    log = tmp / 'merged.log'
    log.write_text(''.join(f'{1000 + i:8d} {122.5 + i:10.3f} U  {line}\n' for i, line in enumerate(lines.splitlines())) + '    2000    130.000 F  FPS 60\n')
    script = SOURCE / 'scripts/decode-usb-probe.py'
    out = run([sys.executable, '-B', str(script), str(log)])
    for want in ('route=inherit', 'DWC_usb31 2.00', 'PRTCAPDIR=设备', '输入限流 0x2952=0x28：2000 mA', '阶段 0x2606=0x83：fullon', '0x2c51=0x02', 'TTBR0=0x80feed000',
                 'RUN=0', 'HALTED=1'):
        if want not in out:
            raise SystemExit(f'FAIL decode: no "{want}" in:\n{out}')
    # the log ends at the announcement of the first DWC3 read: the report must say so
    kept = [line for line in lines.splitlines()]
    stop = next(i for i, line in enumerate(kept) if line.startswith('[usbp] gate'))
    log.write_text('\n'.join(kept[:stop + 1]) + '\n')
    out = run([sys.executable, '-B', str(script), str(log)])
    if '探针没走完' not in out or '第一次读 DWC3' not in out:
        raise SystemExit(f'FAIL decode: a run that stopped at the first DWC3 read:\n{out}')
    log.write_text('nothing here\n')
    missing = subprocess.run([sys.executable, '-B', str(script), str(log)], capture_output=True, text=True)
    if missing.returncode != 1 or '没有 [usbp] 行' not in missing.stdout:
        raise SystemExit(f'FAIL decode: a log without the probe: {missing.stdout}')
    print('PASS decode: the verdict, the numbers and a run cut short')


def check_powerd(path):
    """Every key BatteryProps.c publishes is a whole string of the 21H461 powerd (the binary stays local; nothing of it is kept). The
    adapter's own member names (Watts, AdapterVoltage, Current) are powerd's AdapterDetails pass-through, IOPM.h's names, and our
    diagnostics key is ours."""
    if not path.is_file():
        print(f'skip powerd: {path} is not here')
        return
    strings = set(path.read_bytes().split(b'\0'))
    keys = re.findall(r'put\(out, &n, "([A-Za-z]+)"', (SOURCE / 'BatteryProps.c').read_text())
    missing = [k for k in keys if k != 'PaganiBatterySource' and k.encode() not in strings]
    if len(keys) < 20 or missing:
        raise SystemExit(f'FAIL powerd: {len(keys)} keys, not in powerd: {missing}')
    print(f'PASS powerd: {len(keys) - 1} keys are strings of {path.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, help='also build the kernel archive with this tree\'s compile records')
    parser.add_argument('--powerd', type=Path, default=SOURCE.parents[2] / 'build-userspace/a830-inputs/osmnt-21H461/System/Library/'
                        'CoreServices/powerd.bundle/powerd', help='21H461 powerd (local, never kept)')
    args = parser.parse_args()
    cc = os.environ.get('CC', 'cc')
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name in CORE:
            run([cc, *STRICT, '-c', str(SOURCE / name), '-o', str(tmp / (name + '.o'))])
        print(f'PASS strict compile: {len(CORE)} portable sources (-Wconversion -Wshadow -Wmissing-prototypes)')
        for test, sources in TESTS.items():
            exe = tmp / test
            run([cc, *SANITIZE, *(str(SOURCE / s) for s in sources), '-o', str(exe)])
            out = run([str(exe)]).strip()
            print(out.splitlines()[-1] if out else f'{test}: no output')
            for line in out.splitlines()[:-1]:
                print('  ' + line)
        check_arguments()
        check_quiet()
        check_decode(tmp / 'usb', tmp)
        check_powerd(args.powerd)
        if args.tree:
            out = run([sys.executable, str(SOURCE / 'scripts/build-pagani-battery.py'), '--tree', str(args.tree), '--out',
                       str(tmp / 'kernel')])
            print(out.strip())
    print('PASS PaganiBattery host checks')


if __name__ == '__main__':
    main()
