#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Write the XNU patch of PaganiDiag (phone-diag-sysctl.patch): kern.pagani_diag_line / kern.pagani_diag_dropped, the spool, the
kernel's own check lines and the daemon's start.

  make-diag-patch.py [--kern-exec PATH | --tree TREE] [--out PATCH]

The patch applies to bsd/kern/kern_exec.c as the device series leaves it just before this patch. Without --kern-exec that file is
rebuilt from the GPU line's built tree (TREE, default UniXNU/out-gpu/oneplus13t/ipados-17.7.11-21H461/oneplus13t): the pristine
source (git HEAD of tree/.cache/xnu-work) with every patch of xnu-patches.list before phone-diag-sysctl.patch applied, kern_exec.c
only, after checking that each listed patch still has the sha256 xnu-inputs.json recorded for the build. So the patch can be made
again however the built tree was patched (it holds an older PaganiDiag). The patch adds bsd/kern/pagani_diag.h (kernel/pagani_diag.h:
the line, the limits, the spool and the kernel lines, pure C) and, in kern_exec.c, the block kernel/kern_exec-diag.inc before
exec_activate_image plus four calls: after exec_activate_image saved the path (the start), at its end (its result for the redirected
exec), in process_signature at the launch constraint check (pagani_diag_launch_constraints passes the daemon's own image) and before
it acts on its verdict. It belongs in the device series after
phone-keyboard-trace.patch. The script writes the unified diff (a/ b/ prefixes, as the series is applied with git apply), proves it
applies (git apply --check, patch -p1 --dry-run) and, when rebuilt from the tree, that the later patches touching kern_exec.c still
apply on top: those of the build and those the device series (the series file beside this patch) has gained since. Without --out it
prints the patch."""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
DEFAULT_TREE = Path('/Users/xiaochoumao/unixnu-13t/UniXNU/out-gpu/oneplus13t/ipados-17.7.11-21H461/oneplus13t')
PATCH_NAME = 'phone-diag-sysctl.patch'
SOURCE = 'bsd/kern/kern_exec.c'
BLOCK_ANCHOR = '/*\n * exec_activate_image\n *\n'
CALL_ANCHOR = ('\terror = exec_save_path(imgp, imgp->ip_user_fname, imgp->ip_seg, &excpath);\n\tif (error) {\n'
               '\t\tgoto bad_notrans;\n\t}\n\n\t/* Use excpath, which contains the copyin-ed exec path */\n')
CALL = ('\terror = pagani_diag_launch(imgp, p, &excpath);   /* PaganiDiag: the daemon in place of DumpPanic */\n'
        '\tif (error) {\n\t\tgoto bad_notrans;\n\t}\n\n')
END_ANCHOR = ('\tkfree_type(struct nameidata, ndp);\n\n\treturn error;\n}\n\n/*\n * exec_validate_spawnattr_policy\n')
END_CALL = '\tpagani_diag_activated(imgp, p, error);   /* PaganiDiag: the redirected exec\'s result */\n'
SIGNATURE_ANCHOR = ('\t\t\tsignature_failure_reason = os_reason_create(OS_REASON_EXEC, EXEC_EXIT_REASON_MACHINE_THREAD);\n\t\t}\n\t}\n\n'
                    '\tif (0 == error) {\n\t\t/* The process\'s code signature related properties are\n')
SIGNATURE_CALL = ('\tpagani_diag_signature(imgp, p, error, signature_failure_reason);   /* PaganiDiag: the daemon\'s verdict */\n\n')
CONSTRAINT_ANCHOR = '\tif ((error = mac_proc_check_launch_constraints(p, imgp, &launch_constraint_reason)) != 0) {\n'
CONSTRAINT_CALL = ('\t/* PaganiDiag: the daemon\'s own image passes, everything else as ever */\n'
                   '\tif ((error = pagani_diag_launch_constraints(p, imgp, &launch_constraint_reason)) != 0) {\n')


def fail(message):
    raise SystemExit('make-diag-patch: ' + message)


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, **kwargs)


def series_later(listed, at):
    """What comes after ours: the build's list, and what the device series has gained since that build."""
    series = listed[at].parent / 'series'
    if not series.is_file():
        return listed[at + 1:]
    names = [line.strip() for line in series.read_text().splitlines() if line.strip() and not line.startswith('#')]
    if names.count(PATCH_NAME) != 1:
        fail(f'{series} does not list {PATCH_NAME} once')
    gained = [series.parent / name for name in names[names.index(PATCH_NAME) + 1:]]
    return listed[at + 1:] + [path for path in gained if path not in listed]


def series_around(tree):
    """(patches before ours, patches after ours) of the build's xnu-patches.list, each checked against xnu-inputs.json, and the later
    patches of the device series that list does not have yet."""
    listed = [Path(line) for line in (tree / 'xnu-patches.list').read_text().splitlines() if line.strip()]
    recorded = {Path(p['path']): p['sha256'] for p in json.loads((tree / 'xnu-inputs.json').read_text())['patches']}
    names = [p.name for p in listed]
    if names.count(PATCH_NAME) != 1:
        fail(f'{tree}/xnu-patches.list does not list {PATCH_NAME} once')
    for path in listed:
        if path.name == PATCH_NAME:
            continue
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != recorded.get(path):
            fail(f'{path} changed since the build of {tree} (xnu-inputs.json): rebuild the tree or pass --kern-exec')
    at = names.index(PATCH_NAME)
    return listed[:at], series_later(listed, at)


def touches(patch):
    return any(line.rstrip('\n') == f'+++ b/{SOURCE}' for line in patch.read_text(errors='replace').splitlines())


def apply_kern_exec(root, patches, check=False):
    for patch in patches:
        if touches(patch):
            run('git', 'apply', *(['--check'] if check else []), f'--include={SOURCE}', patch, cwd=root)


def rebuilt_base(tree):
    """kern_exec.c as the series leaves it before PaganiDiag, and the later patches that touch it."""
    before, after = series_around(tree)
    work = tree / 'tree/.cache/xnu-work'
    pristine = run('git', '-C', work, 'show', f'HEAD:{SOURCE}').stdout
    with tempfile.TemporaryDirectory(prefix='diag-base-') as tmp:
        root = Path(tmp)
        (root / SOURCE).parent.mkdir(parents=True)
        (root / SOURCE).write_text(pristine)
        run('git', 'init', '-q', root)
        apply_kern_exec(root, before)
        return (root / SOURCE).read_text(), [p for p in after if touches(p)]


def check_one(text, anchor, where):
    if text.count(anchor) != 1:
        fail(f'kern_exec.c does not have one {where}')


def make(original):
    if 'pagani_diag' in original:
        fail('the file already has PaganiDiag')
    for needed in ('\nexec_save_path(struct image_params *imgp, user_addr_t path, int seg, const char **excpath)\n',
                   '\nexec_reset_save_path(struct image_params *imgp)\n', '#include <sys/sysctl.h>\n', '#include <pexpert/pexpert.h>\n',
                   '\nprocess_signature(proc_t p, struct image_params *imgp)\n'):
        if needed not in original:
            fail('kern_exec.c lacks ' + needed.strip())
    if original.index(BLOCK_ANCHOR) < original.index('\nexec_reset_save_path(struct image_params *imgp)\n'):
        fail('exec_activate_image comes before exec_reset_save_path')
    if original.index(SIGNATURE_ANCHOR) < original.index(BLOCK_ANCHOR):
        fail('process_signature comes before exec_activate_image')
    for anchor, where in ((BLOCK_ANCHOR, 'exec_activate_image comment'), (CALL_ANCHOR, 'exec_save_path call of exec_activate_image'),
                          (END_ANCHOR, 'end of exec_activate_image'), (SIGNATURE_ANCHOR, 'verdict of process_signature'),
                          (CONSTRAINT_ANCHOR, 'launch constraint check of process_signature')):
        check_one(original, anchor, where)
    block = (HERE / 'kernel/kern_exec-diag.inc').read_text()
    text = original.replace(BLOCK_ANCHOR, block + BLOCK_ANCHOR, 1)
    head, tail = CALL_ANCHOR.split('\n\t/* Use excpath', 1)
    text = text.replace(CALL_ANCHOR, head + '\n' + CALL + '\t/* Use excpath' + tail, 1)
    end_head = '\tkfree_type(struct nameidata, ndp);\n\n'
    text = text.replace(END_ANCHOR, end_head + END_CALL + END_ANCHOR[len(end_head):], 1)
    signature_head = SIGNATURE_ANCHOR.split('\tif (0 == error) {\n\t\t/* The process')[0]
    text = text.replace(CONSTRAINT_ANCHOR, CONSTRAINT_CALL, 1)
    return text.replace(SIGNATURE_ANCHOR, signature_head + SIGNATURE_CALL + SIGNATURE_ANCHOR[len(signature_head):], 1)


def unified(path, original, changed):
    with tempfile.TemporaryDirectory(prefix='diag-patch-') as tmp:
        root = Path(tmp)
        for side, content in (('a', original), ('b', changed)):
            target = root / side / path
            target.parent.mkdir(parents=True)
            target.write_text(content)
        result = subprocess.run(['diff', '-u', f'a/{path}', f'b/{path}'], cwd=root, capture_output=True, text=True)
        if result.returncode != 1:
            fail('diff failed: ' + result.stderr)
        lines = result.stdout.splitlines(keepends=True)
        lines[0], lines[1] = f'--- a/{path}\n', f'+++ b/{path}\n'
        return ''.join(lines)


def new_file(path, content):
    lines = content.splitlines(keepends=True)
    if not content.endswith('\n'):
        fail(f'{path} must end with a line break')
    return f'--- /dev/null\n+++ b/{path}\n@@ -0,0 +1,{len(lines)} @@\n' + ''.join('+' + line for line in lines)


def make_patch(original):
    changed = make(original)
    header = (HERE / 'kernel/pagani_diag.h').read_text()
    return unified(SOURCE, original, changed) + new_file('bsd/kern/pagani_diag.h', header), changed


def prove(original, patch, changed, later=()):
    with tempfile.TemporaryDirectory(prefix='diag-patch-apply-') as tmp:
        root = Path(tmp)
        target = root / SOURCE
        target.parent.mkdir(parents=True)
        target.write_text(original)
        (root / 'diag.patch').write_text(patch)
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        subprocess.run(['git', 'apply', '--check', 'diag.patch'], cwd=root, check=True)
        subprocess.run(['patch', '-p1', '--dry-run', '--quiet', '-i', 'diag.patch'], cwd=root, check=True)
        subprocess.run(['git', 'apply', 'diag.patch'], cwd=root, check=True)
        if target.read_text() != changed or (root / 'bsd/kern/pagani_diag.h').read_text() != (HERE / 'kernel/pagani_diag.h').read_text():
            fail('the applied patch differs from the intended files')
        for patch_path in later:
            result = subprocess.run(['git', 'apply', f'--include={SOURCE}', str(patch_path)], cwd=root, capture_output=True, text=True)
            if result.returncode:
                fail(f'{patch_path.name} no longer applies after the new patch: {result.stderr.strip()[-400:]}')
        return target.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kern-exec', type=Path)
    parser.add_argument('--tree', type=Path, default=DEFAULT_TREE)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--final', type=Path, help='also write kern_exec.c with this patch and the later ones applied (for a compile)')
    parser.add_argument('--base', type=Path, help='also write the kern_exec.c the patch applies to')
    args = parser.parse_args()
    if args.kern_exec:
        original, later = args.kern_exec.read_text(), []
    else:
        original, later = rebuilt_base(args.tree)
    patch, changed = make_patch(original)
    final = prove(original, patch, changed, later)
    if args.base:
        args.base.write_text(original)
    if args.final:
        args.final.write_text(final)
    if args.out:
        args.out.write_text(patch)
        print(f'PASS: {args.out} ({len(patch.splitlines())} lines) applies to kern_exec.c '
              f'{"of " + str(args.kern_exec) if args.kern_exec else "as the series leaves it before " + PATCH_NAME}'
              f'{"; later patches still apply: " + ", ".join(p.name for p in later) if later else ""}')
    else:
        sys.stdout.write(patch)


if __name__ == '__main__':
    main()
