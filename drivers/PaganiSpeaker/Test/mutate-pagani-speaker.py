#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Mutants of PaganiSpeaker's safety rules (the digital ceiling, the shared GCC vote lock, the per-run state, the tone's channels): each
is applied to a copy of the sources (and of the kit's PeripheralIO.h), must still compile, and must make at least one host test fail.
A mutant that survives means the tests do not guard that rule.

  mutate-pagani-speaker.py [--kit DIR] [--adsp DIR] [--audio DIR] [--only NAME]"""
import argparse
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
spec = importlib.util.spec_from_file_location('check', HERE / 'check-pagani-speaker-host.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

# name: (file, original text, mutated text, tests that must catch it)
MUTANTS = {
    'ceiling-ignores-incoming-block': ('SpeakerLimit.c', '        if (g1 > need) g1 = need;\n', '', ['limit', 'core']),
    'ceiling-ignores-held-block': ('SpeakerLimit.c', '        if (g1 > l->held_need) g1 = l->held_need;\n', '', ['limit']),
    'ceiling-rounds-up': ('SpeakerLimit.c', '(m * (int64_t)g) >> 16', '(m * (int64_t)g + 65535) >> 16', ['limit']),
    'ceiling-no-knee': ('SpeakerLimit.c', 'speaker_tone_gain(ceiling_db + SPEAKER_LIMIT_KNEE_DB)', 'speaker_tone_gain(ceiling_db + 1)',
                        ['limit']),
    'ceiling-releases-at-once': ('SpeakerLimit.c', 'SPEAKER_LIMIT_RELEASE_Q16 + 65535u) >> 16)', 'SPEAKER_LIMIT_RELEASE_Q16 + 65535u) >> 6)',
                                 ['limit']),
    'ceiling-default-off': ('SpeakerLimit.c', '    if (ceiling_db > SPEAKER_LIMIT_MAX_DB) ceiling_db = SPEAKER_LIMIT_MAX_DB;\n',
                            '    if (ceiling_db > SPEAKER_LIMIT_MAX_DB) ceiling_db = SPEAKER_LIMIT_MAX_DB;\n    ceiling_db = 0;\n',
                            ['limit', 'core']),
    'core-skips-the-ceiling': ('SpeakerCore.c', 'speaker_limit_run(&c->limit, c->period, (int16_t *)(void *)dst, SpeakerPeriodFrames);',
                               'for (uint32_t i = 0; i < SpeakerPeriodFrames * SpeakerChannels; ++i) ((int16_t *)(void *)dst)[i] = c->period[i];',
                               ['core']),
    'core-keeps-last-run-held': ('SpeakerCore.c', '    speaker_limit_reset(&c->limit);   /*', '    /*', ['core']),
    'quiet-no-smmu-stretch': ('SpeakerCore.c', '    guard(c, "smmu");\n', '', ['core']),
    'quiet-no-i2c-stretch': ('SpeakerCore.c', '    guard(c, "i2c");\n', '', ['core']),
    'quiet-no-tone-stretch': ('SpeakerCore.c', '    guard(c, "tone");\n', '', ['core']),
    'quiet-kept-after-the-amps': ('SpeakerCore.c', '    const int amps = stage_pins(c) && stage_rail(c) && stage_bus(c) && stage_amps(c) && stage_amp_config(c);\n    let_go(c);\n',
                                  '    const int amps = stage_pins(c) && stage_rail(c) && stage_bus(c) && stage_amps(c) && stage_amp_config(c);\n', ['core']),
    'quiet-kept-after-the-graphs': ('SpeakerCore.c', '    const int graphs = stage_smmu(c) && stage_buffer(c) && stage_graphs(c);\n    let_go(c);\n',
                                    '    const int graphs = stage_smmu(c) && stage_buffer(c) && stage_graphs(c);\n', ['core']),
    'quiet-line-not-waited-for': ('SpeakerCore.c', '    sleep_for(c, c->config.crumb_ms);\n', '', ['core']),
    'tone-plays-every-channel': ('SpeakerCore.c', '        if (s->channels & (1u << ch)) continue;\n', '        continue;\n', ['core']),
    'speaker-bare-vote': ('SpeakerHw.c', 'const int voted = sm8750_gcc_vote_set(&gcc, SpeakerGccVoteBits, SPEAKER_GCC_OWNER, &vote);',
                          '(void)gcc; const uint32_t was = gcc_read(&self, SpeakerGccVote); gcc_write(&self, SpeakerGccVote, was | SpeakerGccVoteBits); '
                          'vote.after = gcc_read(&self, SpeakerGccVote); vote.waits = 0; vote.wrote = 1; vote.locked = 1; '
                          'const int voted = (vote.after & SpeakerGccVoteBits) == SpeakerGccVoteBits ? 0 : -5;',
                          ['hw', 'gccvote', 'core']),
    'touch-bare-vote': ('PeripheralIO.h', '    sm8750_gcc_vote_set(gcc,SM8750_GCC_VOTE_TOUCH,SM8750_GCC_OWNER_TOUCH,0);\n',
                        '    peripheral_write(gcc,SM8750_GCC_VOTE,peripheral_read(gcc,SM8750_GCC_VOTE)|SM8750_GCC_VOTE_TOUCH);\n',
                        ['gccvote']),
    'lock-always-taken': ('PeripheralIO.h', 'if (__atomic_compare_exchange_n(&sm8750_gcc_vote_owner,&expected,owner,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {*waits=step;return 0;}',
                          'if (1) {(void)expected;sm8750_gcc_vote_owner=owner;*waits=step;return 0;}', ['gccvote']),
    'lock-stale-read': ('PeripheralIO.h', '        uint32_t now=peripheral_read(gcc,SM8750_GCC_VOTE);\n', '        uint32_t now=r.before;\n',
                        ['gccvote']),
    'lock-waits-forever': ('PeripheralIO.h', 'if (step>=SM8750_GCC_VOTE_STEPS) {*waits=step;return -16;}',
                           'if (step>=SM8750_GCC_VOTE_STEPS*1000u) {*waits=step;return -16;}', ['hw', 'gccvote']),
    'lock-not-given-back': ('PeripheralIO.h', '        if (r.locked) sm8750_gcc_vote_give(owner);\n', '', ['hw', 'gccvote']),
    'lock-unlocked-not-counted': ('PeripheralIO.h', '            if (!r.locked) __atomic_add_fetch(&sm8750_gcc_vote_unlocked,1u,__ATOMIC_RELAXED);\n', '',
                                  ['hw', 'gccvote']),
    'geni-keeps-stale-word': ('GeniI2c.c', '        for (unsigned n = 0; n < 16 && (rd(g, GENI_RX_FIFO_STATUS) & 0x1ffffffu); ++n) (void)rd(g, GENI_RX_FIFO);\n', '',
                              ['amp']),
    'amp-sense-output-on': ('Tfa9873.c', '    if (!c->feedback) {', '    if (c->feedback) {', ['amp', 'core']),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kit', type=Path)
    parser.add_argument('--adsp', type=Path)
    parser.add_argument('--audio', type=Path)
    parser.add_argument('--only')
    args = parser.parse_args()
    kit = check.find(args.kit, [SOURCE.parents[2] / 'UniXNU-sm8750-common/kits/Sm8750Platform'], 'PeripheralIO.h')
    adsp = check.find(args.adsp, [SOURCE.parent / 'PaganiAdsp'], 'PaganiAdsp.h')
    audio = check.find(args.audio, [SOURCE.parent / 'PaganiAudio'], 'PaganiAudioEngine.c')
    survivors = []
    names = [args.only] if args.only else list(MUTANTS)
    with tempfile.TemporaryDirectory(prefix='pagani-speaker-mutants-') as tmp:
        tmp = Path(tmp)
        for name in names:
            file, before, after, guards = MUTANTS[name]
            work = tmp / name
            src, kit_copy = work / 'PaganiSpeaker', work / 'kit'
            shutil.copytree(SOURCE, src, ignore=shutil.ignore_patterns('scripts', '__pycache__'))
            shutil.copytree(kit, kit_copy)
            target = (kit_copy if file == 'PeripheralIO.h' else src) / file
            text = target.read_text()
            if text.count(before) != 1:
                raise SystemExit(f'FAIL {name}: the original text is in {file} {text.count(before)} times (the mutant is stale)')
            target.write_text(text.replace(before, after))
            includes = ['-I', src, '-I', src / 'Test', '-I', kit_copy, '-I', adsp, '-I', audio]
            caught = []
            for test in guards:
                sources = check.tests(audio)[test]
                exe = work / test
                built = subprocess.run(['cc', *check.SANITIZE, *map(str, includes),
                                        *(s if s.startswith('/') else str(src / s) for s in sources), '-lm', '-o', str(exe)],
                                       capture_output=True, text=True)
                if built.returncode:
                    raise SystemExit(f'FAIL {name}: the mutant does not compile for {test}:\n{built.stderr[-2000:]}')
                try:
                    ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
                    failed = ran.returncode != 0
                except subprocess.TimeoutExpired:
                    failed = True
                if failed:
                    caught.append(test)
            print(('ok   ' if caught else 'LIVE ') + f'{name}: caught by {", ".join(caught) or "nothing"}')
            if not caught:
                survivors.append(name)
    if survivors:
        raise SystemExit(f'FAIL: {len(survivors)} mutants survive: ' + ', '.join(survivors))
    print(f'PASS: all {len(names)} mutants of the safety rules are caught')


if __name__ == '__main__':
    main()
