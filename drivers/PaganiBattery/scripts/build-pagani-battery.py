#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/pagani-battery/libPaganiBattery.a (the OnePlus 13T battery power source) with the actual XNU compile records of a tree.

Run by the UniXNU kernel step as the PaganiBattery component recipe ("{component}/scripts/build-pagani-battery.py", cwd = the tree).
The archive holds the portable core (BatteryBattmgr.c, BatteryGauge.c, BatteryI2c.c, BatterySpmi.c, BatteryUsb.c, BatteryCore.c, BatteryProps.c),
the kernel glue PaganiBattery.cpp and PaganiBatteryService.cpp, whose class initializer is taken out of __mod_init_func and exported
for the late registration in pagani_battery_start (as PaganiAudio and Stage7Probe stage theirs); no other object may carry a static
initializer. Kernel code here never uses floating point (-mgeneral-regs-only).

PaganiAdsp's interface header (PaganiAdsp.h) is found next to this component (../PaganiAdsp), in TREE/drivers/PaganiAdsp or with --adsp;
the archive then carries weak defaults of its five entry points, which PaganiAdsp's strong ones replace whenever it is linked.
--without-adsp builds the gauge-only variant (no channel code at all).

Linking: with --append-to ARCHIVE (the device's recipe passes build/adreno830/libAdreno830.a, which the kernel link force-loads) the
objects also go into that archive, checked for strong symbols defined twice, so no link setting changes. Without it, link
build/pagani-battery/libPaganiBattery.a with -force_load (CONFIG_XNU_EXTRA_LD_ARCHIVES).

  build-pagani-battery.py [--tree TREE] [--source DIR] [--adsp DIR | --without-adsp] [--out DIR] [--append-to ARCHIVE]

--out defaults to TREE/build/pagani-battery; a check build passes another directory and leaves the tree as it is (the compile records
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
C_SOURCES = ['BatteryBattmgr.c', 'BatteryGauge.c', 'BatteryI2c.c', 'BatterySpmi.c', 'BatteryUsb.c', 'BatteryCore.c', 'BatteryProps.c']
CXX_SOURCES = ['PaganiBattery.cpp']
STAGED = 'PaganiBatteryService.cpp'
HEADERS = ['BatteryText.h', 'BatteryBattmgr.h', 'BatteryGauge.h', 'BatteryI2c.h', 'BatterySpmi.h', 'BatteryUsb.h', 'BatteryCore.h',
           'BatteryProps.h', 'PaganiBattery.h']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
INITIALIZER = '__GLOBAL__sub_I_PaganiBatteryService.cpp'
REQUIRED = ['_pagani_battery_start', '_pagani_battery_status', '_pagani_battery_service_create', '_pagani_battery_service_update',
            '_pagani_battery_core_init', '_pagani_battery_core_run', '_pagani_bm_receive', '_pagani_gauge_read', '_pagani_bi2c_bringup',
            '_pagani_bi2c_xfer', '_pagani_spmi_survey', '_pagani_spmi_map', '_pagani_usb_step', '_pagani_battery_props',
            '_pagani_gauge_transfer_allowed', INITIALIZER]
# Defaults for providers a kernel may lack (BootProgress) and the GCC vote lock word: weak, the real ones or a later user's win (only
# this driver takes the lock word today; the touch and speaker clock code do not, see BatteryI2c.h).
WEAK = ['_boot_progress_trace', '_sm8750_gcc_vote_owner', '_pagani_panel_probe_quiet']
ADSP_WEAK = ['_pagani_adsp_channel', '_pagani_adsp_channel_config', '_pagani_adsp_send', '_pagani_adsp_channel_up', '_pagani_adsp_status']
# What the archive may take from the kernel besides IOKit/libkern C++ symbols: anything else is a typo or a dependency to review.
KERNEL_IMPORTS = {'_IOLog', '_IODelay', '_IOSleep', '_IOSimpleLockAlloc', '_IOMalloc_internal', '_IOFree', '_KHEAP_DEFAULT', '_PE_parse_boot_argn',
                  '_absolutetime_to_nanoseconds', '_mach_absolute_time', '_assert_wait_timeout', '_clear_wait', '_current_thread',
                  '_thread_block', '_thread_wakeup_prim', '_kernel_thread_start', '_thread_deallocate', '_thread_set_thread_name',
                  '_memcpy', '_memset', '_bzero', '_g_kernel_kmod_info', '_kOSBooleanTrue', '_kOSBooleanFalse', '_lck_spin_lock',
                  '_lck_spin_unlock', '_lck_spin_lock_grp', '_IOSimpleLockLock', '_IOSimpleLockUnlock', '___stack_chk_fail',
                  '___stack_chk_guard', '_OSObject_typed_operator_new', '_OSObject_typed_operator_delete',
                  # the record's -fsanitize=signed-integer-overflow (minimal runtime: counted, no panic without -ubsan_force_panic)
                  '___ubsan_handle_add_overflow_minimal', '___ubsan_handle_sub_overflow_minimal',
                  '___ubsan_handle_mul_overflow_minimal', '___ubsan_handle_divrem_overflow_minimal',
                  '___ubsan_handle_negate_overflow_minimal'}
# The PaganiAdsp entry points the glue calls (strong in PaganiAdsp, weak here).
ADSP_IMPORTS = {'_pagani_adsp_channel', '_pagani_adsp_send'}


def fail(message):
    raise SystemExit('build-pagani-battery: ' + message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    prefix = subprocess.run(['brew', '--prefix', 'llvm'], capture_output=True, text=True).stdout.strip()
    candidate = Path(prefix) / 'bin' / name
    if prefix and candidate.is_file():
        return str(candidate)
    fail(f'{name} not found; put Homebrew LLVM on PATH')


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


def audit(obj):
    """ARM64 MH_OBJECT for iOS, minimum 17.0."""
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


def text(obj):
    fileoff, length = sections(obj)[('__TEXT', '__text')]
    return obj.read_bytes()[fileoff:fileoff + length]


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
    """Strong definitions of each member of an archive (to catch one symbol defined by two members)."""
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


def find_adsp(args, tree, source):
    if args.without_adsp:
        return None
    candidates = [args.adsp] if args.adsp else [source.parent / 'PaganiAdsp', tree / 'drivers/PaganiAdsp']
    for candidate in candidates:
        if candidate and (Path(candidate) / 'PaganiAdsp.h').is_file():
            return Path(candidate).resolve()
    fail('PaganiAdsp.h not found (' + ', '.join(map(str, candidates)) + '); pass --adsp DIR, or --without-adsp for the gauge-only build')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiBattery sources (default: this directory)')
    parser.add_argument('--adsp', type=Path, help='directory with PaganiAdsp.h (default: ../PaganiAdsp or TREE/drivers/PaganiAdsp)')
    parser.add_argument('--without-adsp', action='store_true', help='gauge-only build: no battery manager channel')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-battery)')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({RECORD_DIR}); run the UniXNU kernel step first')
    missing = [name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    adsp = find_adsp(args, tree, source)
    out = (args.out or tree / 'build/pagani-battery').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(tree / '.cache/xnu-work/osfmk'), '-mgeneral-regs-only'] + WARNINGS
    extra += ['-I', str(adsp)] if adsp else ['-DPAGANI_BATTERY_NO_ADSP=1']
    objects = [compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.battery.o'), extra) for name in C_SOURCES]
    objects += [compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.battery.o'), extra) for name in CXX_SOURCES]
    for obj in objects:
        if any(sect == '__mod_init_func' for _, sect in sections(obj)):
            fail(f'{obj.name} has a static initializer; only {STAGED} may (registered late by pagani_battery_start)')
    service = compile_one(cxx_record, source / STAGED, out / 'obj/PaganiBatteryService.battery.o', extra)
    late = out / 'obj/PaganiBatteryService.battery.late.o'
    late.unlink(missing_ok=True)
    subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=' + INITIALIZER, str(service),
                    str(late)], check=True)
    if text(service) != text(late) or any(sect == '__mod_init_func' for _, sect in sections(late)):
        fail('staging the service class initializer changed its instructions or kept __mod_init_func')
    objects.append(late)

    archive = out / 'libPaganiBattery.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    members = [m for m in subprocess.run(['xcrun', 'ar', 't', str(archive)], check=True, capture_output=True,
                                         text=True).stdout.splitlines() if m and not m.startswith('__.SYMDEF')]
    if sorted(members) != sorted(obj.name for obj in objects):
        fail(f'archive members {sorted(members)} differ from the objects')
    defined, undefined = symbols([archive])
    weak = WEAK + (ADSP_WEAK if adsp else [])
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in weak if defined.get(name) != 'weak']
    if not adsp:
        problems += [f'{name} defined in the gauge-only build' for name in ADSP_WEAK if name in defined]
    unexpected = sorted(name for name in undefined if name not in KERNEL_IMPORTS and not name.startswith('__Z'))
    problems += [f'unexpected kernel import {name}' for name in unexpected]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS] + [Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [obj.name for obj in objects],
              'source_sha256': {p.name: sha(p) for p in listed}, 'adsp_header': str(adsp / 'PaganiAdsp.h') if adsp else None,
              'staged_initializer': INITIALIZER, 'weak': weak, 'undefined': sorted(undefined)}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list the archive\'s own recipe (Adreno830) before PaganiBattery')
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
    print(f'PASS: {len(objects)} objects in {archive}{where} ({INITIALIZER} staged; {len(REQUIRED)} entry points, {len(weak)} weak '
          f'defaults, {len(undefined)} kernel imports; PaganiAdsp header: {adsp or "none (gauge only)"})')


if __name__ == '__main__':
    main()
