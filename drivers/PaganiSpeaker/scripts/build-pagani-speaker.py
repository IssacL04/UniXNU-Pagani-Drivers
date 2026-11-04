#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/pagani-speaker/libPaganiSpeaker.a (the OnePlus 13T speaker path) with the actual XNU compile records of a tree.

Run by the UniXNU kernel step as the PaganiSpeaker component recipe ("{component}/scripts/build-pagani-speaker.py", cwd = the tree),
like PaganiBattery's and PaganiHaptics'. The archive holds the portable core (GeniI2c.c, PaganiGpr.c, PaganiReach.c, SpeakerBackend.c,
SpeakerCore.c, SpeakerHw.c, SpeakerLimit.c, SpeakerTone.c, Tfa9873.c) and the kernel glue PaganiSpeaker.cpp; no object may carry a
static initializer (the glue registers no IOKit class). Kernel code here never uses floating point (-mgeneral-regs-only).

Headers from other components, found next to this one (../PaganiAdsp, ../PaganiAudio, ../PaganiInput), in TREE/drivers/<name> or
given: PaganiAdsp.h (the archive carries weak defaults of its five entry points, which PaganiAdsp's strong ones replace), PaganiAudio's
PaganiAudioEngine.h/PaganiAudioAbi.h (weak defaults of the service calls; PaganiAudio's force-loaded archive replaces them),
PaganiInput's Drv2Lock.h (the DRV-2 lock word, weak) and the SoC kit Sm8750Platform (TREE/kits/Sm8750Platform or
../../../UniXNU-sm8750-common/kits/Sm8750Platform): PeripheralIO.h (the shared GCC vote lock word sm8750_gcc_vote_owner and its
update, weak), Rpmh.h.

Linking: with --append-to ARCHIVE (the device's recipe passes build/adreno830/libAdreno830.a, which the kernel link force-loads) the
objects also go into that archive, checked for strong symbols defined twice, so no link setting changes. Without it, link
build/pagani-speaker/libPaganiSpeaker.a with -force_load (CONFIG_XNU_EXTRA_LD_ARCHIVES). Either way every strong symbol of the
archive is checked against every other archive the tree built (TREE/build/*/lib*.a): none may define it too.

  build-pagani-speaker.py [--tree TREE] [--source DIR] [--adsp DIR] [--audio DIR] [--input DIR] [--kit DIR] [--out DIR]
                          [--append-to ARCHIVE]

--out defaults to TREE/build/pagani-speaker; a check build passes another directory and leaves the tree as it is (the compile records
and the tree's archives are only read). Zero warnings: -Wall -Wextra -Werror with the policy of the reference drivers."""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
RECORD_DIR = Path('build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837')
CXX_RECORD = RECORD_DIR / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = RECORD_DIR / 'bsd/DEVELOPMENT/bsd_init.o.json'
C_SOURCES = ['GeniI2c.c', 'PaganiGpr.c', 'PaganiReach.c', 'SpeakerBackend.c', 'SpeakerCore.c', 'SpeakerHw.c', 'SpeakerLimit.c',
             'SpeakerTone.c', 'Tfa9873.c']
CXX_SOURCES = ['PaganiSpeaker.cpp']
HEADERS = ['GeniI2c.h', 'PaganiGpr.h', 'PaganiReach.h', 'SpeakerBackend.h', 'SpeakerBoard.h', 'SpeakerCore.h', 'SpeakerHw.h',
           'SpeakerLimit.h', 'SpeakerSmmu.h', 'SpeakerTone.h', 'Tfa9873.h']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
REQUIRED = ['_pagani_speaker_start', '_speaker_core_init', '_speaker_core_bringup', '_speaker_core_tones', '_speaker_core_tone_on',
            '_speaker_core_start', '_speaker_core_stop', '_speaker_core_pump', '_speaker_core_receive', '_speaker_core_state',
            '_speaker_backend_init', '_speaker_clocks_on', '_speaker_bus_transfer', '_speaker_limit_init', '_speaker_limit_run',
            '_speaker_limit_need', '_speaker_tone_fill', '_tfa9873_init_program', '_pagani_geni_i2c_prepare', '_pagani_gpr_encode',
            '_pagani_ar_parse']
# Defaults for providers a kernel may lack (BootProgress, PaganiAudio, PaganiAdsp) and the shared lock words: weak, the real ones (or
# another user's identical weak word) win.
WEAK = ['_boot_progress_trace', '_pagani_audio_set_backend', '_pagani_audio_wake', '_pagani_audio_engine_take',
        '_pagani_audio_engine_played', '_pagani_audio_engine_captured', '_pagani_audio_engine_align_output',
        '_pagani_audio_engine_frame_at', '_pagani_audio_silent_backend', '_sm8750_rpmh_drv2_owner', '_sm8750_gcc_vote_owner',
        '_sm8750_gcc_vote_unlocked', '_pagani_adsp_channel', '_pagani_adsp_channel_config', '_pagani_adsp_send',
        '_pagani_adsp_channel_up', '_pagani_adsp_status', '_pagani_panel_probe_quiet']
# What the archive may take from the kernel besides IOKit/libkern C++ symbols: anything else is a typo or a dependency to review.
KERNEL_IMPORTS = {'_IOLog', '_IODelay', '_IOSleep', '_IOMallocZeroData_impl', '_IOMallocZero_internal', '_IOMalloc_internal',
                  '_KHEAP_DATA_BUFFERS', '_KHEAP_DEFAULT', '_PE_parse_boot_argn', '_absolutetime_to_nanoseconds',
                  '_mach_absolute_time', '_assert_wait_timeout', '_clear_wait', '_current_thread', '_thread_block',
                  '_thread_wakeup_prim', '_kernel_thread_start', '_thread_deallocate', '_thread_set_thread_name', '_gPhysBase',
                  '_gPhysSize', '_memcpy', '_memset', '_bzero', '_strcmp', '___stack_chk_fail', '___stack_chk_guard',
                  # the record's -fsanitize=signed-integer-overflow (minimal runtime: counted, no panic without -ubsan_force_panic)
                  '___ubsan_handle_add_overflow_minimal', '___ubsan_handle_sub_overflow_minimal',
                  '___ubsan_handle_mul_overflow_minimal', '___ubsan_handle_divrem_overflow_minimal',
                  '___ubsan_handle_negate_overflow_minimal'}


def fail(message):
    raise SystemExit('build-pagani-speaker: ' + message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_record(tree, relative):
    path = tree / relative
    if not path.is_file():
        fail(f'{path} missing; the XNU bootstrap pass (XNU_BOOTSTRAP=1) leaves the compile records this build replays')
    return json.loads(path.read_text().strip().rstrip(','))


def command(record, source, obj, extra):
    """The record's command for another source and object: its ABI flags, without its warning and dependency options."""
    arguments, result, i = record['arguments'], [], 0
    while i < len(arguments):
        value = arguments[i]
        if value == record['file']:
            result.append(str(source))
        elif value == '-o':
            result += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ', '-MT', '-MQ'):
            i += 1
        elif value == '-Xclang' and i + 1 < len(arguments) and arguments[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP', '-MMD'):
            result.append(value)
        i += 1
    return result + extra


def sections(obj):
    """{(segment, section): (fileoff, size)} of an MH_OBJECT's one LC_SEGMENT_64."""
    data = obj.read_bytes()
    count = struct.unpack_from('<I', data, 16)[0]
    offset, result = 32, {}
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            for k in range(nsects):
                at = offset + 72 + 80 * k
                sect = data[at:at + 16].rstrip(b'\0').decode()
                seg = data[at + 16:at + 32].rstrip(b'\0').decode()
                _, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                result[(seg, sect)] = (fileoff, length)
        offset += size
    return result


def audit(obj):
    """ARM64 MH_OBJECT for iOS, minimum 17.0, no static initializer."""
    data = obj.read_bytes()
    magic, cpu, _, kind, count, _ = struct.unpack_from('<6I', data)
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c, 1):
        fail(f'{obj}: not an ARM64 Mach-O object')
    offset, version = 32, None
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x32:
            version = struct.unpack_from('<II', data, offset + 8)
        offset += size
    if version != (2, 0x110000):
        fail(f'{obj}: LC_BUILD_VERSION {version} is not iOS 17.0')
    if any(sect == '__mod_init_func' for _, sect in sections(obj)):
        fail(f'{obj.name} has a static initializer')


def compile_one(record, source, obj, extra):
    obj.parent.mkdir(parents=True, exist_ok=True)
    obj.unlink(missing_ok=True)
    result = subprocess.run(command(record, source, obj, extra), cwd=record['directory'], capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    if result.returncode:
        fail(f'{source}:\n{output[-6000:]}')
    if 'warning:' in output:
        fail(f'{source} compiled with warnings:\n{output[-6000:]}')
    audit(obj)
    return obj


def symbols(paths):
    """{name: 'weak'|'strong'} defined and {name} undefined over objects or archives, from nm -m."""
    output = subprocess.run(['xcrun', 'nm', '-m', *map(str, paths)], check=True, capture_output=True, text=True).stdout
    defined, undefined = {}, set()
    for line in output.splitlines():
        match = re.search(r'\(undefined\) (?:weak )?external (?:\[[^\]]*\] )*(\S+)', line)
        if match:
            undefined.add(match.group(1))
            continue
        match = re.search(r'\((?:__\w+,[^)]+|absolute|common)\) (weak )?(?:private )?external (?:\[[^\]]*\] )*'
                          r'(?:automatically hidden )?(\S+)', line)
        if match:
            name = match.group(2)
            kind = 'weak' if match.group(1) else 'strong'
            defined[name] = 'strong' if defined.get(name) == 'strong' else kind
    return defined, undefined - set(defined)


def strong_per_member(archive):
    output = subprocess.run(['xcrun', 'nm', '-m', str(archive)], check=True, capture_output=True, text=True).stdout
    member, owners = None, {}
    for line in output.splitlines():
        # a member's header: "archive(member.o):" or, from newer nm, "member.o:"
        header = re.match(r'^(?:.*\((.+\.o)\)|([^\s()]+\.o)):$', line)
        if header:
            member = header.group(1) or header.group(2)
            continue
        match = re.search(r'\((?:__\w+,[^)]+|absolute)\) (weak )?(?:private )?external (?:\[[^\]]*\] )*'
                          r'(?:automatically hidden )?(\S+)', line)
        if match and not match.group(1):
            owners.setdefault(match.group(2), set()).add(member)
    return owners


def find(given, candidates, marker, what):
    for candidate in ([given] if given else candidates):
        if candidate and (Path(candidate) / marker).is_file():
            return Path(candidate).resolve()
    fail(f'{marker} ({what}) not found in ' + ', '.join(str(c) for c in ([given] if given else candidates)))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiSpeaker sources (default: this directory)')
    parser.add_argument('--adsp', type=Path, help='directory with PaganiAdsp.h')
    parser.add_argument('--audio', type=Path, help='directory with PaganiAudioEngine.h')
    parser.add_argument('--input', type=Path, help='directory with Drv2Lock.h (PaganiInput)')
    parser.add_argument('--kit', type=Path, help='the SoC kit directory with PeripheralIO.h (Sm8750Platform)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-speaker)')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({RECORD_DIR}); run the UniXNU kernel step first')
    missing = [name for name in C_SOURCES + CXX_SOURCES + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    adsp = find(args.adsp, [source.parent / 'PaganiAdsp', tree / 'drivers/PaganiAdsp'], 'PaganiAdsp.h', 'PaganiAdsp')
    audio = find(args.audio, [source.parent / 'PaganiAudio', tree / 'drivers/PaganiAudio'], 'PaganiAudioEngine.h', 'PaganiAudio')
    drv2 = find(args.input, [source.parent / 'PaganiInput', tree / 'drivers/PaganiInput'], 'Drv2Lock.h', 'PaganiInput')
    kit = find(args.kit, [tree / 'kits/Sm8750Platform', source.parents[2] / 'UniXNU-sm8750-common/kits/Sm8750Platform'],
               'PeripheralIO.h', 'the Sm8750Platform kit')
    if 'sm8750_gcc_vote_set' not in (kit / 'PeripheralIO.h').read_text():
        fail(f'{kit}/PeripheralIO.h has no sm8750_gcc_vote_set (the shared GCC vote update this driver and the touch take)')
    out = (args.out or tree / 'build/pagani-speaker').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(adsp), '-I', str(audio), '-I', str(drv2), '-I', str(kit),
             '-I', str(tree / '.cache/xnu-work/osfmk'), '-mgeneral-regs-only'] + WARNINGS
    objects = [compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.speaker.o'), extra) for name in C_SOURCES]
    objects += [compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.speaker.o'), extra) for name in CXX_SOURCES]
    archive = out / 'libPaganiSpeaker.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    members = [m for m in subprocess.run(['xcrun', 'ar', 't', str(archive)], check=True, capture_output=True,
                                         text=True).stdout.splitlines() if m and not m.startswith('__.SYMDEF')]
    if sorted(members) != sorted(obj.name for obj in objects):
        fail(f'archive members {sorted(members)} differ from the objects')
    defined, undefined = symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    unexpected = sorted(name for name in undefined if name not in KERNEL_IMPORTS and not name.startswith('__Z'))
    problems += [f'unexpected kernel import {name}' for name in unexpected]
    twice = {name: m for name, m in strong_per_member(archive).items() if len(m) > 1}
    problems += [f'{name} defined strong by {sorted(m)}' for name, m in twice.items()]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    # Every other archive the tree built (the kernel link takes them): none may define one of these strong symbols too. Members of
    # ours appended by an earlier run (*.speaker.o, replaced by the next append) are not others.
    ours = {name for name, kind in defined.items() if kind == 'strong'}
    others = sorted(p for p in (tree / 'build').glob('*/lib*.a') if p.resolve().parent != out and p.resolve() != archive)

    def foreign_strong(path):
        return {name for name, owners in strong_per_member(path).items() if any(not (m or '').endswith('.speaker.o') for m in owners)}

    clashes = []
    for other in others:
        hit = sorted(ours & foreign_strong(other))
        if hit:
            clashes.append(f'{other.relative_to(tree)}: ' + ', '.join(hit[:8]) + (' ...' if len(hit) > 8 else ''))
    if clashes:
        fail('strong symbols also defined by other archives of the tree:\n  ' + '\n  '.join(clashes))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + HEADERS] + [kit / 'PeripheralIO.h', Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [obj.name for obj in objects],
              'source_sha256': {p.name: sha(p) for p in listed}, 'headers': {'adsp': str(adsp), 'audio': str(audio),
              'drv2': str(drv2), 'kit': str(kit)}, 'weak': WEAK, 'undefined': sorted(undefined),
              'checked_against': [str(p.relative_to(tree)) for p in others]}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list the archive\'s own recipe (Adreno830) before PaganiSpeaker')
        clash = sorted(ours & foreign_strong(target))
        if clash:
            fail(f'strong symbols already in {target}: ' + ', '.join(clash))
        subprocess.run(['xcrun', 'ar', 'rcs', str(target), *map(str, objects)], check=True)
        twice = {name: m for name, m in strong_per_member(target).items() if len(m) > 1}
        if twice:
            fail(f'{target}: symbols defined by more than one member: ' + ', '.join(sorted(twice)))
        report['appended_to'] = str(target)
        report['appended_sha256'] = sha(target)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    where = f' and appended to {args.append_to}' if args.append_to else ''
    print(f'PASS: {len(objects)} objects in {archive}{where} ({len(REQUIRED)} entry points, {len(WEAK)} weak defaults, '
          f'{len(undefined)} kernel imports, no strong symbol in the {len(others)} other archives of the tree)')


if __name__ == '__main__':
    main()
