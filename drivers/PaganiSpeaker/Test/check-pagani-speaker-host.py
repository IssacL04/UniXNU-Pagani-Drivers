#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiSpeaker host checks: the portable sources compiled strictly, then each test with ASan/UBSan and -Werror.

  tone      SpeakerTone.c against libm (sine, dB gains, frequency, level, fades, length, clamping)
  protocol  PaganiGpr.c and PaganiReach.c (GPR codec, framework readiness, every AudioReach/PRM packet field, malformed answers)
  amp       GeniI2c.c and Tfa9873.c against a GENI engine and a TFA9873 register model (programs, runner, container fields)
  hw        SpeakerHw.c against models of the TLMM, the GCC (the shared vote register under its lock word) and the serial engine
  smmu      SpeakerSmmu.h against a model of the apps SMMU (the 13T's tables; two writes on success, none on a refusal, no bank touched)
  limit     SpeakerLimit.c against libm (the curve at every ceiling, the ceiling over random signals, delay, attack, release, linking)
  gccvote   the GCC vote register 0x52008 under four interleaved writers (the touch's sm8750_touch_clock, this driver's
            speaker_clocks_on, PaganiBattery's and PaganiWifi's lock protocols) on thousands of schedules: no vote lost with the lock
            word, votes lost with the updates as they were, a stuck holder costs 50 ms and is counted
  core      SpeakerCore.c and SpeakerBackend.c over the hw models, a DSP model and PaganiAudio's real engine (every stage, the tone and
            the tone walk, the iOS ring, the ceiling under full-scale input, every failure path)
  args      every boot argument PaganiSpeaker.cpp reads is in README.md's table (and the table names no other pagani-speaker or
            pagani-audio-tone argument)
  kernel    with --tree: scripts/build-pagani-speaker.py against the tree's compile records, output in a temporary directory

  check-pagani-speaker-host.py [--tree TREE] [--kit DIR] [--adsp DIR] [--audio DIR] [--verbose]

The kit (Sm8750Platform: PeripheralIO.h) is TREE/kits/Sm8750Platform with --tree, else UniXNU-sm8750-common next to the drivers
repository; PaganiAdsp.h and PaganiAudio come from the sibling drivers (or TREE/drivers). Host checks do not establish that the amplifiers
answer, that the DSP takes the graphs or that a sound comes out on the device."""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
PORTABLE = ['GeniI2c.c', 'PaganiGpr.c', 'PaganiReach.c', 'SpeakerBackend.c', 'SpeakerCore.c', 'SpeakerHw.c', 'SpeakerLimit.c',
            'SpeakerTone.c', 'Tfa9873.c']
STRICT = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Wno-sign-conversion', '-Wcast-qual',
          '-Wstrict-prototypes', '-Wmissing-prototypes', '-O2']
SANITIZE = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def tests(audio):
    hw = ['SpeakerHw.c', 'GeniI2c.c', 'Tfa9873.c']
    return {
        'tone': ['Test/tone.c', 'SpeakerTone.c'],
        'protocol': ['Test/protocol.c', 'PaganiGpr.c', 'PaganiReach.c'],
        'amp': ['Test/amp.c', 'GeniI2c.c', 'Tfa9873.c'],
        'hw': ['Test/hw.c', *hw],
        'smmu': ['Test/smmu.c'],
        'limit': ['Test/limit.c', 'SpeakerLimit.c', 'SpeakerTone.c'],
        'gccvote': ['Test/gccvote.c', *hw],
        'core': ['Test/core.c', 'SpeakerCore.c', 'SpeakerBackend.c', *hw, 'PaganiGpr.c', 'PaganiReach.c', 'SpeakerTone.c',
                 'SpeakerLimit.c', str(audio / 'PaganiAudioEngine.c')],
    }


def run(command, **kwargs):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}')
    return result.stdout


def find(given, candidates, marker):
    for candidate in ([given] if given else candidates):
        if candidate and (Path(candidate) / marker).is_file():
            return Path(candidate).resolve()
    raise SystemExit(f'FAIL: {marker} not found in ' + ', '.join(str(c) for c in ([given] if given else candidates)))


def check_arguments():
    """The boot arguments are an operator's only handle on the driver: every one the glue reads must be in README.md's table, and the
    table must not name one the glue does not read."""
    glue = (SOURCE / 'PaganiSpeaker.cpp').read_text()
    read = set(re.findall(r'(?:argument|PE_parse_boot_argn)\("(pagani-[a-z0-9-]+)"', glue))
    if not {'pagani-speaker', 'pagani-audio-tone', 'pagani-speaker-ceiling', 'pagani-speaker-stop'} <= read:
        raise SystemExit(f'FAIL args: the boot argument reads were not found in PaganiSpeaker.cpp ({sorted(read)})')
    read -= {'pagani-ufslog-ms'}   # BootProgress's: read here only to know how long a line takes to reach its log
    readme = (SOURCE / 'README.md').read_text().split('## Boot arguments', 1)[-1].split('\n## ', 1)[0]
    named = set(re.findall(r'(?<![a-z0-9-])(pagani-[a-z0-9]+(?:-[a-z0-9]+)*)', readme))
    problems = [f'{name} is read but not in README.md' for name in sorted(read - named)]
    problems += [f'README.md names {name}, which is not read' for name in sorted(named - read)
                 if name.startswith(('pagani-speaker', 'pagani-audio-tone'))]
    if problems:
        raise SystemExit('FAIL args:\n  ' + '\n  '.join(problems))
    print(f'PASS args: {len(read)} boot arguments read, each in README.md')


def check_quiet():
    """The word has other drivers' bits (0 panel, 1 USB, 2 Bluetooth, 3 Wi-Fi, 4 camera): a plain store would let go of their holds. This
    driver's bit is its own and ADSP's is another."""
    glue = (SOURCE / 'PaganiSpeaker.cpp').read_text()
    wanted = ('__atomic_fetch_or(&pagani_panel_probe_quiet, SPEAKER_QUIET_BIT, __ATOMIC_RELEASE)',
              '__atomic_fetch_and(&pagani_panel_probe_quiet, ~uint32_t(SPEAKER_QUIET_BIT), __ATOMIC_RELEASE)', 'opPublish, &kIo, opQuiet};',
              'gConfig.crumb_ms = crumbMs();')
    missing = [text for text in wanted if text not in glue]
    bit = re.search(r'#define SPEAKER_QUIET_BIT (\d+)u', (SOURCE / 'SpeakerBoard.h').read_text())
    if missing or '__atomic_store_n(&pagani_panel_probe_quiet' in glue or not bit or int(bit.group(1)) != 32:
        raise SystemExit(f'FAIL quiet: {missing or "the bit of the word"}')
    print('PASS quiet: the glue holds the UFS log\'s writer by its own bit (32), never with a plain store')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, help='also build the kernel archive with this tree\'s compile records')
    parser.add_argument('--kit', type=Path, help='Sm8750Platform kit directory (PeripheralIO.h)')
    parser.add_argument('--adsp', type=Path, help='directory with PaganiAdsp.h')
    parser.add_argument('--audio', type=Path, help='directory with PaganiAudioEngine.c/.h')
    parser.add_argument('--verbose', action='store_true', help='print each test\'s full output')
    args = parser.parse_args()
    tree = args.tree.resolve() if args.tree else None
    kit = find(args.kit, ([tree / 'kits/Sm8750Platform'] if tree else []) +
               [SOURCE.parents[2] / 'UniXNU-sm8750-common/kits/Sm8750Platform'], 'PeripheralIO.h')
    adsp = find(args.adsp, [SOURCE.parent / 'PaganiAdsp'] + ([tree / 'drivers/PaganiAdsp'] if tree else []), 'PaganiAdsp.h')
    audio = find(args.audio, [SOURCE.parent / 'PaganiAudio'] + ([tree / 'drivers/PaganiAudio'] if tree else []), 'PaganiAudioEngine.c')
    includes = ['-I', SOURCE, '-I', HERE, '-I', kit, '-I', adsp, '-I', audio]
    cc = os.environ.get('CC', 'cc')
    with tempfile.TemporaryDirectory(prefix='pagani-speaker-') as tmp:
        tmp = Path(tmp)
        for name in PORTABLE:
            run([cc, *STRICT, *includes, '-c', SOURCE / name, '-o', tmp / (name + '.o')])
        print(f'PASS strict compile: {len(PORTABLE)} portable sources (-Wconversion -Wshadow -Wcast-qual -Wmissing-prototypes), '
              f'kit {kit}')
        for test, sources in tests(audio).items():
            exe = tmp / test
            run([cc, *SANITIZE, *includes, *(s if s.startswith('/') else SOURCE / s for s in sources), '-lm', '-o', exe])
            out = run([exe]).strip()
            lines = out.splitlines()
            print(lines[-1] if lines else f'{test}: no output')
            if args.verbose:
                for line in lines[:-1]:
                    print('  ' + line)
            if not lines or not lines[-1].startswith('PASS'):
                raise SystemExit(f'FAIL {test}: no PASS line')
        check_arguments()
        check_quiet()
        if tree:
            out = run([sys.executable, '-B', SOURCE / 'scripts/build-pagani-speaker.py', '--tree', tree, '--out', tmp / 'kernel',
                       '--kit', kit, '--adsp', adsp, '--audio', audio])
            print(out.strip())
    print('PASS PaganiSpeaker host checks')


if __name__ == '__main__':
    main()
