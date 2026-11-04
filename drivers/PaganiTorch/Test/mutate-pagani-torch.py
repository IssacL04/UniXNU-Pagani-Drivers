#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Mutation check of PaganiTorch's safety rules: each mutant (one source edit) must make a host test fail. Sources are copied to a temporary
directory; nothing here is changed."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
SANITIZE = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
OBJC = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-objc-method-access', '-fno-objc-arc', '-O1', '-g', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', '-DPAGANI_TORCH_PLUGIN_HOST', '-framework', 'Foundation', '-framework', 'CoreFoundation', '-lobjc',
        '-Wl,-export_dynamic']
GATE = 'TorchSpmi.c'
CORE = 'TorchCore.c'
PLUGIN = 'ios/TorchPlugin.m'
IOS = 'TorchIos.c'
MUTANTS = [
    # the gate
    (GATE, 'return ((map >> 8) & 0xfffu) == p->ppid && owner == s->ee;', 'return ((map >> 8) & 0xfffu) == p->ppid;', 'owner re-check'),
    (GATE, 'return ((map >> 8) & 0xfffu) == p->ppid && owner == s->ee;', 'return owner == s->ee;', 'mapping re-check'),
    (GATE, 'return (mask >> ch & 1u) && (v & PGT_ITARGET_MASK) <= s->itarget_max;', 'return (mask >> ch & 1u);', 'target current cap in the gate'),
    (GATE, 'return (mask >> ch & 1u) && (v & PGT_ITARGET_MASK) <= s->itarget_max;', 'return (v & PGT_ITARGET_MASK) <= s->itarget_max;', 'target current: board channels only'),
    (GATE, 'return (mask >> ch & 1u) && v == 0;', 'return (mask >> ch & 1u);', 'safety timer value'),
    (GATE, 'return (mask >> ch & 1u) && v == 0;', 'return v == 0;', 'timer: board channels only'),
    (GATE, 'return (mask >> ch & 1u) && (v & PGT_STROBE_FIELD) == PGT_STROBE_SW_TORCH;', 'return (mask >> ch & 1u);', 'strobe value'),
    (GATE, 'return (mask >> ch & 1u) && (v & PGT_STROBE_FIELD) == PGT_STROBE_SW_TORCH;', 'return (v & PGT_STROBE_FIELD) == PGT_STROBE_SW_TORCH;', 'strobe: board channels only'),
    (GATE, 'return !(v & (uint8_t)~PGT_MODULE_EN);', 'return 1;', 'module enable value'),
    (GATE, 'return (v & mask) == mask;', 'return 1;', '5 mA steps'),
    (GATE, 'if (v & (uint8_t)~mask) return 0;', '', 'CHAN_EN: board channels only'),
    (GATE, 'const uint8_t ready = s->ires_set ? (uint8_t)(s->itarget_set & s->strobe_set & s->timer_set) : 0;', 'const uint8_t ready = (uint8_t)(s->itarget_set & s->strobe_set & s->timer_set);', 'interlock: resolution'),
    (GATE, '(uint8_t)(s->itarget_set & s->strobe_set & s->timer_set)', '(uint8_t)(s->strobe_set & s->timer_set)', 'interlock: target'),
    (GATE, '(uint8_t)(s->itarget_set & s->strobe_set & s->timer_set)', '(uint8_t)(s->itarget_set & s->timer_set)', 'interlock: strobe'),
    (GATE, '(uint8_t)(s->itarget_set & s->strobe_set & s->timer_set)', '(uint8_t)(s->itarget_set & s->strobe_set)', 'interlock: timer'),
    (GATE, '    s->ires_set = 0;\n    s->itarget_set = s->strobe_set = s->timer_set = 0;\n', '', 'open clears the interlock'),
    (GATE, '    default:\n        return 0;\n    }\n}', '    default:\n        return 1;\n    }\n}', 'register whitelist'),
    (GATE, 'if (!s || !data || len != 1 || periph != PGT_FLASH) return 0;', 'if (!s || !data || !len || periph != PGT_FLASH) return 0;', 'single bytes'),
    (GATE, 'if (s->closed || !p->identified || p->owner != s->ee ||', 'if (s->closed || !p->identified ||', 'open: owner'),
    (GATE, 'if (s->closed || !p->identified || p->owner != s->ee ||', 'if (s->closed || p->owner != s->ee ||', 'open: identified'),
    (GATE, "    if (periph >= PGT_PERIPHS || !s->p[periph].writable || s->closed ||", "    if (periph >= PGT_PERIPHS || !s->p[periph].writable ||", 'closed bus'),
    (GATE, 'if (!s || !s->io || !s->p[PGT_FLASH].writable || s->emergency_used) return E_PERM;', 'if (!s || !s->io || !s->p[PGT_FLASH].writable) return E_PERM;', 'one emergency stop'),
    (GATE, '    s->emergency_used = 1;\n    s->closed = 1;\n', '    s->emergency_used = 1;\n', 'emergency stop closes the bus'),
    (GATE, '    const int rc2 = write_transaction(s, PGT_FLASH, PGT_FLASH_EN_CTL, &zero, 1);\n    return rc ? rc : rc2;', '    return rc;', 'emergency stop: the module too'),
    (GATE, "        const uint32_t status = io->chan_read(io->ctx, periph, PGT_ARB_STATUS);\n        if (status & 0xeu) {", "        const uint32_t status = io->chan_read(io->ctx, periph, PGT_ARB_STATUS);\n        if (status & 0x0u) {", 'write status errors'),
    (GATE, 'if (p->apid < 0 || (p->owner != ee && owner == ee)) {', 'if (p->apid < 0) {', 'owned mapping preferred'),
    (GATE, '    note_written(s, reg);\n    return 0;', '    return 0;', 'interlock records writes'),
    # the core
    (CORE, 'if (p->type != PGT_FLASH_TYPE) return "not-a-flash-module";', '', 'identity: type'),
    (CORE, 'if (p->subtype != PGT_FLASH_SUBTYPE_4CH) return "not-the-4-channel-module";', '', 'identity: subtype'),
    (CORE, 'if (p->owner != t->cfg.ee) return "channel-not-owned";', '', 'owner verdict'),
    (CORE, '    if (!verdict) verdict = dump_problem(t);\n', '', 'reserved EN_CTL bits refused early'),
    (CORE, '    if (k->mode == PGT_MODE_DRY) {\n        if (!verdict) probe_channel(t);', '    if (0) {\n        if (!verdict) probe_channel(t);', 'dry run writes nothing'),
    (CORE, '        if (!verdict) probe_channel(t);', '        probe_channel(t);', 'dry-run channel probe only when it would not refuse'),
    (CORE, 'if (hard_ma > PGT_TORCH_POLICY_MA) hard_ma = PGT_TORCH_POLICY_MA;', '', 'config: hard cap 200'),
    (CORE, 'if (cap > hard_ma) cap = hard_ma;', '', 'config: runtime cap under the hard cap'),
    (CORE, 'unsigned cap = max_ma_arg ? max_ma_arg : PGT_DEFAULT_MA;', 'unsigned cap = max_ma_arg ? max_ma_arg : 200u;', 'default 50 mA'),
    (CORE, 'if (k->hard_ma > PGT_TORCH_POLICY_MA) k->hard_ma = PGT_TORCH_POLICY_MA;', '', 'core: hard cap 200'),
    (CORE, 'if (k->max_ma > k->hard_ma) k->max_ma = k->hard_ma;', '', 'core: runtime cap'),
    (CORE, 'if (!k->auto_off_ms || k->auto_off_ms > PGT_AUTO_OFF_MS) k->auto_off_ms = PGT_AUTO_OFF_MS;', '', 'core: 5 minutes'),
    (CORE, 'uint32_t pgt_test_ms(unsigned arg) {return arg < PGT_TEST_MAX_MS ? arg : PGT_TEST_MAX_MS;}', 'uint32_t pgt_test_ms(unsigned arg) {return arg;}', 'test at most 1000 ms'),
    (CORE, '    return code > PGT_ITARGET_MASK ? PGT_ITARGET_MASK : (unsigned)code;', '    return (unsigned)code;', 'code limited to the field'),
    (CORE, '    if (code > t->code_max) code = t->code_max;\n', '', 'level never above the cap'),
    (CORE, '    if (n >= t->deadline_us) {\n        rc = torch_off(t);', '    if (0) {\n        rc = torch_off(t);', 'auto-off'),
    (CORE, '    } else if (hold_ms && n + (uint64_t)hold_ms * 1000u < t->deadline_us) {\n        t->deadline_us = n + (uint64_t)hold_ms * 1000u;\n    }', '    } else {\n        t->deadline_us = n + (uint64_t)t->cfg.auto_off_ms * 1000u;\n    }', 'a level change does not extend'),
    (CORE, 'if (hold_ms && hold_ms < ms) ms = hold_ms;', '', 'the test hold'),
    (CORE, '    const char *f = fault_of(t, t->sts);\n    if (f) {\n        fault(t, f, E_IO);\n        return 0;\n    }\n', '', 'poll: fault bits'),
    (CORE, '    const char *f = fault_of(t, t->sts);\n    return f ? fault(t, f, E_IO) : 0;', '    return 0;', 'on: fault bits at once'),
    (CORE, 'if ((mask >> ch & 1u) && (s[0] >> (2u * ch) & 1u)) return "short-circuit";', 'if ((s[0] >> (2u * ch) & 1u)) return "short-circuit";', 'short circuit of board channels only'),
    (CORE, 'if (s[1] & (PGT_STS2_OTST1 | PGT_STS2_OTST2 | PGT_STS2_BOB_THM_OVLD)) return "over-temperature";', '', 'over temperature'),
    (CORE, 'if (s[1] & (PGT_STS2_BOB_ILIM_S1 | PGT_STS2_BOB_ILIM_S2 | PGT_STS2_BCL_IBAT)) return "over-current";', '', 'over current'),
    (CORE, 'if (s[1] & PGT_STS2_VPH_LOW) return "vph-low";', '', 'low input voltage'),
    (CORE, 'if ((mask >> ch & 1u) && (s[2] >> (2u * ch) & 1u)) return "safety-timer";', '', 'safety timer status'),
    (CORE, 'const int stop = t->spmi.p[PGT_FLASH].writable ? pgt_spmi_emergency_stop(&t->spmi) : E_NODEV;', 'const int stop = E_NODEV;', 'emergency stop on fault'),
    (CORE, '    if (t->state == PGT_ST_FAULT) return E_IO;\n', '', 'off until reboot'),
    (CORE, '    if (rc || bad) return fault(t, rc ? "verify-read" : "verify", rc ? rc : E_IO);\n', '', 'read-back before the channels go on'),
    (CORE, 'int bad = !rc && (v & mask) != mask;', 'int bad = 0;', 'read-back: resolution'),
    (CORE, 'bad = !rc && ((v & PGT_ITARGET_MASK) != code || code > t->code_max);', 'bad = 0;', 'read-back: target'),
    (CORE, 'bad = !rc && v != 0;', 'bad = 0;', 'read-back: timer'),
    (CORE, 'bad = !rc && (v & PGT_STROBE_FIELD) != PGT_STROBE_SW_TORCH;', 'bad = 0;', 'read-back: strobe'),
    (CORE, 'bad = !rc && !(v & PGT_MODULE_EN);', 'bad = 0;', 'read-back: module'),
    (CORE, '    if ((en & mask) || (ctl & PGT_MODULE_EN)) return fault(t, "off-verify", E_IO);\n', '', 'off is verified'),
    (CORE, 'if (!rc && (en & mask)) rc = wr(t, PGT_FLASH_CHAN_EN, (uint8_t)(en & ~mask));\n    if (!rc && (ctl & PGT_MODULE_EN)) rc = wr(t, PGT_FLASH_EN_CTL, (uint8_t)(ctl & ~PGT_MODULE_EN));\n    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc; ++ch) {\n        if (!(mask >> ch & 1u)) continue;\n        uint8_t it = 0;',
     'if (!rc && (ctl & PGT_MODULE_EN)) rc = wr(t, PGT_FLASH_EN_CTL, (uint8_t)(ctl & ~PGT_MODULE_EN));\n    for (unsigned ch = 0; ch < PGT_CHANNELS && !rc; ++ch) {\n        if (!(mask >> ch & 1u)) continue;\n        uint8_t it = 0;', 'off: channels first'),
    (CORE, '    rc = wr(t, PGT_FLASH_CHAN_EN, 0);\n', '    rc = 0;\n', 'init: channels off'),
    (CORE, 'if (!rc && (t->en_ctl & PGT_MODULE_EN)) rc = wr(t, PGT_FLASH_EN_CTL, (uint8_t)(t->en_ctl & ~PGT_MODULE_EN));', '', 'init: module off'),
    (CORE, '(t->cfg.log == 1 && t->event_lines >= EVENT_FIRST && n - t->event_line_us < EVENT_GAP_US)', '0', 'log ration'),
    # the iOS rules
    (IOS, 'if (torch != 1) return refuse(plan, "needs pagani-torch=1 (the driver running)");', '', 'ios: needs the driver on'),
    (IOS, 'if (!driver_ready) return refuse(final, "the torch driver is not running");', '', 'ios: no button without the driver'),
    (IOS, 'if (!plugin_visible) return refuse(final, "the plug-in is not on the root");', '', 'ios: no button without the plug-in'),
    (IOS, 'return scalar <= LEVEL_MAX ? (int)scalar : -22;', 'return (int)scalar;', 'ios: level range'),
    # the plug-in
    (PLUGIN, 'if (!is_flashlight_client(client)) return gCopyDevice(self, cmd, client, inform, error);', '', 'plug-in: other clients keep the vendor'),
    (PLUGIN, 'if (![device isKindOfClass:[PaganiTorchDevice class]]) return gCopyStream(self, cmd, position, type, device);', '', 'plug-in: other devices keep their streams'),
    (PLUGIN, 'if (![device isKindOfClass:[PaganiTorchDevice class]]) gTakeBack(self, cmd, device, client, inform, prefers);', 'gTakeBack(self, cmd, device, client, inform, prefers);', 'plug-in: the token never reaches the vendor'),
    (PLUGIN, 'return [device isKindOfClass:[PaganiTorchDevice class]] ? YES : gActive(self, cmd, device);', 'return gActive(self, cmd, device);', 'plug-in: available with the token'),
    (PLUGIN, 'return [device isKindOfClass:[PaganiTorchDevice class]] ? YES : gActive(self, cmd, device);', 'return YES;', 'plug-in: others answered by the vendor'),
    (PLUGIN, '[description isEqualToString:@"FigFlashlight"]', 'YES', 'plug-in: only FigFlashlight'),
    (PLUGIN, '        [gClients removeObject:@(client)];\n', '', 'plug-in: unregistered clients are forgotten'),
    (PLUGIN, 'if (!key || !CFEqual(key, gTorchLevelKey) || ![value respondsToSelector:@selector(floatValue)]) return PGT_PLUGIN_UNSUPPORTED;', '', 'plug-in: only TorchLevel'),
    (PLUGIN, 'return rc ? PGT_PLUGIN_FAILED : 0;', 'return 0;', 'plug-in: failures reach the caller'),
    (PLUGIN, 'if (!methods[i]) return -2;', '', 'plug-in: all six or none'),
    (PLUGIN, 'if (!vendor || !key || !*key) return -1;', 'if (!vendor) return -1;\n    if (!key) key = &gTorchLevelKey;', 'plug-in: no key, nothing'),
    (PLUGIN, 'if (!(value > 0.0f)) return 0;      // also NaN', 'if (value == 0.0f) return 0;', 'plug-in: NaN and negatives are off'),
    (PLUGIN, 'return level ? level : 1;', 'return level;', 'plug-in: above 0 is on'),
    (PLUGIN, 'if (value >= 1.0f) return 1000;', '', 'plug-in: at most 1000'),
]


def tests(work):
    cc = os.environ.get('CC', 'cc')
    for test, sources in (('spmi', ['Test/spmi.c', 'TorchSpmi.c']), ('core', ['Test/core.c', 'TorchCore.c', 'TorchSpmi.c']), ('ios', ['Test/ios.c', 'TorchIos.c'])):
        yield [cc, *SANITIZE, *(str(work / s) for s in sources), '-o', str(work / test)], work / test
    for name, define in (('plugin', []), ('plugin-no-method', ['-DNO_METHOD']), ('plugin-no-key', ['-DNO_KEY', '-DPGT_PLUGIN_KEY_SYMBOL="kNoSuchSymbolAnywhere"'])):
        yield [cc, *OBJC, *define, str(work / 'Test/plugin.m'), str(work / 'ios/TorchPlugin.m'), '-o', str(work / name)], work / name


def killed_by(work):
    for build, exe in tests(work):
        built = subprocess.run(build, capture_output=True, text=True)
        if built.returncode or subprocess.run([str(exe)], capture_output=True, text=True).returncode:
            return True
    return False


def main():
    survivors = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for index, (name, old, new, what) in enumerate(MUTANTS):
            work = tmp / f'm{index}'
            shutil.copytree(SOURCE, work, ignore=shutil.ignore_patterns('*.o', '__pycache__', 'scripts'))
            text = (work / name).read_text()
            if old not in text:
                raise SystemExit(f'FAIL: mutant {index} ({what}) does not apply')
            (work / name).write_text(text.replace(old, new, 1))
            killed = killed_by(work)
            shutil.rmtree(work)
            print(f'{"killed  " if killed else "SURVIVED"} {what}', flush=True)
            if not killed:
                survivors.append(what)
    if survivors:
        raise SystemExit('FAIL: mutants survived: ' + ', '.join(survivors))
    print(f'PASS mutate: {len(MUTANTS)} mutants, all killed')


if __name__ == '__main__':
    main()
