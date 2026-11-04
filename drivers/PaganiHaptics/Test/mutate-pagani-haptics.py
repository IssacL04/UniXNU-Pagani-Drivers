#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Mutation check of PaganiHaptics' safety rules: each mutant (one source edit) must make a host test fail. Sources are copied to a
temporary directory; nothing here is changed."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
SANITIZE = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
MUTANTS = [
    ('HapticsSpmi.c', 'return ((map >> 8) & 0xfffu) == p->ppid && owner == s->ee;', 'return ((map >> 8) & 0xfffu) == p->ppid;', 'owner re-check'),
    ('HapticsSpmi.c', 'return ((map >> 8) & 0xfffu) == p->ppid && owner == s->ee;', 'return owner == s->ee;', 'mapping re-check'),
    ('HapticsSpmi.c', 'case PGH_CFG_VMAX: return v <= s->vmax_code_max;', 'case PGH_CFG_VMAX: return 1;', 'VMAX hard cap in the bus'),
    ('HapticsSpmi.c', 'return 0;   /* HBOOST, SDAM: never */', 'return 1;   /* HBOOST, SDAM: never */', 'HBOOST/SDAM read only'),
    ('HapticsSpmi.c', "        default: return 0;\n        }\n    }\n    if (periph == PGH_PTN)", "        default: return 1;\n        }\n    }\n    if (periph == PGH_PTN)", 'CFG register whitelist'),
    ('HapticsSpmi.c', 'return !(v & 0x70u) && (v & PGH_PAT_SRC_MASK) <= PGH_SRC_DIRECT;', 'return 1;', 'SPMI_PLAY sources'),
    ('HapticsSpmi.c', 'if (s->closed || !p->identified || p->owner != s->ee ||', 'if (s->closed || !p->identified ||', 'open: owner'),
    ('HapticsSpmi.c', 'if (s->closed || !p->identified || p->owner != s->ee ||', 'if (s->closed || p->owner != s->ee ||', 'open: identified'),
    ('HapticsSpmi.c', "    if (periph >= PGH_PERIPHS || !s->p[periph].writable || s->closed ||", "    if (periph >= PGH_PERIPHS || !s->p[periph].writable ||", 'closed bus'),
    ('HapticsSpmi.c', 'if (!s || !s->io || !s->p[PGH_CFG].writable || s->emergency_used) return E_PERM;', 'if (!s || !s->io || !s->p[PGH_CFG].writable) return E_PERM;', 'one emergency stop'),
    ('HapticsSpmi.c', "        const uint32_t status = io->chan_read(io->ctx, periph, PGH_ARB_STATUS);\n        if (status & PGH_ARB_FAILED) {", "        const uint32_t status = io->chan_read(io->ctx, periph, PGH_ARB_STATUS);\n        if (status & 0x0u) {", 'write status errors'),
    ('HapticsSpmi.c', 'if (p->apid < 0 || (p->owner != ee && owner == ee)) {', 'if (p->apid < 0) {', 'owned mapping preferred'),
    ('HapticsCore.c', 'if (cfg->type != PGH_HAPTICS_TYPE || cfg->subtype != PGH_CFG_SUBTYPE_HAP530) return "cfg-type";', 'if (cfg->type != PGH_HAPTICS_TYPE) return "cfg-type";', 'CFG subtype'),
    ('HapticsCore.c', 'if (cfg->owner != h->cfg.ee) return "cfg-owner";', '', 'CFG owner verdict'),
    ('HapticsCore.c', 'if (ptn->rev[1] != PGH_HAP530_MAJOR) return "ptn-revision";', '', 'PTN revision'),
    ('HapticsCore.c', '    if (c->mode == PGH_MODE_DRY) {\n        if (!problem) probe_channel(h);', '    if (0) {\n        if (!problem) probe_channel(h);', 'dry run writes nothing'),
    ('HapticsCore.c', "    if (mv > cap) {\n        mv = cap;", "    if (0) {\n        mv = cap;", 'runtime cap'),
    ('HapticsCore.c', 'if (h->clamp_5v && cap > PGH_CLAMPED_VMAX_MV) cap = PGH_CLAMPED_VMAX_MV;', '', 'boost 5 V clamp'),
    ('HapticsCore.c', 'if (percent >= 0 && percent < low) cap = h->cfg.vbat_low_mv;', '', 'low battery'),
    ('HapticsCore.c', 'if (left < MIN_BUDGET_MS) {', 'if (0) {', 'budget'),
    ('HapticsCore.c', "    h->deadline = h->t_start + (uint64_t)ms * 1000u;\n    play_line(h, \"constant\", permille, ms);", "    h->deadline = h->t_start + (uint64_t)ms * 10000u;\n    play_line(h, \"constant\", permille, ms);", 'play length'),
    ('HapticsCore.c', 'int rc = wr(h, PGH_CFG, PGH_CFG_SPMI_PLAY, (uint8_t)(h->src | (h->brake_on ? PGH_BRAKE_EN : 0)));', 'int rc = wr(h, PGH_CFG, PGH_CFG_SPMI_PLAY, (uint8_t)(PGH_PLAY_EN | h->src));', 'stop clears PLAY_EN'),
    ('HapticsCore.c', '    uint32_t n = h->fifo_len - fill;', '    uint32_t n = h->fifo_len;', 'FIFO free space'),
    ('HapticsCore.c', "    if (v & PGH_FAULT_SC) {\n        fault(h, \"short circuit\", E_IO);", "    if (0) {\n        fault(h, \"short circuit\", E_IO);", 'short circuit'),
    ('HapticsCore.c', '} else if (!n && t - h->t_empty >= (uint64_t)UNDERRUN_MS * 1000u) {', '} else if (0) {', 'underrun stop'),
    ('HapticsCore.c', 'if ((vreg & PGH_HBST_VREG_ON) && !(follow & PGH_FOLLOW_HW_EN)) {', 'if (0) {', 'boost in use'),
    ('HapticsCore.c', 'const int stop = h->spmi.p[PGH_CFG].writable ? pgh_spmi_emergency_stop(&h->spmi) : E_NODEV;', 'const int stop = E_NODEV;', 'emergency stop on fault'),
    ('HapticsCore.c', "    if (!(h->ending && n == avail)) n &= ~3u;\n    if (n) {", "    if (n) {", '4-byte writes mid-stream'),
    ('HapticsCore.c', 'if (first - tail <= head - tail) ring_advance(h, first - tail);', 'if (0) ring_advance(h, first - tail);', 'old samples dropped'),
    ('HapticsCore.c', 'if (!rc && (v & PGH_PLAY_EN)) rc = wr(h, PGH_CFG, PGH_CFG_SPMI_PLAY, 0);', '', 'running play stopped at init'),
    ('HapticsCore.c', '        if (!problem) probe_channel(h);', '        probe_channel(h);', 'dry-run channel probe only when it would not refuse'),
    ('HapticsSpmi.c', 'if (rc != E_IO || !s->retry_dropped ||', 'if (rc != E_IO ||', 'a drop goes again only with the option'),
    ('HapticsSpmi.c', ' || tries >= PGH_ARB_DROP_RETRIES ||', ' ||', 'bounded repeats'),
    ('HapticsSpmi.c', '(s->last_status & PGH_ARB_FAILED) != PGH_ARB_DROPPED', '!(s->last_status & PGH_ARB_DROPPED)', 'only a drop alone goes again'),
    ('HapticsSpmi.c', '        if (!channel_ours(s, &s->p[periph])) return E_PERM;\n        const int rc = write_transaction', '        const int rc = write_transaction', 'owner re-check before a repeat'),
    ('HapticsCore.c', 'h->spmi.retry_dropped = c->spmi_retry ? 1 : 0;', 'h->spmi.retry_dropped = 1;', 'the repeats without the option'),
    ('HapticsCore.c', 'h->spmi.retry_dropped = c->spmi_retry ? 1 : 0;', '', 'the option reaches the bus'),
]


def main():
    cc = os.environ.get('CC', 'cc')
    survivors = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for index, (name, old, new, what) in enumerate(MUTANTS):
            work = tmp / f'm{index}'
            shutil.copytree(SOURCE, work, ignore=shutil.ignore_patterns('*.o', '__pycache__'))
            text = (work / name).read_text()
            if old not in text:
                raise SystemExit(f'FAIL: mutant {index} ({what}) does not apply')
            (work / name).write_text(text.replace(old, new, 1))
            killed = False
            for test, sources in (('spmi', ['Test/spmi.c', 'HapticsSpmi.c']), ('core', ['Test/core.c', 'HapticsCore.c', 'HapticsSpmi.c'])):
                exe = work / test
                built = subprocess.run([cc, *SANITIZE, *(str(work / s) for s in sources), '-o', str(exe)], capture_output=True, text=True)
                if built.returncode or subprocess.run([str(exe)], capture_output=True, text=True).returncode:
                    killed = True
                    break
            print(f'{"killed  " if killed else "SURVIVED"} {what}')
            if not killed:
                survivors.append(what)
    if survivors:
        raise SystemExit('FAIL: mutants survived: ' + ', '.join(survivors))
    print(f'PASS mutate: {len(MUTANTS)} mutants, all killed')


if __name__ == '__main__':
    main()
