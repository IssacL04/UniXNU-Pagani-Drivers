#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Mutants of the stretches around first touches (AdspCore.h): each one edit in a copy of the sources, each must fail the core test."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DRIVERS = Path(__file__).resolve().parents[2]
MUTANTS = [
    ('AdspCore.c', '    guard(c, "smem-table");\n', '', 'no table stretch'),
    ('AdspCore.c', '    guard(c, "smem");\n', '', 'no partition stretch'),
    ('AdspCore.c', '    guard(c, "lpass");\n', '', 'no lpass stretch'),
    ('AdspCore.c', '    if (!wait_ready(c)) goto failed;\n    let_go(c);\n', '    if (!wait_ready(c)) goto failed;\n', 'held after ready'),
    ('AdspCore.c', 'stopped:\n    let_go(c);\n', 'stopped:\n', 'held after a stop'),
    ('AdspCore.c', 'failed:\n    undo(c);\n    let_go(c);\n', 'failed:\n    undo(c);\n', 'held after a failure'),
    ('AdspCore.c', '    c->ops->sleep_ms(CTX, c->config.crumb_ms);\n', '', 'no wait for the line'),
    ('AdspCore.c', '    c->ops->sleep_ms(CTX, PAGANI_ADSP_QUIET_MS);\n', '', 'no wait for a round'),
    ('AdspCore.c', '    if (!c->config.crumb_ms) return;\n', '', 'held without a log'),
]


def main():
    survivors = []
    for name, old, new, what in MUTANTS:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / 'drivers'
            work.mkdir()
            for part in ('PaganiAdsp', 'PaganiInput', 'NativeUfs'):
                if (DRIVERS / part).is_dir():
                    shutil.copytree(DRIVERS / part, work / part, ignore=shutil.ignore_patterns('__pycache__'))
            source = work / 'PaganiAdsp' / name
            text = source.read_text()
            if old not in text:
                raise SystemExit(f'FAIL: mutant ({what}) does not apply')
            source.write_text(text.replace(old, new, 1))
            result = subprocess.run([sys.executable, '-B', work / 'PaganiAdsp/Test/check-pagani-adsp-host.py', '--only', 'core', '--adreno830',
                                     DRIVERS.parents[1] / 'Adreno830'], capture_output=True, text=True)
            print(f'{"survived" if not result.returncode else "killed  "} {what}', flush=True)
            if not result.returncode:
                survivors.append(what)
    if survivors:
        raise SystemExit(f'FAIL: mutants survived: {survivors}')
    print(f'PASS mutants: all {len(MUTANTS)} killed')


if __name__ == '__main__':
    main()
