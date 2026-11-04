#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Mutants of PaganiBattery's rules that matter on the phone: what opens a read of the USB blocks, the SM8750 offsets, the waits, what the
boot argument turns on, the charger's readings, the average at boot. Each is applied to a copy of the sources, must still compile and must
make at least one host test fail. A mutant that survives means the tests do not guard that rule.

  mutate-pagani-battery.py [--only NAME]"""
import argparse
import importlib.util
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
spec = importlib.util.spec_from_file_location('check', HERE / 'check-pagani-battery-host.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

# name: (file, original text, mutated text, tests that must catch it)
MUTANTS = {
    # the SM8550 offsets, the trap the report warns of
    'gcc-sm8550-aggre': ('BatteryUsb.c', 'kGccAggre = 0x39090', 'kGccAggre = 0x39088', ['usb']),
    'gcc-sm8550-utmi': ('BatteryUsb.c', 'kGccUtmi = 0x3902c', 'kGccUtmi = 0x39028', ['usb']),
    'gcc-sm8550-sleep': ('BatteryUsb.c', 'kGccSleep = 0x39028', 'kGccSleep = 0x39024', ['usb']),
    # step 4's gate, condition by condition
    'gate-reset-ignored': ('BatteryUsb.c', '    if (g->bcr & kBcrAsserted) return "reset";\n', '', ['usb']),
    'gate-pwr-on-ignored': ('BatteryUsb.c', '    if (!(g->gdsc & kGdscPwrOn)) return "gdsc";\n', '', ['usb']),
    'gate-collapse-ignored': ('BatteryUsb.c', '    if (g->gdsc & kGdscCollapse) return "collapse";\n', '', ['usb']),
    'gate-cfg-up-ignored': ('BatteryUsb.c', 'if (!(g->cfg & kCfgUpDone) || (g->cfg & kCfgDownDone)) return "gdsc-cfg";',
                            'if (g->cfg & kCfgDownDone) return "gdsc-cfg";', ['usb']),
    'gate-cfg-down-ignored': ('BatteryUsb.c', 'if (!(g->cfg & kCfgUpDone) || (g->cfg & kCfgDownDone)) return "gdsc-cfg";',
                              'if (!(g->cfg & kCfgUpDone)) return "gdsc-cfg";', ['usb']),
    'gate-root-ignored': ('BatteryUsb.c', '    if (g->master_rcg & kRootOff) return "root";\n', '', ['usb']),
    'gate-utmi-root-ignored': ('BatteryUsb.c', '    if (g->utmi_rcg & kRootOff) return "utmi-root";\n', '', ['usb']),
    'gate-master-ignored': ('BatteryUsb.c', '    if (!branch_on(g->master)) return "master";\n', '', ['usb']),
    'gate-aggre-ignored': ('BatteryUsb.c', '    if (!gated_on(g->aggre)) return "aggre";\n', '', ['usb']),
    'gate-noc-ignored': ('BatteryUsb.c', '    if (!gated_on(g->noc)) return "noc";\n', '', ['usb']),
    'gate-utmi-ignored': ('BatteryUsb.c', '    if (!branch_on(g->utmi)) return "utmi";\n', '', ['usb']),
    'gate-sleep-ignored': ('BatteryUsb.c', '    if (!branch_on(g->sleep)) return "sleep";\n', '', ['usb']),
    'gate-clock-off-ignored': ('BatteryUsb.c', 'static int branch_on(uint32_t v) {return (v & kClkEnable) && !(v & kClkOff);}',
                               'static int branch_on(uint32_t v) {return (v & kClkEnable) != 0;}', ['usb']),
    'gate-gating-excuses-all': ('BatteryUsb.c', 'static int branch_on(uint32_t v) {return (v & kClkEnable) && !(v & kClkOff);}',
                                'static int branch_on(uint32_t v) {return (v & kClkEnable) && (!(v & kClkOff) || (v & kClkHwCtl));}', ['usb']),
    'gate-gating-without-enable': ('BatteryUsb.c', 'static int gated_on(uint32_t v) {return (v & kClkEnable) && (!(v & kClkOff) || (v & kClkHwCtl));}',
                                   'static int gated_on(uint32_t v) {return (v & (kClkEnable | kClkHwCtl)) && (!(v & kClkOff) || (v & kClkHwCtl));}', ['usb']),
    'gate-gating-refused': ('BatteryUsb.c', 'static int gated_on(uint32_t v) {return (v & kClkEnable) && (!(v & kClkOff) || (v & kClkHwCtl));}',
                            'static int gated_on(uint32_t v) {return (v & kClkEnable) && !(v & kClkOff);}', ['usb']),
    'gate-closed-goes-on': ('BatteryUsb.c', '        r->stage = StageDone;\n        return kNext;\n    }\n    pbat_text_str(&t, " next=dwc");',
                            '    }\n    pbat_text_str(&t, " next=dwc");', ['usb']),
    'tcsr-with-the-gcc': ('BatteryUsb.c', '    r->phy_bcr = rd(r, PbuWinGcc, kGccPhyBcr);\n', '    r->phy_bcr = rd(r, PbuWinGcc, kGccPhyBcr);\n    (void)rd(r, PbuWinTcsr, kTcsrUsb2Ref);\n', ['usb']),
    # the snapshot at the driver's start
    'entry-reads-tcsr': ('BatteryUsb.c', '    gcc_read(r, "gcc0", &r->entry);\n', '    gcc_read(r, "gcc0", &r->entry);\n    (void)rd(r, PbuWinTcsr, kTcsrUsb2Ref);\n', ['usb']),
    'entry-not-compared': ('BatteryUsb.c', 'r->gcc_same = r->entry_read && gcc_same(&r->entry, &g);', 'r->gcc_same = r->entry_read;', ['usb']),
    'entry-gated-status-counts': ('BatteryUsb.c', '!((a->noc ^ b->noc) & ~kClkOff)', 'a->noc == b->noc', ['usb']),
    # the id
    'id-not-checked': ('BatteryUsb.c', 'r->id_ok = product == 0x5533 || product == 0x3331 || product == 0x3332;', 'r->id_ok = 1;', ['usb']),
    # the PHY's gate
    'phy-rails-ignored': ('BatteryUsb.c', 'const char *closed = !r->rails_ok ? "rail" : (r->phy_bcr', 'const char *closed = (r->phy_bcr', ['usb']),
    'phy-rail-status-ignored': ('BatteryUsb.c', 'if (rails[i].phy && (rc || !(status & kStatusInRegulation))) ok = 0;', 'if (rails[i].phy && rc) ok = 0;', ['usb']),
    'phy-rail-failure-ignored': ('BatteryUsb.c', 'if (rails[i].phy && (rc || !(status & kStatusInRegulation))) ok = 0;',
                                 'if (rails[i].phy && !rc && !(status & kStatusInRegulation)) ok = 0;', ['usb']),
    'phy-repeater-rails-gate': ('BatteryUsb.c', '{0x1cf, 0, "l15b"}', '{0x1cf, 1, "l15b"}', ['usb']),
    'phy-ref-ignored': ('BatteryUsb.c', '    if (!(ref & 1u)) {\n', '    if (0) {\n', ['usb']),
    'phy-reset-ignored': ('BatteryUsb.c', '!r->rails_ok ? "rail" : (r->phy_bcr & kBcrAsserted) ? "reset" : 0', '!r->rails_ok ? "rail" : 0', ['usb']),
    # the order and the waits: the line must reach the UFS log before a read that can kill the phone
    'no-wait-before-dwc': ('BatteryUsb.c', '    r->stage = StageDwc;\n    return PBU_SETTLE_MS;', '    r->stage = StageDwc;\n    return kNext;', ['usb']),
    'no-wait-before-qscratch': ('BatteryUsb.c', '    r->stage = StageQscratch;\n    return PBU_SETTLE_MS;', '    r->stage = StageQscratch;\n    return kNext;', ['usb']),
    'no-wait-before-tcsr': ('BatteryUsb.c', '    r->stage = StageRef;\n    return PBU_SETTLE_MS;', '    r->stage = StageRef;\n    return kNext;', ['usb']),
    'no-wait-before-phy': ('BatteryUsb.c', '    r->stage = StagePhy;\n    return PBU_SETTLE_MS;', '    r->stage = StagePhy;\n    return kNext;', ['usb']),
    'no-wait-before-banks': ('BatteryUsb.c', 'return r->smmu_banks ? PBU_SETTLE_MS : 0;', 'return r->smmu_banks ? kNext : 0;', ['usb']),
    'banks-with-the-table': ('BatteryUsb.c', '    pbat_text_kd(&t, "found", found);\n    finish(r);\n', '    pbat_text_kd(&t, "found", found);\n    finish(r);\n    for (uint32_t i = 0; i < r->smmu_banks; ++i) smmu_bank(r, r->smmu_bank[i]);\n', ['usb']),
    'settle-shorter-than-ufs': ('BatteryUsb.h', '#define PBU_SETTLE_MS 4000u', '#define PBU_SETTLE_MS 2000u', ['usb']),
    'no-hold-at-all': ('BatteryUsb.c', 'if (can_hang(r->stage) && !r->held) {', 'if (0) {', ['usb']),
    'no-hold-for-dwc': ('BatteryUsb.c', 'return stage == StageDwc || stage == StageQscratch', 'return stage == StageQscratch', ['usb']),
    'no-hold-for-qscratch': ('BatteryUsb.c', 'stage == StageDwc || stage == StageQscratch ||', 'stage == StageDwc ||', ['usb']),
    'no-hold-for-tcsr': ('BatteryUsb.c', 'stage == StageRef || stage == StagePhy', 'stage == StagePhy', ['usb']),
    'no-hold-for-phy': ('BatteryUsb.c', 'stage == StagePhy || stage == StageBanks', 'stage == StageBanks', ['usb']),
    'no-hold-for-banks': ('BatteryUsb.c', 'stage == StagePhy || stage == StageBanks', 'stage == StagePhy', ['usb']),
    'hold-with-no-time-to-end-a-round': ('BatteryUsb.h', '#define PBU_QUIET_MS 1000u', '#define PBU_QUIET_MS 400u', ['usb']),
    'hold-not-let-go': ('BatteryUsb.c', '        r->held = 0;\n        r->io->quiet(r->io->ctx, 0);\n', '        r->held = 0;\n', ['usb']),
    'gcc-before-pmic': ('BatteryUsb.c', 'enum {StageRails, StagePmic, StageSmmu, StageGcc, StageDwc,', 'enum {StageGcc, StageRails, StagePmic, StageSmmu, StageDwc,', ['usb']),
    # the host mode's registers only in host mode
    'xhci-in-device-mode': ('BatteryUsb.c', 'if (((r->gctl >> 12) & 3u) == kRoleHost) {\n        const uint32_t cap', 'if (1) {\n        const uint32_t cap', ['usb']),
    # the verdict
    'route-ignores-siddq': ('BatteryUsb.c', 'r->phy_read && !(r->phy_common0 & kPhySiddq) && !(r->phy_ctrl5 & kPhyPor)', 'r->phy_read && !(r->phy_ctrl5 & kPhyPor)', ['usb']),
    'route-ignores-por': ('BatteryUsb.c', 'r->phy_read && !(r->phy_common0 & kPhySiddq) && !(r->phy_ctrl5 & kPhyPor)', 'r->phy_read && !(r->phy_common0 & kPhySiddq)', ['usb']),
    'route-ignores-phy-read': ('BatteryUsb.c', 'if (role == kRoleDevice && r->phy_read && ', 'if (role == kRoleDevice && ', ['usb']),
    # the SMMU table
    'smmu-mask-ignored': ('BatteryUsb.c', '((smr ^ kSid) & 0xffffu & ~((smr >> 16) & 0x7fffu))', '((smr ^ kSid) & 0xffffu)', ['usb']),
    'smmu-valid-ignored': ('BatteryUsb.c', 'if (!(smr & 0x80000000u) || ((smr ^ kSid)', 'if (((smr ^ kSid)', ['usb']),
    'smmu-bank-unchecked': ('BatteryUsb.c', 'if (!((s2cr >> 16) & 3u) && (s2cr & 255u) < banks)', 'if (!((s2cr >> 16) & 3u))', ['usb']),
    'smmu-bypass-has-a-bank': ('BatteryUsb.c', 'if (!((s2cr >> 16) & 3u) && (s2cr & 255u) < banks)', 'if ((s2cr & 255u) < banks)', ['usb']),
    'smmu-past-128-groups': ('BatteryUsb.c', 'if (count > kSmmuGroups) count = kSmmuGroups;\n', '', ['usb']),
    'smmu-geometry-unchecked': ('BatteryUsb.c', 'if ((idr1 >> 31) || !count || pages > 128 || banks < 2 || banks > 128) {', 'if (!count) {', ['usb']),
    # a line that leaves its U record
    'line-beyond-the-record': ('BatteryUsb.h', '#define PBU_LINE 200u', '#define PBU_LINE 240u', ['usb']),
    # the charger's readings
    'plug-bit-only': ('BatterySpmi.h', '(c->reg[PbsUsbinRt] & 0x14u) == 0x10u', '(c->reg[PbsUsbinRt] & 0x10u) == 0x10u', ['spmi', 'core']),
    'plug-any-bit': ('BatterySpmi.h', '(c->reg[PbsUsbinRt] & 0x14u) == 0x10u', 'c->reg[PbsUsbinRt] != 0', ['spmi']),
    'stage-with-the-high-bits': ('BatterySpmi.h', 'return c->reg[PbsChgrStatus] & 7u;', 'return c->reg[PbsChgrStatus];', ['spmi', 'core']),
    'terminate-counts-as-charging': ('BatterySpmi.h', 'stage <= PbsStageTaper', 'stage <= PbsStageTerminate', ['spmi', 'core']),
    'trickle-not-charging': ('BatterySpmi.h', 'stage >= PbsStageTrickle &&', 'stage >= PbsStagePre &&', ['spmi', 'core']),
    'limit-other-register': ('BatterySpmi.c', '{0x729, 0x52}', '{0x729, 0x50}', ['spmi']),
    'rt-other-register': ('BatterySpmi.c', '{0x729, 0x10}, {0x729, 0x06}', '{0x729, 0x06}, {0x729, 0x10}', ['spmi']),
    'one-failed-read-still-a-sample': ('BatterySpmi.c', '        if (rc) return rc;\n    }\n    return 0;\n}\n\nvoid pagani_spmi_charger_text', '        (void)rc;\n    }\n    return 0;\n}\n\nvoid pagani_spmi_charger_text', ['spmi', 'core']),
    'map-takes-the-last-owner': ('BatterySpmi.c', 'if (apid[i] < 0 || (io->cnfg(io->ctx, 4u * n) & 7u) == 0) apid[i] = (int)n;', 'apid[i] = (int)n;', ['spmi']),
    # the core
    'average-counts-at-boot': ('BatteryCore.c', '(settled && a_ok && a >= kChargingMa)', '(a_ok && a >= kChargingMa)', ['core']),
    'average-never-counts': ('BatteryCore.c', '(settled && a_ok && a >= kChargingMa)', '0', ['core']),
    'settle-too-short': ('BatteryCore.c', 'kAverageSettleMs = 60000', 'kAverageSettleMs = 10000', ['core']),
    'pmic-used-when-only-logged': ('BatteryCore.c', 'gf && c->cfg.pmic >= 2 && pmic_fresh(c, now)', 'gf && c->cfg.pmic >= 1 && pmic_fresh(c, now)', ['core']),
    'pmic-stale-still-used': ('BatteryCore.c', 'gf && c->cfg.pmic >= 2 && pmic_fresh(c, now)', 'gf && c->cfg.pmic >= 2 && c->pmic_seen', ['core']),
    'pmic-before-dsp': ('BatteryCore.c', '    if (bf && BH(PbmUsbOnline)) {\n        s->sources |= PbatSrcCharger;', '    if (bf && BH(PbmUsbOnline) && !(gf && c->cfg.pmic >= 2 && pmic_fresh(c, now))) {\n        s->sources |= PbatSrcCharger;', ['core']),
    'pmic-read-by-default': ('BatteryCore.c', 'const int pmic = c->cfg.pmic && c->ops->pmic;', 'const int pmic = c->ops->pmic != 0;', ['core']),
    'pmic-one-sample-moves-vbus': ('BatteryCore.c', 'kPmicVotes = 2,', 'kPmicVotes = 1,', ['core']),
    'pmic-full-below-the-top': ('BatteryCore.c', 's->fully_charged = s->external && !s->charging && known && s->percent >= kTopPercent;', 's->fully_charged = s->external && !s->charging;', ['core']),
    'pmic-full-while-charging': ('BatteryCore.c', 's->fully_charged = s->external && !s->charging && known', 's->fully_charged = s->external && known', ['core']),
    'pmic-plug-lines-unlimited': ('BatteryCore.c', '        if (c->pmic_plug_lines < kPmicPlugLines) {\n', '        if (1) {\n', ['core']),
    'pmic-change-lines-unlimited': ('BatteryCore.c', '        if (c->pmic_change_lines < kPmicChangeLines) {\n', '        if (1) {\n', ['core']),
    'pmic-trend-without-vbus': ('BatteryCore.c', 'if (!why && c->pmic_plug && now', 'if (!why && now', ['core']),
    'pmic-trend-unlimited': ('BatteryCore.c', ' && c->pmic_trend_lines < kPmicTrendLines) {', ') {', ['core']),
    # the boot argument
    'mode-any-value-on': ('BatteryCore.c', 'return (value & PBAT_MODE_ON) && value <= PBAT_MODE_ALL ? value : 0;', 'return value;', ['core']),
    'mode-unknown-bits-on': ('BatteryCore.c', 'return (value & PBAT_MODE_ON) && value <= PBAT_MODE_ALL ? value : 0;', 'return (value & PBAT_MODE_ON) ? value : 0;', ['core']),
    'mode-even-on': ('BatteryCore.c', 'return (value & PBAT_MODE_ON) && value <= PBAT_MODE_ALL ? value : 0;', 'return value <= PBAT_MODE_ALL ? value : 0;', ['core']),
    'mode-use-without-the-log-bit': ('BatteryCore.c', 'return mode & PBAT_MODE_PMIC_USE ? 2 : mode & PBAT_MODE_PMIC_LOG ? 1 : 0;', 'return mode & PBAT_MODE_PMIC_LOG ? 1 : 0;', ['core']),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--only')
    args = parser.parse_args()
    survivors = []
    names = [args.only] if args.only else list(MUTANTS)
    with tempfile.TemporaryDirectory(prefix='pagani-battery-mutants-') as tmp:
        tmp = Path(tmp)
        for name in names:
            file, before, after, guards = MUTANTS[name]
            src = tmp / name / 'PaganiBattery'
            shutil.copytree(SOURCE, src, ignore=shutil.ignore_patterns('scripts', '__pycache__'))
            target = src / file
            text = target.read_text()
            if text.count(before) != 1:
                raise SystemExit(f'FAIL {name}: the original text is in {file} {text.count(before)} times (the mutant is stale)')
            target.write_text(text.replace(before, after))
            caught = []
            for test in guards:
                exe = tmp / name / test
                # a condition taken out leaves a parameter unused: that warning is the mutant's, not the test's business
                built = subprocess.run(['cc', *check.SANITIZE, '-Wno-error', *(str(src / s) for s in check.TESTS[test]), '-o', str(exe)],
                                       capture_output=True, text=True)
                if built.returncode:
                    raise SystemExit(f'FAIL {name}: the mutant does not compile for {test}:\n{built.stderr[-2000:]}')
                try:
                    failed = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300).returncode != 0
                except subprocess.TimeoutExpired:
                    failed = True
                if failed:
                    caught.append(test)
            print(('ok   ' if caught else 'LIVE ') + f'{name}: caught by {", ".join(caught) or "nothing"}', flush=True)
            if not caught:
                survivors.append(name)
            shutil.rmtree(tmp / name, ignore_errors=True)
    if survivors:
        raise SystemExit(f'FAIL: {len(survivors)} mutants survive: ' + ', '.join(survivors))
    print(f'PASS: all {len(names)} mutants of the rules are caught')


if __name__ == '__main__':
    main()
