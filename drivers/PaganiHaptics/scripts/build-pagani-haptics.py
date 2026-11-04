#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/pagani-haptics/libPaganiHaptics.a (the OnePlus 13T vibrator) with the actual XNU compile records of a tree.

Run by the UniXNU kernel step as the PaganiHaptics component recipe ("{component}/scripts/build-pagani-haptics.py", cwd = the tree),
like PaganiBattery's. The archive holds the portable core (HapticsSpmi.c, HapticsCore.c, HapticsIos.c) and the kernel glue PaganiHaptics.cpp and
PaganiHapticsIos.cpp (the iOS haptics switch: /product/haptics with pagani-haptics-ios); no
object may carry a static initializer (the glue registers no IOKit class). Kernel code here never uses floating point
(-mgeneral-regs-only).

Linking: with --append-to ARCHIVE (the device's recipe passes build/adreno830/libAdreno830.a, which the kernel link force-loads) the
objects also go into that archive, checked for strong symbols defined twice, so no link setting changes. Without it, link
build/pagani-haptics/libPaganiHaptics.a with -force_load (CONFIG_XNU_EXTRA_LD_ARCHIVES).

  build-pagani-haptics.py [--tree TREE] [--source DIR] [--out DIR] [--append-to ARCHIVE]

--out defaults to TREE/build/pagani-haptics; a check build passes another directory and leaves the tree as it is (the compile records
are only read). Zero warnings: -Wall -Wextra -Werror with the policy of the reference drivers."""
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
C_SOURCES = ['HapticsSpmi.c', 'HapticsCore.c', 'HapticsIos.c']
CXX_SOURCES = ['PaganiHaptics.cpp', 'PaganiHapticsIos.cpp']
HEADERS = ['HapticsRegs.h', 'HapticsText.h', 'HapticsSpmi.h', 'HapticsCore.h', 'HapticsIos.h', 'PaganiHaptics.h', 'PaganiHapticsBoard.h']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
REQUIRED = ['_pagani_haptics_start', '_pagani_haptics_status', '_pagani_haptics_constant', '_pagani_haptics_effect',
            '_pagani_haptics_stream_begin', '_pagani_haptics_stream_write', '_pagani_haptics_stream_end', '_pagani_haptics_stop',
            '_pagani_haptics_info', '_pgh_core_init', '_pgh_core_command', '_pgh_core_poll', '_pgh_spmi_write', '_pgh_ring_push',
            '_pagani_haptics_ios_publish', '_pagani_haptics_ios_commit', '_pagani_haptics_ios_status', '_pgh_ios_plan', '_pgh_ios_final']
# Defaults for providers a kernel may lack: BootProgress's trace, PaganiBattery's status and PaganiAudio's actuator answer (weak, the real
# ones win).
WEAK = ['_boot_progress_trace', '_pagani_battery_status', '_pagani_audio_actuator_enabled']
# What the archive may take from the kernel besides IOKit/libkern C++ symbols: anything else is a typo or a dependency to review.
KERNEL_IMPORTS = {'_IOLog', '_IODelay', '_IOSleep', '_IOSimpleLockAlloc', '_IOSimpleLockLock', '_IOSimpleLockUnlock', '_IOLockAlloc',
                  '_IOLockLock', '_IOLockUnlock', '_IOMalloc_internal', '_IOMallocZero_internal', '_KHEAP_DEFAULT',
                  '_PE_parse_boot_argn', '_absolutetime_to_nanoseconds', '_mach_absolute_time', '_assert_wait_timeout', '_clear_wait',
                  '_current_thread', '_thread_block', '_thread_wakeup_prim', '_kernel_thread_start', '_thread_deallocate',
                  '_thread_set_thread_name', '_memcpy', '_memset', '_bzero', '_lck_spin_lock', '_lck_spin_unlock',
                  # the iOS haptics switch (PaganiHapticsIos.cpp): the device tree plane and IOService's allocation
                  '_gIODTPlane', '_OSObject_typed_operator_new', '_OSObject_typed_operator_delete',
                  '_lck_spin_lock_grp', '_lck_mtx_lock', '_lck_mtx_unlock', '___stack_chk_fail', '___stack_chk_guard',
                  '___ubsan_handle_add_overflow_minimal', '___ubsan_handle_sub_overflow_minimal',
                  '___ubsan_handle_mul_overflow_minimal', '___ubsan_handle_divrem_overflow_minimal',
                  '___ubsan_handle_negate_overflow_minimal'}


def fail(message):
    raise SystemExit('build-pagani-haptics: ' + message)


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
        header = re.match(r'^.*\((.+\.o)\):$', line)
        if header:
            member = header.group(1)
            continue
        match = re.search(r'\((?:__\w+,[^)]+|absolute)\) (weak )?(?:private )?external (?:\[[^\]]*\] )*'
                          r'(?:automatically hidden )?(\S+)', line)
        if match and not match.group(1):
            owners.setdefault(match.group(2), set()).add(member)
    return owners


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiHaptics sources (default: this directory)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-haptics)')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({RECORD_DIR}); run the UniXNU kernel step first')
    missing = [name for name in C_SOURCES + CXX_SOURCES + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    out = (args.out or tree / 'build/pagani-haptics').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(tree / '.cache/xnu-work/osfmk'), '-mgeneral-regs-only'] + WARNINGS
    objects = [compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.haptics.o'), extra) for name in C_SOURCES]
    objects += [compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.haptics.o'), extra) for name in CXX_SOURCES]
    archive = out / 'libPaganiHaptics.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    defined, undefined = symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    unexpected = sorted(name for name in undefined if name not in KERNEL_IMPORTS and not name.startswith('__Z'))
    problems += [f'unexpected kernel import {name}' for name in unexpected]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + HEADERS] + [Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [obj.name for obj in objects],
              'source_sha256': {p.name: sha(p) for p in listed}, 'weak': WEAK, 'undefined': sorted(undefined)}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list the archive\'s own recipe (Adreno830) before PaganiHaptics')
        target_defined, _ = symbols([target])
        clash = [name for name, kind in defined.items() if kind == 'strong' and target_defined.get(name) == 'strong']
        if clash:
            fail(f'strong symbols already in {target}: ' + ', '.join(sorted(clash)))
        subprocess.run(['xcrun', 'ar', 'rcs', str(target), *map(str, objects)], check=True)
        twice = {name: m for name, m in strong_per_member(target).items() if len(m) > 1}
        if twice:
            fail(f'{target}: symbols defined by more than one member: ' + ', '.join(sorted(twice)))
        report['appended_to'] = str(target)
        report['appended_sha256'] = sha(target)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    where = f' and appended to {args.append_to}' if args.append_to else ''
    print(f'PASS: {len(objects)} objects in {archive}{where} ({len(REQUIRED)} entry points, {len(WEAK)} weak defaults, '
          f'{len(undefined)} kernel imports)')


if __name__ == '__main__':
    main()
