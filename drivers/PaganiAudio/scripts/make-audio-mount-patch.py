#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Write the XNU patch that shows the owner's audio plug-in directory over /System/Library/Audio/Plug-Ins/HAL.

  make-audio-mount-patch.py --kern-exec PATH [--out PATCH]

PATH is bsd/kern/kern_exec.c as the patch series before this patch leaves it (a built tree's .cache/xnu-work copy is only read). The
patch adds native_audio_addon_stage/status after the last addon status variable, and in native_gpu_addon_prepare, before the cryptex
view, one more native_gpu_addon_mount: /private/var/ngpu/audio over /System/Library/Audio/Plug-Ins/HAL at scratch offset 0xe40, only
with boot-arg pagani-audio, followed by one boot_progress_trace U line "overlay audio stage S status E". (UniXNU-sheng's
zzyd-sheng-audio-addon-mount.patch mounts the same directory at 0xa00; on the 13T 0xa00, 0xb00 and 0xd00 are taken.)

The script checks that the audio mount's scratch range (type at +0, mount point at +0x20, source at +0x100) overlaps no other use of the
scratch page in PATH, writes the unified diff (a/ b/ prefixes, as the series is applied with git apply), and proves it applies with
git apply --check and patch -p1 --dry-run to a copy of PATH. Without --out it prints the patch."""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

OFFSET = 0xe40
SOURCE = '/private/var/ngpu/audio'
POINT = '/System/Library/Audio/Plug-Ins/HAL'
VARIABLES = '''/* The OnePlus 13T audio overlay (pagani-audio-addon-mount.patch): stage and status, as for the addons above. */
volatile int native_audio_addon_stage = 0;
volatile int native_audio_addon_status = -1;
'''
MOUNT = '''	/* The OnePlus 13T audio server plug-in (PaganiAudio.driver, UniXNU-Pagani-Drivers drivers/PaganiAudio) beside the system's: the
	 * audio server (audiomxd) loads every driver bundle in /System/Library/Audio/Plug-Ins/HAL. The owner overlay /private/var/ngpu/audio
	 * (drivers/PaganiAudio/scripts/add-pagani-audio-data.py) is shown over the directory like the overlays above, only with boot-arg
	 * pagani-audio (the kernel service the plug-in opens runs only then). Its mount arguments take scratch 0xe40..0xf58, clear of every
	 * other use of the page. Stage 5 status 0: mounted with the system's BuiltinAudioPlugin.driver and the owner's plug-in visible;
	 * stage 1: no source directory (a Data seed without the plug-in). */
	uint32_t audio_enabled = 0;
	if (PE_parse_boot_argn("pagani-audio", &audio_enabled, sizeof(audio_enabled)) && audio_enabled) {
		static const char audio_source[] = "%(source)s", audio_point[] = "%(point)s";
		native_gpu_addon_mount(p, scratch, %(offset)#x, audio_source, sizeof(audio_source), audio_point, sizeof(audio_point),
		    "/System/Library/Audio/Plug-Ins/HAL/BuiltinAudioPlugin.driver", "/System/Library/Audio/Plug-Ins/HAL/PaganiAudio.driver/Info.plist",
		    &native_audio_addon_stage, &native_audio_addon_status);
		if (boot_progress_trace) {
			char line[64];
			snprintf(line, sizeof(line), "overlay audio stage %%d status %%d", native_audio_addon_stage, native_audio_addon_status);
			boot_progress_trace(line);
		}
	}
''' % {'source': SOURCE, 'point': POINT, 'offset': OFFSET}


def fail(message):
    raise SystemExit('make-audio-mount-patch: ' + message)


def scratch_ranges(text):
    """[(start, end, what)] of the scratch page uses in kern_exec.c: every native_gpu_addon_mount (type +0, point +0x20, source
    +0x100 with its literal's length), and the fixed argument areas of the other users of the page."""
    literals = {name: ''.join(re.findall(r'"([^"]*)"', value))
                for name, value in re.findall(r'(\w+)\[\] = ((?:"[^"]*"\s*)+)', text)}
    ranges = []
    for offset, source, point in re.findall(r'native_gpu_addon_mount\(p, scratch, (0x[0-9a-f]+), (\w+), sizeof\(\w+\), (\w+),', text):
        start = int(offset, 16)
        if source not in literals or point not in literals:
            fail(f'cannot size the mount at {offset}: {source} or {point} is not a literal')
        if len(literals[point]) + 1 > 0xe0:
            fail(f'the mount point at {offset} overruns its source area')
        ranges.append((start, start + 0x100 + len(literals[source]) + 1, f'mount {offset} {literals[source]}'))
    # The other users: the Data and SystemOS mounts (type, path, device path, 0x150 bytes of payload), the environment (0x400-0x6ff),
    # the cryptex binds (0xa00 type, 0xa20 point, 0xb00 source), the staged-apps mkdir (0xa00), parents/unlink paths (0xc00, 192).
    for start, end, what in ((0x100, 0x350, 'Data/SystemOS mount'), (0x400, 0x700, 'launchd environment'),
                             (0xa00, 0xb40, 'cryptex binds and staged apps'), (0xc00, 0xcc0, 'mkdir/unlink paths')):
        if re.search(r'scratch \+ %#x\b' % start, text):
            ranges.append((start, end, what))
    return ranges


def make(original):
    if 'native_audio_addon_stage' in original:
        fail('the file already has the audio overlay')
    statuses = list(re.finditer(r'^volatile int native_gpu_addon\d*_status = -1;\n', original, re.M))
    if not statuses:
        fail('no addon status variable to follow')
    anchor = '\tnative_cryptex_os_view(p, scratch);\n'
    if original.count(anchor) != 1 or 'extern void boot_progress_trace(const char *text)' not in original:
        fail('native_gpu_addon_prepare is not the expected one (cryptex view call and boot_progress_trace declaration)')
    if 'PE_parse_boot_argn("sheng-gpu-addon"' not in original:
        fail('native_gpu_addon_prepare does not read sheng-gpu-addon')
    end = OFFSET + 0x100 + len(SOURCE) + 1
    for start, stop, what in scratch_ranges(original):
        if start < end and OFFSET < stop:
            fail(f'scratch {OFFSET:#x}..{end:#x} overlaps {what} ({start:#x}..{stop:#x})')
    if end > 0x1000 or len(POINT) + 1 > 0xe0:
        fail('the audio mount arguments do not fit the page')
    at = statuses[-1].end()
    text = original[:at] + VARIABLES + original[at:]
    return text.replace(anchor, MOUNT + anchor, 1)


def unified(original, changed):
    with tempfile.TemporaryDirectory(prefix='audio-mount-patch-') as tmp:
        root = Path(tmp)
        for side, content in (('a', original), ('b', changed)):
            path = root / side / 'bsd/kern/kern_exec.c'
            path.parent.mkdir(parents=True)
            path.write_text(content)
        result = subprocess.run(['diff', '-u', 'a/bsd/kern/kern_exec.c', 'b/bsd/kern/kern_exec.c'], cwd=root, capture_output=True,
                                text=True)
        if result.returncode != 1:
            fail('diff failed: ' + result.stderr)
        lines = result.stdout.splitlines(keepends=True)
        # Plain headers without time stamps, as the device series' other patches.
        lines[0], lines[1] = '--- a/bsd/kern/kern_exec.c\n', '+++ b/bsd/kern/kern_exec.c\n'
        return ''.join(lines)


def prove(original, patch):
    with tempfile.TemporaryDirectory(prefix='audio-mount-apply-') as tmp:
        root = Path(tmp)
        target = root / 'bsd/kern/kern_exec.c'
        target.parent.mkdir(parents=True)
        target.write_text(original)
        (root / 'audio.patch').write_text(patch)
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        subprocess.run(['git', 'apply', '--check', 'audio.patch'], cwd=root, check=True)
        subprocess.run(['patch', '-p1', '--dry-run', '--quiet', '-i', 'audio.patch'], cwd=root, check=True)
        subprocess.run(['git', 'apply', 'audio.patch'], cwd=root, check=True)
        applied = target.read_text()
        for needle in ('native_gpu_addon_mount(p, scratch, 0xe40,', 'volatile int native_audio_addon_status = -1;',
                       '"overlay audio stage %d status %d"'):
            if needle not in applied:
                fail('the applied patch lacks ' + needle)
        return applied


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kern-exec', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    original = args.kern_exec.read_text()
    patch = unified(original, make(original))
    prove(original, patch)
    if args.out:
        args.out.write_text(patch)
        print(f'PASS: {args.out} applies to {args.kern_exec} (git apply --check, patch -p1); scratch {OFFSET:#x}..'
              f'{OFFSET + 0x100 + len(SOURCE) + 1:#x} is free')
    else:
        sys.stdout.write(patch)


if __name__ == '__main__':
    main()
