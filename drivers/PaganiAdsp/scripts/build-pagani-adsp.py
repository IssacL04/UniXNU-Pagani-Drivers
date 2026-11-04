#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/pagani-adsp/libPaganiAdsp.a (the OnePlus 13T ADSP bring-up) with the actual XNU compile records of a UniXNU tree.

Run by the UniXNU kernel step as the PaganiAdsp component recipe ("{component}/scripts/build-pagani-adsp.py", cwd = the tree), after
the Adreno830 component: PaganiAdsp calls the SM8750 platform layer that libAdreno830.a carries (Sm8750Scm, Sm8750AossQmp,
Sm8750Rpmh) and does not compile it again. With --append-to ARCHIVE (the device's recipe passes build/adreno830/libAdreno830.a) the
objects also go into that archive, which the kernel link already force-loads, so no link setting changes; the archive is checked to
define every platform symbol PaganiAdsp needs, and none of PaganiAdsp's strong symbols twice.

Same warning policy as the Adreno830 recipe (no warning accepted), every object audited as an ARM64 MH_OBJECT for iOS 17.0.
--tree may be any assembled tree with compile records (it is only read; --out takes the objects elsewhere).
"""
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
C_SOURCES = ['QcomSmem.c', 'QcomSmp2p.c', 'QcomGlink.c', 'QcomQrtr.c', 'QcomMdt.c', 'FatVolume.c', 'AdspFirmware.c', 'AdspCore.c',
             'AdspQrtr.c', 'AdspPlatform.c']
CXX_SOURCES = ['PaganiAdsp.cpp']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
REQUIRED = ['_pagani_adsp_start', '_pagani_adsp_channel', '_pagani_adsp_channel_config', '_pagani_adsp_send',
            '_pagani_adsp_channel_up', '_pagani_adsp_status', '_pagani_adsp_qrtr_open', '_pagani_adsp_qrtr_port_number',
            '_pagani_adsp_qrtr_server', '_pagani_adsp_qrtr_lookup', '_pagani_adsp_qrtr_send', '_pagani_adsp_qrtr_close']
# Defaults for providers a kernel may lack (BootProgress, NativeUfs before the partition access, PaganiArmCpu) and the shared lock
# words (AdspPlatform.h): weak, so the real ones or the other users' copies win.
WEAK = ['_boot_progress_trace', '_native_ufs_part_find', '_native_ufs_part_read', '_native_ufs_part_status', '_armcpu_spi_attach',
        '_armcpu_spi_disable', '_sm8750_scm_owner', '_sm8750_aoss_qmp_owner', '_pagani_panel_probe_quiet']
# What libAdreno830.a must define for PaganiAdsp.
PLATFORM = ['_Sm8750ScmInit', '_Sm8750ScmCall', '_Sm8750ScmPasInitImage', '_Sm8750ScmPasMemSetup', '_Sm8750ScmPasAuthAndReset',
            '_Sm8750ScmSmcInstruction', '_Sm8750AossQmpInit', '_Sm8750AossQmpSend', '_Sm8750CmdDbCopy', '_Sm8750CmdDbArc',
            '_Sm8750RpmhArcLevel', '_Sm8750RpmhArcTop', '_Sm8750RpmhWriteActive']


def fail(message):
    raise SystemExit('build-pagani-adsp: ' + message)


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


def compile_one(record, source, obj, extra):
    obj.parent.mkdir(parents=True, exist_ok=True)
    obj.unlink(missing_ok=True)
    result = subprocess.run(command(record, source, obj, extra), cwd=record['directory'], capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    if result.returncode:
        fail(f'{source}:\n{output[-4000:]}')
    if 'warning:' in output:
        fail(f'{source} compiled with warnings:\n{output[-4000:]}')
    audit(obj)
    return obj


def audit(obj):
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='PaganiAdsp sources (default: this directory)')
    parser.add_argument('--adreno830', type=Path, help='Adreno830 sources (default: TREE/drivers/Adreno830)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/pagani-adsp)')
    parser.add_argument('--append-to', type=Path, help='also add the objects to this archive (libAdreno830.a)')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    adreno = (args.adreno830 or tree / 'drivers/Adreno830').resolve()
    platform = adreno / 'platform/sm8750'
    if not (platform / 'Sm8750Scm.h').is_file():
        fail(f'{platform}/Sm8750Scm.h missing; PaganiAdsp depends on the Adreno830 component (its SM8750 platform layer)')
    out = (args.out or tree / 'build/pagani-adsp').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    extra = ['-I', str(source), '-I', str(platform), '-I', str(tree / '.cache/xnu-work/osfmk')] + WARNINGS
    objects = []
    for name in C_SOURCES:
        objects.append(compile_one(c_record, source / name, out / 'obj' / (Path(name).stem + '.adsp.o'), extra))
    for name in CXX_SOURCES:
        objects.append(compile_one(cxx_record, source / name, out / 'obj' / (Path(name).stem + '.o'), extra))
    archive = out / 'libPaganiAdsp.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    defined, undefined = symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if defined.get(name) != 'strong']
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    problems += [f'{name} must come from libAdreno830.a, not be compiled here' for name in PLATFORM if name in defined]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    report = {'archive': str(archive), 'archive_sha256': sha(archive), 'objects': [o.name for o in objects],
              'source_sha256': {name: sha(source / name) for name in C_SOURCES + CXX_SOURCES},
              'platform': str(platform), 'undefined': sorted(undefined)}
    if args.append_to:
        target = args.append_to.resolve()
        if not target.is_file():
            fail(f'{target} missing; list "component:Adreno830" (and its board recipe) before PaganiAdsp')
        target_defined, _ = symbols([target])
        missing = [name for name in PLATFORM if target_defined.get(name) != 'strong']
        if missing:
            fail(f'{target} lacks the SM8750 platform symbols: ' + ', '.join(missing))
        clash = [name for name, kind in defined.items() if kind == 'strong' and target_defined.get(name) == 'strong']
        if clash:
            fail(f'strong symbols already in {target}: ' + ', '.join(sorted(clash)))
        subprocess.run(['xcrun', 'ar', 'rcs', str(target), *map(str, objects)], check=True)
        twice = {name: members for name, members in strong_per_member(target).items() if len(members) > 1}
        if twice:
            fail(f'{target}: symbols defined by more than one member: ' + ', '.join(sorted(twice)))
        report['appended_to'] = str(target)
        report['appended_sha256'] = sha(target)
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    where = f' and appended to {args.append_to}' if args.append_to else ''
    print(f'PASS: {len(objects)} objects in {archive}{where}; {len(REQUIRED)} entry points, {len(WEAK)} weak defaults, '
          f'{len([n for n in undefined if n.startswith("_Sm8750")])} platform imports')


if __name__ == '__main__':
    main()
