#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Host tests of PaganiAdsp (the OnePlus 13T ADSP bring-up), all with -Wall -Wextra -Werror and ASan/UBSan:

  cores     MDT parsing, SMEM partitions/items and the 13T access guard, SMP2P, GLINK (ported from UniXNU-sheng)
  qrtr      the IPC router, the service locator answer bytes and the SM8750 domain table
  clients   the IPC router client ports (PaganiAdsp.h) and the router hooks under them, the locator's crash report answer
  firmware  modem_a as FAT16 (4096-byte sectors) and FAT32 (512-byte sectors) volumes made by mtools, holding synthetic
            NAME.mdt/NAME.bNN images; the carveout and metadata each must give are computed here, independently
  platform  the PAS/QMP/RPMh adapters over the Adreno830 SM8750 platform layer (its own sources, MIT), against models
  core      the whole bring-up and run time against a simulated SoC and DSP (SMEM with the 13T's unreadable pages made
            inaccessible, IPCC, the DSP's SMP2P, GLINK, IPC router and locator queries), every failure path and timeout
  partition NativeUfs PartitionTable.h: GPT lookup on every LUN (synthetic; with --gpt-dir also the PKX110 GPT dumps)
  quiet     the kernel glue holds the UFS log's writer with its own bit of pagani_panel_probe_quiet (the word is shared bit by bit)

Needs cc, mformat/mmd/mcopy/mdel (mtools). --adreno830 points at the Adreno830 checkout (default: ../../../../Adreno830 from here).
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DRIVER = HERE.parent
DRIVERS = DRIVER.parent
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
CORES = ['QcomSmem.c', 'QcomSmp2p.c', 'QcomGlink.c', 'QcomQrtr.c', 'QcomMdt.c', 'FatVolume.c']
PORTABLE = CORES + ['AdspFirmware.c', 'AdspCore.c', 'AdspQrtr.c']


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def build(out, name, sources, extra=()):
    exe = out / name
    run(['cc', *CFLAGS, *extra, *map(str, sources), '-o', str(exe)])
    return exe


def check_output(exe, *args):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    text = (result.stdout + result.stderr).strip()
    print(text)
    if result.returncode or not text.splitlines()[-1].startswith('PASS'):
        sys.exit(f'FAIL: {exe.name} {" ".join(map(str, args))}')
    return text


# ---- synthetic firmware: an ELF32 the way the PAS images are made ----
PT_NULL, PT_LOAD = 0, 1
HASH_FLAGS, HEADER_FLAGS, RELOC = 0x02200000, 0x07000000, 0x08000000


def align(v, a):
    return (v + a - 1) // a * a


def make_image(rng, loads, reloc=True, hash_size=0x2c0, hash_flags=HASH_FLAGS, hash_last=False):
    """loads: (paddr, filesz, memsz). Returns the program headers, the pieces by header index and the joined image. hash_last puts the
    hash segment's header last, as the PKX110's adsp (b54) and adsp_dtb (b02) images have it."""
    phnum = 2 + len(loads)
    header_size = 52 + 32 * phnum
    hash_phdr = [PT_NULL, 0x1000, 0, hash_size, 0x1000, hash_flags]
    phdrs = [[PT_NULL, 0, 0, header_size, 0, HEADER_FLAGS]] + ([] if hash_last else [hash_phdr])
    offset = align(0x1000 + hash_size, 0x1000)
    for paddr, filesz, memsz in loads:
        phdrs.append([PT_LOAD, offset if filesz else 0, paddr, filesz, memsz, 0x5 | (RELOC if reloc else 0)])
        if filesz:
            offset = align(offset + filesz, 0x1000)
    if hash_last:
        phdrs.append(hash_phdr)
    hash_index = phdrs.index(hash_phdr)
    elf = bytearray(b'\x7fELF\x01\x01\x01' + bytes(9))
    elf += struct.pack('<HHIIIIIHHHHHH', 2, 164, 1, 0, 52, 0, 0, 52, 32, phnum, 0, 0, 0)
    for kind, off, paddr, filesz, memsz, flags in phdrs:
        elf += struct.pack('<8I', kind, off, paddr, paddr, filesz, memsz, flags, 0x1000)
    assert len(elf) == header_size
    pieces = {0: bytes(elf), hash_index: rng.randbytes(hash_size)}
    for i, (kind, off, paddr, filesz, memsz, flags) in enumerate(phdrs):
        if kind == PT_LOAD and filesz:
            pieces[i] = rng.randbytes(filesz)
    joined = bytearray(max(p[1] + p[3] for p in phdrs))
    for i, p in enumerate(phdrs):
        if p[3]:
            joined[p[1]:p[1] + p[3]] = pieces[i]
    return {'phdrs': phdrs, 'pieces': pieces, 'joined': bytes(joined), 'header_size': header_size, 'reloc': reloc, 'hash': hash_index}


def expected_region(image, base, size):
    region = bytearray(b'\xa5' * size)
    loads = [(i, p) for i, p in enumerate(image['phdrs']) if p[0] == PT_LOAD and p[4]]
    low = min(p[2] for _, p in loads)
    for index, (kind, off, paddr, filesz, memsz, flags) in loads:
        at = paddr - low if image['reloc'] else paddr - base
        data = image['joined'][off:off + filesz] if filesz else b''
        region[at:at + memsz] = data + bytes(memsz - filesz)
    return bytes(region)


def metadata(image):
    return image['pieces'][0] + image['pieces'][image['hash']]


def random_loads(rng, count, base=0x80000000, bss_every=7):
    loads, address = [], base
    for i in range(count):
        filesz = 0 if i % bss_every == 3 else rng.randrange(1, 24 * 1024)
        memsz = filesz + (rng.randrange(0, 9000) if i % 5 == 0 else 0) or 0x800
        loads.append((address, filesz, memsz))
        address = align(address + memsz, 0x1000)
    return loads


def firmware_cases(rng):
    """(name, files {name: bytes}, rc, base, size, image or None)"""
    cases = []
    big = make_image(rng, random_loads(rng, 53), hash_last=True)          # 55 program headers, the hash in b54, as on the PKX110
    loads = [p for p in big['phdrs'] if p[0] == PT_LOAD and p[4]]
    span = align(max(p[2] + p[4] for p in loads), 0x1000) - min(p[2] for p in loads)
    files = {f'adsp.b{i:02d}': d for i, d in big['pieces'].items()}
    files['adsp.mdt'] = metadata(big)
    cases.append(('adsp', files, 0, 0x9ee00000, span + 0x10000, big))
    dtb = make_image(rng, [(0x9ed80000, 0x6123, 0x8000)], hash_last=True)   # adsp_dtb: b00 headers, b01 the DTB, b02 the hash
    files = {f'adsp_dtb.b{i:02d}': d for i, d in dtb['pieces'].items()}
    files['adsp_dtb.mdt'] = metadata(dtb)
    cases.append(('adsp_dtb', files, 0, 0x9ed80000, 0x80000, dtb))
    # no .mdt: the headers from .b00, the hash from .b01
    nomdt = make_image(rng, [(0x1000, 0x3000, 0x3000), (0x5000, 0x10, 0x2000)])
    cases.append(('nomdt', {f'nomdt.b{i:02d}': d for i, d in nomdt['pieces'].items()}, 0, 0x9ed80000, 0x80000, nomdt))
    # the hash inside the .mdt at its own offset (not packed)
    hashin = make_image(rng, [(0x1000, 0x1800, 0x2000)])
    files = {f'hashin.b{i:02d}': d for i, d in hashin['pieces'].items() if i >= 2}
    files['hashin.mdt'] = hashin['joined'][:0x1000 + 0x2c0]
    cases.append(('hashin', files, 0, 0x9ed80000, 0x80000, hashin))
    # a non-split image: the segment's bytes are inside the .mdt, no .b02
    inline = make_image(rng, [(0x1000, 0x900, 0x1000)])
    cases.append(('inline', {'inline.mdt': inline['joined']}, 0, 0x9ed80000, 0x80000, inline))
    # not relocatable: placed by address
    fixed = make_image(rng, [(0x9ed81000, 0x777, 0x1000), (0x9ed90000, 0x2000, 0x4000)], reloc=False)
    files = {f'fixed.b{i:02d}': d for i, d in fixed['pieces'].items()}
    files['fixed.mdt'] = metadata(fixed)
    cases.append(('fixed', files, 0, 0x9ed80000, 0x80000, fixed))
    # a long name (VFAT entries)
    lng = make_image(rng, [(0x1000, 0x2345, 0x3000)])
    files = {f'adsp_longname.b{i:02d}': d for i, d in lng['pieces'].items()}
    files['adsp_longname.mdt'] = metadata(lng)
    cases.append(('adsp_longname', files, 0, 0x9ed80000, 0x80000, lng))
    # refusals
    bad = make_image(rng, [(0x1000, 0x2000, 0x2000)])
    files = {f'badsize.b{i:02d}': d for i, d in bad['pieces'].items()}
    files['badsize.mdt'] = metadata(bad)
    files['badsize.b02'] = files['badsize.b02'][:-1]
    cases.append(('badsize', files, -6, 0x9ed80000, 0x80000, None))
    files = {f'missing.b{i:02d}': d for i, d in bad['pieces'].items() if i != 2}
    files['missing.mdt'] = metadata(bad)
    cases.append(('missing', files, -3, 0x9ed80000, 0x80000, None))
    cases.append(('notelf', {'notelf.mdt': rng.randbytes(600)}, -4, 0x9ed80000, 0x80000, None))
    nohash = make_image(rng, [(0x1000, 0x100, 0x100)], hash_flags=0)
    cases.append(('nohash', {'nohash.mdt': metadata(nohash), 'nohash.b02': nohash['pieces'][2]}, -4, 0x9ed80000, 0x80000, None))
    outside = make_image(rng, [(0x1000, 0x100, 0x100), (0x90000, 0x100, 0x100)])
    files = {f'outside.b{i:02d}': d for i, d in outside['pieces'].items()}
    files['outside.mdt'] = metadata(outside)
    cases.append(('outside', files, -7, 0x9ed80000, 0x80000, None))
    cases.append(('huge', {'huge.mdt': bytes(300 * 1024)}, -6, 0x9ed80000, 0x80000, None))
    cases.append(('absent', {}, -3, 0x9ed80000, 0x80000, None))
    return cases


def make_volume(tmp, name, mformat_args, size, cases, rng):
    img = tmp / name
    with open(img, 'wb') as f:
        f.truncate(size)
    run(['mformat', '-i', str(img), *mformat_args, '::'], stdout=subprocess.DEVNULL)
    run(['mmd', '-i', str(img), '::image'], stdout=subprocess.DEVNULL)
    # holes first, so the files written after them are fragmented
    holes = []
    for i in range(8):
        hole = tmp / f'hole{i}'
        hole.write_bytes(rng.randbytes(20000))
        run(['mcopy', '-i', str(img), str(hole), f'::image/hole{i}.bin'])
        holes.append(f'::image/hole{i}.bin')
    for h in holes[::2]:
        run(['mdel', '-i', str(img), h])
    staging = tmp / ('files-' + name)
    staging.mkdir()
    for case in cases:
        for fname, data in case[1].items():
            path = staging / fname
            path.write_bytes(data)
            run(['mcopy', '-i', str(img), str(path), f'::image/{fname}'])
    return img


def write_expectations(directory, cases):
    directory.mkdir(parents=True, exist_ok=True)
    lines = []
    for name, files, rc, base, size, image in cases:
        lines.append(f'{name} {rc} {base:x} {size:x}')
        if image:
            (directory / f'{name}.meta').write_bytes(metadata(image))
            (directory / f'{name}.region').write_bytes(expected_region(image, base, size))
    (directory / 'cases').write_text('\n'.join(lines) + '\n')


def test_firmware(out, tmp):
    exe = build(out, 'firmware', [HERE / 'firmware.c', DRIVER / 'AdspFirmware.c', DRIVER / 'FatVolume.c', DRIVER / 'QcomMdt.c'])
    rng = random.Random(8750)
    cases = firmware_cases(rng)
    write_expectations(tmp / 'firmware-cases', cases)
    layouts = [('fat16-4k.img', ['-M', '4096', '-c', '4', '-T', '32768', '-h', '1', '-s', '32768'], 128 << 20),
               ('fat32-512.img', ['-F', '-c', '1'], 48 << 20)]
    for name, args, size in layouts:
        img = make_volume(tmp, name, args, size, cases, rng)
        check_output(exe, img, tmp / 'firmware-cases')


def platform(args):
    """The Adreno830 SM8750 platform layer's own sources (MIT): what libAdreno830.a links into the kernel."""
    base = args.adreno830 / 'platform/sm8750'
    sources = [base / name for name in ('Sm8750Scm.c', 'Sm8750AossQmp.c', 'Sm8750Rpmh.c')]
    missing = [str(s) for s in sources if not s.is_file()]
    if missing:
        sys.exit('missing Adreno830 platform sources (use --adreno830): ' + ', '.join(missing))
    return sources


def test_core(out, tmp, args):
    exe = build(out, 'core', [HERE / 'core.c'] + [DRIVER / s for s in PORTABLE])
    rng = random.Random(13)
    adsp = make_image(rng, random_loads(rng, 53, base=0x0), hash_last=True)
    dtb = make_image(rng, [(0x0, 0x26e34, 0x27000)], hash_last=True)
    expect = tmp / 'core-expect'
    expect.mkdir()
    files = {}
    for name, image in (('adsp', adsp), ('adsp_dtb', dtb)):
        loads = [p for p in image['phdrs'] if p[0] == PT_LOAD and p[4]]
        span = align(max(p[2] + p[4] for p in loads), 0x1000) - min(p[2] for p in loads)
        (expect / f'{name}.meta').write_bytes(metadata(image))
        (expect / f'{name}.region').write_bytes(expected_region(image, 0, span))
        files.update({f'{name}.b{i:02d}': d for i, d in image['pieces'].items()})
        files[f'{name}.mdt'] = metadata(image)
    img = make_volume(tmp, 'core.img', ['-M', '4096', '-c', '4', '-T', '32768', '-h', '1', '-s', '32768'], 128 << 20,
                      [('core', files, 0, 0, 0, None)], rng)
    check_output(exe, img, expect, *(['-v'] if args.verbose else []))


def test_partition(out, args):
    exe = out / 'PartitionTable'
    run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         str(DRIVERS / 'NativeUfs/tests/PartitionTable.cpp'), '-o', str(exe)])
    check_output(exe)
    if args.gpt_dir:
        check_output(exe, args.gpt_dir)


def test_quiet():
    """The word has other drivers' bits (panel, USB, Bluetooth, Wi-Fi, camera, speaker): a plain store would let go of their holds."""
    glue = (DRIVER / 'PaganiAdsp.cpp').read_text()
    wanted = ('__atomic_fetch_or(&pagani_panel_probe_quiet, PAGANI_ADSP_QUIET_BIT, __ATOMIC_RELEASE)',
              '__atomic_fetch_and(&pagani_panel_probe_quiet, ~uint32_t(PAGANI_ADSP_QUIET_BIT), __ATOMIC_RELEASE)', 'diskFind, diskRead, opAlloc, opFree}, opQuiet,',
              'config.crumb_ms = crumbMs();')
    missing = [text for text in wanted if text not in glue]
    bit = re.search(r'#define PAGANI_ADSP_QUIET_BIT (\d+)u', (DRIVER / 'AdspCore.h').read_text())
    if missing or '__atomic_store_n(&pagani_panel_probe_quiet' in glue or not bit or int(bit.group(1)) != 64:
        sys.exit(f'FAIL quiet: {missing or "the bit of the word"}')
    print('PASS quiet: the glue holds the UFS log\'s writer by its own bit (64), never with a plain store')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--adreno830', type=Path, default=DRIVERS.parents[1] / 'Adreno830')
    parser.add_argument('--gpt-dir', type=Path, help='PKX110 GPT dumps (sdX_主GPT.bin) for the partition lookup test')
    parser.add_argument('--only', action='append', default=[])
    parser.add_argument('-v', '--verbose', action='store_true', help='print the core test\'s log lines')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        out = tmp / 'bin'
        out.mkdir()
        tests = {
            'cores': lambda: check_output(build(out, 'cores', [HERE / 'cores.c'] + [DRIVER / s for s in CORES[:3] + ['QcomMdt.c']])),
            'qrtr': lambda: check_output(build(out, 'qrtr', [HERE / 'qrtr.c'] + [DRIVER / s for s in PORTABLE])),
            'clients': lambda: check_output(build(out, 'qrtr_client', [HERE / 'qrtr_client.c'] + [DRIVER / s for s in PORTABLE])),
            'firmware': lambda: test_firmware(out, tmp),
            'platform': lambda: check_output(build(out, 'platform', [HERE / 'platform.c', DRIVER / 'AdspPlatform.c'] + platform(args),
                                                   ['-I', str(args.adreno830 / 'platform/sm8750')])),
            'core': lambda: test_core(out, tmp, args),
            'partition': lambda: test_partition(out, args),
            'quiet': test_quiet,
        }
        for name, test in tests.items():
            if args.only and name not in args.only:
                continue
            print(f'== {name}')
            test()
    print('PASS: PaganiAdsp host tests')


if __name__ == '__main__':
    main()
