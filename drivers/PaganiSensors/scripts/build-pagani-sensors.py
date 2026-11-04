#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/pagani-sensors/libPaganiSensors.a (the OnePlus 13T sensors for iOS) with the actual XNU compile records of a tree.

Run by the UniXNU kernel step as the PaganiSensors component recipe ("{component}/scripts/build-pagani-sensors.py", cwd = the tree), like
PaganiBattery's. The archive holds the portable core (SensorsPb.c, SensorsFixed.c, SensorsSsc.c, SensorsCore.c, SensorsHid.c,
SensorsOrient.c, SensorsPack.c, SensorsRpc.c), the kernel glue PaganiSensors.cpp and PaganiSensorsRpc.cpp, the HID device class
PaganiSensorsHid.cpp (its class initializer taken out of __mod_init_func and exported for the late registration in pagani_sensors_start;
no other object may carry a static initializer) and the bindings of the original 21H461 IOHIDEvent constructors and IOHIDEventDriver
methods it calls. Kernel code here never uses floating point (-mgeneral-regs-only):
the sensor hub's floats are converted with integer arithmetic (SensorsFixed.c).

Qualification (nothing is built when any of it fails):
  - the pinned original kernelcache (OriginalImage checks its sha256): the six event constructors' whole bodies, the first bytes of
    IOHIDEventDriver::setProperties and ::copyEvent, the event driver's table slots 44 (setProperties: the slot of
    IORegistryEntry::setProperties in the original IOService table), 198 (dispatchEvent) and 201 (copyEvent);
  - PaganiSensorHid's vtable (read from the object's relocations): it overrides exactly PaganiInput's slots and newUserClient (the
    orientation service's 'ashd' client), every IOHIDDevice symbol it needs is one build-pagani-peripherals.py bound
    (TREE/build/pagani-peripherals/peripheral-bindings.S, already linked for PaganiInput), every other slot is the original IOService one;
  - the archive's symbols: the entry points strong, the BootProgress and PaganiAdsp entry points weak (PaganiAdsp's strong ones replace
    them whenever it is linked; without it the sensors run as services only), nothing undefined beyond the reviewed kernel imports.

Linking: with --append-to ARCHIVE (the device's recipe passes build/adreno830/libAdreno830.a, which the kernel link force-loads) the
objects also go into that archive, checked for strong symbols defined twice, so no link setting changes.

  build-pagani-sensors.py --kernelcache KC [--tree TREE] [--source DIR] [--adsp DIR] [--kit DIR] [--out DIR] [--append-to ARCHIVE]

--out defaults to TREE/build/pagani-sensors; a check build passes another directory and leaves the tree as it is (the compile records,
the generated HID ABI headers and bindings are only read). Zero warnings: -Wall -Wextra -Werror with the policy of the reference drivers."""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
RECORD_DIR = Path('build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837')
CXX_RECORD = RECORD_DIR / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = RECORD_DIR / 'bsd/DEVELOPMENT/bsd_init.o.json'
PERIPHERALS = Path('build/pagani-peripherals')
C_SOURCES = ['SensorsPb.c', 'SensorsFixed.c', 'SensorsSsc.c', 'SensorsCore.c', 'SensorsHid.c', 'SensorsOrient.c', 'SensorsPack.c',
             'SensorsRpc.c']
CXX_SOURCES = ['PaganiSensors.cpp', 'PaganiSensorsRpc.cpp']
STAGED = 'PaganiSensorsHid.cpp'
HEADERS = ['SensorsPb.h', 'SensorsFixed.h', 'SensorsSsc.h', 'SensorsCore.h', 'SensorsHid.h', 'SensorsOrient.h', 'SensorsPack.h',
           'SensorsRpc.h', 'SensorsText.h', 'SensorsKernel.h', 'PaganiSensors.h']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
INITIALIZER = '__GLOBAL__sub_I_PaganiSensorsHid.cpp'
CLASS = 'PaganiSensorHid'
OWN_SLOTS = [0, 1, 7, 14, 86, 87, 111, 171, 173, 180]   # exactly PaganiInput's (build-pagani-peripherals.py); one more: NEW_USER_CLIENT
NEW_USER_CLIENT = '__ZN15PaganiSensorHid13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient'

# The original 21H461 code PaganiSensorsHid.cpp calls, at the project's addresses (IDA - 0x1000000, ios-sensors.md section 3.1).
# Each constructor's whole body (up to its ret) is pinned: the event type constants (0xd, 0x14, 0x15, 0xc, 0xe, 0x1f) are inside.
BINDINGS = {
    '_pagani_sensors_event_accelerometer': (0xfffffff004afb970,
        'f657bda9f44f01a9fd7b02a9fd830091f30306aaf40305aaf50304aae40303aae30302aae20301aae10300aaa0018052e50307aa91ffff97800000b4'
        '080840f915d10329132500b9fd7b42a9f44f41a9f657c3a8c0035fd6'),
    '_pagani_sensors_event_gyro': (0xfffffff004afb9c8,
        'f657bda9f44f01a9fd7b02a9fd830091f30306aaf40305aaf50304aae40303aae30302aae20301aae10300aa80028052e50307aa7bffff97800000b4'
        '080840f915d10329132500b9fd7b42a9f44f41a9f657c3a8c0035fd6'),
    '_pagani_sensors_event_compass': (0xfffffff004afba20,
        'f657bda9f44f01a9fd7b02a9fd830091f30306aaf40305aaf50304aae40303aae30302aae20301aae10300aaa0028052e50307aa65ffff97800000b4'
        '080840f915d10329132500b9fd7b42a9f44f41a9f657c3a8c0035fd6'),
    '_pagani_sensors_event_ambient_light': (0xfffffff004afba78,
        'fc6fbaa9fa6701a9f85f02a9f65703a9f44f04a9fd7b05a9fd430191f60306aaf30305aaf40304aaf50303aaf80302aaf90301aafa0300aa809000b0'
        '00400591010a8052ae4e0094f70300aa7bff00907b630c91e1031baa904f00946890009008610c91080000f9e0031baabe4f0094e00317aa010a8052'
        'fbfdff9780010036e80a40f9890180520a008252ea2200f9fa1a00f9f64a00b909d900291961022915510329132100b906000014e80240f9081540f9'
        'e00317aa00013fd6170080d2e00317aafd7b45a9f44f44a9f65743a9f85f42a9fa6741a9fc6fc6a8c0035fd6'),
    '_pagani_sensors_event_proximity': (0xfffffff004afbc3c,
        'f85fbca9f65701a9f44f02a9fd7b03a9fdc30091f50303aaf30302aaf60301aaf70300aa809000b000400591010a8052424e0094f40300aa78ff0090'
        '18630c91e10318aa244f00946890009008610c91080000f9e00318aa524f0094e00314aa010380528ffdff9780010036880a40f9c90180520a008852'
        '8a2200f9971a00f9954a00b909d50029162100791f250079131500b906000014880240f9081540f9e00314aa00013fd6140080d2e00314aafd7b43a9'
        'f44f42a9f65741a9f85fc4a8c0035fd6'),
    '_pagani_sensors_event_pressure': (0xfffffff004afc934,
        'f85fbca9f65701a9f44f02a9fd7b03a9fdc30091f70303aaf30302aaf50301aaf60300aa8090009000400591010a8052044b0094f40300aa58ff00f0'
        '18630c91e10318aae64b0094489000f008610c91080000f9e00318aa144c0094e00314aa0103805251faff9760010036e8020032890a40f9ea038052'
        '0b00b0528b2200f9961a00f9884a00b92aa10029354d022906000014880240f9081540f9e00314aa00013fd6140080d2e00314aafd7b43a9f44f42a9'
        'f65741a9f85fc4a8c0035fd6'),
    '_pagani_sensors_driver_set_properties': (0xfffffff004af5988,
        'ff4307d1fc6f18a9f85f19a9f6571aa9f44f1ba9fd7b1ca9fd030791f30301aa'),
    '_pagani_sensors_driver_copy_event': (0xfffffff004af6314,
        'ffc302d1fc6f05a9fa6706a9f85f07a9f65708a9f44f09a9fd7b0aa9fd830291f50302aaf30300aa3f040071000800543f280071401300543f0c007141'
        '250054'),
}
DRIVER_TABLE = 0xfffffff005d06a68
DRIVER_SLOTS = {44: 0xfffffff004af5988, 198: 0xfffffff004aface8, 201: 0xfffffff004af6314}

REQUIRED = ['_pagani_sensors_start', '_pagani_sensors_status', '_pagani_sensors_hid_setup', '_pagani_sensors_hid_publish',
            '_pagani_sensors_hid_bind', '_pagani_sensors_hid_dispatch', '_pagani_sensors_hid_node', '_pagani_sensors_hid_orientation',
            '_pagani_sensors_rpc_start', '_sns_core_init', '_sns_core_receive', '_sns_core_tick', '_ssc_build_request',
            '_ssc_parse_attr_event', '_sfx_scale', '_sns_hid_personalities', '_sno_update', '_ssr_core_step', '_ssp_find',
            INITIALIZER] + sorted(BINDINGS)
WEAK = ['_boot_progress_trace']
ADSP_WEAK = ['_pagani_adsp_channel', '_pagani_adsp_channel_config', '_pagani_adsp_send', '_pagani_adsp_channel_up', '_pagani_adsp_status',
             '_pagani_adsp_qrtr_open', '_pagani_adsp_qrtr_port_number', '_pagani_adsp_qrtr_server', '_pagani_adsp_qrtr_lookup',
             '_pagani_adsp_qrtr_send', '_pagani_adsp_qrtr_close']
# What the archive may take from the kernel besides IOKit/libkern C++ symbols: anything else is a typo or a dependency to review.
KERNEL_IMPORTS = {'_IOLog', '_IODelay', '_IOSleep', '_IOSimpleLockAlloc', '_IOSimpleLockLock', '_IOSimpleLockUnlock', '_IOFree',
                  '_IOFreeData', '_IOMallocData', '_IOMallocZero', '___IOMalloc_internal', '_IOMalloc_internal', '_KHEAP_DEFAULT',
                  '_KHEAP_DATA_BUFFERS', '_PE_parse_boot_argn', '_absolutetime_to_nanoseconds', '_mach_absolute_time',
                  '_assert_wait_timeout', '_clear_wait', '_current_thread', '_thread_block', '_thread_wakeup_prim', '_kernel_thread_start',
                  '_thread_deallocate', '_thread_set_thread_name', '_memcpy', '_memmove', '_memset', '_bzero', '_g_kernel_kmod_info',
                  '_kOSBooleanTrue', '_kOSBooleanFalse', '_gIOCatalogue', '_gIOServicePlane', '_gIODTPlane', '_lck_spin_lock',
                  '_lck_spin_unlock', '_gPhysBase', '_gPhysSize', '_strcmp', '_IOMallocZeroData', '_vfs_rootvnode', '_vfs_context_create',
                  '_vfs_context_rele', '_vfs_context_ucred', '_vfs_context_proc', '_vnode_put', '_vnode_lookup', '_vnode_open',
                  '_vnode_close', '_vnode_getattr', '_vn_rdwr',
                  '_lck_spin_lock_grp', '___stack_chk_fail', '___stack_chk_guard', '_OSObject_typed_operator_new',
                  '_OSObject_typed_operator_delete',
                  # the record's -fsanitize=signed-integer-overflow (minimal runtime: counted, no panic without -ubsan_force_panic)
                  '___ubsan_handle_add_overflow_minimal', '___ubsan_handle_sub_overflow_minimal',
                  '___ubsan_handle_mul_overflow_minimal', '___ubsan_handle_divrem_overflow_minimal',
                  '___ubsan_handle_negate_overflow_minimal'}
# Defined by build-pagani-peripherals.py's peripheral-bindings.o (in the display's late archive), linked for PaganiInput.
PERIPHERAL_IMPORTS = {'_pagani_original_hid_driver_vtable'}


def fail(message):
    raise SystemExit('build-pagani-sensors: ' + message)


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
    candidates = [args.adsp] if args.adsp else [source.parent / 'PaganiAdsp', tree / 'drivers/PaganiAdsp']
    for candidate in candidates:
        header = Path(candidate) / 'PaganiAdsp.h' if candidate else None
        if header and header.is_file() and 'pagani_adsp_qrtr_open' in header.read_text():
            return Path(candidate).resolve()
    fail('a PaganiAdsp.h with the IPC router client API (pagani_adsp_qrtr_open) not found in ' + ', '.join(map(str, candidates)) +
         '; pass --adsp DIR')


def original_image(tree, kernelcache):
    """OriginalImage of the tree's scripts (it pins the kernelcache's sha256)."""
    for scripts in (tree / 'scripts', Path(__file__).resolve().parents[4] / 'UniXNU/scripts'):
        if (scripts / 'original_stage7.py').is_file():
            sys.path.insert(0, str(scripts))
            for kit in (scripts.parent / 'kits/NativePrelink', SOURCE.parents[1] / 'kits/NativePrelink'):
                if (kit / 'native_prelink.py').is_file():
                    sys.path.insert(0, str(kit))
            from original_stage7 import OriginalImage      # noqa: E402 (found at run time)
            from native_prelink import macho                 # noqa: E402
            return OriginalImage(kernelcache), macho
    fail('original_stage7.py not found (TREE/scripts)')


def qualify_original(original):
    """The constructors' bodies, the two methods' first bytes, the driver table's slots, slot 44's meaning."""
    for name, (address, body) in BINDINGS.items():
        expected = bytes.fromhex(body)
        if original.read(address, len(expected)) != expected:
            fail(f'{name}: the original bytes at {address:#x} differ from the pinned ones')
    table = original.pointers(DRIVER_TABLE, 220)
    for slot, address in DRIVER_SLOTS.items():
        if table[slot] != address:
            fail(f'IOHIDEventDriver table slot {slot} is {table[slot]:#x}, not {address:#x}')
    service = original.pointers(original.names['__ZTV9IOService'] + 16, 168)
    if service[44] != original.names.get('__ZN15IORegistryEntry13setPropertiesEP8OSObject'):
        fail('IOService table slot 44 is not IORegistryEntry::setProperties')
    return {'driver_table': hex(DRIVER_TABLE), 'driver_slots': {str(k): hex(v) for k, v in DRIVER_SLOTS.items()},
            'bodies': {name: [hex(a), len(b) // 2] for name, (a, b) in BINDINGS.items()}}


def vtable_slots(path, classname, count, macho):
    """The symbols a class's vtable slots name, from the object's relocations (as build-pagani-peripherals.py reads them)."""
    raw = path.read_bytes()
    image = macho(raw)
    st = next(p for c, p, _ in image['commands'] if c == 2)
    sy, total, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    names = []
    for i in range(total):
        ix, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        names.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), address, kind))
    vtable = next(a for n, a, _ in names if n == f'__ZTV{len(classname)}{classname}')
    relocs = {}
    for segment in image['segments']:
        for sec in segment['sections']:
            if not sec['va'] <= vtable < sec['va'] + sec['size']:
                continue
            offset, length = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(length):
                at, bits = struct.unpack_from('<2I', raw, offset + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocs[sec['va'] + at] = names[bits & 0xffffff][0]
    slots = [relocs.get(vtable + 16 + i * 8) for i in range(count)]
    if not all(slots) or vtable + 16 + count * 8 in relocs:
        fail(f'{classname}: vtable is not exactly {count} slots')
    return slots


def qualify_class(obj, original, macho, bindings):
    slots = vtable_slots(obj, CLASS, 207, macho)
    service = original.pointers(original.names['__ZTV9IOService'] + 16, 168)
    own = []
    for i, name in enumerate(slots):
        if name.startswith((f'__ZN{len(CLASS)}{CLASS}', f'__ZNK{len(CLASS)}{CLASS}')):
            own.append(i)
        elif '11IOHIDDevice' in name:
            if name not in bindings:
                fail(f'{CLASS} slot {i} names {name}, which build-pagani-peripherals.py did not bind')
        elif not (i < 168 and original.names.get(name) == service[i]):
            fail(f'{CLASS} slot {i} ({name}) is not the original IOService slot')
    extra = [i for i in own if i not in OWN_SLOTS]
    if [i for i in own if i in OWN_SLOTS] != OWN_SLOTS or len(extra) != 1 or slots[extra[0]] != NEW_USER_CLIENT:
        fail(f'{CLASS} overrides slots {own}, not exactly PaganiInput\'s {OWN_SLOTS} and newUserClient')
    if service[extra[0]] != original.names.get('__ZN9IOService13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient'):
        fail(f'slot {extra[0]} of the original IOService table is not IOService::newUserClient')
    return own


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiSensors sources (default: this directory)')
    parser.add_argument('--adsp', type=Path, help='directory with PaganiAdsp.h (default: ../PaganiAdsp or TREE/drivers/PaganiAdsp)')
    parser.add_argument('--kit', type=Path, help='the SoC kit with SmmuTakeover.h (default: TREE/kits/Sm8750Platform or ../../../UniXNU-sm8750-common/...)')
    parser.add_argument('--kernelcache', type=Path, required=True, help='the pinned 21H461 kernelcache.decompressed')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-sensors)')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({RECORD_DIR}); run the UniXNU kernel step first')
    peripherals = tree / PERIPHERALS
    for name in ('OriginalHidDeviceABI.h', 'OriginalHidEventABI.h', 'peripheral-bindings.S'):
        if not (peripherals / name).is_file():
            fail(f'{peripherals / name} missing; build-pagani-peripherals.py (PaganiInput) runs before PaganiSensors')
    missing = [name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    adsp = find_adsp(args, tree, source)
    kits = [args.kit] if args.kit else [tree / 'kits/Sm8750Platform', source.parents[2] / 'UniXNU-sm8750-common/kits/Sm8750Platform']
    kit = next((Path(k).resolve() for k in kits if (Path(k) / 'SmmuTakeover.h').is_file()), None)
    if not kit:
        fail('SmmuTakeover.h (the Sm8750Platform kit) not found in ' + ', '.join(map(str, kits)) + '; pass --kit DIR')
    original, macho = original_image(tree, args.kernelcache.resolve())
    report_original = qualify_original(original)
    bound = set(re.findall(r'^\.globl (\S+)$', (peripherals / 'peripheral-bindings.S').read_text(), re.M))

    out = (args.out or tree / 'build/pagani-sensors').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(tree / '.cache/xnu-work/osfmk'), '-mgeneral-regs-only'] + WARNINGS
    objects = [compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.sensors.o'), extra) for name in C_SOURCES]
    objects += [compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.sensors.o'), extra + ['-I', str(adsp), '-I', str(kit)])
                for name in CXX_SOURCES]
    for obj in objects:
        if any(sect == '__mod_init_func' for _, sect in sections(obj)):
            fail(f'{obj.name} has a static initializer; only {STAGED} may (registered late by pagani_sensors_start)')
    hid = compile_one(cxx_record, source / STAGED, out / 'obj/PaganiSensorsHid.sensors.o', extra + ['-I', str(peripherals)])
    own = qualify_class(hid, original, macho, bound)
    late = out / 'obj/PaganiSensorsHid.sensors.late.o'
    late.unlink(missing_ok=True)
    subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=' + INITIALIZER, str(hid),
                    str(late)], check=True)
    if text(hid) != text(late) or any(sect == '__mod_init_func' for _, sect in sections(late)):
        fail('staging the class initializer changed its instructions or kept __mod_init_func')
    objects.append(late)
    # The bindings: absolute symbols, only after the qualification above.
    asm = out / 'obj/sensors-bindings.S'
    asm.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n' for name, (address, _) in sorted(BINDINGS.items())))
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True, capture_output=True, text=True).stdout.strip()
    binding = out / 'obj/sensors-bindings.sensors.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', str(asm), '-o', str(binding)], check=True)
    audit(binding)
    objects.append(binding)

    archive = out / 'libPaganiSensors.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    members = [m for m in subprocess.run(['xcrun', 'ar', 't', str(archive)], check=True, capture_output=True,
                                         text=True).stdout.splitlines() if m and not m.startswith('__.SYMDEF')]
    if sorted(members) != sorted(obj.name for obj in objects):
        fail(f'archive members {sorted(members)} differ from the objects')
    defined, undefined = symbols([archive])
    weak = WEAK + ADSP_WEAK
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in weak if defined.get(name) != 'weak']
    hid_imports = {name for name in undefined if '11IOHIDDevice' in name}
    problems += [f'{name} is not bound by build-pagani-peripherals.py' for name in sorted(hid_imports - bound)]
    unexpected = sorted(name for name in undefined
                        if name not in KERNEL_IMPORTS and name not in PERIPHERAL_IMPORTS and not name.startswith('__Z'))
    problems += [f'unexpected kernel import {name}' for name in unexpected]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS] + [Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [obj.name for obj in objects],
              'source_sha256': {p.name: sha(p) for p in listed}, 'adsp_header': str(adsp / 'PaganiAdsp.h'),
              'original_sha256': original.sha256, 'original': report_original, 'class_own_slots': own,
              'hid_imports_bound': sorted(hid_imports), 'staged_initializer': INITIALIZER, 'weak': weak,
              'undefined': sorted(undefined)}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list the archive\'s own recipe (Adreno830) before PaganiSensors')
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
          f'defaults, {len(undefined)} imports, {len(hid_imports)} of them qualified IOHIDDevice bindings; class slots {own})')


if __name__ == '__main__':
    main()
