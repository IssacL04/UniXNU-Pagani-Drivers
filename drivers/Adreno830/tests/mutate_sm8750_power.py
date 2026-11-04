#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation check for tests/test_sm8750_power.c.

Each mutant is a regression the power module must never ship with (wrong order, a dropped wait, an
error ignored, a source stopped under an RCG). The script builds the host test against every mutant
of platform/sm8750/Sm8750GpuPower.c and fails if any mutant still passes, or if a mutation no longer
applies to the source. Run it after changing the module or the test.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'platform/sm8750/Sm8750GpuPower.c'

PLL_THEN_RCG = '''    trace->step = Sm8750GpuStepPll0;
    if ((s = pll0(c)))
        return s;

    /* gmu_clk_src = PLL0_OUT_EVEN / 1 (500 MHz); hub_clk_src = GPLL0 / 3 (200 MHz). */
    trace->step = Sm8750GpuStepRcg;
    if ((s = rcg(c, GmuRcg, 0x201)) || (s = rcg(c, HubRcg, 0x505)))
        return s;
'''
RCG_THEN_PLL = '''    trace->step = Sm8750GpuStepRcg;
    if ((s = rcg(c, GmuRcg, 0x201)) || (s = rcg(c, HubRcg, 0x505)))
        return s;
    trace->step = Sm8750GpuStepPll0;
    if ((s = pll0(c)))
        return s;
'''
PARK = '''    trace->step = Sm8750GpuStepOffRcg;
    if ((s = park(c, HubRcg, Sm8750GpuOffHubRcgNotParked)) || (s = park(c, GmuRcg, Sm8750GpuOffGmuRcgNotParked)))
        return s;
'''
PLL_OFF = '''    trace->step = Sm8750GpuStepOffPll0;
    if ((s = update(c, Sm8750GpuGpuCc, Pll0Mode, PllOutctrl, 0)) ||
        (s = update(c, Sm8750GpuGpuCc, Pll0UserCtl, PllOutputs, 0)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0Opmode, PllStandby)))
        return s;
'''
OUTPUTS_THEN_OUTCTRL = '''        (s = update(c, Sm8750GpuGpuCc, Pll0UserCtl, PllOutputs, PllOutputs)))
        return s;
    return update(c, Sm8750GpuGpuCc, Pll0Mode, PllOutctrl, PllOutctrl);'''
OUTCTRL_THEN_OUTPUTS = '''        (s = update(c, Sm8750GpuGpuCc, Pll0Mode, PllOutctrl, PllOutctrl)))
        return s;
    return update(c, Sm8750GpuGpuCc, Pll0UserCtl, PllOutputs, PllOutputs);'''

# name: [(original text, replacement)], each original must occur exactly once
MUTANTS = {
    'rcg_before_pll0': [(PLL_THEN_RCG, RCG_THEN_PLL)],
    'pll0_running_always_rejected': [(
        '        return expect(c, Sm8750GpuGpuCc, Pll0Mode, PllLockDet | PllResetN | PllOutctrl,\n'
        '            PllLockDet | PllResetN | PllOutctrl);',
        '        return Sm8750GpuPowerUnexpectedState;')],
    'pll0_enabled_if_either_bit': [(
        '    if ((mode & PllOutctrl) && (opmode & PllRun)) {', '    if ((mode & PllOutctrl) || (opmode & PllRun)) {')],
    'pll0_always_reprogrammed': [('    if ((mode & PllOutctrl) && (opmode & PllRun)) {', '    if (0) {')],
    'pll0_not_stopped_before_writes': [(
        '    if ((s = update(c, Sm8750GpuGpuCc, Pll0Mode, PllOutctrl, 0)) ||\n'
        '        (opmode != PllStandby && (s = wr(c, Sm8750GpuGpuCc, Pll0Opmode, PllStandby))))\n        return s;\n', '')],
    'pll_outputs_after_outctrl': [(OUTPUTS_THEN_OUTCTRL, OUTCTRL_THEN_OUTPUTS)],
    'branch_never_waits': [('    if (s || halt == HaltSkip)\n        return s;', '    if (s || 1)\n        return s;')],
    'branch_ignores_noc_fsm': [(
        '    return on ? !(value & CbcrClkOff) || (value & CbcrNocFsm) == CbcrNocFsmOn : !!(value & CbcrClkOff);',
        '    return on ? !(value & CbcrClkOff) : !!(value & CbcrClkOff);')],
    'branch_any_noc_fsm_bit': [(
        '    return on ? !(value & CbcrClkOff) || (value & CbcrNocFsm) == CbcrNocFsmOn : !!(value & CbcrClkOff);',
        '    return on ? !(value & CbcrClkOff) || (value & CbcrNocFsmOn) : !!(value & CbcrClkOff);')],
    'ignore_read_errors': [(
        '    if (c->io->read32(c->io->cookie, block, offset, value)) {',
        '    if (c->io->read32(c->io->cookie, block, offset, value) && 0) {')],
    'ignore_write_errors': [(
        '    return c->io->write32(c->io->cookie, block, offset, value) ? Sm8750GpuPowerIoError : Sm8750GpuPowerOk;',
        '    (void)c->io->write32(c->io->cookie, block, offset, value);\n    return Sm8750GpuPowerOk;')],
    'read_error_keeps_old_value': [('        c->trace->value = ~0u;\n', '')],
    'branches_before_gdsc': [(
        '    trace->step = Sm8750GpuStepCxGdsc;',
        '    for (size_t i = 0; i < WaitedCbcrs; i++)\n'
        '        if ((s = branch(c, Sm8750GpuGpuCc, waited_cbcrs[i], 1, HaltWait)))\n            return s;\n'
        '    trace->step = Sm8750GpuStepCxGdsc;')],
    'gdsc_not_awaited': [('(s = poll(c, Sm8750GpuGpuCc, CxGdsHwCtrl, GdscPwrOn, GdscPwrOn, 2000)) || ', '')],
    'gdsc_no_retain_ff': [(
        ' ||\n        (s = update(c, Sm8750GpuGpuCc, CxGdscr, GdscRetainFf, GdscRetainFf)))', ')')],
    'rcg_root_not_forced': [(
        '    Sm8750GpuPowerStatus s = update(c, Sm8750GpuGpuCc, cmd, RcgRootEn, RcgRootEn);\n    if (!s)\n'
        '        s = poll(c, Sm8750GpuGpuCc, cmd, RcgRootOff, 0, 500);\n    if (!s)\n'
        '        s = update(c, Sm8750GpuGpuCc, cmd + 4, RcgCfgMask, cfg);',
        '    Sm8750GpuPowerStatus s = update(c, Sm8750GpuGpuCc, cmd + 4, RcgCfgMask, cfg);')],
    'gpll0_rate_unchecked': [(
        '        (s = expect(c, Sm8750GpuGcc, GccGpll0L, 0xffff, 0x1f)) ||\n'
        '        (s = expect(c, Sm8750GpuGcc, GccGpll0Alpha, 0xffff, 0x4000)))', '        0)')],
    'gmu_bcr_not_released': [(
        'if ((s = deassert(c, CxBcr)) || (s = deassert(c, GmuBcr)))', 'if ((s = deassert(c, CxBcr)))')],
    'cfg_ahb_without_readback_and_delay': [(
        '    if (!(value & CbcrEnable) && ((s = wr(c, Sm8750GpuGcc, GccGpuCfgAhb, value | CbcrEnable)) ||\n'
        '        (s = rd(c, Sm8750GpuGcc, GccGpuCfgAhb, &value)) || (s = wait_us(c, 10))))',
        '    if (!(value & CbcrEnable) && ((s = wr(c, Sm8750GpuGcc, GccGpuCfgAhb, value | CbcrEnable))))')],
    'cfg_ahb_always_written': [('    if (!(value & CbcrEnable) && ((s = wr(', '    if (1 && ((s = wr(')],
    'off_rcgs_not_parked': [(PARK, '')],
    'off_rcgs_parked_after_sources': [(PARK, ''), (
        "    trace->step = Sm8750GpuStepOffGdsc;", PARK + "    trace->step = Sm8750GpuStepOffGdsc;")],
    'off_park_loses_rate': [('    return wr(c, Sm8750GpuGpuCc, cmd + 4, cfg);', '    return Sm8750GpuPowerOk;')],
    'off_pll_left_running': [(PLL_OFF, '')],
    'off_cxo_timeout_fatal': [('        if (s == Sm8750GpuPowerTimeout) {', '        if (s == Sm8750GpuPowerTimeout && 0) {')],
    'off_gdsc_not_given_time': [('(s = wait_us(c, 500)) ||\n', '\n')],
}


def build_and_run(directory, source):
    (directory / 'platform/sm8750').mkdir(parents=True)
    (directory / 'tests').mkdir()
    shutil.copy(ROOT / 'platform/sm8750/Sm8750GpuPower.h', directory / 'platform/sm8750/')
    (directory / 'platform/sm8750/Sm8750GpuPower.c').write_text(source)
    shutil.copy(ROOT / 'tests/test_sm8750_power.c', directory / 'tests/')
    built = subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Wno-unused-function',
                            '-fsanitize=address,undefined', 'tests/test_sm8750_power.c',
                            'platform/sm8750/Sm8750GpuPower.c', '-o', 't'], cwd=directory, capture_output=True, text=True)
    if built.returncode:
        return 'build failed: ' + built.stderr.strip().splitlines()[-1]
    ran = subprocess.run(['./t'], cwd=directory, capture_output=True, text=True, timeout=120)
    return 'survived' if ran.returncode == 0 else 'killed'


def main():
    original = SOURCE.read_text()
    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        if build_and_run(base / 'original', original) != 'survived':
            print('FAIL: the unmutated module does not pass its test')
            return 1
        for name, edits in MUTANTS.items():
            source = original
            for before, after in edits:
                if source.count(before) != 1:
                    failures.append(f'{name}: mutation no longer applies')
                    break
                source = source.replace(before, after)
            else:
                verdict = build_and_run(base / name, source)
                print(f'{name:40s} {verdict}')
                if verdict != 'killed':
                    failures.append(f'{name}: {verdict}')
    for failure in failures:
        print('FAIL', failure)
    if not failures:
        print(f'PASS: {len(MUTANTS)} power-module mutants killed by test_sm8750_power')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
