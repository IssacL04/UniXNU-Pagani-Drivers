#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiArmDTPlatform host checks: the portable headers (PlatformRtc.h, PlatformHalt.h) under Test/platform.c.

  strict   the test compiled with -Wall -Wextra -Werror and the conversion warnings
  run      the test with ASan/UBSan, at -O0 and -O2
  args     every boot argument ArmDTPlatform.cpp reads for this driver is in README.md's table, and README.md names no other
  mutants  each deliberate defect in the headers must fail the test (a test that passes them tests nothing)

  check-platform-host.py [--no-mutants]

Host checks do not establish that the phone's RTC answers, or what its firmware does with SYSTEM_OFF."""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
HEADERS = ['PlatformRtc.h', 'PlatformHalt.h']
STRICT = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Wno-sign-conversion', '-Wcast-qual',
          '-Wstrict-prototypes', '-Wmissing-prototypes', '-O2']
SANITIZE = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']

# (header, the text, what it becomes): a defect the test must notice.
MUTANTS = [
    ('PlatformRtc.h', '((map >> 8) & 0xfffu) != ppid', '((map >> 8) & 0xffu) != ppid'),
    ('PlatformRtc.h', 'io->core(io->ctx, 4) & 0x7ffu', 'io->core(io->ctx, 4) & 0x3ffu'),
    ('PlatformRtc.h', '|| version >= 0x80000000u ', ''),
    ('PlatformRtc.h', 'version < 0x70000000u || ', ''),
    ('PlatformRtc.h', '|| count > 1024u', ''),
    ('PlatformRtc.h', 'if (found < 0 || !(io->cnfg(io->ctx, 4u * n) & 7u)) found', 'if (found < 0) found'),
    ('PlatformRtc.h', 'if (found < 0 || !(io->cnfg(io->ctx, 4u * n) & 7u)) found', 'found'),
    ('PlatformRtc.h', '1u << 27', '1u << 26'),
    ('PlatformRtc.h', '(uint32_t)reg << 4', '(uint32_t)reg << 5'),
    ('PlatformRtc.h', 'if (status & 0xeu) return PRTC_EIO;', 'if (status & 0x6u) return PRTC_EIO;'),
    ('PlatformRtc.h', 'if (status & 0xeu) return PRTC_EIO;', 'if (status & 0xcu) return PRTC_EIO;'),
    ('PlatformRtc.h', 'if (status & 0xeu) return PRTC_EIO;', ''),
    ('PlatformRtc.h', 'channel + 0x18u', 'channel + 0x1cu'),
    ('PlatformRtc.h', 'channel + 8u', 'channel + 4u'),
    ('PlatformRtc.h', '(uint32_t)apid * 0x20u + 0x1cu > PRTC_OBS_BYTES', '(uint32_t)apid * 0x20u > PRTC_OBS_BYTES + 0x20u'),
    ('PlatformRtc.h', 'apid < 0 || ', ''),
    ('PlatformRtc.h', 'io->delay_us(io->ctx, 1);', ''),
    ('PlatformRtc.h', '(uint32_t)rdata[3] << 24', '(uint32_t)rdata[3] << 16'),
    ('PlatformRtc.h', '(uint32_t)rdata[1] << 8', '(uint32_t)rdata[2] << 8'),
    ('PlatformRtc.h', 'if (!(*control & PRTC_CTRL_ENABLE)) return PRTC_EDISABLED;', ''),
    ('PlatformRtc.h', 'if (attempt && value == previous)', 'if (value == previous)'),
    ('PlatformRtc.h', 'if (attempt && value == previous)', 'if (attempt)'),
    ('PlatformRtc.h', '#define PRTC_READS 4u', '#define PRTC_READS 2u'),
    ('PlatformRtc.h', '#define PRTC_READS 4u', '#define PRTC_READS 5u'),
    ('PlatformRtc.h', 'seconds > -PRTC_LIMIT && seconds < PRTC_LIMIT', 'seconds >= -PRTC_LIMIT && seconds < PRTC_LIMIT'),
    ('PlatformRtc.h', 'seconds > -PRTC_LIMIT && seconds < PRTC_LIMIT', 'seconds > -PRTC_LIMIT && seconds <= PRTC_LIMIT'),
    ('PlatformRtc.h', 'seconds > -PRTC_LIMIT && seconds < PRTC_LIMIT', 'seconds < PRTC_LIMIT'),
    ('PlatformRtc.h', '(length != 4 && length != 8)', '(length != 4)'),
    ('PlatformRtc.h', '(length != 4 && length != 8)', '(length > 8)'),
    ('PlatformRtc.h', '!in || ', 'in == (const uint8_t *)1 || '),
    ('PlatformRtc.h', 'value < (uint64_t)PRTC_LIMIT ? (int64_t)value : 0', '(int64_t)value'),
    ('PlatformRtc.h', 'have_offset && prtc_usable(offset)', 'have_offset != 0'),
    ('PlatformRtc.h', 'clock->arg = clock->have_arg ? offset : 0;', 'clock->arg = offset;\n    clock->have_arg = have_offset != 0;'),
    ('PlatformRtc.h', 'if ((int64_t)raw + offset < clock->floor) {', 'if (0) {'),
    ('PlatformRtc.h', 'if ((int64_t)raw + offset < clock->floor) {', 'if (!clock->have_arg && (int64_t)raw + offset < clock->floor) {'),
    ('PlatformRtc.h', 'offset = clock->floor - (int64_t)raw;', 'offset = clock->floor;'),
    ('PlatformRtc.h', 'if (!clock->known) {', 'if (1) {'),
    ('PlatformRtc.h', 'utc = clock->anchor_utc + (int64_t)(uptime - clock->anchor_uptime);', 'utc = clock->anchor_utc;'),
    ('PlatformRtc.h', '} else if (clock->anchored) {', '} else if (0) {'),
    ('PlatformRtc.h', 'utc = clock->floor + (int64_t)uptime;', 'utc = clock->floor;'),
    ('PlatformRtc.h', 'if (utc < 0) utc = 0;', ''),
    ('PlatformRtc.h', 'clock->offset = utc - (int64_t)raw;', 'clock->offset = utc;'),
    ('PlatformRtc.h', 'clock->from = PRTC_FROM_SET;', ''),
    ('PlatformRtc.h', 'clock->floor = utc;', ''),
    ('PlatformRtc.h', 'clock->have_arg = 0;', ''),
    ('PlatformRtc.h', '        clock->known = 0;\n', ''),
    ('PlatformRtc.h', 'if (!prtc_usable(utc) || utc < 0) return;', ''),
    ('PlatformRtc.h', 'if (!prtc_usable(utc) || utc < 0) return;', 'if (!prtc_usable(utc)) return;'),
    ('PlatformRtc.h', 'if (!prtc_usable(utc) || utc < 0) return;', 'if (utc <= 0) return;'),
    ('PlatformRtc.h', 'return difference > PRTC_LATE_SECONDS || difference < -PRTC_LATE_SECONDS;', 'return difference > PRTC_LATE_SECONDS;'),
    ('PlatformRtc.h', 'return difference > PRTC_LATE_SECONDS || difference < -PRTC_LATE_SECONDS;', 'return difference < -PRTC_LATE_SECONDS;'),
    ('PlatformRtc.h', 'return difference > PRTC_LATE_SECONDS || difference < -PRTC_LATE_SECONDS;',
     'return difference >= PRTC_LATE_SECONDS || difference <= -PRTC_LATE_SECONDS;'),
    ('PlatformHalt.h', 'if (mode != 1 && mode != 2) return PHALT_INHERIT;', 'if (!mode) return PHALT_INHERIT;'),
    ('PlatformHalt.h', 'if (mode != 1 && mode != 2) return PHALT_INHERIT;', ''),
    ('PlatformHalt.h', '    case PHALT_TYPE_UPS_HALT:\n', ''),
    ('PlatformHalt.h', 'return mode == 2 ? PHALT_RESTART : PHALT_POWER_OFF;', 'return PHALT_POWER_OFF;'),
    ('PlatformHalt.h', 'return mode == 2 ? PHALT_RESTART : PHALT_POWER_OFF;', 'return PHALT_RESTART;'),
    ('PlatformHalt.h', '*reason = "panic restart";', '*reason = "restart";'),
    ('PlatformHalt.h', 'PHALT_TYPE_UPS_HALT = 3,', 'PHALT_TYPE_UPS_HALT = 2,'),
]


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}')
    return result.stdout


def check_arguments():
    """An operator's only switches: each read in the source must be documented, and the documentation must not promise others."""
    read = set(re.findall(r'PE_parse_boot_argn \("(pagani-[a-z-]+)"', (SOURCE / 'ArmDTPlatform.cpp').read_text()))
    if read != {'pagani-halt', 'pagani-rtc', 'pagani-rtc-offset'}:
        raise SystemExit(f'FAIL args: ArmDTPlatform.cpp reads {sorted(read)}')
    table = (SOURCE / 'README.md').read_text().split('## Boot arguments', 1)[-1].split('\n## ', 1)[0]
    named = set(re.findall(r'`(pagani-[a-z-]+)', table))
    if read != named:
        raise SystemExit(f'FAIL args: read {sorted(read)}, README.md names {sorted(named)}')
    print(f'args: {len(read)} read, all in README.md')


def mutants(work):
    survivors = []
    for name, old, new in MUTANTS:
        text = (SOURCE / name).read_text()
        if text.count(old) != 1:
            raise SystemExit(f'FAIL mutants: {old!r} is in {name} {text.count(old)} times')
        tree = work / 'mutant'
        (tree / 'Test').mkdir(parents=True, exist_ok=True)
        for header in HEADERS:
            (tree / header).write_text((SOURCE / header).read_text())
        (tree / 'Test/platform.c').write_text((HERE / 'platform.c').read_text())
        (tree / name).write_text(text.replace(old, new))
        binary = tree / 'platform'
        compiled = subprocess.run(['cc', '-std=c11', '-O1', '-w', tree / 'Test/platform.c', '-o', binary], capture_output=True, text=True)
        if compiled.returncode:
            raise SystemExit(f'FAIL mutants: does not compile, so it tests nothing: {old!r} -> {new!r}\n{compiled.stderr}')
        if subprocess.run([binary], capture_output=True, timeout=120).returncode == 0:
            survivors.append((name, old, new))
    for survivor in survivors:
        print('SURVIVED', survivor)
    if survivors:
        raise SystemExit(f'FAIL mutants: {len(survivors)} of {len(MUTANTS)} survived')
    print(f'mutants: {len(MUTANTS)} of {len(MUTANTS)} caught')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--no-mutants', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='platform-host-') as folder:
        work = Path(folder)
        run(['cc', *STRICT, '-c', HERE / 'platform.c', '-o', work / 'strict.o'])
        for optimize in ('-O0', '-O2'):
            run(['cc', *SANITIZE, optimize, HERE / 'platform.c', '-o', work / 'platform'])
            print(optimize, run([work / 'platform']).strip())
        check_arguments()
        if not args.no_mutants:
            mutants(work)
    print('ALL PASS')


if __name__ == '__main__':
    main()
