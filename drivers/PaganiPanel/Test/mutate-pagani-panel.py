#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""PaganiPanel's bit 8 (the formal path): each criterion as a source change one of the host tests must catch. The sources are copied to a temporary directory; nothing here changes the
tree. A mutant that still compiles and passes means the tests do not guard that line. Host tests do not establish that anything works on the phone.

    python3 Test/mutate-pagani-panel.py [--platform <kits/Sm8750Platform>] [--package <device package>] [--jobs N] [index ...]
"""
import argparse
import concurrent.futures
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PANEL = HERE.parent
DRIVERS = PANEL.parent
TOP = DRIVERS.parents[1]
P, B, K = 'PanelProbe.h', 'PanelBrightness.h', 'PaganiPanel.cpp'
M = [
 # the clock path
 (P, 'if (careful) {\n        panel_probe_enter(p, "ahb", PANEL_BLOCK_DISP);', 'if (1) {\n        panel_probe_enter(p, "ahb", PANEL_BLOCK_DISP);'),
 (P, 'if (careful) {\n        panel_probe_enter(p, "ahb", PANEL_BLOCK_DISP);', 'if (0) {\n        panel_probe_enter(p, "ahb", PANEL_BLOCK_DISP);'),
 (P, 'else if (!panel_probe_a1_write(p, careful)) return 0;\n    if (careful) panel_probe_again', 'else (void)panel_probe_a1_write(p, careful);\n    if (careful) panel_probe_again'),
 (P, 'else if (!panel_probe_a1_write(p, careful)) return 0;\n    if (careful) panel_probe_again', 'else if (!panel_probe_a1_write(p, 1)) return 0;\n    if (careful) panel_probe_again'),
 (P, 'else if (!panel_probe_a1_write(p, careful)) return 0;\n    if (careful) panel_probe_again', 'else if (!panel_probe_a1_write(p, 0)) return 0;\n    if (careful) panel_probe_again'),
 (P, 'if (careful) panel_probe_again(p, "ahb", PANEL_BLOCK_DSI);', 'if (1) panel_probe_again(p, "ahb", PANEL_BLOCK_DSI);'),
 (P, 'if (careful) panel_probe_again(p, "ahb", PANEL_BLOCK_DSI);', ''),
 (P, 'if (g.ahb_on && g.gdsc_on) return 1;', 'if (g.ahb_on) return 1;'),
 (P, 'if (g.ahb_on && g.gdsc_on) return 1;', 'if (g.gdsc_on) return 1;'),
 (P, 'if (g.ahb_on && g.gdsc_on) return 1;', 'return 1;'),
 (P, 'if (s.ahb & 1) PANEL_PROBE_LOG(p, "panel: ahb %x already on", s.ahb);\n    else if', 'if'),
 (P, '    if (careful) {\n        panel_probe_flush(p);\n        panel_probe_a1_read(p, &s);', '    if (0) {\n        panel_probe_flush(p);\n        panel_probe_a1_read(p, &s);'),
 (P, '    if (careful) {\n        panel_probe_flush(p);\n        panel_probe_a1_read(p, &s);', '    if (1) {\n        panel_probe_flush(p);\n        panel_probe_a1_read(p, &s);'),
 (P, 'else if (!panel_probe_a1_write(p, 1)) return 0;\n    p->dispcc = 1;', 'else if (!panel_probe_a1_write(p, 0)) return 0;\n    p->dispcc = 1;'),
 (P, '>> 2 & 1;', '>> 1 & 1;'),
 (P, 'rises += level && !last;', 'rises += level;'),
 (P, '        last = level;\n    }\n    return rises;', '    }\n    return rises;'),
 (P, '        next = now + PANEL_PROBE_PHASE_TICKS;\n        const uint32_t level', '        const uint32_t level'),
 (P, 'p->host->quiet(p->host->context, 1);\n    p->host->sleep_ms(p->host->context, PANEL_PROBE_QUIET_MS);', 'p->host->sleep_ms(p->host->context, PANEL_PROBE_QUIET_MS);'),
 (P, 'p->host->quiet(p->host->context, 1);\n    p->host->sleep_ms(p->host->context, PANEL_PROBE_QUIET_MS);', 'p->host->quiet(p->host->context, 1);'),
 (P, '{0x0af08000, 0x1000, PANEL_PROBE_A1 | PANEL_PROBE_AHB, "ahb"}', '{0x0af08000, 0x1000, PANEL_PROBE_A1, "ahb"}'),
 (P, '{0x0ae00000, 0x1000, PANEL_PROBE_A1, "mdss"}', '{0x0ae00000, 0x1000, PANEL_PROBE_A1 | PANEL_PROBE_AHB, "mdss"}'),
 (P, '{0x0f156000, 0x1000, PANEL_PROBE_P0 | PANEL_PROBE_A1, "te"}', '{0x0f156000, 0x1000, PANEL_PROBE_P0 | PANEL_PROBE_A1 | PANEL_PROBE_AHB, "te"}'),
 # the options
 (B, 'if (pagani_panel_ahb_start(options)) return !first_dcs && pagani_panel_ahb_slider(options);', 'if (pagani_panel_ahb_start(options)) return pagani_panel_ahb_slider(options);'),
 (B, 'if (pagani_panel_ahb_start(options)) return !first_dcs && pagani_panel_ahb_slider(options);', 'if (pagani_panel_ahb_start(options)) return !first_dcs;'),
 (B, 'if (pagani_panel_ahb_start(options)) return !first_dcs && pagani_panel_ahb_slider(options);\n', ''),
 (B, 'return pagani_panel_ahb_start(options) && !(options & PAGANI_PANEL_OPTION_A0_DCS); }', 'return pagani_panel_ahb_start(options) && (options & PAGANI_PANEL_OPTION_A0_DCS); }'),
 (B, 'return pagani_panel_ahb_start(options) && !(options & PAGANI_PANEL_OPTION_A0_DCS); }', 'return pagani_panel_ahb_start(options); }'),
 (B, 'return pagani_panel_ahb_start(options) && (options & PAGANI_PANEL_OPTION_A0_DCS); }', 'return (options & PAGANI_PANEL_OPTION_A0_DCS) != 0; }'),
 (B, 'return pagani_panel_probe_stages(options) || pagani_panel_gcc_ahb(options) || pagani_panel_ahb_start(options); }', 'return pagani_panel_probe_stages(options) || pagani_panel_gcc_ahb(options); }'),
 (B, 'PAGANI_PANEL_OPTION_AHB_START = 256,', 'PAGANI_PANEL_OPTION_AHB_START = 8192,'),
 # the gate and the send path
 (B, 'if (formal && !r->ahb && !r->held) r->held = PAGANI_PANEL_DCS_HELD_AHB;\n', ''),
 (B, 'r->checked = (uint8_t)formal;', 'r->checked = 0;'),
 (B, 'r->checked = (uint8_t)formal;', 'r->checked = 1;'),
 (B, 'formal ? options | PAGANI_PANEL_OPTION_NO_FORCE : options', 'options'),
 (B, 'formal ? options | PAGANI_PANEL_OPTION_NO_FORCE : options', 'options | PAGANI_PANEL_OPTION_NO_FORCE'),
 (B, '    if (r->checked && !panel_probe_dsi_shape(peripheral_read(io, 0x0), peripheral_read(io, 0x4), peripheral_read(io, 0x84))) {\n        r->held = PAGANI_PANEL_DCS_HELD_LOST;\n        return -19;\n    }\n', ''),
 (B, '        r->held = PAGANI_PANEL_DCS_HELD_LOST;\n        return -19;\n    }\n    return dsi_panel_packet_mode', '        return -19;\n    }\n    return dsi_panel_packet_mode'),
 (B, 'peripheral_read(io, 0x0), peripheral_read(io, 0x4), peripheral_read(io, 0x84)', 'peripheral_read(io, 0x0), peripheral_read(io, 0x4), 4'),
 (B, 'peripheral_read(io, 0x0), peripheral_read(io, 0x4), peripheral_read(io, 0x84)', 'peripheral_read(io, 0x0), 5, peripheral_read(io, 0x84)'),
 (B, 'peripheral_read(io, 0x0), peripheral_read(io, 0x4), peripheral_read(io, 0x84)', '0x20090000, peripheral_read(io, 0x4), peripheral_read(io, 0x84)'),
 # the look at what the command left
 (B, 'if (after->version != before->version || !panel_probe_dsi_shape(after->version, after->ctrl, after->trigger)) return "shape";', 'if (!panel_probe_dsi_shape(after->version, after->ctrl, after->trigger)) return "shape";'),
 (B, 'if (after->version != before->version || !panel_probe_dsi_shape(after->version, after->ctrl, after->trigger)) return "shape";', 'if (after->version != before->version) return "shape";'),
 (B, 'after->ctrl != before->ctrl || after->trigger', 'after->trigger'),
 (B, 'after->trigger != before->trigger || after->dma', 'after->dma'),
 (B, 'after->dma != before->dma || after->clocks', 'after->clocks'),
 (B, ' || after->clocks != before->clocks) return "config";', ') return "config";'),
 (B, '    if (after->tpg) return "tpg";\n', ''),
 (B, '    if (after->status & 2) return "busy";\n', ''),
 (B, 'if (after->status & 2) return "busy";', 'if (after->status & 4) return "busy";'),
 (B, 'if (after->timeout & ~before->timeout & PAGANI_DSI_TIMEOUTS) return "timeout";', 'if (after->timeout & PAGANI_DSI_TIMEOUTS) return "timeout";'),
 (B, 'if (after->timeout & ~before->timeout & PAGANI_DSI_TIMEOUTS) return "timeout";', 'if (after->timeout & ~before->timeout) return "timeout";'),
 (B, 'if (after->timeout & ~before->timeout & PAGANI_DSI_TIMEOUTS) return "timeout";\n', ''),
 (B, 'if (after->fifo & ~before->fifo & PAGANI_DSI_FIFO_ERRORS) return "fifo";', 'if (after->fifo & PAGANI_DSI_FIFO_ERRORS) return "fifo";'),
 (B, 'if (after->fifo & ~before->fifo & PAGANI_DSI_FIFO_ERRORS) return "fifo";', 'if (after->fifo & ~before->fifo) return "fifo";'),
 (B, 'if (after->fifo & ~before->fifo & PAGANI_DSI_FIFO_ERRORS) return "fifo";\n', ''),
 (B, 'if (after->ints & ~before->ints & PAGANI_DSI_ERROR) return "error";', 'if (after->ints & PAGANI_DSI_ERROR) return "error";'),
 (B, 'if (after->ints & ~before->ints & PAGANI_DSI_ERROR) return "error";\n', ''),
 (B, 'PAGANI_DSI_TIMEOUTS = 0x111', 'PAGANI_DSI_TIMEOUTS = 0x11'),
 (B, 'PAGANI_DSI_FIFO_ERRORS = 0xcccc0480u', 'PAGANI_DSI_FIFO_ERRORS = 0xcccc0080u'),
 (B, 'PAGANI_DSI_ERROR = 1u << 24', 'PAGANI_DSI_ERROR = 1u << 20'),
 # around the first DCS
 (B, '    if (!r->ahb) return 0;\n', ''),
 (B, '    if (pagani_panel_ahb_careful(options)) panel_probe_flush(&p);\n', ''),
 (B, '    if (pagani_panel_ahb_careful(options)) panel_probe_flush(&p);\n', '    panel_probe_flush(&p);\n'),
 (B, '    return panel_probe_frames(&p);\n', '    return 0;\n'),
 (B, '        still = panel_probe_frames(&p);\n        why = pagani_panel_readback', '        still = frames;\n        why = pagani_panel_readback'),
 (B, '        why = pagani_panel_readback(seen, &after);\n', ''),
 (B, '        if (!why && frames && !still) why = "stream";\n', ''),
 (B, '        if (!why && frames && !still) why = "stream";\n', '        if (!why && !still) why = "stream";\n'),
 (B, '        if (why) rc = -5;\n', ''),
 (B, '    if (rc && !r->held) r->held = PAGANI_PANEL_DCS_HELD_LOST;\n', ''),
 (B, '    if (!rc && !pagani_panel_ahb_slider(options)) r->held = PAGANI_PANEL_DCS_HELD_ONE;\n', ''),
 (B, '    if (!rc && !pagani_panel_ahb_slider(options)) r->held = PAGANI_PANEL_DCS_HELD_ONE;\n', '    if (!rc) r->held = PAGANI_PANEL_DCS_HELD_ONE;\n'),
 (B, '    host->quiet(host->context, 0);\n    return rc;', '    return rc;'),
 (B, 'const char *why = rc ? "no read" : 0;', 'const char *why = 0;'),
 # the kernel's side
 (K, 'ahb=formal && setDispAhb();', 'ahb=formal;'),
 (K, 'ahb=formal && setDispAhb();', 'ahb=setDispAhb();'),
 (K, 'const bool formal=pagani_panel_ahb_start(options),ahb=formal && setDispAhb();\n', 'const bool formal=pagani_panel_ahb_start(options);bool ahb=false;\n'),
 (K, '        run.ahb=ahb;\n', ''),
 (K, 'if (!added) {if (formal) panelProbeQuiet(nullptr,0);observe', 'if (!added) {observe'),
 (K, 'frames=pagani_panel_ahb_before(&host,&run,options);', 'pagani_panel_ahb_before(&host,&run,options);'),
 (K, '            frames=pagani_panel_ahb_before(&host,&run,options);\n', ''),
 (K, 'if (formal) rc=pagani_panel_ahb_after(frames,&host,&run,options,rc,&seen);', 'if (formal) pagani_panel_ahb_after(frames,&host,&run,options,rc,&seen);'),
 (K, '        if (formal) rc=pagani_panel_ahb_after(frames,&host,&run,options,rc,&seen);\n', ''),
 (K, 'panelProbeHost(host);host.context=earlyLines[0];host.io', 'panelProbeHost(host);host.io'),
 (K, 'i==PANEL_BLOCK_AHB?0:kIOMapReadOnly', '0'),
 (K, 'on=panel_probe_ahb(&probe,pagani_panel_ahb_careful(options)?1u:0u)!=0;}\n        for (PeripheralWindow &window:windows) window.release();', 'on=panel_probe_ahb(&probe,pagani_panel_ahb_careful(options)?1u:0u)!=0;}'),
 (K, 'on=panel_probe_ahb(&probe,pagani_panel_ahb_careful(options)?1u:0u)!=0;}', 'on=panel_probe_ahb(&probe,1u)!=0;}'),
 (K, 'on=panel_probe_ahb(&probe,pagani_panel_ahb_careful(options)?1u:0u)!=0;}', 'on=panel_probe_ahb(&probe,0u)!=0;}'),
 (K, 'panelProbeMap(windows,host,PANEL_PROBE_AHB)', 'panelProbeMap(windows,host,PANEL_PROBE_A1)'),
 (K, 'else __atomic_fetch_and(&pagani_panel_probe_quiet,~1u,__ATOMIC_RELEASE);', 'else __atomic_store_n(&pagani_panel_probe_quiet,0u,__ATOMIC_RELEASE);'),
 (K, 'if (on) __atomic_fetch_or(&pagani_panel_probe_quiet,1u,__ATOMIC_RELEASE);', 'if (on) __atomic_store_n(&pagani_panel_probe_quiet,1u,__ATOMIC_RELEASE);'),
 (K, 'if (run.held) panelLog(earlyLines[1],', 'if (0) panelLog(earlyLines[1],'),
 (K, 'panelLog(earlyLines[0],"panel: ahb map failed %s",unmapped->name);', ';'),
]


def trimmed_probe(text):
    # Only A1 and bit 8 here: the other sections of the probe's test take long, and the regression runs them.
    old = 'healthy(); dsiZero(); gccOff(); dispccZero(); gates(); third(); preempted(); stages(); a0();\n'
    if old not in text:
        raise SystemExit('FAIL: the sections of Test/panel-probe.cpp changed: trimmed_probe needs a look')
    return text.replace(old, '')


def run(args):
    index, tmp, platform, package = args
    name, old, new = M[index]
    work = Path(tempfile.mkdtemp(prefix=f'm{index:03d}-', dir=tmp))
    panel = work / 'drivers/PaganiPanel'
    shutil.copytree(PANEL, panel, ignore=shutil.ignore_patterns('__pycache__'))
    (work / 'drivers/BootProgress').mkdir()
    shutil.copy(DRIVERS / 'BootProgress/PeripheralStatus.h', work / 'drivers/BootProgress/')
    path = panel / name
    text = path.read_text()
    if text.count(old) != 1:
        return index, 'BAD', f'matches {text.count(old)}'
    path.write_text(text.replace(old, new))
    probe = panel / 'Test/panel-probe.cpp'
    probe.write_text(trimmed_probe(probe.read_text()))
    killed = None
    flags = ['xcrun', 'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-I', platform, '-I', package]
    for test in ('panel-brightness', 'panel-probe'):
        binary = work / test
        built = subprocess.run(flags + [str(panel / 'Test' / (test + '.cpp')), '-o', str(binary)], capture_output=True, text=True, timeout=300)
        if built.returncode:
            killed = test + ' (compile)'
            break
        try:
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=240)
        except subprocess.TimeoutExpired:
            killed = test + ' (timeout)'
            break
        if ran.returncode:
            line = next((l for l in (ran.stderr + ran.stdout).splitlines() if l.startswith('FAIL')), '')
            killed = test + ': ' + line[:110]
            break
    if not killed:
        ran = subprocess.run([sys.executable, '-B', str(panel / 'Test/panel-start.py'), str(panel), platform, package], capture_output=True, text=True, timeout=300)
        if ran.returncode:
            line = next((l for l in (ran.stderr + ran.stdout).splitlines() if l.startswith('FAIL') or 'error:' in l), '')
            killed = 'panel-start: ' + line[-120:]
    shutil.rmtree(work, ignore_errors=True)
    return index, 'killed' if killed else 'SURVIVED', killed or ''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--platform', default=str(TOP / 'UniXNU-sm8750-common/kits/Sm8750Platform'))
    parser.add_argument('--package', default=str(TOP / 'UniXNU-Pagani_SM8750-gpu/package'))
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('index', nargs='*', type=int)
    args = parser.parse_args()
    indexes = args.index or list(range(len(M)))
    survivors = bad = 0
    with tempfile.TemporaryDirectory(prefix='panel-mutants-') as tmp, concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for index, state, detail in pool.map(run, [(i, tmp, args.platform, args.package) for i in indexes]):
            name, old, new = M[index]
            print(f'{index:3d} {state:8s} {name:18s} {old[:58]!r} -> {new[:40]!r} | {detail}', flush=True)
            survivors += state == 'SURVIVED'
            bad += state == 'BAD'
    if survivors or bad:
        raise SystemExit(f'FAIL: {len(indexes)} mutants, {survivors} survived, {bad} do not apply')
    print(f'PASS mutate: {len(indexes)} mutants, all killed')


if __name__ == '__main__':
    main()
