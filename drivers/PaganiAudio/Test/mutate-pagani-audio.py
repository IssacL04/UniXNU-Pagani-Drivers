#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Mutation check of the routing layer's repairs and of the verdict's amplifier and tone rows: each mutant (one source edit) must make check-pagani-audio-host.py fail. The directory is
copied to a temporary one; nothing here is changed. --no-ios: the mutants are about what the host checks see."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
PLUGIN, KERNEL, VERDICT = 'hal/PaganiAudioPlugIn.c', 'PaganiAudio.cpp', 'scripts/sound-verdict.py'
MUTANTS = [
    (KERNEL, 'if (value & 2u) gOptions |= PaganiAudioOptionVaSettings;', 'gOptions |= PaganiAudioOptionVaSettings;', 'the settings without their bit'),
    (KERNEL, 'if (value & 1u) gOptions |= PaganiAudioOptionSrzlRepair;', 'if (value) gOptions |= PaganiAudioOptionSrzlRepair;', 'the repair on any value'),
    (KERNEL, 'if (gOptions & PaganiAudioOptionVaSettings) snprintf(settings', 'snprintf(settings', 'the row changed without the option'),
    (PLUGIN, 'if ((c->options & PaganiAudioOptionVaSettings) && haveActuator()) vaSettingsAsk();', 'vaSettingsAsk();', 'asked without the option'),
    (PLUGIN, 'if ((c->options & PaganiAudioOptionVaSettings) && haveActuator()) vaSettingsAsk();',
     'if (c->options & PaganiAudioOptionVaSettings) vaSettingsAsk();', 'asked without the actuator'),
    (PLUGIN, 'if (!vaImageMatches(h, L->va_uuid)) m->va_settings = PaganiAudioVaSettingsOtherBuild;\n    else if', 'if', 'written into another build'),
    (PLUGIN, 'else if (gVaProduct != L->va_product) m->va_settings = PaganiAudioVaSettingsOtherProduct;\n', '', 'written on another product'),
    (PLUGIN, 'else if (*guard & 1u) m->va_settings = PaganiAudioVaSettingsPresent;\n', '', 'written over settings that are there'),
    (PLUGIN, '*(volatile uint32_t *)(image + (uintptr_t)L->va_settings_kind) = 1;', '', 'the kind word left out'),
    (PLUGIN, "*(volatile uint32_t *)(image + (uintptr_t)L->va_settings_format) = 'dflt';", "*(volatile uint32_t *)(image + (uintptr_t)L->va_settings_format) = 'dfat';", 'another format word'),
    (PLUGIN, '__atomic_store_n(guard, (uint8_t)1, __ATOMIC_RELEASE);', '', 'the guard left open'),
    (PLUGIN, 'if (atomic_load(&gVaSettingsAsked)) vaSettingsGive(h);', '', 'an image that arrives after Initialize'),
    (PLUGIN, 'if (h) vaSettingsGive(h);', '', 'an image known before Initialize'),
    (PLUGIN, 'if (!m || atomic_exchange(&gVaSettingsDone, 1)) return;', 'if (!m) return;', 'more than once'),
    (PLUGIN, 'once == ~0ull || (once & 3u) == 2u ? PaganiAudioMxDone', 'once == ~0ull ? PaganiAudioMxDone', "dispatch_once's generation form"),
    (PLUGIN, '(once & 3u) == 2u ? PaganiAudioMxDone', '(once & 3u) ? PaganiAudioMxDone', 'a waiter taken for done'),
    (PLUGIN, '.va_settings_guard = 0x5d2c78ull', '.va_settings_guard = 0x5d2c70ull', 'the guard address'),
    (PLUGIN, '.va_settings_kind = 0x5d2bd8ull', '.va_settings_kind = 0x5d2bdcull', 'the kind address'),
    (PLUGIN, '.va_settings_format = 0x5d2c28ull', '.va_settings_format = 0x5d2c2cull', 'the format address'),
    (VERDICT, " or a.get('sws', '1') != '1'", '', 'an output stage that does not switch passes'),
    (VERDICT, " or a.get('ampen', '1') != '1'", '', 'an amplifier not enabled passes'),
    (VERDICT, " or a.get('ampen', '1') != '1'", " or a.get('ampen', '1') != '1' or a.get('tdmerr', '0') != '0'", 'a sticky bit taken for a stop'),
    (VERDICT, " or not number(t.get('out'))", '', 'a silent tone passes'),
    (VERDICT, "if log.arg('pagani-speaker') == 1 and log.arg('pagani-audio-tone'):", "if log.arg('pagani-speaker') == 1:", 'a tone asked of a run without one'),
]


def main():
    survivors = []
    with tempfile.TemporaryDirectory(prefix='pagani-audio-mutants-') as tmp:
        for index, (name, old, new, what) in enumerate(MUTANTS):
            work = Path(tmp) / f'm{index}'
            shutil.copytree(SOURCE, work, ignore=shutil.ignore_patterns('__pycache__'))
            text = (work / name).read_text()
            if old not in text:
                raise SystemExit(f'FAIL: mutant {index} ({what}) does not apply')
            (work / name).write_text(text.replace(old, new, 1))
            result = subprocess.run([sys.executable, '-B', work / 'scripts/check-pagani-audio-host.py', '--no-ios'], capture_output=True, text=True)
            print(f'{"survived" if result.returncode == 0 else "killed  "} {index:2} {what}', flush=True)
            if result.returncode == 0:
                survivors.append(what)
    if survivors:
        raise SystemExit(f'FAIL: {len(survivors)} of {len(MUTANTS)} mutants survived: {survivors}')
    print(f'PASS mutants: all {len(MUTANTS)} killed')


if __name__ == '__main__':
    main()
