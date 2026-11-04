#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiWifi: the criteria of the UFS log's holds and of the Settings paths, each as a source change some host test must catch. The sources are
copied to a temporary directory; nothing here changes the tree. Needs Ath12kWifi/deps for the station cases (run tools/sta-sim.sh once first: it
links against out/all). Host tests do not establish that anything works on the phone."""
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
MODULE = SOURCE / 'Ath12kWifi'
SAN = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-w']
B, H, A, S, N = 'WifiBringup.cpp', 'WifiBringup.h', 'WifiApple80211.h', 'Ath12kWifi/src/nw_sta.c', 'PaganiWifiNet.cpp'
MUTANTS = [
    # the holds
    (B, '    b.hw->msleep(b.hw->ctx, b.opt.crumbMs);\n', '', 'bringup', 'the line naming a stretch reaches the log first'),
    (B, '    b.hw->msleep(b.hw->ctx, kQuietMs);\n', '', 'bringup', 'a round under way ends first'),
    (B, '    b.hw->quiet(b.hw->ctx, 1);\n', '', 'bringup', 'the writer is held'),
    (B, '    b.hw->quiet(b.hw->ctx, 0);\n', '', 'bringup', 'the writer is let go'),
    (B, '    if (!b.opt.crumbMs) return;\n', '', 'bringup', 'no log, no waits'),
    (B, '    if (!b.hw->quiet) return;\n', '    if (!b.hw->quiet) {b.held = false;}\n', 'bringup', 'a board without the hook'),
    (B, '    letGo(b);\n    char text[80];\n', '    char text[80];\n', 'bringup', 'a guard lets the last stretch go first'),
    (B, '    guard(b, "rails");\n', '', 'bringup', 'the votes run under the hold'),
    (B, '    guard(b, "wlan_en");\n', '', 'bringup', 'WLAN_EN and the TCSR page run under the hold'),
    (B, '    guard(b, "parf");\n', '', 'bringup', 'the first PARF and PHY accesses run under the hold'),
    (B, '    guard(b, "dbi");\n', '', 'bringup', 'the first DBI access runs under the hold'),
    (B, '    guard(b, "config");\n', '', 'bringup', 'the first configuration read runs under the hold'),
    (B, '        guard(b, "controller");\n', '', 'bringup', 'the survey\'s controller reads run under the hold'),
    (B, '    line(b, text);\n    letGo(b);\n    return error;\n', '    line(b, text);\n    return error;\n', 'bringup', 'a failure lets go'),
    (B, '    if (b.opt.stopAfter != step) return 0;\n    letGo(b);\n', '    if (b.opt.stopAfter != step) return 0;\n', 'bringup', 'a stop lets go'),
    (B, '    rc = done(b, kStepResets);\n    letGo(b);\n', '    rc = done(b, kStepResets);\n', 'bringup', 'the power stage ends its stretch (stage 2)'),
    (B, '    rc = done(b, kStepEndpointId);\n    letGo(b);\n', '    rc = done(b, kStepEndpointId);\n', 'bringup', 'the link stage ends its stretch'),
    (B, '    pw_text_str(&t, "[wifi] guard ");\n', '    pw_text_str(&t, "[wifi] ");\n', 'bringup', 'the line a hang is read from'),
    (H, 'constexpr unsigned kQuietMs = 1000;', 'constexpr unsigned kQuietMs = 400;', 'bringup', 'kQuietMs covers the writer\'s look of up to 500 ms'),
    (H, 'kQuietBit = 1u << 3;', 'kQuietBit = 1u << 2;', 'quiet', 'bit 3: bit 2 is PaganiBt\'s'),
    ('PaganiWifi.cpp', '__atomic_fetch_and(&pagani_panel_probe_quiet, ~kQuietBit, __ATOMIC_RELEASE)', '__atomic_store_n(&pagani_panel_probe_quiet, 0u, __ATOMIC_RELEASE)', 'quiet', 'the word is shared bit by bit'),
    ('PaganiWifi.cpp', '    guard(gB, "dma");', '   ', 'quiet', 'the first touch of the DMA window runs under the hold'),
    ('PaganiWifi.cpp', '    guard(gB, "bar0");', '   ', 'quiet', 'the first BAR0 read runs under the hold'),
    ('PaganiWifi.cpp', '    if (gStopAfter != step) return false;\n    letGo(gB);\n', '    if (gStopAfter != step) return false;\n', 'quiet', 'a kernel side stop lets go'),
    ('PaganiWifi.cpp', '    guard(gB, "chip");\n', '', 'quiet', 'the chip registers run under the hold'),
    ('PaganiWifi.cpp', '    guard(gB, "mhi");', '   ', 'quiet', 'the MHI boot runs under the hold'),
    ('PaganiWifi.cpp', '    guard(gB, "stack");', '   ', 'quiet', 'the stack\'s chip resets run under the hold'),
    ('PaganiWifi.cpp', '        if (gB.held && (nw_wifi_stats.stage >= 3 || nowMs() >= holdEnds)) letGo(gB);\n', '', 'quiet', 'the stack\'s stretch ends'),
    ('PaganiWifi.cpp', '    trace(line);\n    letGo(gB);\n    return error ? error : -1;\n', '    trace(line);\n    return error ? error : -1;\n', 'quiet', 'a kernel side failure lets go'),
    ('PaganiWifi.cpp', '    letGo(gB);              // the chip answers', '    // the chip answers', 'quiet', 'a crash dump gets the writer'),
    ('PaganiWifi.cpp', 'void pHalt()\n{\n    letGo(gB);\n', 'void pHalt()\n{\n', 'quiet', 'a halted stack lets go'),
    # the Settings paths: what the requests are answered with
    (A, 'constexpr uint32_t kChanActive = 0x80, kChanDfs = 0x100;', 'constexpr uint32_t kChanActive = 0x80, kChanDfs = 0x80;', 'apple80211', 'a DFS channel is marked'),
    (A, 'c.flags | (isDfs(mhz) ? kChanDfs : kChanActive)', 'c.flags | kChanActive', 'apple80211', 'a DFS channel is not scanned actively'),
    (A, '5260, 5280, 5300, 5320,\n', '5280, 5300, 5320,\n', 'apple80211', 'the list holds UNII-2A'),
    (A, 'mhz >= 5250 && mhz <= 5730', 'mhz >= 5250 && mhz <= 5500', 'apple80211', 'UNII-2C is DFS'),
    (A, 'return !fromRealScan || ageMs <= kBssStaleMs;', 'return ageMs <= kBssStaleMs;', 'apple80211', 'the cache offers everything'),
    (A, 'return !fromRealScan || ageMs <= kBssStaleMs;', 'return !fromRealScan || ageMs < kBssStaleMs;', 'apple80211', 'the edge of the age'),
    (A, 'return !fromRealScan || ageMs <= kBssStaleMs;', 'return true;', 'apple80211', 'a network a scan did not hear goes'),
    (A, 'e.involuntary = asked ? 0 : 1;', 'e.involuntary = 1;', 'apple80211', 'a leave is voluntary'),
    (A, 'e.reason = asked ? 0 : reason;', 'e.reason = reason;', 'apple80211', 'a leave has no reason of its own'),
    (A, 'e.down = 1;', 'e.down = 0;', 'apple80211', 'the link went down'),
    (A, 'kAssocFlags = 0x1d0', 'kAssocFlags = 0x1d4', 'apple80211', 'the private address flag'),
    (A, 'kAssocPrivateMac = 0x1fc', 'kAssocPrivateMac = 0x1f8', 'apple80211', 'the private address'),
    # the Settings paths: the service, request by request
    (N, 'gUserJoin = true; gLeaving = st.state >= NW_STA_AUTH; log(kLogStep, "wifi-net: leave");', 'gUserJoin = true; log(kLogStep, "wifi-net: leave");', 'net', 'a leave is voluntary'),
    (N, 'gAssocResult = kAssocFailed; gUserJoin = true; gLeaving = st.state >= NW_STA_AUTH;', 'gAssocResult = kAssocFailed; gUserJoin = true;', 'net', 'a join from a joined network is voluntary'),
    (N, 'if (!gPower) { gUserJoin = true; gLeaving = st.state >= NW_STA_AUTH; nw_wifi_disconnect(); }', 'if (!gPower) { gUserJoin = true; nw_wifi_disconnect(); }', 'net', 'the radio off is voluntary'),
    (N, 'if (!gPower) { gUserJoin = true; gLeaving = st.state >= NW_STA_AUTH; nw_wifi_disconnect(); }', 'if (!gPower) { gUserJoin = true; }', 'net', 'the radio off leaves'),
    (N, 'void on_connect_done(int result) {\n  gLeaving = false;\n', 'void on_connect_done(int result) {\n', 'net', 'no voluntary mark outlives a join'),
    (N, '  post_link(false);\n  link_event ev = linkDown(', '  link_event ev = linkDown(', 'net', 'the kernel\'s link event'),
    (N, 'nw_wifi_disconnect(); if (st.state != NW_STA_UP) post_link(false); return 0;', 'nw_wifi_disconnect(); post_link(false); return 0;', 'net', 'the kernel\'s link event once'),
    (N, '  post_event(kEvLinkChanged, &ev, sizeof ev);\n  post_event(kEvBssidChanged, nullptr, 0); post_event(kEvSsidChanged, nullptr, 0);\n', '  post_event(kEvBssidChanged, nullptr, 0); post_event(kEvSsidChanged, nullptr, 0);\n', 'net', 'wifid\'s state moves on LINK_CHANGED down'),
    (N, '  post_event(kEvLinkChanged, &ev, sizeof ev);\n  post_event(kEvBssidChanged, nullptr, 0); post_event(kEvSsidChanged, nullptr, 0);\n', '  post_event(kEvLinkChanged, &ev, sizeof ev);\n', 'net', 'the AirPort state refreshes on the BSSID change'),
    (N, 'if (find_bss(now.bssid, b)) gJoined = b; post_link(true);', 'post_link(true);', 'net', 'the record of the join'),
    (N, 'if (!find_bss(st.bssid, b)) { if (memcmp(gJoined.bssid, st.bssid, 6)) return ENOENT; b = gJoined; }', 'if (!find_bss(st.bssid, b)) return ENOENT;', 'net', 'CURRENT_NETWORK without the list\'s entry'),
    (N, '    } while (!bssOffered(b.age_ms, gScanFresh));\n', '    } while (false);\n', 'net', 'a network a scan did not hear is not offered'),
    (N, 'gScanning = 1; gScanCursor = 0; gScanFresh = false; gResultsShown = 24;', 'gScanning = 1; gScanCursor = 0; gResultsShown = 24;', 'net', 'the cache offers everything'),
    (N, 'gScanning = 1; gScanCursor = 0; gScanFresh = true; gResultsShown = 0;', 'gScanning = 1; gScanCursor = 0; gResultsShown = 0;', 'net', 'a real scan is marked'),
    (N, 'if (hasKey && a.key_cipher == kKeyCipherPmk && a.key_length == 32) { pmkHex(a.key, pass); keyLength = 64; }', '', 'net', 'the PMK goes as 64 hex digits'),
    (N, 'raw[kAssocFlags] & kAssocFlagPrivateMac ? ", private address asked (the board\'s stays)" : ""', '""', 'net', 'the log says a private address was asked for'),
    (N, 'for (uint16_t f : kListedMhz) out.chans.list[n++] = listedChannel(f);', 'for (uint16_t f : kListedMhz) out.chans.list[n++] = channelOf(f);', 'net', 'the list carries its flags'),
    (N, 'return r ? EINVAL : 0; }', 'return 0; }', 'net', 'a request the module refuses is refused'),
    (N, 'log(kLogStep, "wifi-net: leave");', 'log(6, "wifi-net: leave");', 'net', 'a step of the walk reaches the boot log'),
    (N, 'result == NW_RES_BAD_KEY ? kAssocBadKey : ', '', 'net', 'a wrong key reads as one'),
    (N, 'default: return EOPNOTSUPP;\n    }\n  }\n  switch (type) {', 'default: return 0;\n    }\n  }\n  switch (type) {', 'net', 'a set nobody answers is refused'),
    # the Settings paths: the module
    (S, '  if (C.state == NW_STA_UP) teardown(1, 0);\n  else if (C.state >= NW_STA_AUTH) { teardown(0, 0); C.state = NW_STA_IDLE; }\n  if (C.state != NW_STA_IDLE', '  if (C.state != NW_STA_IDLE', 'sta', 'a join from a joined network gets through'),
    (S, '  if (C.state == NW_STA_UP) teardown(1, 0);\n  else if (C.state >= NW_STA_AUTH) { teardown(0, 0); C.state = NW_STA_IDLE; }\n  if (C.state != NW_STA_IDLE', '  if (C.state >= NW_STA_AUTH) { teardown(0, 0); C.state = NW_STA_IDLE; }\n  if (C.state != NW_STA_IDLE', 'sta', 'the old link is reported gone'),
    (S, '  if (C.state == NW_STA_UP) teardown(1, 0);\n  else if (C.state >= NW_STA_AUTH) { teardown(0, 0); C.state = NW_STA_IDLE; }\n  if (C.state != NW_STA_IDLE', '  if (C.state == NW_STA_UP) teardown(1, 0);\n  if (C.state != NW_STA_IDLE', 'sta', 'a join replaces one under way'),
    (S, 'if (C.state == NW_STA_UP) teardown(1, 0); else connect_done(NW_RES_FAILED); }', 'teardown(0, 0); C.state = NW_STA_IDLE; }', 'sta', 'a leave reports the link gone'),
    (S, 'if (C.state == NW_STA_UP) teardown(1, 0); else connect_done(NW_RES_FAILED); }', 'if (C.state == NW_STA_UP) teardown(1, 0); else { teardown(0, 0); C.state = NW_STA_IDLE; } }', 'sta', 'a leave ends a join with a result'),
    (S, 'connect_done(C.state == NW_STA_KEYS && C.hs_stage == 1 ? NW_RES_BAD_KEY : NW_RES_REJECTED)', 'connect_done(NW_RES_REJECTED)', 'sta', 'a deauthentication after message 2 is a key error'),
    (S, 'connect_done(C.state == NW_STA_KEYS && C.hs_stage == 1 ? NW_RES_BAD_KEY : NW_RES_REJECTED)', 'connect_done(NW_RES_BAD_KEY)', 'sta', 'a deauthentication before message 1 is a refusal'),
    (S, '  if (C.state >= NW_STA_AUTH && !memcmp(b.bssid, C.ap.bssid, 6)) C.ap.signal = b.signal;', '', 'sta', 'the signal follows the beacons'),
]


def apply(root, file, old, new):
    path = root / file
    text = path.read_text()
    if text.count(old) < 1:
        raise SystemExit(f'FAIL mark not found in {file}: {old!r}')
    path.write_text(text.replace(old, new, 1))


def run_test(root, kind, out):
    if kind == 'bringup':
        cmd = ['c++', '-std=c++17', *SAN, str(root / 'Test/bringup.cpp'), str(root / 'WifiBringup.cpp'), '-o', str(out)]
    elif kind == 'apple80211':
        cmd = ['c++', '-std=c++17', *SAN, str(root / 'Test/apple80211.cpp'), '-o', str(out)]
    elif kind == 'net':
        code = ('import importlib.util,sys;from pathlib import Path;s=importlib.util.spec_from_file_location("c",sys.argv[1]);m=importlib.util.module_from_spec(s);'
                's.loader.exec_module(m);m.net_includes(Path(sys.argv[2]));print(" ".join(m.NET_FLAGS))')
        includes = out.parent / 'net-includes'
        made = subprocess.run([sys.executable, '-B', '-c', code, str(root / 'Test/check-pagani-wifi-host.py'), str(includes)], capture_output=True, text=True)
        cmd = ['c++', '-std=c++17', *SAN, '-I', str(includes), '-I', str(root / 'Test'), '-I', str(MODULE / 'include'), '-I', str(root), str(root / 'Test/net.cpp'), '-o', str(out)]
        if made.returncode:
            return None
    elif kind == 'quiet':
        code = ('import importlib.util,sys;s=importlib.util.spec_from_file_location("c",sys.argv[1]);m=importlib.util.module_from_spec(s);'
                's.loader.exec_module(m);m.test_quiet()')
        return subprocess.run([sys.executable, '-B', '-c', code, str(root / 'Test/check-pagani-wifi-host.py')], capture_output=True, text=True).returncode == 0
    else:
        flags = ['-std=gnu11', '-w', '-g', '-DCONFIG_ATH12K_DEBUG', '-include', 'lx_pci.h', '-I', 'shim/include', '-I', 'include', '-I', 'out/pinned', '-I', 'src',
                 '-I', 'out/ath12k-src']
        obj = out.with_suffix('.o')
        resource = subprocess.run(['clang', '-print-resource-dir'], capture_output=True, text=True).stdout.strip()
        compile_flags = ['-c', '-nostdinc', '-isystem', 'shim/libc', '-isystem', resource + '/include', '-std=gnu11', '-w', '-g0', '-DCONFIG_ATH12K_DEBUG', '-include',
                         'lx_pci.h', '-I', 'shim/include', '-I', 'include', '-I', 'out/pinned', '-I', 'src', '-I', 'out/ath12k-src', '-I', 'out/mhi-src']
        built = subprocess.run(['clang', *compile_flags, str(root / 'Ath12kWifi/src/nw_sta.c'), '-o', str(obj)], cwd=MODULE, capture_output=True, text=True)
        if built.returncode:
            return None
        built = subprocess.run(['clang', *flags, 'tests/sta_sim.c', 'out/all/lx_rt.o', str(obj), 'out/all/nw_crypto.o', 'out/all/nw_mac80211.o', 'out/all/nw_net.o',
                                '-o', str(out)], cwd=MODULE, capture_output=True, text=True)
        if built.returncode:
            return None
        return subprocess.run([str(out)], capture_output=True, text=True, timeout=120).returncode == 0
    built = subprocess.run(cmd, capture_output=True, text=True)
    if built.returncode:
        return False if 'static assertion failed' in built.stderr else None     # the header's own check caught it
    return subprocess.run([str(out)], capture_output=True, text=True, timeout=120).returncode == 0


def one(args):
    index, (file, old, new, kind, what) = args
    with tempfile.TemporaryDirectory(prefix='pw-mutant-') as scratch:
        root = Path(scratch) / 'PaganiWifi'
        (root / 'Ath12kWifi/src').mkdir(parents=True)
        for path in list(SOURCE.glob('*.h')) + list(SOURCE.glob('*.c')) + list(SOURCE.glob('*.cpp')):
            shutil.copy(path, root)
        shutil.copytree(SOURCE / 'Test', root / 'Test')
        shutil.copy(MODULE / 'src/nw_sta.c', root / 'Ath12kWifi/src')
        apply(root, file, old, new)
        passed = run_test(root, kind, Path(scratch) / 'test')
    return index, kind, what, passed


def main():
    if not (MODULE / 'out/all/nw_sta.o').is_file():
        subprocess.run(['bash', str(MODULE / 'tools/sta-sim.sh')], capture_output=True)
    with tempfile.TemporaryDirectory(prefix='pw-base-') as scratch:
        for kind in ('bringup', 'apple80211', 'net', 'quiet', 'sta'):
            root = Path(scratch) / kind / 'PaganiWifi'
            (root / 'Ath12kWifi/src').mkdir(parents=True)
            for path in list(SOURCE.glob('*.h')) + list(SOURCE.glob('*.c')) + list(SOURCE.glob('*.cpp')):
                shutil.copy(path, root)
            shutil.copytree(SOURCE / 'Test', root / 'Test')
            shutil.copy(MODULE / 'src/nw_sta.c', root / 'Ath12kWifi/src')
            if not run_test(root, kind, Path(scratch) / kind / 'test'):
                raise SystemExit(f'FAIL: the {kind} test does not pass unchanged')
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(one, enumerate(MUTANTS)))
    survivors = [r for r in results if r[3] is not False]
    counts = {}
    for _, kind, _, _ in results:
        counts[kind] = counts.get(kind, 0) + 1
    for _, kind, what, passed in survivors:
        print(f'SURVIVED ({kind}, {"did not build" if passed is None else "tests pass"}): {what}')
    if survivors:
        raise SystemExit(f'FAIL: {len(survivors)} of {len(MUTANTS)} mutants survived')
    print(f'PASS PaganiWifi mutants: {len(MUTANTS)} caught ({", ".join(f"{k} {v}" for k, v in sorted(counts.items()))})')


if __name__ == '__main__':
    main()
