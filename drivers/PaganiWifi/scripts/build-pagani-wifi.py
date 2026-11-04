#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/pagani-wifi/libPaganiWifi.a (the OnePlus 13T Wi-Fi, PaganiWifi) with the actual XNU compile records of a UniXNU tree.

Run by the UniXNU kernel step as the PaganiWifi component recipe ("{component}/scripts/build-pagani-wifi.py", cwd = the tree), after
the Adreno830 component: PaganiWifi calls the SM8750 platform layer that libAdreno830.a carries (Sm8750Rpmh for the rail and bus votes,
Sm8750AossQmp for the optional PDC messages) and does not compile it again. Like PaganiAdsp/PaganiBattery/PaganiHaptics, --append-to
ARCHIVE (the device's recipe passes build/adreno830/libAdreno830.a) also adds the objects to that force-loaded archive, so no link setting
changes; the archive is checked to define every platform symbol PaganiWifi needs and none of PaganiWifi's strong symbols twice.

The archive holds the portable core (MhiBoot.c, WifiFat.c, WifiFirmware.c, WifiDma.c, WifiPmic.c, WifiMhiLoad.c, WifiRddm.c, WifiBringup.cpp), the
kernel glue PaganiWifi.cpp and PaganiWifiNet.cpp, whose class initializer is taken out of __mod_init_func and exported for the late
registration in PaganiWifi.cpp (netStart), as PaganiBattery stages its service; no other object may carry a static initializer. Zero
warnings (-Wall -Wextra -Werror, the policy of the other Pagani drivers); every object audited as an ARM64 MH_OBJECT for iOS 17.0.

--with-ath12k also builds the Ath12kWifi stack (drivers/PaganiWifi/Ath12kWifi: Ath12kWifi f6bf561 with the peach entry, tools/peach.py)
the way UniXNU-sheng's build-ath12k-wifi.py does: its generated trees laid out by tools/prepare.sh (which needs tools/fetch-sources.sh
run once, or --fetch-deps: Linux v7.0.14 and FreeBSD releng/15.1, public sources, kept out of the repository), compiled with -w
(Linux code; the Linux sources' warnings are theirs, the zero-warning rule holds for every PaganiWifi object), every
global symbol renamed _nwk... except the entry points the board calls. Without it the archive carries weak defaults of those entry
points, and pagani-wifi=6 reports that the stack is not linked; stages 1 to 5 work either way.

  build-pagani-wifi.py [--tree TREE] [--source DIR] [--adreno830 DIR] [--sm8750-kit DIR] [--out DIR] [--with-ath12k [--fetch-deps]]
                       [--append-to A]

--out defaults to TREE/build/pagani-wifi; a check build passes another directory and leaves the tree as it is (the compile records are
only read). --append-to with a copy of libAdreno830.a checks the archive without touching the tree's."""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
RECORD_DIR = Path('build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837')
CXX_RECORD = RECORD_DIR / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = RECORD_DIR / 'bsd/DEVELOPMENT/bsd_init.o.json'
C_SOURCES = ['MhiBoot.c', 'WifiFat.c', 'WifiFirmware.c', 'WifiDma.c', 'WifiPmic.c', 'WifiMhiLoad.c', 'WifiRddm.c']
CXX_SOURCES = ['WifiBringup.cpp', 'PaganiWifi.cpp']
STAGED = 'PaganiWifiNet.cpp'
HEADERS = ['PaganiWifi.h', 'PaganiWifiTables.h', 'WifiBringup.h', 'WifiFirmware.h', 'WifiFat.h', 'FatVolume.h', 'FatVolume.c', 'WifiDma.h',
           'WifiPmic.h', 'WifiMhiLoad.h', 'MhiBoot.h', 'WifiUtil.h', 'WifiApple80211.h', 'WifiRddm.h']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
INITIALIZER = '__GLOBAL__sub_I_PaganiWifiNet.cpp'
REQUIRED = ['_pagani_wifi_start', '_pagani_wifi_file', '_pagani_wifi_wake', '_pagani_wifi_logf', '_pagani_wifi_net_start',
            '_pagani_wifi_status', '_nw_board_option', '_pw_fw_get', '_pw_dma_alloc', '_pw_mhi_load', '_mhi_fw_layout', '_pw_rddm_scan',
            '_pagani_wifi_fat_mount', INITIALIZER]
# Providers a kernel may lack (BootProgress, NativeUfs, PaganiArmCpu) and the shared lock words: weak, the real ones win.
WEAK = ['_boot_progress_trace', '_boot_log_export_put', '_native_ufs_part_find', '_native_ufs_part_read', '_native_ufs_part_status',
        '_armcpu_spi_attach', '_armcpu_spi_disable', '_sm8750_gcc_vote_owner', '_sm8750_aoss_qmp_owner', '_pagani_panel_probe_quiet']
# The Ath12kWifi entry points: weak defaults here, strong in the stack's objects (--with-ath12k).
STACK = ['_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stats', '_nw_wifi_where', '_nw_wifi_set_events', '_nw_wifi_scan',
         '_nw_wifi_bss_count', '_nw_wifi_bss_get', '_nw_wifi_connect', '_nw_wifi_disconnect', '_nw_wifi_tx', '_nw_wifi_status_get',
         '_nw_wifi_tx_reasons']
# What libAdreno830.a must define for PaganiWifi (and this archive must not).
PLATFORM = ['_Sm8750CmdDbCopy', '_Sm8750CmdDbVrm', '_Sm8750CmdDbBcm', '_Sm8750BcmCommand', '_Sm8750RpmhWriteActive',
            '_Sm8750AossQmpInit', '_Sm8750AossQmpSend']
# The stack's symbols the board must still see after the renaming (sheng's keep list, plus the board option hook of the peach entry).
KEEP = set(STACK) | {'_nw_wifi_stop', '_nw_board_option'}


def fail(message):
    raise SystemExit('build-pagani-wifi: ' + message)


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


def build_stack(module, out, fetch):
    """The Ath12kWifi stack as kernel objects (UniXNU-sheng scripts/build-ath12k-wifi.py, commit 08cde20e, adapted): returns them."""
    if not (module / 'deps/linux/drivers/net/wireless/ath/ath12k').is_dir():
        if not fetch:
            fail(f'{module}/deps missing: run {module}/tools/fetch-sources.sh once (Linux v7.0.14, FreeBSD releng/15.1), '
                 'or pass --fetch-deps')
        subprocess.run(['bash', str(module / 'tools/fetch-sources.sh')], check=True)
    subprocess.run(['bash', str(module / 'tools/prepare.sh')], check=True, stdout=subprocess.DEVNULL)
    clang = subprocess.run(['xcrun', '-f', 'clang'], check=True, capture_output=True, text=True).stdout.strip()
    resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'
    flags = ['-target', 'arm64-apple-ios17.0', '-mkernel', '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common',
             '-fno-strict-aliasing', '-fwrapv', '-fno-stack-protector', '-Wno-incompatible-pointer-types', '-Wno-error',
             '-fno-delete-null-pointer-checks', '-mgeneral-regs-only', '-momit-leaf-frame-pointer', '-std=gnu11', '-w',
             '-DCONFIG_ATH12K_DEBUG', '-nostdinc', '-isystem', str(module / 'shim/libc'), '-isystem', resource, '-include', 'lx_pci.h',
             '-I', str(module / 'shim/include'), '-I', str(module / 'include'), '-I', str(module / 'out/pinned'),
             '-I', str(module / 'src')]
    trees = {'ath12k': ['-I', str(module / 'out/ath12k-src'), '-I', str(module / 'out/ath12k-src/wifi7')],
             'mhi': ['-I', str(module / 'out/mhi-src')]}
    stack = out / 'ath12k'
    shutil.rmtree(stack, ignore_errors=True)
    stack.mkdir(parents=True)
    jobs = []
    for line in (module / 'tools/sources.txt').read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        tree, _, name = line.partition(':') if ':' in line else ('', '', line)
        if tree == 'ath12k':
            src = module / 'out/ath12k-src' / name
        elif tree == 'mhi':
            src = module / 'out/mhi-src' / name
        else:
            src = module / name
        obj = stack / (('w7_' if '/wifi7/' in str(src) else tree + '_' if tree else '') + src.stem + '.nwk.o')
        extra = trees.get(tree, trees['ath12k'] if name.startswith('src/') else [])
        jobs.append((src, obj, extra))

    def one(job):
        src, obj, extra = job
        r = subprocess.run([clang, '-c', *flags, *extra, str(src), '-o', str(obj)], capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write(r.stderr[-4000:])
            fail(f'stack object failed: {src}')
        return obj

    with ThreadPoolExecutor(8) as pool:
        objects = list(pool.map(one, jobs))
    for obj in objects:
        # Nothing of the stack may run without pagani-wifi=6: no object of it reaches the kernel's start by itself.
        if any(sect == '__mod_init_func' or sect == '__init_offsets' for _, sect in sections(obj)):
            fail(f'{obj.name} has a static initializer: the stack would run without pagani-wifi=6')
    names = set()
    for obj in objects:
        listing = subprocess.run([tool('llvm-nm'), '-g', '--defined-only', str(obj)], check=True, capture_output=True, text=True).stdout
        for entry in listing.splitlines():
            parts = entry.split()
            if len(parts) == 3 and parts[2] not in KEEP:
                names.add(parts[2])
    mapping = stack / 'rename.txt'
    mapping.write_text(''.join(f'{n} _nwk{n}\n' for n in sorted(names)))
    for obj in objects:
        subprocess.run([tool('llvm-objcopy'), f'--redefine-syms={mapping}', str(obj)], check=True)
        audit(obj)
    return objects


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiWifi sources (default: this directory)')
    parser.add_argument('--adreno830', type=Path, help='Adreno830 sources (default: TREE/drivers/Adreno830)')
    parser.add_argument('--sm8750-kit', type=Path, help='Sm8750Platform kit (default: TREE/kits/Sm8750Platform)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-wifi)')
    parser.add_argument('--with-ath12k', action='store_true', help='also build the Ath12kWifi stack (SOURCE/Ath12kWifi)')
    parser.add_argument('--fetch-deps', action='store_true', help='with --with-ath12k: fetch the stack\'s public sources when missing')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / RECORD_DIR).is_dir():
        fail(f'{tree} has no XNU compile records ({RECORD_DIR}); run the UniXNU kernel step first')
    missing = [name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS if not (source / name).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    adreno = (args.adreno830 or tree / 'drivers/Adreno830').resolve()
    platform = adreno / 'platform/sm8750'
    kit = (args.sm8750_kit or tree / 'kits/Sm8750Platform').resolve()
    module = source / 'Ath12kWifi'
    for need, why in ((platform / 'Sm8750Rpmh.h', 'the Adreno830 component (its SM8750 platform layer)'),
                      (kit / 'SmmuTakeover.h', 'the Sm8750Platform kit (the SMMU stream planner)'),
                      (module / 'include/nw_abi.h', 'the Ath12kWifi copy (its nw_abi.h)')):
        if not need.is_file():
            fail(f'{need} missing; PaganiWifi depends on {why}')
    out = (args.out or tree / 'build/pagani-wifi').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(platform), '-I', str(kit), '-I', str(module / 'include'),
             '-I', str(tree / '.cache/xnu-work/osfmk'), '-mgeneral-regs-only'] + WARNINGS
    objects = [compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.wifi.o'), extra) for name in C_SOURCES]
    objects += [compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.wifi.o'), extra) for name in CXX_SOURCES]
    for obj in objects:
        if any(sect == '__mod_init_func' for _, sect in sections(obj)):
            fail(f'{obj.name} has a static initializer; only {STAGED} may (registered late by netStart)')
    service = compile_one(cxx_record, source / STAGED, out / 'obj/PaganiWifiNet.wifi.o', extra)
    late = out / 'obj/PaganiWifiNet.wifi.late.o'
    late.unlink(missing_ok=True)
    subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=' + INITIALIZER, str(service),
                    str(late)], check=True)
    if text(service) != text(late) or any(sect == '__mod_init_func' for _, sect in sections(late)):
        fail('staging the network class initializer changed its instructions or kept __mod_init_func')
    objects.append(late)
    stack = build_stack(module, out, args.fetch_deps) if args.with_ath12k else []

    archive = out / 'libPaganiWifi.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects + stack)], check=True)
    defined, undefined = symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    wanted = 'strong' if stack else 'weak'
    problems += [f'{name} must be {wanted}' for name in STACK if defined.get(name) != wanted]
    problems += [f'{name} must come from libAdreno830.a, not be compiled here' for name in PLATFORM if name in defined]
    problems += [f'{name} is defined by FatVolume under its own name (clash with PaganiAdsp)' for name in ('_fat_mount', '_fat_open',
                                                                                                         '_fat_read') if name in defined]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    listed = [source / name for name in C_SOURCES + CXX_SOURCES + [STAGED] + HEADERS] + [Path(__file__).resolve()]
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [obj.name for obj in objects],
              'stack_objects': len(stack), 'source_sha256': {p.name: sha(p) for p in listed},
              'staged_initializer': INITIALIZER, 'platform': str(platform), 'kit': str(kit),
              'undefined': sorted(n for n in undefined if not n.startswith('_nwk'))}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list "component:Adreno830" (and its board recipe) before PaganiWifi')
        target_defined, _ = symbols([target])
        absent = [name for name in PLATFORM if target_defined.get(name) != 'strong']
        if absent:
            fail(f'{target} lacks the SM8750 platform symbols: ' + ', '.join(absent))
        clash = [name for name, kind in defined.items() if kind == 'strong' and target_defined.get(name) == 'strong']
        if clash:
            fail(f'strong symbols already in {target}: ' + ', '.join(sorted(clash)))
        subprocess.run(['xcrun', 'ar', 'rcs', str(target), *map(str, objects + stack)], check=True)
        twice = {name: m for name, m in strong_per_member(target).items() if len(m) > 1}
        if twice:
            fail(f'{target}: symbols defined by more than one member: ' + ', '.join(sorted(twice)))
        report['appended_to'] = str(target)
        report['appended_sha256'] = sha(target)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    where = f' and appended to {args.append_to}' if args.append_to else ''
    print(f'PASS: {len(objects)} objects{f" + {len(stack)} stack objects" if stack else ""} in {archive}{where} ({INITIALIZER} '
          f'staged; {len(REQUIRED)} entry points, {len(WEAK)} weak defaults, stack entry points {wanted}; '
          f'{len([n for n in undefined if n.startswith("_Sm8750")])} platform imports)')


if __name__ == '__main__':
    main()
