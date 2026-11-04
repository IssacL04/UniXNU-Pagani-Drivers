#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host checks of PaganiDiag beyond the core test (Test/run.sh runs them all):

  check-paganidiag-host.py patch     kernel/phone-diag-sysctl.patch is what scripts/make-diag-patch.py makes for the device series (it
                                     applies, and the series' later patches still apply on it); kern_exec.c with the series up to and
                                     including this patch compiles with the build's own compile record (kern_exec.o.json) with zero
                                     errors and not one warning more than without PaganiDiag; the object has the sysctl handler and the
                                     weak key and touch counters.
  check-paganidiag-host.py build     scripts/build-paganidiag.py into a temporary directory: verified ad hoc signature, 4 KiB pages,
                                     identifier, entitlements exactly paganidiag.entitlements, a thin arm64 iOS 17.0 MH_EXECUTE,
                                     trust.json's CDHash and the path the kernel starts.
  check-paganidiag-host.py flow      the daemon built for this Mac and run with --print (the keychain probe left out, skip=kc: this must
                                     not touch the host's keychain): start, a summary for each of three rounds, the details once, end;
                                     every line short enough, every probe that answers on a Mac answered, the widget census through
                                     this Mac's real LaunchServices (the same private calls as on the phone).
  check-paganidiag-host.py imports   every symbol the daemon imports is exported by 21H461's shared cache (ipsw dyld tbd): a binary built
                                     with a newer SDK must not die in dyld on the phone, before a single line.
  check-paganidiag-host.py policy    Test/check-exec-policy.py: the Sandbox platform policy of the built kernel lets xpcproxy run the
                                     daemon from where it is, and not from where the A package had it.
  check-paganidiag-host.py trust     scripts/make-diag-trust.py on an owner trust bundle: the old owner entries plus the daemon's, less
                                     --drop, the Apple modules unchanged, refused when the daemon does not match trust.json.
  check-paganidiag-host.py replace   scripts/replace-diag-in-seed.py on a copy of the final seed: Test/check-seed.py passes on the result,
                                     the trust bundle holds the new CDHash and every other plug-in's, the seed report carries it for the
                                     layout script; a seed whose launch policy disables com.apple.DumpPanic is refused.
  check-paganidiag-host.py seed      integration/make-phone-seed.diff applies to 工具/make-phone-seed.py or is merged in it; the copy
                                     that carries it stages the daemon into a copy of a phone seed (Test/check-seed.py checks it) and
                                     refuses a seed whose launch policy disables com.apple.DumpPanic.
  check-paganidiag-host.py layout    integration/make-a830-layout.diff applies to 工具/make-a830-layout.sh or is merged in it; with it the
                                     layout's own host test (工具/test-phone-island-layout.py) passes, A830_DIAG reaches make-phone-seed.py, the image
                                     gets boot-arg pagani-diag=1 and no sheng-no-lc (only with A830_DIAG), A830_DIAG=1 works with a
                                     prebuilt seed, and an owner trust bundle without the daemon is refused.
Environment: XNU_OBJ points at another built tree; PAGANIDIAG_BASE_SEED at another phone seed. A check whose inputs are missing says SKIP."""
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parents[1]
TOP = Path('/Users/xiaochoumao/unixnu-13t')
TREE = TOP / 'UniXNU/out-gpu/oneplus13t/ipados-17.7.11-21H461/oneplus13t/tree'
XNU_WORK = TREE / '.cache/xnu-work'
XNU_OBJ = Path(os.environ.get('XNU_OBJ', TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT'))
INPUTS = TOP / 'build-userspace/a830-inputs'
SCRATCH = Path(os.environ['TMPDIR']) if os.environ.get('TMPDIR') else None      # hdiutil and the 2 GiB sparse copies want a real disk
APPS_TRUST = INPUTS / '2026-10-31-pkgAB/phone-island-apps-audio-owner-trust.bundle'
FINAL_SEED = INPUTS / 'final/s3-full.dbrdisk'
FINAL_TRUST = INPUTS / 'final/s3-full-owner-trust.bundle'
BASE_SEED = Path(os.environ.get('PAGANIDIAG_BASE_SEED', TOP / '工具/seeds/phone-island-apps.dbrdisk'))
DSC = INPUTS / 'systemos-21H461/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'
# libSystem re-exports these; the dylibs the daemon links directly come first.
DSC_IMAGES = ('libc++.1.dylib', 'libc++abi.dylib', 'libobjc.A.dylib', 'Foundation', 'CoreFoundation', 'OSLog', 'libSystem.B.dylib',
              'libsystem_c.dylib', 'libsystem_kernel.dylib', 'libsystem_pthread.dylib', 'libsystem_malloc.dylib', 'libsystem_platform.dylib',
              'libsystem_info.dylib', 'libsystem_notify.dylib', 'libsystem_blocks.dylib', 'libdispatch.dylib', 'libdyld.dylib',
              'libsystem_m.dylib', 'libsystem_trace.dylib', 'libcompiler_rt.dylib', 'libsystem_darwin.dylib')


class Skip(Exception):
    pass


def fail(message):
    raise SystemExit('check-paganidiag-host: FAIL: ' + message)


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, **kwargs)


def compile_kern_exec(source, keep):
    """Compiles source with kern_exec.o.json's own command; returns (returncode, warning and error lines, object symbols)."""
    record_path = XNU_OBJ / 'kern_exec.o.json'
    if not record_path.is_file():
        raise Skip(f'no compile record {record_path}')
    record = json.JSONDecoder().raw_decode(record_path.read_text())[0]
    arguments, skip = [], False
    obj = Path(keep) / (Path(source).stem + '.o')
    for argument in record['arguments']:
        if skip:
            skip = False
            continue
        if argument == record['file']:
            arguments.append(str(source))
        elif argument == '-o':
            arguments += ['-o', str(obj)]
            skip = True
        elif not (argument.startswith('-MD') or argument.startswith('-MF') or argument == '-MMD'):
            arguments.append(argument)
    arguments += ['-iquote', str(XNU_WORK / 'bsd/kern')]
    environment = dict(os.environ, PATH=os.environ.get('PATH', '') + ':/opt/homebrew/opt/llvm/bin',
                       CCC_OVERRIDE_OPTIONS='# +-Wno-unnecessary-virtual-specifier')
    result = subprocess.run(arguments, cwd=record['directory'], capture_output=True, text=True, env=environment)
    lines = [re.sub(r':\d+:\d+:', ':L:', line.replace(str(source), 'kern_exec.c')) for line in result.stderr.splitlines()
             if ' warning: ' in line or ' error: ' in line]
    symbols = run('xcrun', 'nm', obj).stdout if obj.exists() else ''
    return result.returncode, lines, symbols


def check_patch():
    with tempfile.TemporaryDirectory(prefix='paganidiag-patch-') as tmp:
        root = Path(tmp)
        fresh, base_path, final_path = root / 'fresh.patch', root / 'base.c', root / 'final.c'
        try:
            made = run(sys.executable, '-B', HERE / 'scripts/make-diag-patch.py', '--out', fresh, '--base', base_path, '--final', final_path)
        except subprocess.CalledProcessError as failed:
            if 'changed since the build' in failed.stderr or 'No such file' in failed.stderr:
                raise Skip(failed.stderr.strip()[-200:])
            fail('make-diag-patch.py: ' + failed.stderr[-600:])
        if fresh.read_text() != (HERE / 'kernel/phone-diag-sysctl.patch').read_text():
            fail('kernel/phone-diag-sysctl.patch is not what make-diag-patch.py makes (run it again)')
        patched, base = root / 'patched', root / 'plain'
        patched.mkdir()
        base.mkdir()
        shutil.copyfile(final_path, patched / 'kern_exec.c')
        shutil.copyfile(HERE / 'kernel/pagani_diag.h', patched / 'pagani_diag.h')
        shutil.copyfile(base_path, base / 'kern_exec.c')
        rc_base, base_lines, _ = compile_kern_exec(base / 'kern_exec.c', base)
        rc, lines, symbols = compile_kern_exec(patched / 'kern_exec.c', patched)
        if rc_base:
            raise Skip('the unpatched kern_exec.c does not compile with its record here')
        errors = [line for line in lines if ' error: ' in line]
        if rc or errors:
            fail('the patched kern_exec.c does not compile:\n' + '\n'.join(errors[:10]))
        new = list(lines)
        for line in base_lines:
            if line in new:
                new.remove(line)
        if new:
            fail(f'{len(new)} new warnings:\n' + '\n'.join(new[:10]))
        for symbol in ('_sysctl_pagani_diag_line', '_pagani_input_key_events', '_pagani_input_touch_events'):
            if symbol not in symbols:
                fail(f'the object lacks {symbol}')
        later = made.stdout.split('later patches still apply: ')[-1].strip() if 'later patches' in made.stdout else 'none'
        print(f'PASS patch: applies to kern_exec.c as the series leaves it; later patches apply ({later}); compiled with kern_exec.o.json, '
              f'0 errors, {len(lines)} warnings as without it')


def imported(binary):
    symbols = []
    for line in run('xcrun', 'nm', '-um', binary).stdout.splitlines():
        found = re.search(r'external (\S+) \(from (\S+)\)', line)
        if found:
            symbols.append(found.group(1))
    return symbols


def check_imports():
    if not shutil.which('ipsw'):
        raise Skip('no ipsw')
    if not DSC.is_file():
        raise Skip(f'no shared cache {DSC}')
    with tempfile.TemporaryDirectory(prefix='paganidiag-imports-') as tmp:
        out = Path(tmp) / 'build'
        run(sys.executable, '-B', HERE / 'scripts/build-paganidiag.py', '--out', out)
        stubs = Path(tmp) / 'tbd'
        stubs.mkdir()
        for image in DSC_IMAGES:
            run('ipsw', 'dyld', 'tbd', DSC, image, '-o', stubs)
        text = ''.join(path.read_text(errors='replace') for path in stubs.glob('*.tbd'))
        tokens = set(re.findall(r"'?(_[A-Za-z0-9_$.@]+|\$[A-Za-z0-9_$.]+)'?", text))
        classes = set(re.findall(r'[ \[,]([A-Z][A-Za-z0-9_]+)[,\s\]]', text))
        symbols = imported(out / 'paganidiag')
        missing = [symbol for symbol in symbols if symbol not in tokens and not
                   (symbol.startswith(('_OBJC_CLASS_$_', '_OBJC_METACLASS_$_', '_OBJC_EHTYPE_$_')) and symbol.split('$_')[1] in classes)]
        if not symbols or missing:
            fail(f'{len(missing)} of {len(symbols)} imports are not in the shared cache of 21H461: ' + ' '.join(missing[:20]))
        print(f'PASS imports: all {len(symbols)} imports of the daemon are exported by the shared cache of 21H461')


def check_flow():
    with tempfile.TemporaryDirectory(prefix='paganidiag-flow-') as tmp:
        root = Path(tmp)
        sdk = run('xcrun', '--sdk', 'macosx', '--show-sdk-path').stdout.strip()
        binary = root / 'paganidiag'
        run('xcrun', 'clang++', '-target', 'arm64-apple-macos14.0', '-isysroot', sdk, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-fobjc-arc',
            '-I', HERE, HERE / 'DiagCore.cpp', '-x', 'objective-c++', HERE / 'PaganiDiag.mm', '-framework', 'Foundation', '-framework',
            'CoreFoundation', '-framework', 'OSLog', '-o', binary)
        (root / 'conf').write_text('rounds=1,2,3\ngap_ms=100\nskip=kc\n')
        result = subprocess.run([str(binary), '--print'], capture_output=True, text=True, timeout=180,
                                env=dict(os.environ, PAGANIDIAG_CONFIG=str(root / 'conf')))
        lines = result.stdout.splitlines()
        if result.returncode or not lines:
            fail(f'--print exited {result.returncode}: {result.stderr[-400:]}')
        summaries = [line for line in lines if re.match(r'diag: r\d sum @\d+ ', line)]
        details = [line for line in lines if re.match(r'diag: r\d@\d+ ', line)]
        problems = []
        if not lines[0].startswith('diag: start v2 pid ') or not lines[-1].startswith('diag: end r3 p0 '):
            problems.append('start or end')
        if len(summaries) != 3:
            problems.append(f'{len(summaries)} summaries')
        if any(len(line) > 6 + 170 for line in lines):
            problems.append('a line too long')
        if any(' kc -/-/- ' not in line for line in summaries) or any(' kc add ' in line for line in lines):
            problems.append('skip=kc was not honoured')
        if any(not re.search(r' wk \d+/\d+$', line) for line in summaries):
            problems.append('a summary without the census')
        text = '\n'.join(details)
        for needed in (' pwr n', ' hid ff00/3:', ' io in:', ' mg bat', ' net ', ' time ', ' wk n', ' wkv cap '):
            if needed not in text:
                problems.append(f'no{needed}line')
        census = re.search(r' wk n(\d+) fs (\d+)/(\d+) reg \d+/(\d+) ok \d+/(\d+) ep \d+/(\d+) pf (\S+)', text)
        if not census or re.search(r' wkv cap 4001 fg \S+/0 fid [01] chr \d+ ext \d+ wid \d+', text) is None:
            problems.append('the census lines')
        elif int(census.group(2)) or int(census.group(1)) != int(census.group(3)):
            problems.append('a bundle counted in a view on a Mac')
        if not any(line.startswith('diag: r1@') for line in details) or sum(line.startswith('diag: r1@') for line in details) < 8:
            problems.append('round 1 lacks its details')
        if problems:
            fail('; '.join(problems) + '\n' + result.stdout[-2000:])
        print(f'PASS flow: on this Mac the daemon wrote {len(lines)} lines over 3 rounds (details {len(details)}); ' + summaries[0][6:150])


def check_policy():
    result = subprocess.run([sys.executable, '-B', str(HERE / 'Test/check-exec-policy.py')], capture_output=True, text=True)
    if result.returncode:
        fail(result.stderr[-800:] + result.stdout[-800:])
    print(('PASS policy: ' if result.stdout.strip().splitlines()[-1].startswith('PASS') else '') + result.stdout.strip().splitlines()[-1])


def check_build():
    with tempfile.TemporaryDirectory(prefix='paganidiag-build-') as tmp:
        out = Path(tmp)
        run(sys.executable, '-B', HERE / 'scripts/build-paganidiag.py', '--out', out)
        daemon = out / 'paganidiag'
        run('codesign', '--verify', '--strict', '--verbose=2', daemon)
        detail = subprocess.run(['codesign', '--display', '--verbose=4', str(daemon)], capture_output=True, text=True).stderr
        facts = dict(re.findall(r'^(CDHash|Page size|Identifier|Signature|Format)=(.*)$', detail, re.M))
        if facts.get('Page size') != '4096' or facts.get('Identifier') != 'org.unixnu.pagani.diag' or facts.get('Signature') != 'adhoc' \
                or facts.get('Format') != 'Mach-O thin (arm64)':
            fail('signature: ' + json.dumps(facts))
        signed = subprocess.run(['codesign', '-d', '--entitlements', ':-', str(daemon)], capture_output=True, check=True).stdout
        entitlements = plistlib.loads(signed) if signed else {}
        if entitlements != plistlib.loads((HERE / 'paganidiag.entitlements').read_bytes()):
            fail('signed entitlements: ' + json.dumps(entitlements))
        trust = json.loads((out / 'trust.json').read_text())
        if trust['cdhash'] != facts['CDHash']:
            fail('trust.json does not name the daemon')
        if f'#define PAGANI_DIAG_DAEMON          "{trust["path"]}"' not in (HERE / 'kernel/pagani_diag.h').read_text():
            fail(f'{trust["path"]} is not the path the kernel starts')
        info = run('xcrun', 'vtool', '-show-build', daemon).stdout
        if not re.search(r'platform IOS', info) or not re.search(r'minos 17\.0', info):
            fail('build version: ' + info)
        print(f'PASS build: {trust["executable_bytes"]} bytes, CDHash {trust["cdhash"]}, 4 KiB pages, arm64 iOS 17.0, '
              f'entitlements {sorted(entitlements)}')


def check_trust():
    if not APPS_TRUST.is_file():
        raise Skip(f'no {APPS_TRUST}')
    with tempfile.TemporaryDirectory(prefix='paganidiag-trust-') as tmp:
        out = Path(tmp) / 'build'
        run(sys.executable, '-B', HERE / 'scripts/build-paganidiag.py', '--out', out)
        bundle = Path(tmp) / 'trust.bundle'
        run(sys.executable, '-B', HERE / 'scripts/make-diag-trust.py', APPS_TRUST, bundle, out)
        report = json.loads(bundle.with_suffix('.json').read_text())
        cdhash = json.loads((out / 'trust.json').read_text())['cdhash']
        if set(report['owner_hashes']) != set(report['base_hashes']) | {cdhash} or not report['apple_modules_unchanged']:
            fail('owner entries: ' + json.dumps(report))
        if bytes.fromhex(cdhash) not in bundle.read_bytes():
            fail('the bundle lacks the CDHash')
        drop = report['base_hashes'][0]
        dropped = Path(tmp) / 'dropped.bundle'
        run(sys.executable, '-B', HERE / 'scripts/make-diag-trust.py', '--drop', drop, APPS_TRUST, dropped, out)
        after = json.loads(dropped.with_suffix('.json').read_text())
        if drop in after['owner_hashes'] or after['dropped'] != [drop]:
            fail('--drop kept the hash')
        again = subprocess.run([sys.executable, '-B', str(HERE / 'scripts/make-diag-trust.py'), str(APPS_TRUST), str(bundle), str(out)],
                               capture_output=True, text=True)
        if again.returncode == 0 or 'refuse to overwrite' not in again.stderr:
            fail('an existing output was not refused')
        (out / 'trust.json').write_text(json.dumps({'cdhash': '0' * 40}))
        stale = subprocess.run([sys.executable, '-B', str(HERE / 'scripts/make-diag-trust.py'), str(APPS_TRUST),
                                str(Path(tmp) / 'stale.bundle'), str(out)], capture_output=True, text=True)
        if stale.returncode == 0 or 'does not carry the CDHash' not in stale.stderr:
            fail('a daemon that does not match trust.json was not refused')
        print(f'PASS trust: {len(report["base_hashes"])} owner entries + {cdhash}, Apple modules unchanged, stale and existing refused')


def patched_copy(diff, original, directory):
    """A copy of original that carries the integration diff (patch -p1 relative to TOP, after a dry run against the real file), or the
    file as it is when the diff has gone into it already: patch takes a merged diff for a reversed one and would undo it in the copy."""
    if not original.is_file():
        raise Skip(f'no {original}')
    tree = Path(directory)
    target = tree / original.relative_to(TOP)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(original, target)
    merged = subprocess.run(['patch', '-p1', '--dry-run', '--quiet', '--force', '--reverse', '-i', str(diff)], cwd=TOP, capture_output=True,
                            stdin=subprocess.DEVNULL).returncode == 0
    if not merged:
        run('patch', '-p1', '--dry-run', '--quiet', '--force', '--forward', '-i', diff, cwd=TOP, stdin=subprocess.DEVNULL)
        run('patch', '-p1', '--quiet', '--force', '--forward', '-i', diff, cwd=tree, stdin=subprocess.DEVNULL)
    return target


def check_replace():
    if not FINAL_SEED.is_file() or not FINAL_TRUST.is_file():
        raise Skip(f'no {FINAL_SEED}')
    with tempfile.TemporaryDirectory(prefix='paganidiag-replace-', dir=SCRATCH) as tmp:
        root = Path(tmp)
        out = root / 'build'
        run(sys.executable, '-B', HERE / 'scripts/build-paganidiag.py', '--out', out)
        (out / 'paganidiag.conf').write_text('rounds=60,180\n')
        seed = root / 'seed/s4.dbrdisk'
        run(sys.executable, '-B', HERE / 'scripts/replace-diag-in-seed.py', '--build', out, '--trust', FINAL_TRUST, FINAL_SEED, seed)
        checked = run(sys.executable, '-B', HERE / 'Test/check-seed.py', seed, out).stdout
        report = json.loads(seed.with_suffix('.json').read_text())
        cdhash = json.loads((out / 'trust.json').read_text())['cdhash']
        bundle = seed.with_name('s4-owner-trust.bundle')
        base = json.loads(FINAL_SEED.with_suffix('.json').read_text())
        if report['diag_daemon']['cdhash'] != cdhash or bytes.fromhex(cdhash) not in bundle.read_bytes():
            fail('trust: ' + json.dumps(report['diag_daemon']))
        plugin = base.get('plugin_cdhash') or report.get('plugin_cdhash')
        if not set(report['owner_trust']['base_hashes']) - set(report['owner_trust']['dropped']) <= set(report['owner_trust']['owner_hashes']):
            fail('an owner entry was lost')
        if plugin and plugin not in report['owner_trust']['owner_hashes']:
            fail(f'the Metal plug-in {plugin} left the trust bundle')
        again = subprocess.run([sys.executable, '-B', str(HERE / 'scripts/replace-diag-in-seed.py'), '--build', str(out), '--trust', str(FINAL_TRUST),
                                str(FINAL_SEED), str(seed)], capture_output=True, text=True)
        if again.returncode == 0 or 'Refuse to overwrite' not in again.stderr:
            fail('an existing output was not refused')
        # Twice over: the daemon of the first run is replaced, its hash goes.
        (out / 'paganidiag.conf').unlink()
        second = root / 'seed/s5.dbrdisk'
        run(sys.executable, '-B', HERE / 'scripts/replace-diag-in-seed.py', '--build', out, '--trust', bundle, seed, second)
        run(sys.executable, '-B', HERE / 'Test/check-seed.py', second, out)
        # A launch policy that disables the job the daemon replaces is refused, on a stand-in for the mounted seed.
        sys.path.insert(0, str(HERE / 'scripts'))
        import importlib.util
        spec = importlib.util.spec_from_file_location('replace_diag', HERE / 'scripts/replace-diag-in-seed.py')
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        mount = root / 'mount'
        (mount / 'mobile').mkdir(parents=True)
        (mount / 'ngpu').mkdir()
        (mount / 'db/com.apple.xpc.launchd').mkdir(parents=True)
        (mount / tool.LAUNCH_POLICY).write_bytes(plistlib.dumps({'com.apple.DumpPanic': True}))
        try:
            tool.stage(mount, out, cdhash)
            fail('a seed that disables com.apple.DumpPanic was not refused')
        except SystemExit as refused:
            if 'com.apple.DumpPanic' not in str(refused) or 'FAIL' in str(refused):
                raise
        print(f'PASS replace: {FINAL_SEED.name} takes the daemon {cdhash}, payload headroom {report["payload"]["headroom"]} bytes, '
              f'{len(report["owner_trust"]["owner_hashes"])} owner entries; rerun replaces it; existing output refused; ' + checked.strip().splitlines()[-1])


def check_seed():
    if not BASE_SEED.is_file():
        raise Skip(f'no phone seed {BASE_SEED}')
    with tempfile.TemporaryDirectory(prefix='paganidiag-seed-', dir=SCRATCH) as tmp:
        script = patched_copy(HERE / 'integration/make-phone-seed.diff', TOP / '工具/make-phone-seed.py', tmp)
        out = Path(tmp) / 'build'
        run(sys.executable, '-B', HERE / 'scripts/build-paganidiag.py', '--out', out)
        seed = Path(tmp) / 'seed/diag.dbrdisk'
        run(sys.executable, '-B', script, '--disable-jobs', '', '--diag-daemon', out, BASE_SEED, seed)
        report = json.loads(seed.with_suffix('.json').read_text())
        if report['diag_daemon']['cdhash'] != json.loads((out / 'trust.json').read_text())['cdhash']:
            fail('seed report: ' + json.dumps(report['diag_daemon']))
        checked = run(sys.executable, '-B', HERE / 'Test/check-seed.py', seed, out).stdout
        refused = subprocess.run([sys.executable, '-B', str(script), '--disable-jobs', 'com.apple.DumpPanic', '--diag-daemon', str(out),
                                  str(BASE_SEED), str(Path(tmp) / 'seed/refused.dbrdisk')], capture_output=True, text=True)
        if refused.returncode == 0 or 'disables com.apple.DumpPanic' not in refused.stderr:
            fail('a seed whose launch policy disables com.apple.DumpPanic was not refused: ' + refused.stderr[-400:])
        print('PASS seed: diff applies or is merged; ' + checked.strip().splitlines()[-1] + '; DumpPanic-disabled seed refused')


def check_layout():
    test = TOP / '工具/test-phone-island-layout.py'
    if not test.is_file():
        raise Skip(f'no {test}')
    with tempfile.TemporaryDirectory(prefix='paganidiag-layout-') as tmp:
        root = Path(tmp)
        script = patched_copy(HERE / 'integration/make-a830-layout.diff', TOP / '工具/make-a830-layout.sh', root)
        for name in ('make-a830-images.sh', 'test-phone-island-layout.py'):
            shutil.copyfile(TOP / '工具' / name, root / '工具' / name)
        for name in ('UniXNU-Pagani_SM8750-gpu', 'UniXNU', '一加13T原厂备份'):
            (root / name).symlink_to(TOP / name)
        result = subprocess.run([sys.executable, '-B', str(root / '工具/test-phone-island-layout.py')], capture_output=True, text=True)
        if result.returncode:
            fail('the layout test fails with the diff:\n' + result.stderr[-3000:])
        tests = re.search(r'Ran (\d+) tests', result.stderr)
        sys.path.insert(0, str(root / '工具'))
        import importlib
        layout_test = importlib.import_module('test-phone-island-layout')
        build = root / 'build'
        run(sys.executable, '-B', HERE / 'scripts/build-paganidiag.py', '--out', build)
        bundle = root / 'diag.bundle'
        if not APPS_TRUST.is_file():
            raise Skip(f'no {APPS_TRUST}')
        run(sys.executable, '-B', HERE / 'scripts/make-diag-trust.py', APPS_TRUST, bundle, build)
        box = layout_test.Sandbox()
        try:
            result, calls, seeds = box.run('phone-island-apps', A830_DIAG=str(build), A830_OWNER_TRUST=str(bundle))
            if result.returncode or not seeds or seeds[0][seeds[0].index('--diag-daemon') + 1] != str(build):
                fail('A830_DIAG did not reach make-phone-seed.py: ' + result.stderr[-800:] + json.dumps(seeds))
            if not calls or str(bundle) not in calls[0]['argv']:
                fail('the payload did not take the diag trust bundle')
            arguments = layout_test.kernel_args(calls[0]['argv'])
            if 'pagani-diag=1' not in arguments or any(arg.startswith('sheng-no-lc') for arg in arguments):
                fail('a diag image should have pagani-diag=1 and no sheng-no-lc: ' + json.dumps(arguments))
            result, calls, seeds = box.run('phone-island-apps', A830_DIAG=str(build), A830_OWNER_TRUST=str(APPS_TRUST))
            if result.returncode == 0 or 'PaganiDiag' not in result.stderr or calls:
                fail('an owner trust bundle without the daemon was not refused')
            result, calls, seeds = box.run('phone-island-apps')
            if result.returncode or any('--diag-daemon' in seed for seed in seeds):
                fail('without A830_DIAG the seed changed')
            if not calls or any(arg.startswith(('sheng-no-lc', 'pagani-diag')) for arg in layout_test.kernel_args(calls[0]['argv'])):
                fail('without A830_DIAG the image changed')
            # A prebuilt seed that carries the daemon: A830_DIAG=1 adds the argument, its .json names the CDHash.
            prebuilt = root / 'prebuilt.dbrdisk'
            prebuilt.write_bytes(b'')
            cdhash = json.loads((build / 'trust.json').read_text())['cdhash']
            prebuilt.with_suffix('.json').write_text(json.dumps({'diag_daemon': {'cdhash': cdhash}}))
            result, calls, seeds = box.run('phone-island-apps', A830_DIAG='1', A830_PREBUILT_SEED=str(prebuilt), A830_OWNER_TRUST=str(bundle))
            if result.returncode or not calls or 'pagani-diag=1' not in layout_test.kernel_args(calls[0]['argv']):
                fail('A830_DIAG=1 with a prebuilt seed: ' + result.stderr[-600:])
            result, calls, seeds = box.run('phone-island-apps', A830_DIAG='1', A830_PREBUILT_SEED=str(prebuilt), A830_OWNER_TRUST=str(APPS_TRUST))
            if result.returncode == 0 or 'PaganiDiag' not in result.stderr:
                fail('a prebuilt seed whose bundle lacks the daemon was not refused')
            result, calls, seeds = box.run('phone-island-apps', A830_DIAG=str(build), A830_PREBUILT_SEED=str(prebuilt), A830_OWNER_TRUST=str(bundle))
            if result.returncode == 0 or 'A830_DIAG=1' not in result.stderr:
                fail('A830_DIAG=<dir> with a prebuilt seed was not refused')
            result, calls, seeds = box.run('phone-island-apps', A830_DIAG='1')
            if result.returncode == 0 or 'A830_PREBUILT_SEED' not in result.stderr:
                fail('A830_DIAG=1 without a prebuilt seed was not refused')
        finally:
            box.close()
        print(f'PASS layout: diff applies or is merged; the layout test passes with it ({tests.group(1) if tests else "?"} tests); A830_DIAG reaches '
              f'the seed and adds pagani-diag=1 and no sheng-no-lc, A830_DIAG=1 works with a prebuilt seed, a trust bundle without the daemon is refused')


CHECKS = {'patch': check_patch, 'build': check_build, 'imports': check_imports, 'flow': check_flow, 'policy': check_policy, 'trust': check_trust,
          'replace': check_replace, 'seed': check_seed, 'layout': check_layout}


def main():
    names = sys.argv[1:] or list(CHECKS)
    for name in names:
        if name not in CHECKS:
            sys.exit(__doc__)
        try:
            CHECKS[name]()
        except Skip as why:
            print(f'SKIP {name}: {why}')


if __name__ == '__main__':
    main()
