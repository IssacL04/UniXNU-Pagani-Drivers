#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the PaganiAudio host checks.

  check-pagani-audio-host.py [--tree TREE] [--kern-exec PATH] [--patch PATCH] [--sheng DIR] [--no-ios]

Always: the shared memory layout (Test/abi.c; against sheng's ShengAudioAbi.h field by field when --sheng or the default reference
checkout is there; the header's own asserts also compiled as C++ and for arm64 iOS 17), the engine (Test/engine.c), the haptic actuator
bridge against a recording vibrator (Test/bridge.c), the plug-in on macOS against the engine as its kernel service (Test/plugin.c, run
as before, as revision 2 with the actuator device, its IO into the real bridge, MediaExperience's progress and the 'srzl' repair on a
fake MediaExperience and VirtualAudio, and with the actuator expected but its ring out of reach) and the kernel glue PaganiAudio.cpp and PaganiAudioService.cpp on the kernel
stand-ins of Test/kstubs (Test/glue.cpp, with the vibrator of Test/glue_haptics.c), all with AddressSanitizer and
UndefinedBehaviorSanitizer, PaganiAudio.cpp's weak vibrator prototypes against PaganiHaptics.h, the trust bundle merge of
add-pagani-audio-data.py on synthetic bundles, and the barriers of the route probe's seqlock in arm64 code at -O2 (the plug-in writes
the odd seq, dmb ish, the fields, the even seq with stlr; the kernel reads seq with acquire, the fields, dmb ishld, seq again), which
no test on one thread can see. Unless --no-ios: the iOS plug-in bundle (build-pagani-audio-hal.py into a temporary directory) and its
writer's barriers. With --tree: the kernel archive with that tree's XNU compile records (build-pagani-audio.py --out a temporary
directory; the tree is only read) and its reader's barriers. With --kern-exec: the mount patch is made against that kern_exec.c and
proved to apply; with --patch too, the patch file must be what the script makes (a stale patch fails)."""
import argparse
import importlib.util
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
SHENG = Path('/Users/xiaochoumao/unixnu-13t/参考资料/UniXNU-sheng')
SANITIZE = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
WARN = ['-Wall', '-Wextra', '-Werror']


def run(command, **kw):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kw)
    if result.returncode:
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}\n{(result.stdout + result.stderr)[-4000:]}')
    return result.stdout


def last_line(text):
    lines = [line for line in text.splitlines() if line.strip()]
    return lines[-1] if lines else ''


def check_trust_merge():
    spec = importlib.util.spec_from_file_location('add_pagani_audio_data', SOURCE / 'scripts/add-pagani-audio-data.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    def entry(h):
        return h + bytes([2, 0, 0, 0])

    def trust_module(hashes):
        return struct.pack('<I16sI', 2, bytes(16), len(hashes)) + b''.join(entry(h) for h in sorted(hashes))

    apple = [trust_module([bytes([0x10] * 20), bytes([0x80] * 20)]), trust_module([bytes([0x20] * 20)])]
    owner = trust_module([bytes([0x30] * 20), bytes([0xf0] * 20)])
    with tempfile.TemporaryDirectory(prefix='pagani-audio-trust-') as tmp:
        base, original = Path(tmp) / 'owner.bundle', Path(tmp) / 'apple.bundle'
        base.write_bytes(module.bundle_of([owner, *apple]))
        original.write_bytes(module.bundle_of(apple))
        audio = 'a5' * 20
        data, report = module.add_trust(base, original, audio)
        modules = module.modules_of(data)
        hashes = [e[:20].hex() for e in module.entries_of(modules[0])]
        assert module.valid_like_the_loader(data) and modules[1:] == apple and report['apple_modules_unchanged']
        assert hashes == sorted(hashes) and audio in hashes and len(hashes) == 3 and not report['already_present']
        # Idempotent: adding it again changes nothing but the flag.
        base.write_bytes(data)
        again, report = module.add_trust(base, original, audio)
        assert again == data and report['already_present']
        # A rebuilt plug-in replaces the old one (--replace): the old CDHash leaves the owner module, the other owner hashes and the
        # Apple modules stay; dropping the hash being added keeps it; a hash that is not there is refused.
        rebuilt = 'b6' * 20
        replaced, report = module.add_trust(base, original, rebuilt, [audio])
        modules = module.modules_of(replaced)
        hashes = [e[:20].hex() for e in module.entries_of(modules[0])]
        assert module.valid_like_the_loader(replaced) and modules[1:] == apple and report['dropped'] == [audio]
        assert hashes == sorted([bytes([0x30] * 20).hex(), bytes([0xf0] * 20).hex(), rebuilt])
        same, report = module.add_trust(base, original, audio, [audio])
        assert same == data and report['dropped'] == [] and report['already_present']
        try:
            module.add_trust(base, original, rebuilt, ['c7' * 20])
        except SystemExit:
            pass
        else:
            raise SystemExit('FAIL: dropping a CDHash the bundle does not carry was accepted')
        # Apple modules only: a new owner module goes in front.
        base.write_bytes(module.bundle_of(apple))
        data, _ = module.add_trust(base, original, audio)
        modules = module.modules_of(data)
        assert len(modules) == 3 and modules[1:] == apple and [e[:20].hex() for e in module.entries_of(modules[0])] == [audio]
        # A bundle whose Apple modules were changed is refused.
        base.write_bytes(module.bundle_of([owner, apple[1], apple[0]]))
        try:
            module.add_trust(base, original, audio)
        except SystemExit:
            pass
        else:
            raise SystemExit('FAIL: a reordered Apple module was accepted')
    return 'PASS trust: merge, order, idempotence, replacement of the old plug-in hash, Apple modules unchanged, refusals'


# ---- the route probe's seqlock in the instructions (PaganiAudioAbi.h, PaganiAudioProbe) -----------------------------------------------
# A missing barrier shows only as a torn probe on a weakly ordered CPU, now and then, so the order is checked where it lives: in the
# arm64 code of the writer (pagani_audio_probe_route) and of the reader (routeRow, which the compiler inlines into the service thread).
PROBE_OFFSET = int(re.search(r'#define PAGANI_AUDIO_PROBE_OFFSET (\d+)u', (SOURCE / 'PaganiAudioAbi.h').read_text()).group(1))
WRITTEN = list(range(4, 68, 4))   # reason .. vad_volume_milli: what a probe writes between the odd and the even seq
READ = list(range(4, 72, 4))      # those and vol_queries: what the row reads between its two reads of seq


def disassemble(binary, symbol=None):
    """[(mnemonic, operands)] of the binary's code, or of one of its functions, from llvm-objdump."""
    command = ['xcrun', 'objdump', '-d', '--no-show-raw-insn', '--no-leading-addr', binary]
    if symbol:
        command.append('--disassemble-symbols=' + symbol)
    code = []
    for line in run(command).splitlines():
        match = re.match(r'\s+([a-z][\w.]*)(?:\s+([^;]*?))?\s*(?:;.*)?$', line)
        if match:
            code.append((match.group(1), match.group(2) or ''))
    return code


def address(operands):
    """(base register, offset) of an instruction's memory operand, or None."""
    match = re.search(r'\[(\w+)(?:, #(-?(?:0x[0-9a-f]+|\d+)))?\]', operands)
    return (match.group(1), int(match.group(2) or '0', 0)) if match else None


def check_writer(code, where):
    """The odd seq stored, dmb ish, every field of the probe, then the even seq with stlr; nothing else ordered in the function."""
    ordered = [i for i, (m, _) in enumerate(code) if m.startswith(('ldar', 'ldapr', 'stlr', 'dmb'))]
    shape = [code[i][0] + (' ' + code[i][1] if code[i][0] == 'dmb' else '') for i in ordered]
    if shape != ['dmb ish', 'stlr']:
        raise SystemExit(f'FAIL barriers: {where}: the probe writer orders its stores with {shape}, not dmb ish then stlr')
    fence, release = ordered
    seq = address(code[release][1])
    fields = sorted(a[1] for m, o in code[fence + 1:release] if m.startswith('st') for a in [address(o)] if a and a[0] == seq[0])
    odd = [a for m, o in code[:fence] if m.startswith('st') for a in [address(o)] if a]
    if seq[1] or fields != WRITTEN or not odd or odd[-1] != seq:
        raise SystemExit(f'FAIL barriers: {where}: expected the odd seq stored at [{seq[0]}] right before dmb ish and the fields '
                         f'{WRITTEN} between it and stlr; found {odd[-1:]} and {fields}')
    return f'{where}: str seq, dmb ish, {len(fields)} fields, stlr seq'


def check_reader(code, where):
    """At every dmb ishld: seq read with acquire shortly before, every field read between the two, seq read again right after."""
    fences = [i for i, (m, o) in enumerate(code) if m == 'dmb' and o == 'ishld']
    if not fences:
        raise SystemExit(f'FAIL barriers: {where}: no dmb ishld, so the probe fields may be read after seq is read the second time')
    for fence in fences:
        acquire = max((i for i in range(fence) if code[i][0].startswith(('ldar', 'ldapr'))), default=-1)
        seq = address(code[acquire][1]) if acquire >= 0 else None
        reads = [a for m, o in code[acquire + 1:fence] if m.startswith('ld') for a in [address(o)] if a]
        offsets = {off - PROBE_OFFSET if off >= PROBE_OFFSET else off for _, off in reads}
        again = next((a for m, o in code[fence + 1:fence + 4] if m.startswith('ld') for a in [address(o)] if a), None)
        if not seq or fence - acquire > 48 or not set(READ) <= offsets or not again or (again[1] != PROBE_OFFSET and again != seq):
            raise SystemExit(f'FAIL barriers: {where}: expected seq read with acquire, the fields {READ}, dmb ishld and seq read again; '
                             f'found {code[acquire:fence + 4] if acquire >= 0 else code[fence:fence + 4]}')
    return f'{where}: {code[acquire][0]} seq, {len(READ)} fields, dmb ishld, seq'


def check_barriers(work):
    """The seqlock at -O2 for arm64 on this compiler: the plug-in's writer, the kernel glue's reader on the stand-ins of Test/kstubs."""
    plugin, glue = work / 'barrier-plugin.o', work / 'barrier-glue.o'
    run(['xcrun', 'clang', '-target', 'arm64-apple-macos12', '-std=gnu11', '-O2', *WARN, '-I', SOURCE, '-c',
         SOURCE / 'hal/PaganiAudioPlugIn.c', '-o', plugin])
    run(['xcrun', 'clang++', '-target', 'arm64-apple-macos12', '-std=c++17', '-O2', *WARN, '-Wno-unused-parameter', '-I',
         SOURCE / 'Test/kstubs', '-I', SOURCE, '-c', SOURCE / 'PaganiAudio.cpp', '-o', glue])
    return (f'PASS barriers: {check_writer(disassemble(plugin, "_pagani_audio_probe_route"), "plug-in (arm64 -O2)")}; '
            f'{check_reader(disassemble(glue), "kernel glue (arm64 -O2)")}')


# ---- the addresses of VirtualAudio's actuator settings against the binary itself ----------------------------------------------------------
VIRTUAL_AUDIO = Path('/Users/xiaochoumao/unixnu-13t/build-userspace/a830-inputs/osmnt-21H461/Library/Audio/Plug-Ins/HAL/'
                     'VirtualAudio.plugin/VirtualAudio')


def layout_value(name):
    match = re.search(r'\.' + name + r' = (0x[0-9a-f]+)ull', (SOURCE / 'hal/PaganiAudioPlugIn.c').read_text())
    if not match:
        raise SystemExit(f'FAIL settings: no .{name} in the plug-in\'s layout')
    return int(match.group(1), 16)


def stores(code, base, start, end):
    """{address: value} of the word stores between start and end: adrp + add + str w (unsigned offset), the value from mov/movk w, or wzr 0."""
    pages, constants, found = {}, {}, {}
    for at in range(start, end, 4):
        word = struct.unpack_from('<I', code, at - base)[0]
        rd = word & 31
        if word & 0x9f000000 == 0x90000000:        # adrp
            imm = ((word >> 29) & 3) | ((word >> 5) & 0x7ffff) << 2
            pages[rd] = (at & ~0xfff) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12)
        elif word & 0xffc00000 == 0x91000000:      # add x, x, #imm12
            if (word >> 5) & 31 in pages:
                pages[rd] = pages[(word >> 5) & 31] + ((word >> 10) & 0xfff)
        elif word & 0xffe00000 == 0x52800000:      # mov w, #imm16
            constants[rd] = (word >> 5) & 0xffff
        elif word & 0xffe00000 == 0x72a00000:      # movk w, #imm16, lsl #16
            constants[rd] = constants.get(rd, 0) | ((word >> 5) & 0xffff) << 16
        elif word & 0xffc00000 == 0xb9000000:      # str w, [x, #imm]
            source, register = rd, (word >> 5) & 31
            if register in pages:
                found[pages[register] + ((word >> 10) & 0xfff) * 4] = 0 if source == 31 else constants.get(source)
    return found


def check_va_settings(path=VIRTUAL_AUDIO):
    """The image the plug-in writes into: its UUID, the guard sub_2AC470 tests, the two words the table's row sets, and the throw. The binary
    is the phone's, read only, and stays out of the repository: without it this says so."""
    if not path.is_file():
        return f'SKIP settings: {path} not there'
    code = path.read_bytes()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', code)
    if magic != 0xfeedfacf:
        raise SystemExit(f'FAIL settings: {path} is not a thin 64-bit Mach-O')
    at, uuid = 32, None
    for _ in range(ncmds):
        command, size = struct.unpack_from('<2I', code, at)
        if command == 0x1b:
            uuid = code[at + 8:at + 24]
        at += size
    plugin = (SOURCE / 'hal/PaganiAudioPlugIn.c').read_text()
    wanted = bytes(int(v, 16) for v in re.search(r'\.va_uuid = \{([^}]*)\}', plugin).group(1).split(','))
    if uuid != wanted:
        raise SystemExit(f'FAIL settings: VirtualAudio UUID {uuid.hex()} is not the layout\'s {wanted.hex()}')
    guard, kind, word = layout_value('va_settings_guard'), layout_value('va_settings_kind'), layout_value('va_settings_format')
    # sub_2AC470: adrl x8, guard; ldarb w8, [x8]; tbz w8, #0, the first use
    first = struct.unpack_from('<4I', code, 0x2ac47c)
    if first[0] & 0x9f00001f != 0x90000008 or first[1] & 0xffc003ff != 0x91000108 or first[2] != 0x08dffd08 or first[3] & 0xfff8001f != 0x36000008:
        raise SystemExit(f'FAIL settings: sub_2AC470 is not adrl x8; ldarb w8; tbz w8, #0: {[hex(w) for w in first]}')
    imm = ((first[0] >> 29) & 3) | ((first[0] >> 5) & 0x7ffff) << 2
    tested = (0x2ac47c & ~0xfff) + (imm << 12) + ((first[1] >> 10) & 0xfff)
    if tested != guard:
        raise SystemExit(f'FAIL settings: sub_2AC470 tests {tested:#x}, the layout says {guard:#x}')
    # sub_2AC4DC: the table's row for a product without I/V sense: kind 1, the format word, written nowhere else at load
    row = stores(code, 0, 0x2ac4dc, 0x2ac718)
    expected = int.from_bytes(re.search(r"va_settings_format\) = '(....)'", plugin).group(1).encode(), 'big')
    if row.get(kind) != 1 or row.get(word) != expected:
        raise SystemExit(f'FAIL settings: the row writes {kind:#x} = {row.get(kind)} and {word:#x} = {row.get(word)}, the plug-in 1 and {expected:#x}')
    if b'Invalid Product Type' not in code:
        raise SystemExit('FAIL settings: the refusal this answers is not in the binary')
    return f'PASS settings: VirtualAudio {uuid.hex()}: guard {guard:#x}, kind {kind:#x} = 1, format {word:#x} = {expected:#x}'


# ---- the log verdict against the rows it reads ---------------------------------------------------------------------------------------------
def check_verdict():
    """sound-verdict.py on the rows of the 13T's boots and on the rows a working path gives; every prefix it looks for is a string of the
    driver that writes it, so a reworded row fails here and not silently on the phone's log."""
    spec = importlib.util.spec_from_file_location('sound_verdict', SOURCE / 'scripts/sound-verdict.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    head = '1 1.0 A  pagani-audio=1 pagani-haptics=1 pagani-haptics-ios=2 pagani-audio-srzl=%s%s\n2 32.1 U  audio: open 1 by audiomxd pid 93\n'
    dead = ('3 53.0 U  audio route #1 r1 dev=0 va=67 vad=0 ours=35 vold=1 run=0 vcsp=0/560947818 vc=1 cat=.... mode=.... out=0 .... ?h?t '
            'ports=2003332927 vol=4294967295 q=12\n4 53.0 U  audio haptics plug loads=1 vad=0 hog=1 writes=0 frames=0 peak=0 ahead=0..0 drop=0/0 '
            'unk=0 ....@0 fmt=0 stale=0 skip=0 fifo=8000 cap=3600 lim=0\n5 54.0 U  audio mx p=0x27fd filt=2 acq=-2 srzl=2 st=0/0 after=- filt=0>0 n=0\n')
    alive = ('3 53.0 U  audio route #2 r1 dev=4 va=67 vad=72 ours=35 vold=1 run=1 vcsp=1/0 vc=1 cat=csav mode=imdf out=1 ptbo .... ports=0 vol=300 q=20\n'
             '4 53.0 U  audio mx p=0x3fff filt=3 acq=2 srzl=1 st=0/0 after=- filt=0>0 n=0 va=1\n')
    play = ('5 60.0 U  audio plug loads=1 opens=1/0 maps=4 by audiomxd vol=300 sets=2 writes=900 miss=0 unk=0 clk=19200000/0\n'
            '6 60.0 U  audio svc st=f users=1/0 req=2 bad=0 out=1/0/0 in=0/0 taken=48000 played=48000 late=0 holes=0 skip=0 lead=14400 peak=9000/0\n')
    speaker = ('7 40.0 U  [spk] backend adsp handed to the PaganiAudio service (st=12)\n8 61.0 U  [spk] amp 0x34 state=9 clks=1 plls=1 sws=1 ampen=1 '
               'noclk=0 tdmerr=0 st=0x40\n9 61.0 U  [spk] amp 0x35 state=9 clks=1 plls=1 sws=1 ampen=1 noclk=0 tdmerr=0 st=0x40\n'
               '10 70.0 U  [spk] play stop run=1 sent=900 played=896 werr=0 sendfail=0 stale=0 stalls=0 ms=9000 lim=0 gmin=1000 in=9000 out=9000 clamp=0\n')
    haptics = ('11 80.0 U  audio haptics runs=1/1 io=1/1 frames=9 loud=9 peak=1/1 streams=1/1/0 fail=0/0 sent=9/0 clip=0 lat=9/9us\n'
               '12 80.0 U  audio haptics plug loads=1 vad=74 hog=1 writes=9 frames=9 peak=1 ahead=380..400 drop=0/0 unk=0 ....@0 fmt=1 stale=0 skip=0 fifo=8000 cap=3600 lim=0\n'
               '13 80.0 U  [haptics] stream a=8000 b=3600 mv=3600\n')

    def rows(text):
        return {name: status for name, status, _ in module.verdict(module.Log(text))}

    # The 13T's boots since pagani-haptics-ios=2: the routing layer never initialised, nothing behind the volume HUD.
    found = rows(head % ('1', '') + dead)
    assert found['VirtualAudio 初始化'] == module.STOP and found['声音'] == module.STOP and found['震动'] == module.STOP and found['后端'] == module.OFF
    # The same with the listener lost: no route, the initialisation fine.
    lost = dead.replace('vad=0 ', 'vad=72 ').replace('.... ?h?t', '.... ShT0').replace('cat=....', 'cat=csav').replace('out=0 .... ShT0', 'out=0 .... sh0t')
    found = rows(head % ('3', '') + lost)
    assert found['VirtualAudio 初始化'] == module.GOOD and found['路由'] == module.STOP
    # All of it: what is left is for the ears and the hands.
    found = rows(head % ('3', ' pagani-adsp=1 pagani-speaker=1') + alive + play + speaker + haptics)
    assert found == {'插件': module.GOOD, 'VirtualAudio 马达设置': module.GOOD, 'VirtualAudio 初始化': module.GOOD, '路由': module.GOOD, 'IO': module.GOOD,
                     '音量': module.GOOD, '后端': module.GOOD, '放音': module.HUMAN, '震动': module.HUMAN}, found
    # Silence in the DSP, an amplifier that did not start, no IO, no volume: each stops or asks at its own row.
    found = rows(head % ('3', ' pagani-adsp=1 pagani-speaker=1') + alive + play + speaker.replace('in=9000', 'in=0') + haptics)
    assert found['放音'] == module.STOP
    found = rows(head % ('3', ' pagani-adsp=1 pagani-speaker=1') + alive + play + speaker.replace('state=9', 'state=4') + haptics)
    assert found['放音'] == module.STOP
    found = rows(head % ('3', ' pagani-adsp=1 pagani-speaker=1') + alive + play + speaker.replace('sws=1', 'sws=0') + haptics)
    assert found['放音'] == module.STOP
    found = rows(head % ('3', ' pagani-adsp=1 pagani-speaker=1') + alive + play + speaker.replace('ampen=1', 'ampen=0') + haptics)
    assert found['放音'] == module.STOP
    # A sticky bit is read once and may be from the clocks starting: written down, it stops nothing.
    found = rows(head % ('3', ' pagani-adsp=1 pagani-speaker=1') + alive + play + speaker.replace('tdmerr=0', 'tdmerr=1') + haptics)
    assert found['放音'] == module.HUMAN
    # The kernel's tone (C6, C7, no iOS): the log goes as far as the amplifier, the rest is for the ears.
    tone = ('1 1.0 A  pagani-adsp=1 pagani-speaker=1 pagani-audio-tone=1\n2 40.0 U  [spk] amp 0x34 state=9 clks=1 plls=1 sws=1 ampen=1 noclk=0 tdmerr=0 st=0x40\n'
            '3 40.0 U  [spk] amp 0x35 state=9 clks=1 plls=1 sws=1 ampen=1 noclk=0 tdmerr=0 st=0x40\n4 41.0 U  [spk] tone rc=0 hz=440 ms=500 atten=30 gain=1036 '
            'periods=50 sent=52 played=52 amps=0x3 clks_failed=0 ch=0x3 out=1036 ceil=8231\n')
    assert rows(tone) == {'提示音': module.HUMAN}, rows(tone)
    for broken in (('rc=0', 'rc=-5'), ('played=52', 'played=0'), ('out=1036', 'out=0'), ('clks_failed=0', 'clks_failed=1'), ('state=9', 'state=4'), ('sws=1', 'sws=0'),
                   ('ampen=1', 'ampen=0'), ('[spk] tone rc', '[spk] tune rc'), ('[spk] amp 0x3', '[spk] map 0x3')):
        assert rows(tone.replace(*broken)) == {'提示音': module.STOP}, broken
    assert rows(tone.replace('pagani-audio-tone=1', 'pagani-audio-tone=0')) == {'服务': module.OFF}
    found = rows(head % ('3', '') + alive)
    assert found['IO'] == module.HUMAN and found['后端'] == module.OFF
    found = rows(head % ('3', '') + alive + play.replace('sets=2', 'sets=0'))
    assert found['音量'] == module.HUMAN
    # Every prefix read is a string of its driver.
    texts = ''.join(path.read_text(errors='replace') for path in [*(SOURCE.glob('*.cpp')), *(SOURCE.parent / 'PaganiSpeaker').glob('*.c*'),
                                                                 *(SOURCE.parent / 'PaganiHaptics').glob('*.c*')])
    prefixes = ['audio route #', 'audio mx p=', 'audio plug loads=', 'audio svc st=', 'audio: open ', 'audio haptics plug loads=', 'audio haptics runs=']
    missing = [p for p in prefixes if p not in texts]
    missing += [p for p in ('backend adsp handed to the PaganiAudio service', 'play stop', '"tone"', '"clks_failed"', '"sws"', '"ampen"', '"noclk"', '"tdmerr"',
                            '[haptics] ready', '[haptics] FAULT ') if (SOURCE.parent / 'PaganiSpeaker').is_dir() and p not in texts]
    if missing:
        raise SystemExit(f'FAIL verdict: rows it reads are not in the drivers: {missing}')
    return 'PASS verdict: the boots of 2026-11-01, a lost listener, a whole path and each stop; every row prefix found in its driver'


def check_vibrator_prototypes(work):
    """PaganiAudio.cpp's weak defaults of the vibrator's stream API declare what PaganiHaptics.h declares: one translation unit with both
    (a C function declared twice with other parameters does not compile)."""
    header = SOURCE.parent / 'PaganiHaptics/PaganiHaptics.h'
    found = re.findall(r'extern "C" __attribute__\(\(weak\)\) (int pagani_haptics_stream_\w+\([^)]*\))', (SOURCE / 'PaganiAudio.cpp').read_text())
    if len(found) != 3:
        raise SystemExit(f'FAIL vibrator: expected the three weak stream calls in PaganiAudio.cpp, found {found}')
    if not header.is_file():
        return f'SKIP vibrator: {header} not there ({len(found)} weak calls in PaganiAudio.cpp)'
    unit = work / 'vibrator.cpp'
    unit.write_text(f'#include "{header}"\n' + ''.join(f'extern "C" {proto};\n' for proto in found))
    run(['xcrun', 'clang++', '-std=c++17', *WARN, '-fsyntax-only', unit])
    return f'PASS vibrator: {len(found)} weak stream calls of PaganiAudio.cpp declared as PaganiHaptics.h declares them'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path)
    parser.add_argument('--kern-exec', type=Path)
    parser.add_argument('--patch', type=Path)
    parser.add_argument('--sheng', type=Path, default=SHENG)
    parser.add_argument('--no-ios', action='store_true')
    args = parser.parse_args()
    results = []
    with tempfile.TemporaryDirectory(prefix='pagani-audio-host-') as tmp:
        work = Path(tmp)
        sheng_abi = args.sheng / 'package/ShengAudioAbi.h'
        extra = [f'-DSHENG_ABI_H="{sheng_abi}"'] if sheng_abi.is_file() else []
        run(['xcrun', 'clang', '-std=c11', *WARN, '-I', SOURCE, *extra, SOURCE / 'Test/abi.c', '-o', work / 'abi'])
        results.append(last_line(run([work / 'abi'])))
        run(['xcrun', 'clang++', '-std=c++17', *WARN, '-x', 'c++', '-fsyntax-only', SOURCE / 'PaganiAudioAbi.h'])
        run(['xcrun', 'clang++', '-std=c++17', *WARN, '-x', 'c++', '-fsyntax-only', '-I', SOURCE, SOURCE / 'PaganiAudioEngine.h'])
        if not args.no_ios:
            ios = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
            run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-std=c11', *WARN, '-I', SOURCE, '-fsyntax-only',
                 SOURCE / 'Test/abi.c'])
        run(['xcrun', 'clang', '-std=c11', *WARN, *SANITIZE, '-I', SOURCE, SOURCE / 'Test/engine.c', SOURCE / 'PaganiAudioEngine.c',
             '-o', work / 'engine'])
        results.append(last_line(run([work / 'engine'])))
        run(['xcrun', 'clang', '-std=c11', *WARN, *SANITIZE, '-I', SOURCE, SOURCE / 'Test/bridge.c', SOURCE / 'PaganiAudioEngine.c',
             SOURCE / 'PaganiAudioHaptics.c', '-o', work / 'bridge'])
        results.append(last_line(run([work / 'bridge'])))
        run(['xcrun', 'clang', '-std=gnu11', *WARN, *SANITIZE, '-I', SOURCE, SOURCE / 'Test/plugin.c', SOURCE / 'PaganiAudioEngine.c',
             SOURCE / 'PaganiAudioHaptics.c', SOURCE / 'hal/PaganiAudioPlugIn.c', '-framework', 'CoreFoundation', '-o', work / 'plugin'])
        results.append(last_line(run([work / 'plugin'])))
        results.append(last_line(run([work / 'plugin', 'actuator'])))
        for case in ('expected', 'header', 'revision', 'memory'):
            results.append(last_line(run([work / 'plugin', 'listed', case])))
        for case in ('before', 'after', 'late', 'present', 'uuid', 'product', 'nosymbol', 'off', 'noactuator'):
            results.append(last_line(run([work / 'plugin', 'vasettings', case])))
        # The kernel glue (PaganiAudio.cpp, PaganiAudioService.cpp) with the kernel stand-ins of Test/kstubs; glue_log.c is a translation
        # unit of its own so that its strong boot_progress_trace replaces the weak default, as BootProgress does in the kernel link.
        run(['xcrun', 'clang', '-std=c11', *WARN, *SANITIZE, '-I', SOURCE, '-c', SOURCE / 'PaganiAudioEngine.c', '-o', work / 'glue-engine.o'])
        run(['xcrun', 'clang', '-std=c11', *WARN, *SANITIZE, '-I', SOURCE, '-c', SOURCE / 'PaganiAudioHaptics.c', '-o',
             work / 'glue-haptics.o'])
        run(['xcrun', 'clang', '-std=c11', *WARN, *SANITIZE, '-c', SOURCE / 'Test/glue_log.c', '-o', work / 'glue-log.o'])
        run(['xcrun', 'clang', '-std=c11', *WARN, *SANITIZE, '-c', SOURCE / 'Test/glue_haptics.c', '-o', work / 'glue-vibrator.o'])
        run(['xcrun', 'clang++', '-std=c++17', *WARN, '-Wno-unused-parameter', *SANITIZE, '-I', SOURCE / 'Test/kstubs', '-I', SOURCE,
             '-c', SOURCE / 'Test/glue.cpp', '-o', work / 'glue.o'])
        run(['xcrun', 'clang++', *SANITIZE, work / 'glue.o', work / 'glue-log.o', work / 'glue-engine.o', work / 'glue-haptics.o',
             work / 'glue-vibrator.o', '-o', work / 'glue'])
        results.append(last_line(run([work / 'glue'])))
        results.append(check_vibrator_prototypes(work))
        results.append(check_va_settings())
        results.append(check_verdict())
        results.append(check_barriers(work))
        results.append(check_trust_merge())
        if not args.no_ios:
            results.append(last_line(run([sys.executable, '-B', SOURCE / 'scripts/build-pagani-audio-hal.py', '--out', work / 'hal'])))
            executable = work / 'hal/PaganiAudio.driver/PaganiAudio'
            results.append('PASS barriers: ' + check_writer(disassemble(executable, '_pagani_audio_probe_route'), 'iOS plug-in'))
        if args.tree:
            results.append(last_line(run([sys.executable, '-B', SOURCE / 'scripts/build-pagani-audio.py', '--tree', args.tree, '--out',
                                          work / 'kernel'])))
            results.append('PASS barriers: ' + check_reader(disassemble(work / 'kernel/obj/PaganiAudio.o'), 'kernel archive'))
        if args.kern_exec:
            made = work / 'mount.patch'
            results.append(last_line(run([sys.executable, '-B', SOURCE / 'scripts/make-audio-mount-patch.py', '--kern-exec', args.kern_exec,
                                          '--out', made])))
            if args.patch:
                if args.patch.read_bytes() != made.read_bytes():
                    raise SystemExit(f'FAIL: {args.patch} is not the patch make-audio-mount-patch.py makes for {args.kern_exec}')
                results.append(f'PASS patch: {args.patch} is current')
    print('\n'.join(results))


if __name__ == '__main__':
    main()
