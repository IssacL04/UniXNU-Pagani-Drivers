#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build build/adreno830/libAdreno830.a, the A830 kernel GPU driver, with the actual XNU compile records of a UniXNU tree.

Run by the UniXNU kernel step as the Adreno830 component recipe ("{component}/scripts/build-adreno830.py", cwd = the tree;
ARCHITECTURE 7.1.2). The archive holds the generation-independent driver core, the Gen8 code, the SM8750 platform layer and
the two kits it calls, plus the staged AGXAccelerator class. Board inputs (PaganiGpuBoard and the firmware images) are
appended afterwards by the device repository (scripts/build-pagani-gpu.py).

The sources are explicit lists (ARCHITECTURE 7.1.2): host-only and retired A7xx files must never reach the kernel. Every
NativeAdreno*.cpp must be classified (INHERITED or EXCLUDED) and every Adreno830*/A830*.cpp and platform/sm8750/*.c must be
listed in GEN8 or PLATFORM; --extra compiles another source into a test archive (e.g. --out /tmp/a830 --extra x.c), and
--only compiles single sources with the kernel recipe without archiving them.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
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

# Generation-independent core inherited from Adreno740 (ARCHITECTURE 2.2: kept or parameterized).
INHERITED = [
    'NativeAdrenoPublish.cpp', 'NativeAdrenoUserClient.cpp', 'NativeAdrenoSubmit.cpp', 'NativeAdrenoRing.cpp',
    'NativeAdrenoVm.cpp', 'NativeAdrenoMemory.cpp', 'NativeAdrenoCpu.cpp', 'NativeAdrenoSmmuMemory.cpp',
    'NativeAdrenoSmmuContext.cpp', 'NativeAdrenoGmuContext.cpp', 'NativeAdrenoGpuContext.cpp', 'NativeAdrenoGmuMemory.cpp',
    'NativeAdrenoTimer.cpp', 'NativeAdrenoPwrup.cpp', 'NativeAdrenoFirmware.cpp', 'NativeAdrenoFirmwareDefaults.cpp',
    'NativeAdrenoSha256.cpp', 'NativeAdrenoKernelTime.cpp',
]
# Built staged: its static initializer runs after IOGPUFamily has loaded (step 3).
ACCELERATOR = 'NativeAdrenoAccelerator.cpp'
# Gen8 driver and SM8750 platform sources in the kernel archive (ARCHITECTURE 2.2, IMPLEMENTATION_PLAN M0-M13): the
# orchestrator and status block (M0), SMMU (M2), GMU firmware/core, HFI and votes (M3), CP (M4), ZAP (M5), interrupts,
# fences and the S8 self-tests (M6), recovery, power states and DCVS (M10, M11, M13).
GEN8 = ['Adreno830Status.cpp', 'Adreno830Start.cpp', 'A830Smmu.cpp', 'A830GmuFirmware.cpp', 'A830Gmu.cpp', 'A830Votes.cpp',
        'A830Hfi.cpp', 'A830Cp.cpp', 'A830Zap.cpp', 'A830Irq.cpp', 'A830Fence.cpp', 'A830SelfTest.cpp',
        'A830PowerState.cpp', 'A830Recovery.cpp', 'A830Dcvs.cpp']
# Power-up (M0/M1), RPMh, SCM, SMEM/fuses (M1), AOSS QMP (M3, M13 ACD), LLCC (M11) and tsens (M11 thermal).
PLATFORM = ['platform/sm8750/Sm8750GpuPower.c', 'platform/sm8750/Sm8750Rpmh.c', 'platform/sm8750/Sm8750Scm.c',
            'platform/sm8750/Sm8750SocInfo.c', 'platform/sm8750/Sm8750AossQmp.c', 'platform/sm8750/Sm8750Llcc.c',
            'platform/sm8750/Sm8750Tsens.c']
# Never in the kernel archive: host-only test support. The A7xx/sheng files the architecture retired (NativeAdrenoCpInit,
# NativeAdrenoIrq, NativeAdrenoGpu, NativeAdrenoGmuPlatform/GmuConfig, the three *Service personalities) are deleted; one
# that came back would be an unclassified NativeAdreno source, and their symbols stay FORBIDDEN below.
EXCLUDED = {
    'NativeAdrenoClient.cpp': 'host-only user-space client of the ABI tests',
    'NativeAdrenoBoard.cpp': 'host-only FDT parser of the ABI tests',
}
# The warning policy of the UniXNU reference drivers (build-arm-driver.py, ARCHITECTURE 7.1.2). Apple clang 21 (Xcode 27)
# turns sign conversion and 64-to-32 shortening on by default under -mkernel; the policy predates that, so both stay off
# as they were for every driver qualified with it (the XNU records themselves do not make either an error).
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-sign-conversion',
            '-Wno-shorten-64-to-32']
REQUIRED = ['_adreno830_start', '_adreno830_status', '_native_adreno_publish', '_native_adreno_new_user_client',
            '__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp', '_native_adreno_accelerator_allocate',
            '_native_adreno_accelerator_instance_size', '_native_adreno_accelerator_set_coherent_imports',
            '_Sm8750GpuPowerOn', '_Sm8750GpuPowerOff',
            # publish shell hooks of the orchestrator, A830Irq and A830Recovery (M7, M10)
            '_native_adreno_note_page_fault', '_native_adreno_device_lost', '_native_adreno_recovery_begin',
            '_native_adreno_recovery_end', '_native_adreno_retire_completed', '_native_adreno_coherent_imports',
            '_native_adreno_report_rows', '_native_adreno_publish_status', '_native_adreno_client_tick',
            # run-time entry points: recovery reports, the power state, DCVS and the display/input hooks (M10-M13)
            '_adreno830_recovery_report', '_adreno830_power_state', '_adreno830_dcvs_submit', '_adreno830_dcvs_vsync',
            '_adreno830_dcvs_late_frame', '_adreno830_dcvs_underruns', '_pagani_display_vsync', '_pagani_display_late',
            '_pagani_display_underrun',
            # what PaganiArmCpu's governor reads (pagani-cpufreq=2; ArmCpu.cpp carries weak defaults), 2026-10-30
            '_adreno830_thermal_state', '_adreno830_cpu_hint']
# Defaults that boards without the provider still need: the statically linked kernel refuses undefined weak references.
WEAK = ['_native_gop_handoff_call', '_native_gop_handoff_memory', '_native_gop_handoff_closed', '_boot_log_export_put',
        '_boot_progress_gpu_rows',
        '_boot_progress_display_status',
        # BootProgress's ramoops log, which the bring-up register trace writes through (A830Trace.h, pagani-gpu-trace)
        '_boot_progress_trace', '_boot_progress_trace_enabled',
        '_native_adreno_firmware_image', '_native_adreno_firmware_image_size',
        '_native_adreno_sqe_firmware_image', '_native_adreno_sqe_firmware_image_size',
        '_native_adreno_aqe_firmware_image', '_native_adreno_aqe_firmware_image_size',
        '_native_adreno_zap_image', '_native_adreno_zap_image_size',
        # PaganiArmCpu exports (ARCHITECTURE 4.12.1), PaganiInput's touch counter (M11) and the DRV-2 owner word (M1)
        '_armcpu_spi_attach', '_armcpu_spi_count', '_armcpu_spi_disable', '_armcpu_spi_enable',
        '_pagani_input_touch_events', '_sm8750_rpmh_drv2_owner',
        # PaganiPanel's display power word, read by the screen rule (pagani-screenoff bit 1, A830PowerState), 2026-10-30
        '_pagani_screen_state']
FORBIDDEN = re.compile(r'ShengGmu|NativeAdrenoGpuService|NativeAdrenoGmuService|NativeAdrenoSmmuService|NativeAdrenoCpInit'
                       r'|NativeAdrenoGmuPlatform|NativeAdrenoGmuConfig')


def fail(message):
    raise SystemExit('build-adreno830: ' + message)


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


def compile_one(job):
    record, source, obj, extra = job
    obj.parent.mkdir(parents=True, exist_ok=True)
    obj.unlink(missing_ok=True)
    result = subprocess.run(command(record, source, obj, extra), cwd=record['directory'], capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    if result.returncode:
        fail(f'{source}:\n{output[-4000:]}')
    # The UniXNU build may export CCC_OVERRIDE_OPTIONS=+-Wno-error for XNU itself; this component still allows no warning.
    if 'warning:' in output:
        fail(f'{source} compiled with warnings:\n{output[-4000:]}')
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


def import_macho(tree):
    try:
        from native_prelink import macho   # PYTHONPATH from [kernel.prelink] tool (kernel.py)
        return macho
    except ImportError:
        pass
    for kit in ('kits/NativePrelink', 'kits/XnuPrelink'):
        if (tree / kit / 'native_prelink.py').is_file() or (tree / kit / 'native_prelink').is_dir():
            sys.path.insert(0, str(tree / kit))
            from native_prelink import macho
            return macho
    fail('native_prelink not importable; set PYTHONPATH to the tree\'s prelink kit')


def text_section(macho, path):
    raw = path.read_bytes()
    section = next(s for g in macho(raw)['segments'] for s in g['sections'] if s['name'] == '__text')
    return raw[section['fileoff']:section['fileoff'] + section['size']]


def stage_accelerator(tree, source, out, record, extra):
    """ARCHITECTURE 7.1.2 step 3, as UniXNU build-adreno740-accelerator.py: bind the audited original IOAcceleratorES
    imports and move the class initializer out of __mod_init_func, exported for the board probe."""
    plan_path = tree / 'build/gpu-analysis/ioaccelerator-base-abi.json'
    if not plan_path.is_file():
        plan_path = tree / 'scripts/data/ioaccelerator-base-abi.json'
    plan = json.loads(plan_path.read_text())
    if plan['operational_overrides'] or plan['instance_bytes'] != 136 or plan['virtual_entries'] != 168:
        fail('the audited original IOAcceleratorES is not the expected 136-byte, 168-entry class without overrides')
    obj = compile_one((record, source / ACCELERATOR, out / 'obj/NativeAdrenoAccelerator.o', extra + ['-Wno-overloaded-virtual']))
    bindings = {'__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(plan['protected_compatible_constructor_aliases'][0], 16),
                '__ZN15IOAcceleratorESD2Ev': int(plan['nondeleting_destructor'], 16),
                '__ZN15IOAcceleratorES10gMetaClassE': int(plan['metaclass'], 16)}
    _, undefined = symbols([obj])
    imports = {name for name in undefined if 'IOAcceleratorES' in name}
    if imports - set(bindings) or not imports:
        fail(f'unexpected original-base imports: {sorted(imports - set(bindings))}')
    assembly = out / 'obj/original-bindings.S'
    assembly.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n' for name, address in sorted(bindings.items())
                                if name in imports))
    sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], check=True, capture_output=True, text=True).stdout.strip()
    bound = out / 'obj/original-bindings.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', str(assembly), '-o', str(bound)],
                   check=True)
    late = out / 'obj/NativeAdrenoAccelerator.late.o'
    late.unlink(missing_ok=True)
    subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
                    '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp', str(obj), str(late)], check=True)
    macho = import_macho(tree)
    if text_section(macho, obj) != text_section(macho, late):
        fail('staging the accelerator changed its instructions')
    return [late, bound], {name: hex(address) for name, address in bindings.items()}, plan_path


def include_shim(tree, out):
    """drivers/Adreno830 is a symbolic link in the tree, and clang resolves "../X" in a quoted include against the real
    directory, not the tree. A real directory under the output, on the include path, makes both spellings of the
    accelerator ABI header ("../NativeMetal/..." and "../AdrenoMetal/...") resolve to the tree's AdrenoMetal."""
    shim = out / 'include/shim'
    shim.mkdir(parents=True, exist_ok=True)
    metal = tree / 'drivers/AdrenoMetal'
    if not (metal / 'OriginalAcceleratorABI.h').is_file():
        fail(f'{metal}/OriginalAcceleratorABI.h missing; select AdrenoMetal in the board components')
    for name in ('NativeMetal', 'AdrenoMetal'):
        link = out / 'include' / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(metal.resolve())
    return shim


def classify(source, extra):
    listed = set(INHERITED) | {ACCELERATOR} | set(EXCLUDED)
    unknown = sorted(p.name for p in source.glob('NativeAdreno*.cpp') if p.name not in listed)
    if unknown:
        fail('unclassified NativeAdreno sources (add each to INHERITED or EXCLUDED): ' + ', '.join(unknown))
    missing = [name for name in INHERITED + GEN8 + PLATFORM + [ACCELERATOR] if not (source / name).is_file()]
    if missing:
        fail('missing driver sources: ' + ', '.join(missing))
    sources = [source / name for name in INHERITED + GEN8]
    notes = []
    platform = [source / name for name in PLATFORM]
    for path in extra:
        path = path if path.is_absolute() else source / path
        if not path.is_file() or path.suffix not in ('.c', '.cpp'):
            fail(f'--extra {path}: not a C or C++ source')
        (platform if path.suffix == '.c' else sources).append(path.resolve())
    chosen = {p.resolve() for p in sources + platform}
    pending = sorted(str(p.relative_to(source)) for p in [*source.glob('Adreno830*.cpp'), *source.glob('A830*.cpp'),
                                                           *(source / 'platform/sm8750').glob('*.c')]
                     if p.resolve() not in chosen)
    if pending:
        fail('Gen8/platform sources outside the kernel archive (list each in GEN8 or PLATFORM): ' + ', '.join(pending))
    return sources, platform, notes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=Path.cwd(), help='UniXNU build tree (default: the working directory)')
    parser.add_argument('--source', type=Path, default=SOURCE, help='Adreno830 sources (default: this repository)')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/adreno830)')
    parser.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    parser.add_argument('--extra', type=Path, action='append', default=[],
                        help='also compile and archive this source (relative to --source); for qualification builds')
    parser.add_argument('--only', type=Path, action='append', default=[],
                        help='compile only this source (relative to --source) with the kernel recipe and stop, without the '
                             'archive and symbol checks: a per-file check while other sources are being written')
    args = parser.parse_args()
    tree, source = args.tree.resolve(), args.source.resolve()
    if not (tree / 'components.toml').is_file():
        fail(f'{tree} is not a UniXNU build tree (no components.toml)')
    out = (args.out or tree / 'build/adreno830').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)   # objects of sources no longer listed must not linger
    out.mkdir(parents=True, exist_ok=True)
    cxx_record, c_record = load_record(tree, CXX_RECORD), load_record(tree, C_RECORD)
    kits = {name: tree / 'kits' / name for name in ('ArmSmmu4K', 'AdrenoPM4', 'Sm8750Platform')}
    for name, path in kits.items():
        if not path.is_dir():
            fail(f'{path} missing; Adreno830 depends on the {name} component')
    shim = include_shim(tree, out)
    includes = ['-I', str(source), '-I', str(source / 'platform/sm8750'), '-I', str(kits['ArmSmmu4K']),
                '-I', str(kits['AdrenoPM4']), '-I', str(kits['Sm8750Platform']), '-I', str(tree / 'drivers/AdrenoMetal'),
                '-I', str(shim), '-I', str(tree / '.cache/xnu-work/osfmk')]
    extra = includes + WARNINGS
    if args.only:
        jobs = []
        for path in args.only:
            path = (path if path.is_absolute() else source / path).resolve()
            if not path.is_file() or path.suffix not in ('.c', '.cpp'):
                fail(f'--only {path}: not a C or C++ source')
            cxx = path.suffix == '.cpp'
            jobs.append((cxx_record if cxx else c_record, path, out / 'obj' / (path.stem + ('.o' if cxx else '.platform.o')),
                         extra))
        with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
            objects = list(pool.map(compile_one, jobs))
        print(f'PASS: {len(objects)} sources compiled with the kernel recipe into {out / "obj"} (no archive)')
        return
    drivers, platform, notes = classify(source, args.extra)
    kit_sources = [kits['AdrenoPM4'] / 'AdrenoPM4.c'] + sorted(kits['ArmSmmu4K'].glob('ArmSmmu4K*.c'))
    jobs = [(cxx_record, path, out / 'obj' / (path.stem + '.o'), extra) for path in drivers]
    jobs += [(c_record, path, out / 'obj' / (path.stem + '.platform.o'), extra) for path in platform]
    jobs += [(c_record, path, out / 'obj' / (path.stem + '.kit.o'), extra) for path in kit_sources]
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        objects = list(pool.map(compile_one, jobs))
    staged, bindings, plan_path = stage_accelerator(tree, source, out, cxx_record, extra)
    objects += staged

    archive = out / 'libAdreno830.a'
    archive.unlink(missing_ok=True)   # a member of a source dropped from the lists must not survive (-force_load links all)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    members = [m for m in subprocess.run(['xcrun', 'ar', 't', str(archive)], check=True, capture_output=True,
                                         text=True).stdout.splitlines() if m and not m.startswith('__.SYMDEF')]
    if sorted(members) != sorted(obj.name for obj in objects) or len(set(members)) != len(members):
        fail(f'archive members {sorted(members)} differ from the source lists')
    retired = [m for m in members if Path(m).stem.split('.')[0] in {Path(name).stem for name in EXCLUDED}]
    if retired:
        fail('retired or host-only objects in the archive: ' + ', '.join(retired))

    defined, undefined = symbols([archive])
    problems = [f'{name} not defined' for name in REQUIRED if name not in defined]
    problems += [f'{name} must be a weak definition' for name in WEAK if defined.get(name) != 'weak']
    problems += [f'forbidden symbol {name}' for name in sorted(set(defined) | undefined) if FORBIDDEN.search(name)]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))

    listed = drivers + platform + kit_sources + [source / ACCELERATOR]
    report = {
        'archive': str(archive.relative_to(tree)) if archive.is_relative_to(tree) else str(archive),
        'archive_sha256': sha(archive),
        'objects': [obj.name for obj in objects],
        'source_sha256': {str(p): sha(p) for p in listed},
        'accelerator_bindings': bindings, 'accelerator_abi': str(plan_path), 'accelerator_abi_sha256': sha(plan_path),
        'excluded': EXCLUDED, 'notes': notes,
        'undefined': sorted(undefined),
    }
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    for note in notes:
        print('note: ' + note)
    print(f'PASS: {len(objects)} objects in {archive} ({len(drivers)} driver, {len(platform)} platform, {len(kit_sources)} kit, '
          f'staged accelerator); {len(REQUIRED)} required and {len(WEAK)} weak definitions present')


if __name__ == '__main__':
    main()
