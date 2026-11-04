#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiHaptics host checks: the portable sources compiled strictly, then each test with ASan/UBSan and -Werror.

  spmi   HapticsSpmi.c against a model of the SPMI PMIC arbiter v7 (discovery, owned-mapping preference, observer reads, the write
         whitelist exhaustively, opening only owned and identified CFG/PTN channels, the command word and data packing, re-verification
         before every write, closing on errors, the one emergency stop)
  core   HapticsCore.c on the arbiter + HAP530 model (dry run, refusals before any write, the init sequence, DIRECT_PLAY, effects,
         streams with refill/drain/abort/underrun, voltage and length caps, the on-time budget, faults)
  args   every boot argument PaganiHaptics.cpp reads is in README.md's table and in PaganiHaptics.h (and they name no other)
  ios    the iOS haptics switch (HapticsIos.c, PaganiHapticsIos.cpp) on an IORegistry model (Test/iosstubs): the /product/haptics node of
         stages A and B as libMobileGestalt finds it (recursive device tree iteration, names and property keys by their obfuscated
         md5 form; checked against the haptics RE's mg/haptics-keys.tsv when it is there), the refusals and the failures

  check-pagani-haptics-host.py [--tree TREE]   (--tree: also the kernel archive build, output in a temporary directory)

Host checks do not establish that the actuator moves on the device."""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
CORE = ['HapticsSpmi.c', 'HapticsCore.c', 'HapticsIos.c']
MG_KEYS = Path('/Users/xiaochoumao/unixnu-13t/build-userspace/a830-inputs/haptics/ios/mg/haptics-keys.tsv')
TESTS = {
    'spmi': ['Test/spmi.c', 'HapticsSpmi.c'],
    'core': ['Test/core.c', 'HapticsCore.c', 'HapticsSpmi.c'],
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
    """Every boot argument the glue reads is documented in README.md's table and PaganiHaptics.h, and nothing else is."""
    read = set(re.findall(r'(?:argument|PE_parse_boot_argn)\("(pagani-[a-z0-9-]+)"', (SOURCE / 'PaganiHaptics.cpp').read_text()))
    if 'pagani-haptics' not in read or len(read) < 5:
        raise SystemExit(f'FAIL args: the boot argument reads were not found in PaganiHaptics.cpp ({sorted(read)})')
    readme = (SOURCE / 'README.md').read_text().split('## Boot arguments', 1)[-1].split('\n## ', 1)[0]
    header = (SOURCE / 'PaganiHaptics.h').read_text().split('#ifndef', 1)[0]
    problems = []
    for where, text in (('README.md', readme), ('PaganiHaptics.h', header)):
        named = set(re.findall(r'(?<![a-z-])(pagani-audio-srzl|pagani-haptics(?:-[a-z0-9]+)*)', text))
        problems += [f'{name} is read but not in {where}' for name in sorted(read - named)]
        problems += [f'{where} names {name}, which is not read' for name in sorted(named - read)]
    if problems:
        raise SystemExit('FAIL args:\n  ' + '\n  '.join(problems))
    print(f'PASS args: {len(read)} boot arguments read, each in README.md and PaganiHaptics.h')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, help='also build the kernel archive with this tree\'s compile records (read only)')
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
        check_arguments()
        run([cc, *SANITIZE, '-c', str(SOURCE / 'HapticsIos.c'), '-o', str(tmp / 'ios-plan.o')])
        run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-I', str(HERE / 'iosstubs'), '-I', str(SOURCE), str(HERE / 'ios.cpp'), str(tmp / 'ios-plan.o'),
             '-o', str(tmp / 'ios')])
        out = run([str(tmp / 'ios'), *([str(MG_KEYS)] if MG_KEYS.is_file() else [])]).strip()
        print(out.splitlines()[-1] if out else 'ios: no output')
        if args.tree:
            out = run([sys.executable, str(SOURCE / 'scripts/build-pagani-haptics.py'), '--tree', str(args.tree), '--out',
                       str(tmp / 'kernel')])
            print(out.strip())
    print('PASS PaganiHaptics host checks')


if __name__ == '__main__':
    main()
