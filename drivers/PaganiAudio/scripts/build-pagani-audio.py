#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build build/pagani-audio/libPaganiAudio.a, the OnePlus 13T audio service, with the actual XNU compile records of a UniXNU tree.

Run by the UniXNU kernel step as the PaganiAudio component recipe ("{component}/scripts/build-pagani-audio.py", cwd = the tree). The
archive holds PaganiAudio.cpp (start, service thread, boot log), PaganiAudioEngine.c (rings, timeline, silent backend),
PaganiAudioHaptics.c (the haptic actuator bridge to PaganiHaptics' stream API, whose weak defaults PaganiAudio.cpp carries) and
PaganiAudioService.cpp, whose class initializer is taken out of __mod_init_func and exported for the late registration in
pagani_audio_start (as build-stage7-display.py stages the peripherals); no other object may carry a static initializer. Kernel code
here never uses floating point (-mgeneral-regs-only). The kernel links the archive with -force_load through the device defconfig's
CONFIG_XNU_EXTRA_LD_ARCHIVES; Stage7Probe calls pagani_audio_start (weak default there).

  build-pagani-audio.py [--tree TREE] [--source DIR] [--out DIR]

--out defaults to TREE/build/pagani-audio; a check build passes another directory and leaves the tree as it is (the compile records are
only read). Zero warnings: -Wall -Wextra -Werror with the policy of the reference drivers (build-adreno830.py)."""
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
CXX_SOURCES = ['PaganiAudio.cpp']
STAGED = 'PaganiAudioService.cpp'
C_SOURCES = ['PaganiAudioEngine.c', 'PaganiAudioHaptics.c']
HEADERS = ['PaganiAudioAbi.h', 'PaganiAudioEngine.h', 'PaganiAudioHaptics.h']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
INITIALIZER = '__GLOBAL__sub_I_PaganiAudioService.cpp'
REQUIRED = ['_pagani_audio_start', '_pagani_audio_set_backend', '_pagani_audio_wake', '_pagani_audio_service_publish', '_pagani_audio_memory',
            '_pagani_audio_request', '_pagani_audio_client_opened', '_pagani_audio_client_closed', '_pagani_audio_host_clock_allowed',
            '_pagani_audio_silent_backend', '_pagani_audio_engine_init', '_pagani_audio_engine_poll', '_pagani_audio_engine_take',
            '_pagani_audio_engine_played', '_pagani_audio_engine_captured', '_pagani_audio_status', '_pagani_audio_clock',
            '_pagani_audio_actuator_enabled', '_pagani_audio_haptics_init', '_pagani_audio_haptics_poll', '_pagani_audio_haptics_scale',
            INITIALIZER]
# Defaults for kernels without BootProgress or PaganiHaptics (the statically linked kernel refuses undefined weak references); their
# definitions win.
WEAK = ['_boot_progress_trace', '_boot_progress_trace_enabled', '_pagani_haptics_stream_begin', '_pagani_haptics_stream_write',
        '_pagani_haptics_stream_end']
# What the archive may take from the kernel besides IOKit/libkern C++ symbols: anything else is a typo or a dependency to review.
KERNEL_IMPORTS = {'_IOLog', '_IOLockAlloc', '_IOLockLock', '_IOLockUnlock', '_IOLockWakeup', '_IOLockSleepDeadline', '_PE_parse_boot_argn',
                  '_absolutetime_to_nanoseconds', '_bzero', '_clock_interval_to_deadline', '_clock_timebase_info', '_copyin',
                  '_g_kernel_kmod_info', '_get_task_pmap', '_kernel_task', '_kernel_thread_start', '_mach_absolute_time', '_memcpy',
                  '_memset', '_phystokv', '_pmap_find_pa', '_pmap_valid_address', '_proc_selfname', '_proc_selfpid', '_snprintf', '_strlcpy', '_thread_deallocate',
                  '_thread_set_thread_name', '_vsnprintf', '_current_thread', '___stack_chk_fail', '___stack_chk_guard',
                  '_OSObject_typed_operator_new', '_OSObject_typed_operator_delete', '_lck_mtx_lock', '_lck_mtx_unlock',
                  # the record's -fsanitize=signed-integer-overflow (minimal runtime: counted, no panic without -ubsan_force_panic)
                  '___ubsan_handle_add_overflow_minimal', '___ubsan_handle_sub_overflow_minimal',
                  '___ubsan_handle_mul_overflow_minimal', '___ubsan_handle_divrem_overflow_minimal',
                  '___ubsan_handle_negate_overflow_minimal'}


def fail(message):
    raise SystemExit('build-pagani-audio: ' + message)


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


def audit(obj):
    """ARM64 MH_OBJECT for iOS, minimum 17.0 (build-arm-driver.py)."""
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
                addr, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
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
        match = re.search(r'\((?:__\w+,[^)]+|absolute)\) (weak )?(?:private )?external (?:\[[^\]]*\] )*'
                          r'(?:automatically hidden )?(\S+)', line)
        if match:
            name = match.group(2)
            kind = 'weak' if match.group(1) else 'strong'
            defined[name] = 'strong' if defined.get(name) == 'strong' else kind
    return defined, undefined - set(defined)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiAudio sources (default: this directory)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-audio)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({RECORD_DIR}); run the UniXNU kernel step first')
    out = (args.out or tree / 'build/pagani-audio').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    missing = [name for name in CXX_SOURCES + [STAGED] + C_SOURCES + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    extra = ['-I', str(source), '-I', str(tree / '.cache/xnu-work/osfmk'), '-mgeneral-regs-only'] + WARNINGS
    objects = [compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.o'), extra) for name in CXX_SOURCES]
    objects += [compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.o'), extra) for name in C_SOURCES]
    for obj in objects:
        if any(sect == '__mod_init_func' for _, sect in sections(obj)):
            fail(f'{obj.name} has a static initializer; only {STAGED} may (registered late by pagani_audio_start)')
    service = compile_one(cxx_record, source / STAGED, out / 'obj/PaganiAudioService.o', extra)
    late = out / 'obj/PaganiAudioService.late.o'
    late.unlink(missing_ok=True)
    subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=' + INITIALIZER, str(service),
                    str(late)], check=True)
    if text(service) != text(late) or any(sect == '__mod_init_func' for _, sect in sections(late)):
        fail('staging the service class initializer changed its instructions or kept __mod_init_func')
    objects.append(late)

    archive = out / 'libPaganiAudio.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    members = [m for m in subprocess.run(['xcrun', 'ar', 't', str(archive)], check=True, capture_output=True,
                                         text=True).stdout.splitlines() if m and not m.startswith('__.SYMDEF')]
    if sorted(members) != sorted(obj.name for obj in objects):
        fail(f'archive members {sorted(members)} differ from the objects')
    defined, undefined = symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if name not in defined]
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    unexpected = sorted(name for name in undefined if name not in KERNEL_IMPORTS and not name.startswith('__Z'))
    problems += [f'unexpected kernel import {name}' for name in unexpected]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    listed = [source / name for name in CXX_SOURCES + [STAGED] + C_SOURCES + HEADERS] + [Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [obj.name for obj in objects],
              'source_sha256': {str(p): sha(p) for p in listed}, 'staged_initializer': INITIALIZER, 'weak': WEAK,
              'undefined': sorted(undefined)}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(objects)} objects in {archive} ({INITIALIZER} staged, {len(REQUIRED)} required and {len(WEAK)} weak '
          f'definitions present, {len(undefined)} kernel imports)')


if __name__ == '__main__':
    main()
