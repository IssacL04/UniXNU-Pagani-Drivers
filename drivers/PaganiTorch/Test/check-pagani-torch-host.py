#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiTorch host checks: the portable sources compiled strictly, then each test with ASan/UBSan and -Werror.

  spmi    TorchSpmi.c against a model of the SPMI PMIC arbiter v7 (discovery, owned-mapping preference, observer reads, the write whitelist
          over every register and value, the interlock, opening only an owned and identified channel, the command word, re-verification
          before every write, closing on errors, the one emergency stop)
  core    TorchCore.c on the arbiter + flash module model (the boot arguments' configuration, dry run, refusals before any write, the exact
          on and off sequences, every level of every cap, the 5 minute limit, every fault bit, a failure at every write and read, the log)
  ios     TorchIos.c (the plan, the last check before launchd, the level of a user client call)
  plugin  ios/TorchPlugin.m on this Mac's Objective-C runtime against a stand-in vendor, the messages in FigFlashlight's order; other
          clients untouched; a missing method or key replaces nothing
  args    every boot argument the glue reads is in README.md's table and in PaganiTorch.h (and they name no other)
  mg      the row of camera-flash in the iOS inventory names the node and property TorchIos.h makes, when the inventory is there

  check-pagani-torch-host.py [--tree TREE]   (--tree: also the kernel archive and the plug-in, output in a temporary directory)

Host checks do not establish that the LED lights on the device."""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
CORE = ['TorchSpmi.c', 'TorchCore.c', 'TorchIos.c']
MG_TABLE = Path('/Users/xiaochoumao/unixnu-13t/build-userspace/a830-inputs/inventory/ios-data/mg-all.tsv')
TESTS = {
    'spmi': ['Test/spmi.c', 'TorchSpmi.c'],
    'core': ['Test/core.c', 'TorchCore.c', 'TorchSpmi.c'],
    'ios': ['Test/ios.c', 'TorchIos.c'],
}
STRICT = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Wno-sign-conversion', '-Wcast-qual',
          '-Wstrict-prototypes', '-Wmissing-prototypes', '-O2']
SANITIZE = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
OBJC = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-objc-method-access', '-fno-objc-arc', '-O1', '-g', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', '-DPAGANI_TORCH_PLUGIN_HOST', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-lobjc',
        '-Wl,-export_dynamic']


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}')
    return result.stdout


def check_arguments():
    """Every boot argument the glue reads is documented in README.md's table and PaganiTorch.h, and nothing else is."""
    read = set(re.findall(r'argument\("(pagani-[a-z0-9-]+)"', (SOURCE / 'PaganiTorch.cpp').read_text()))
    if 'pagani-torch' not in read or len(read) < 5:
        raise SystemExit(f'FAIL args: the boot argument reads were not found in PaganiTorch.cpp ({sorted(read)})')
    readme = (SOURCE / 'README.md').read_text().split('## Boot arguments', 1)[-1].split('\n## ', 1)[0]
    header = (SOURCE / 'PaganiTorch.h').read_text().split('#ifndef', 1)[0]
    problems = []
    for where, text in (('README.md', readme), ('PaganiTorch.h', header)):
        named = set(re.findall(r'(?<![a-z-])(pagani-torch(?:-[a-z]+)*)', text))
        problems += [f'{name} is read but not in {where}' for name in sorted(read - named)]
        problems += [f'{where} names {name}, which is not read' for name in sorted(named - read)]
    if problems:
        raise SystemExit('FAIL args:\n  ' + '\n  '.join(problems))
    print(f'PASS args: {len(read)} boot arguments read, each in README.md and PaganiTorch.h')


def check_gestalt():
    if not MG_TABLE.is_file():
        print('skip mg: no inventory table')
        return
    row = [line for line in MG_TABLE.read_text().splitlines() if line.startswith('camera-flash\t')]
    if len(row) != 1 or '"IODeviceTree:/product/camera"' not in row[0] or '@"flash"' not in row[0]:
        raise SystemExit(f'FAIL mg: {row}')
    print('PASS mg: camera-flash is the property flash of IODeviceTree:/product/camera')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, help='also build the kernel archive with this tree\'s compile records (read only) and the plug-in')
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
        for name, define in (('plugin', []), ('plugin-no-method', ['-DNO_METHOD']), ('plugin-no-key', ['-DNO_KEY', '-DPGT_PLUGIN_KEY_SYMBOL="kNoSuchSymbolAnywhere"'])):
            exe = tmp / name
            run([cc, *OBJC, *define, str(SOURCE / 'Test/plugin.m'), str(SOURCE / 'ios/TorchPlugin.m'), '-o', str(exe)])
            out = run([str(exe)]).strip()
            print(out.splitlines()[-1] if out else f'{name}: no output')
        check_arguments()
        check_gestalt()
        if args.tree:
            out = run([sys.executable, '-B', str(SOURCE / 'scripts/build-pagani-torch.py'), '--tree', str(args.tree), '--out', str(tmp / 'kernel')])
            print(out.strip())
            out = run([sys.executable, '-B', str(SOURCE / 'scripts/build-pagani-torch-plugin.py'), '--out', str(tmp / 'plugin-out')])
            print(out.strip())
    print('PASS PaganiTorch host checks')


if __name__ == '__main__':
    main()
