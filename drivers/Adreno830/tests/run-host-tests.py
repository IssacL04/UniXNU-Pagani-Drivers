#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the Adreno830 host tests (IMPLEMENTATION_PLAN 0.3 C1).

Every test is compiled with xcrun clang/clang++ -O2 -Wall -Wextra -Werror -fsanitize=address,undefined against host
doubles: IOKit/IOTypes.h and the other kernel headers the driver includes are replaced by tests/XnuMemoryFixture.h (and
kern/clock.h by tests/Adreno830HostClock.h). Each test must exit 0 and print its PASS line; known tests must print exactly
the pinned line. Tests not in the table (new modules) are discovered: tests/test_sm8750_<name>.c is built with the
platform/sm8750 sources whose name contains <name> (or all of them), tests/test_<name>.cpp with the repository sources
named like <name>; a test may instead list its sources in a comment line "host-test: <path> <path> ...".
With a UniXNU build tree (--tree, or found under --unixnu), every kernel object is also compiled with the tree's real
XNU compile records and the archive symbols are checked (scripts/build-adreno830.py into build/host-tests/kernel).
"""
import argparse
import shlex
import os
import json
import re
import subprocess
import sys
from pathlib import Path

CC = shlex.split(os.environ.get('CC', 'xcrun clang' if sys.platform == 'darwin' else 'clang'))
CXX = shlex.split(os.environ.get('CXX', 'xcrun clang++' if sys.platform == 'darwin' else 'clang++'))
ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'
FLAGS = ['-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
SMMU_HOST = ['ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams']
CORE = ['NativeAdrenoMemory.cpp', 'NativeAdrenoSmmuMemory.cpp']
SUBMIT = CORE + ['NativeAdrenoVm.cpp', 'NativeAdrenoRing.cpp', 'NativeAdrenoSubmit.cpp']
# name: (repository sources, extra host objects, pinned PASS line or None, note). Every other test_*.c/.cpp is discovered
# (its "host-test:" line, else the sources named like it).
KNOWN = {
    'test_memory': (['NativeAdrenoMemory.cpp'], [],
        'PASS: 18 allocation profiles, 16 allocation/cache/lifetime failure controls, 20 argument/CPU refusal controls', ''),
    'test_smmu_memory': (CORE, ['smmu'], 'PASS: 34 native allocator/table-pool bindings and 4 capacity/scatter/publication/'
        'ownership refusals; no hardware context enabled', ''),
    'test_vm_alias': ([], [], 'PASS: physical alias index 2048 oracle profiles / 524288 queries; 65536 reversed spans and '
        'exact gap boundaries', ''),
    'test_vm': (CORE + ['NativeAdrenoVm.cpp'], ['smmu'],
        'PASS: VM over the A830 programmer: attach proves regions and routes once, 200 map/unmap rounds under pending faults '
        'never poison, per-buffer fence unmap, 64-bit TLBIVAL, 4 GiB (2053), 2 GiB (1027) and 16 GiB (8209, BOs across and past 4 GiB) apertures with reserved windows, '
        '13 coherence-policy checks, coherent table walk, 3 poisoning paths, kit-stream attach, 600 locked concurrent '
        'transactions', 'M2/M7/M10'),
    'test_ring': (['NativeAdrenoRing.cpp'], ['pm4'],
        'PASS: Gen8 ring: register-free attach, kernel/raw/framed packets with both SRAM inits, fenced WPTR 10 us x 200 with '
        'poison on failure, Busy vs NoResources, exact publication, A830Fence waiter, ring lock, recovery (15 cases)', 'M4/M10'),
    'test_submit': (SUBMIT, ['smmu', 'pm4'],
        'PASS: 12 execbuf cases over software SMMU/CP peers: exact framed IB packet and fence with no SMMU access, cache '
        'maintenance or TLBI on submit; 9 pre-reference refusals; 44 job slots with one reserved per range (32) and Busy only with '
        'work in flight; ring-full retry within 200 ms; teardown waits; halt/recover/resume; the hang\'s culprit range by fence '
        '(never a passed or poisoned job); poisoned-commit retention; lock hooks', 'M7/M10'),
    'test_userclient': (SUBMIT + ['NativeAdrenoUserClient.cpp', 'NativeAdrenoTimer.cpp'], ['smmu', 'pm4', 'proc'],
        'PASS: 22 user-client blocks over software SMMU/CP peers: transaction with wait_fence validated against the ring (no '
        'CPU wait), per-connection faults with FAR attribution, 32 slots with 4 GiB (1+3 large+8 small)/16 GiB (1+3 large+28 '
        'small)/2 GiB windows, slot classes (apps leave 3/4 small slots to the listed processes, which may borrow a large one; an '
        'app never takes a large one), terms per process (the render server\'s in any slot), per-name rows (an idle row without '
        'hangs given up), the grant and large-client lists and the open policy with the hang refusal, capacity 256..16384 and the '
        'render server\'s reserve (mapping records, table ranges, BO pool), imports with direction-split maintenance, M9 scanout '
        'import, FIFO handle quarantine (4096) and slot encoding, ring-full retry vs Busy, M10 lock use (no Memory lock outside the '
        'pool), dispatch sizes, refusals, teardown, device info, two clients, late scope, retire, contiguous '
        'fallback, poisoned commit, CPU leases, queues, connection bytes by kind, teardown past a refused object', 'M7/M9/M10'),
    'test_timer': (['NativeAdrenoTimer.cpp'], [],
        'PASS: GMU AO counter at gmu +0x47100/+0x47104 and CP 0x8E7/0x8E8, 19 high-low-high reads (rollovers included), 12 '
        'refusals over a software counter peer', 'M6'),
    'test_client': (SUBMIT + ['NativeAdrenoUserClient.cpp', 'NativeAdrenoTimer.cpp', 'NativeAdrenoClient.cpp'],
                    ['smmu', 'pm4', 'proc'],
        'PASS: 1 userspace-to-kernel round trip including CPU mapping leases, queue lifecycle and a wait_fence validated '
        'without CPU wait, 1 version mismatch, 1 altered-size rejection and 8 client-side refusals over software SMMU/CP '
        'peers', 'M7'),
    'test_board': (['NativeAdrenoBoard.cpp'], [],
        'PASS: 4 real SM8750/PKX110 cell sets (GPU 4+3 windows, GMU 2 windows, #iommu-cells=2 SIDs 0/1/2/5) and 19 '
        'malformed/limit refusals over the device-tree parser', '13T device tree'),
    'test_sha256': (['NativeAdrenoSha256.cpp', 'NativeAdrenoFirmware.cpp'], [], None, 'run per gen80000 image'),
    'test_gmumem': (CORE + ['NativeAdrenoVm.cpp', 'NativeAdrenoGmuMemory.cpp'], ['smmu', 'pm4'],
        'PASS: 5 PREALLOC plans (PKX110: 4 CACHE regions 0x76000 bytes, persist/log/HFI at 0x60000000/4000/8000), 24 '
        'refusals, 4 allocations with write-combined regions and a scattered fallback over host doubles', 'M3'),
    'test_gmumap': (CORE + ['NativeAdrenoVm.cpp', 'NativeAdrenoGmuMemory.cpp'], ['smmu', 'pm4'],
        'PASS: 7 PREALLOC GMU regions mapped at their GMU addresses (CACHE privileged RWX, persist/log/HFI privileged RW), 1 '
        'rolled-back failed map, 5 refusals, unmap and release over a software SMMU peer', 'M3'),
    'test_smmuctx': (CORE + ['NativeAdrenoSmmuContext.cpp'], ['smmu'],
        'PASS: 4 SMMU context cases with an injected programmer: kit image from the SMMU\'s IDs, claim before program, route '
        'after, LPAC on the enabled bank, TTBR1 guard as ASID|table, fault before disable, 5 poisoning failures', 'M2'),
    # test_gmuctx includes ../A830Smmu.cpp itself: A830Smmu.cpp must not be listed here.
    'test_gmuctx': (CORE + ['NativeAdrenoVm.cpp', 'NativeAdrenoGmuMemory.cpp', 'NativeAdrenoSmmuContext.cpp',
                    'NativeAdrenoGmuContext.cpp'], ['smmu', 'mmio'],
        'PASS: 5 GMU context cases with the A830Smmu programmer (CB2, slot 1, SID 5, ASID 3) over the hypervisor-emulated '
        'SMMU model: complete bring-up with the GMU memory mapped, moved slot, dropped TCR write, map-hook failure, refusals',
        'M2/M3'),
    'test_pwrup': (['NativeAdrenoPwrup.cpp'], [],
        'PASS: 1 exact cpu_gpu_lock image, 1 Gen8 list (77 IFPC + 49 pwrup pairs, 60 pipe triplets) with zero tail, 1 empty '
        'list, 6 capacity/argument refusals and 3 aperture refusals', 'M4'),
    'test_status': (['Adreno830Status.cpp'], [],
        'PASS: 29 exact row layouts, 20384 bounded rows (worst cases on every page and random blocks, 80524 rows checked), '
        '11 publish transitions and 10 run-time writers; seq, the overlay and the registry move only on changed text', 'M0'),
    'test_start': (['Adreno830Status.cpp', 'Adreno830Start.cpp', 'NativeAdrenoFirmware.cpp', 'NativeAdrenoSha256.cpp'], [],
        'PASS: 17 full runs, 12 pagani-gpu-stop runs, 54 injected stage failures (each failing once with the module\'s step '
        'and reason, S5 cold boot at most 3 attempts), 120 S0 board faults, 12 power/recovery hooks and 26 boot-argument '
        'cases over host doubles', 'M0'),
    'test_sm8750_power': (['platform/sm8750/Sm8750GpuPower.c'], [],
        'PASS: SM8750 GPU power-up follows the Linux clock semantics observed on PKX110', ''),
    # test_a830trace builds tests/test_start.cpp (main renamed) for its doubles: the same sources, no kit objects.
    'test_a830trace': (['Adreno830Status.cpp', 'Adreno830Start.cpp', 'NativeAdrenoFirmware.cpp', 'NativeAdrenoSha256.cpp'], [],
        'PASS: A830Trace 13 line-level cases; S1-S3 over scripted register traffic, design and annibale profiles: 2 verbatim '
        'runs (every access in order with stage, step, block, address and value, writes before they land, SMCs before the '
        'instruction; design 1113 lines), 2 counted runs (polls 1-4 and powers of two, sequential runs to 16, 9 end lines; '
        'design 463 lines), 8 silent runs (pagani-gpu-trace=0 or the log off), 2 stage failure and S0 refusal (trace=3) '
        'cases, 1 quiet CX seed, 3 operation scopes, 6 aperture notes (16, 4 and 2 GiB)', 'pagani-gpu-trace'),
}
# Tests expected to fail until their milestone rewrites them; an unexpected pass is reported so the entry can go. Empty
# since the integration of M1-M13: test_vm, test_gmumap, test_submit, test_userclient and test_client pass.
EXPECTED_FAIL = {}
# Tests of code the architecture retired. Their files are gone (test_gmuconfig, test_gmuplatform, test_gpu, test_irq,
# test_cpinit were deleted with the code); a test listed here would be reported as SKIP.
RETIRED = {}


def run(command, **kw):
    return subprocess.run([str(c) for c in command], capture_output=True, text=True, **kw)


class Runner:
    def __init__(self, args):
        self.out = args.out
        self.out.mkdir(parents=True, exist_ok=True)
        self.kits = args.unixnu / 'kits'
        self.failures, self.passed, self.skipped, self.expected_failures = [], 0, [], []
        includes = self.out / 'host-include'
        for name in ('IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h', 'IOKit/IOLib.h', 'kern/task.h'):
            path = includes / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "XnuMemoryFixture.h"\n')
        (includes / 'kern/clock.h').write_text('#include "Adreno830HostClock.h"\n')
        self.includes = ['-I', includes, '-I', TESTS, '-I', ROOT]
        self.objects = {}

    def host_objects(self, kinds):
        result = []
        for kind in kinds:
            if kind not in self.objects:
                if kind == 'proc':   # kernel process query the user client records with each import
                    source = self.out / 'proc-host.c'
                    source.write_text('int proc_selfpid(void);\nint proc_selfpid(void) {return 100;}\n')
                    obj = self.out / 'proc-host.o'
                    result_ = run([*CC, '-std=c11', *FLAGS, '-c', source, '-o', obj])
                    if result_.returncode:
                        raise SystemExit(f'host double proc_selfpid: {result_.stderr[-2000:]}')
                    self.objects[kind] = [obj]
                    result += self.objects[kind]
                    continue
                stems = {'smmu': SMMU_HOST, 'mmio': ['ArmSmmu4KMmio'], 'pm4': ['AdrenoPM4']}[kind]
                kit = self.kits / ('AdrenoPM4' if kind == 'pm4' else 'ArmSmmu4K')
                built = []
                for stem in stems:
                    obj = self.out / f'{stem}-host.o'
                    result_ = run([*CC, '-std=c11', *FLAGS, '-c', kit / f'{stem}.c', '-o', obj])
                    if result_.returncode:
                        raise SystemExit(f'host kit {stem}: {result_.stderr[-2000:]}')
                    built.append(obj)
                self.objects[kind] = built
            result += self.objects[kind]
        return result

    def build(self, name, test, sources, kinds):
        binary = self.out / name
        c_sources = [s for s in sources if s.suffix == '.c']
        cpp_sources = [s for s in sources if s.suffix == '.cpp']
        objects = list(self.host_objects(kinds))
        includes = self.includes + ['-I', self.kits / 'ArmSmmu4K', '-I', self.kits / 'AdrenoPM4', '-I', ROOT / 'platform/sm8750']
        if test.suffix == '.c' and not cpp_sources:
            command = [*CC, '-std=c11', *FLAGS, '-I', ROOT / 'platform/sm8750', '-I', TESTS, test, *c_sources,
                       '-o', binary]
        else:
            for source in c_sources:
                obj = self.out / f'{name}-{source.stem}.o'
                result = run([*CC, '-std=c11', *FLAGS, *includes, '-c', source, '-o', obj])
                if result.returncode:
                    return None, result.stdout + result.stderr
                objects.append(obj)
            std = '-std=c++17' if name == 'test_vm_alias' else '-std=c++23'
            command = [*CXX, std, *FLAGS, '-DADRENO830_HOST_TEST', *includes, *cpp_sources, test, *objects,
                       '-o', binary]
        result = run(command)
        if result.returncode:
            return None, result.stdout + result.stderr
        return binary, ''

    def check(self, name, binary, expected, arguments=(), label=None):
        label = label or name
        result = run([binary, *arguments], timeout=600)
        lines = [line for line in result.stdout.splitlines() if line.startswith('PASS')]
        if name in EXPECTED_FAIL:
            if result.returncode or not lines:
                detail = (result.stdout + result.stderr).strip().splitlines()
                print(f'XFAIL {label}: {EXPECTED_FAIL[name]} ({detail[-1] if detail else "no output"})')
                self.expected_failures.append(name)
            else:
                self.failures.append(f'{label}: passed but is listed in EXPECTED_FAIL; remove the entry')
                print(f'XPASS {label}: {lines[-1]}')
            return
        if result.returncode or not lines:
            self.failures.append(f'{label}: exit {result.returncode}\n{(result.stdout + result.stderr)[-3000:]}')
            print(f'FAIL {label}')
            return
        if expected and lines[-1] != expected:
            self.failures.append(f'{label}: printed\n  {lines[-1]}\nexpected\n  {expected}')
            print(f'FAIL {label}: unexpected PASS line')
            return
        self.passed += 1
        print(f'{label}: {lines[-1]}')


def discovered_sources(test):
    text = test.read_text(errors='replace')
    directive = re.search(r'host-test:\s*([^\n*]+)', text)
    if directive:
        return [ROOT / item for item in directive.group(1).split()]
    name = test.stem.removeprefix('test_')
    if name.startswith('sm8750_'):
        token = name.removeprefix('sm8750_').replace('_', '')
        platform = sorted((ROOT / 'platform/sm8750').glob('*.c'))
        matched = [p for p in platform if token in p.stem.lower()]
        return matched or platform
    token = name.replace('_', '')
    return [p for p in sorted(ROOT.glob('*.cpp')) if p.stem.lower().removeprefix('nativeadreno') == token
            or p.stem.lower() == token or p.stem.lower().removeprefix('adreno830') == token]


def find_tree(unixnu):
    for out in ('out-gpu', 'out'):
        for tree in sorted((unixnu / out).glob('*/*/*/tree')):
            if (tree / 'drivers/Adreno830').exists() and (tree / RECORD).is_file():
                return tree
    return None


RECORD = Path('build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, default=ROOT.parent / 'UniXNU', help='UniXNU checkout (kits, build trees)')
    parser.add_argument('--firmware', type=Path, help='directory with the gen80000 images (test_sha256 per image)')
    parser.add_argument('--pins', type=Path, help='firmware pins (default: <firmware>/../../../scripts/data/sm8750-gpu-inputs.json)')
    parser.add_argument('--tree', type=Path, help='UniXNU build tree for the kernel-object check (default: found under --unixnu)')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/host-tests')
    parser.add_argument('names', nargs='*', help='run only these tests (e.g. test_status)')
    args = parser.parse_args()
    runner = Runner(args)
    tests = sorted(TESTS.glob('test_*.c*'))
    for test in tests:
        name = test.stem
        if args.names and name not in args.names:
            continue
        if name in RETIRED:
            print(f'SKIP {name}: {RETIRED[name]}')
            runner.skipped.append(name)
            continue
        if name in KNOWN:
            sources, kinds, expected, note = KNOWN[name]
            sources = [ROOT / s for s in sources]
        else:
            sources, kinds, expected, note = discovered_sources(test), ['smmu', 'pm4'] if test.suffix == '.cpp' else [], None, 'discovered'
        binary, error = runner.build(name, test, sources, kinds)
        if not binary:
            runner.failures.append(f'{name}: build failed\n{error[-3000:]}')
            print(f'FAIL {name}: build')
            continue
        if note and note != 'M0':
            print(f'  ({name}: {note})')
        if name != 'test_sha256':
            runner.check(name, binary, expected)
            continue
        runner.check(name, binary, 'PASS: 4 FIPS vectors; no firmware path supplied', label='test_sha256 (vectors)')
        if not args.firmware:
            print('SKIP test_sha256 gen80000 images: no --firmware')
            runner.skipped.append('test_sha256 firmware')
            continue
        pins_path = args.pins or args.firmware.resolve().parents[2] / 'scripts/data/sm8750-gpu-inputs.json'
        pins = json.loads(pins_path.read_text())['firmware']['files']
        for file, pin in pins.items():
            path = args.firmware / file
            if not path.is_file():
                runner.failures.append(f'test_sha256: {path} missing')
                continue
            runner.check(name, binary,
                         f'PASS: 4 FIPS vectors and the pinned GMU firmware ({pin["bytes"]} bytes) validated, 6 malformed/limit refusals',
                         [path, pin['sha256']], label=f'test_sha256 {file}')
    if not args.names:
        tree = args.tree or find_tree(args.unixnu)
        if tree is None:
            print('SKIP kernel objects: no UniXNU build tree with drivers/Adreno830 (pass --tree)')
            runner.skipped.append('kernel objects')
        else:
            result = run([sys.executable, ROOT / 'scripts/build-adreno830.py', '--tree', tree, '--source', ROOT,
                          '--out', args.out / 'kernel'])
            output = (result.stdout + result.stderr).strip()
            if result.returncode:
                runner.failures.append(f'kernel objects ({tree}):\n{output[-3000:]}')
                print('FAIL kernel objects')
            else:
                runner.passed += 1
                print('kernel objects: ' + output.splitlines()[-1])
    for failure in runner.failures:
        print('\n' + failure, file=sys.stderr)
    print(f'\n{runner.passed} passed, {len(runner.failures)} failed, {len(runner.expected_failures)} expected failures, '
          f'{len(runner.skipped)} skipped')
    sys.exit(1 if runner.failures else 0)


if __name__ == '__main__':
    main()
