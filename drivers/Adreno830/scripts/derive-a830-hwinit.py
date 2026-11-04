#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate A830HwInit.h, the Gen8 (A830, gen8_0_0/gen8_0_1) hardware-init tables (ARCHITECTURE 2.2, 4.8, 4.11, 4.17).

The tables come from two pinned GPL sources that are read here for their numeric facts only (no text is copied into the
header): the OnePlus 13T KGSL (adreno-gpulist.h, adreno_gen8.c, gen8_reg.h; ARCHITECTURE 0.2 K/) and Linux drm/msm
(a8xx_catalog.c, a8xx_gpu.c; L/). Register names of the Linux tables resolve through the Mesa register export kept in
this repository (docs/inputs/regs/a8xx_regs.txt, MIT); KGSL names through gen8_reg.h.

What is emitted, and from where (ARCHITECTURE 1.3 #8, 4.8, 4.11 step 10, 4.17 P2):
  non-context registers   --variant kgsl (default): KGSL gen8_0_0_nonctxt_regs; --variant linux: Linux a830_nonctxt_regs
  CP protect table        KGSL gen8_0_0_protected_regs (entries 0-46 and 63), encoded start | (end-start)<<18 | noaccess<<31
  GBIF CX table           KGSL gen8_0_0_gbif_cx_regs (the GMU writes it in B6, ARCHITECTURE 4.7.3)
  AO HWCG                 KGSL gen8_ao_hwcg_regs (ARCHITECTURE 4.7.8)
  bicubic weights         KGSL gen8_0_0_bicubic_regs with entry 0 = 0 (the hardware default), = Linux hw_init
  pwrup / ifpc / dyn      Linux a830_pwrup_reglist, a830_ifpc_reglist, a830_dyn_pwrup_reglist (ARCHITECTURE 4.11 step 10)

Every value is checked against both sources: entries that exist in both must agree, and every disagreement must be one
of the differences pinned below (KNOWN); a new upstream difference stops the generator. --check regenerates the header
and compares it byte for byte with the one in the repository (exit 1 on any difference).
"""
import argparse
import hashlib
import re
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path('/private/tmp/claude-501/-Users-xiaochoumao-Library-Application-Support-Claude-scratch-workspaces-'
               '8d57cd08-f137-4208-a8b6-3362ffa35fb5-03638cf4-7c10-4079-a7eb-13992523ab3d-scratch-2026-10-29-ff4716/'
               '5559c563-618d-4813-9848-be3768908d8c/scratchpad')
K = SCRATCH / 'design/smmu-cp/dl_kgsl13t'
L = SCRATCH / 'design/smmu-cp/dl_linux'
SOURCES = {   # name: (default path, pinned sha256)
    'kgsl_gpulist': (K / 'adreno-gpulist.h', '7faff3fd1afc101dd1db984a9ca632ce884e8406fa856d4f60632e281077f3f6'),
    'kgsl_gen8': (K / 'adreno_gen8.c', None),
    'kgsl_regs': (K / 'gen8_reg.h', None),
    'linux_catalog': (L / 'drivers_gpu_drm_msm_adreno_a8xx_catalog.c',
                      'a53ef7a20e0c0c9e765a78b9510efa2039671607b5dde7bbfc82f4521aa74afb'),
    'linux_gpu': (L / 'drivers_gpu_drm_msm_adreno_a8xx_gpu.c', None),
    'mesa_regs': (ROOT / 'docs/inputs/regs/a8xx_regs.txt', None),
}
OUTPUT = ROOT / 'A830HwInit.h'
PIPES = {'PIPE_NONE': 0, 'PIPE_BR': 1, 'PIPE_BV': 2, 'PIPE_LPAC': 3, 'PIPE_AQE0': 4, 'PIPE_AQE1': 5,
         'PIPE_DDE_BR': 6, 'PIPE_DDE_BV': 7}
PIPE_NAMES = {v: k[5:] for k, v in PIPES.items()}

# Pinned disagreements between the two drivers. Anything else that differs is an error.
KNOWN = {
    # non-context table (ARCHITECTURE 1.3 #8): offset -> (kgsl value or None, linux value or None)
    'nonctxt': {
        0x08e06: (None, 0x00002200),        # RB_CCU_DBG_ECO_CNTL: Linux only
        0x00e04: (0x00080000, None),        # UCHE_CACHE_WAYS: KGSL only
        0x0b600: (0x10100000, 0x10000000),  # TPL1_DBG_ECO_CNTL: KGSL also sets bit 20 (alpha-only disable)
        0x0b602: (0x00000724, 0x04000724),  # TPL1_DBG_ECO_CNTL1: Linux also sets bit 26 (bicubic final clamp off)
    },
    # AO clock gating: KGSL gen8_ao_hwcg_regs vs Linux a8xx_set_hwcg (a830 has no gmu_cgc_mode in the catalog)
    'ao_hwcg': {0x23b09: (0x00020000, 0x00000000), 0x23b0a: (0x00010111, 0x00110111), 0x23b0b: (0x00005555, 0x00055555)},
    # GBIF CX: GMUCX_MRC_GBIF_QOS_CTRL is written by KGSL only
    'gbif': {0x1f50b: (0x00000033, None)},
}


def fail(message):
    raise SystemExit('derive-a830-hwinit: ' + message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def strip_comments(text):
    return re.sub(r'/\*.*?\*/', ' ', re.sub(r'//[^\n]*', ' ', text), flags=re.S)


def evaluate(expression):
    """A C integer expression of literals, BIT(), PIPE_*, |, <<, +, -, * and parentheses."""
    text = ' '.join(expression.split())
    while 'BIT(' in text:
        text = re.sub(r'BIT\(([^()]*)\)', r'(1<<(\1))', text)
    for name, value in PIPES.items():
        text = re.sub(r'\b%s\b' % name, str(value), text)
    text = re.sub(r'\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', text)
    if not re.fullmatch(r'[0-9a-fA-Fx()|<>+\-* ]*', text):
        fail(f'cannot evaluate {expression!r} ({text!r})')
    return int(eval(text, {'__builtins__': {}}, {})) & 0xffffffff   # noqa: S307 - checked character set


def array_body(text, name):
    match = re.search(r'\b%s\s*\[\s*\]\s*=\s*\{' % re.escape(name), text)
    if not match:
        fail(f'table {name} not found')
    depth, start = 1, match.end()
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if not depth:
                return text[start:i]
    fail(f'table {name} not terminated')


def entries(body):
    """The brace-enclosed initializers of a table, each split on top-level commas; the { 0 } / { } sentinel ends it."""
    result = []
    for match in re.finditer(r'\{([^{}]*)\}', body):
        fields = [f.strip() for f in match.group(1).split(',') if f.strip()]
        if not fields or fields == ['0']:
            break
        result.append(fields)
    return result


def words(body):
    return [w.strip() for w in body.split(',') if w.strip()]


class Registers:
    def __init__(self, kgsl_regs, mesa_regs):
        self.kgsl = {}
        for match in re.finditer(r'^#define\s+(GEN8_\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*$', kgsl_regs, re.M):
            self.kgsl[match.group(1)] = int(match.group(2), 0)
        self.mesa, self.arrays = {}, {}
        for line in mesa_regs.splitlines():
            array = re.fullmatch(r'\s*(0x[0-9a-fA-F]+)\s+(\w+)\[(\d+) x stride (\d+)\]\s+array\s*', line)
            if array:
                self.arrays[array.group(2)] = (int(array.group(1), 16), int(array.group(3)), int(array.group(4)))
                continue
            single = re.fullmatch(r'\s*(0x[0-9a-fA-F]+)\s+(\w+)\s+reg(?:32|64)\s*', line)
            if single:
                self.mesa[single.group(2)] = int(single.group(1), 16)

    def kgsl_offset(self, expression):
        match = re.fullmatch(r'(GEN8_\w+)(?:\s*\+\s*(\d+))?', expression.strip())
        if not match or match.group(1) not in self.kgsl:
            fail(f'KGSL register {expression!r} not in gen8_reg.h')
        return self.kgsl[match.group(1)] + int(match.group(2) or 0)

    def linux_offset(self, expression):
        match = re.fullmatch(r'REG_A[678]XX_(\w+?)(?:\((\d+)\))?(?:\s*\+\s*(\d+))?', expression.strip())
        if not match:
            fail(f'Linux register {expression!r} not understood')
        name, index, plus = match.group(1), match.group(2), int(match.group(3) or 0)
        if index is not None:
            if name not in self.arrays:
                fail(f'Linux register array {name} not in a8xx_regs.txt')
            base, count, stride = self.arrays[name]
            if int(index) >= count:
                fail(f'{expression}: index beyond {count}')
            return base + stride * int(index) + plus
        if name not in self.mesa:
            fail(f'Linux register {name} not in a8xx_regs.txt')
        return self.mesa[name] + plus

    def name(self, offset):
        """A Mesa name for an offset (for comments), else the KGSL one."""
        for name, value in self.mesa.items():
            if value == offset:
                return name
        for name, (base, count, stride) in self.arrays.items():
            if base <= offset < base + count * stride and (offset - base) % stride == 0:
                return f'{name}[{(offset - base) // stride}]'
        for name, value in self.kgsl.items():
            if value == offset:
                return name[5:]
        return f'0x{offset:05x}'


def load(args):
    texts, digests = {}, {}
    for key, (default, pin) in SOURCES.items():
        path = Path(getattr(args, key) or default)
        if not path.is_file():
            fail(f'{key}: {path} missing (pass --{key.replace("_", "-")})')
        digests[key] = sha(path)
        if pin and digests[key] != pin and not args.allow_unpinned:
            fail(f'{key}: {path} sha256 {digests[key]} is not the pinned {pin} (use --allow-unpinned to compare)')
        texts[key] = path.read_text(errors='replace')
    return texts, digests


def derive(texts):
    regs = Registers(texts['kgsl_regs'], texts['mesa_regs'])
    gpulist, gen8 = strip_comments(texts['kgsl_gpulist']), strip_comments(texts['kgsl_gen8'])
    catalog, gpu = strip_comments(texts['linux_catalog']), strip_comments(texts['linux_gpu'])
    t = {}
    t['nonctxt_kgsl'] = [(regs.kgsl_offset(r), evaluate(v), evaluate(p))
                         for r, v, p in entries(array_body(gpulist, 'gen8_0_0_nonctxt_regs'))]
    t['nonctxt_linux'] = [(regs.linux_offset(r), evaluate(v), evaluate(p))
                          for r, v, p in entries(array_body(catalog, 'a830_nonctxt_regs'))]
    protect = []
    for reg, start, end, noaccess in entries(array_body(gpulist, 'gen8_0_0_protected_regs')):
        index = regs.kgsl_offset(reg) - regs.kgsl['GEN8_CP_PROTECT_REG_GLOBAL']
        start, end, noaccess = evaluate(start), evaluate(end), evaluate(noaccess)
        if not 0 <= index < 64 or end < start:
            fail(f'protect entry {reg} out of range')
        protect.append((index, (start & 0x3ffff) | ((end - start) & 0x1fff) << 18 | (noaccess & 1) << 31))
    t['protect'] = protect
    t['protect_linux'] = []
    for kind, start, length in re.findall(r'A6XX_PROTECT_(RDONLY|NORDWR)\(\s*(\w+)\s*,\s*(\w+)\s*\)',
                                          array_body(catalog, 'a830_protect_regs')):
        value = (int(start, 0) & 0x3ffff) | (int(length, 0) & 0x3fff) << 18 | (1 << 31 if kind == 'NORDWR' else 0)
        t['protect_linux'].append(value)
    t['gbif'] = [(regs.kgsl_offset(r), evaluate(v)) for r, v in entries(array_body(gpulist, 'gen8_0_0_gbif_cx_regs'))]
    t['gbif_linux'] = [(regs.linux_offset(r), evaluate(v)) for r, v in entries(array_body(catalog, 'a840_gbif'))]
    t['ao_hwcg'] = [(regs.kgsl_offset(r), evaluate(v)) for r, v in entries(array_body(gpulist, 'gen8_ao_hwcg_regs'))]
    hwcg = re.search(r'static void a8xx_set_hwcg.*?\n}', gpu, re.S).group(0)
    delay = re.search(r'GMU_CGC_DELAY_CNTL,\s*state \? (0x[0-9a-fA-F]+)', hwcg)
    hyst = re.search(r'GMU_CGC_HYST_CNTL,\s*state \? (0x[0-9a-fA-F]+)', hwcg)
    a830 = re.search(r'ADRENO_CHIP_IDS\(0x44050001\).*?\n\t\}, \{', catalog, re.S)
    if not (delay and hyst and a830):
        fail('Linux a8xx_set_hwcg or the a830 catalog entry not found')
    mode = re.search(r'\.gmu_cgc_mode\s*=\s*(0x[0-9a-fA-F]+)', a830.group(0))
    t['ao_hwcg_linux'] = [(0x23b09, int(mode.group(1), 16) if mode else 0), (0x23b0a, int(delay.group(1), 16)),
                          (0x23b0b, int(hyst.group(1), 16))]
    bicubic = {regs.kgsl_offset(r): evaluate(v) for r, v in entries(array_body(gen8, 'gen8_0_0_bicubic_regs'))}
    base = regs.kgsl['GEN8_TPL1_BICUBIC_WEIGHTS_TABLE_0']
    t['bicubic'] = [bicubic.get(base + i, 0) for i in range(20)]
    if sorted(bicubic) != [base + i for i in range(1, 20)]:
        fail('KGSL gen8_0_0_bicubic_regs is not entries 1..19')
    linux_bicubic = {}
    for index, value in re.findall(r'REG_A8XX_TPL1_BICUBIC_WEIGHTS_TABLE\((\d+)\),\s*(0x[0-9a-fA-F]+|0)\)', gpu):
        linux_bicubic[int(index)] = int(value, 0)
    t['bicubic_linux'] = [linux_bicubic.get(i) for i in range(20)]
    t['pwrup'] = [regs.linux_offset(r) for r in words(array_body(catalog, 'a830_pwrup_reglist_regs'))]
    t['ifpc'] = [regs.linux_offset(r) for r in words(array_body(catalog, 'a830_ifpc_reglist_regs'))]
    t['dyn'] = [(regs.linux_offset(r), evaluate(p)) for r, _, p in entries(array_body(catalog, 'a830_dyn_pwrup_reglist_regs'))]
    t['pwrup_kgsl'] = [regs.kgsl_offset(r) for r in words(array_body(gen8, 'gen8_pwrup_reglist'))]
    t['ifpc_kgsl'] = [regs.kgsl_offset(r) for r in words(array_body(gen8, 'gen8_ifpc_pwrup_reglist'))]
    t['dyn_kgsl'] = [(regs.kgsl_offset(r), evaluate(p)) for r, p in entries(array_body(gen8, 'gen8_0_0_pwrup_extlist'))]
    return regs, t


def cross_check(regs, t):
    """Both drivers must agree except for the pinned differences; returns the notes the header records."""
    notes = []
    kgsl = {o: (v, p) for o, v, p in t['nonctxt_kgsl']}
    linux = {o: (v, p) for o, v, p in t['nonctxt_linux']}
    if len(kgsl) != len(t['nonctxt_kgsl']) or len(linux) != len(t['nonctxt_linux']):
        fail('duplicate register in a non-context table')
    for offset in sorted(set(kgsl) | set(linux)):
        k, li = kgsl.get(offset), linux.get(offset)
        if k and li and k == li:
            continue
        if k and li and k[1] != li[1]:
            fail(f'non-context {regs.name(offset)}: pipes differ (KGSL {k[1]:#x}, Linux {li[1]:#x})')
        got = (k[0] if k else None, li[0] if li else None)
        if KNOWN['nonctxt'].get(offset) != got:
            fail(f'non-context {regs.name(offset)} differs: KGSL {got[0]}, Linux {got[1]} (not a pinned difference)')
        notes.append(f'{regs.name(offset)} ({offset:#07x}): KGSL ' + (f'{got[0]:#010x}' if got[0] is not None else 'absent') +
                     ', Linux ' + (f'{got[1]:#010x}' if got[1] is not None else 'absent'))
    if set(KNOWN['nonctxt']) - (set(kgsl) | set(linux)):
        fail('a pinned non-context difference no longer exists in either source')
    # Protect: KGSL index i (0..46) is Linux entry i; KGSL index 63 is the last Linux entry.
    protect = dict(t['protect'])
    if sorted(protect) != list(range(47)) + [63] or len(t['protect_linux']) != 48:
        fail(f'protect layout changed: KGSL {sorted(protect)}, Linux {len(t["protect_linux"])} entries')
    for i in range(47):
        if protect[i] != t['protect_linux'][i]:
            fail(f'protect[{i}] differs: KGSL {protect[i]:#010x}, Linux {t["protect_linux"][i]:#010x}')
    if protect[63] != t['protect_linux'][47]:
        fail(f'protect last span differs: KGSL[63] {protect[63]:#010x}, Linux[47] {t["protect_linux"][47]:#010x}')
    # GBIF CX
    linux_gbif = dict(t['gbif_linux'])
    for offset, value in t['gbif']:
        if offset in linux_gbif:
            if linux_gbif[offset] != value:
                fail(f'GBIF {regs.name(offset)} differs')
        elif KNOWN['gbif'].get(offset) != (value, None):
            fail(f'GBIF {regs.name(offset)} only in KGSL and not pinned')
    if set(linux_gbif) - {o for o, _ in t['gbif']}:
        fail('Linux GBIF table has registers KGSL does not write')
    # AO HWCG
    linux_hwcg = dict(t['ao_hwcg_linux'])
    for offset, value in t['ao_hwcg']:
        if linux_hwcg.get(offset) != value and KNOWN['ao_hwcg'].get(offset) != (value, linux_hwcg.get(offset)):
            fail(f'AO HWCG {regs.name(offset)}: KGSL {value:#x}, Linux {linux_hwcg.get(offset)} (not pinned)')
        if linux_hwcg.get(offset) != value:
            notes.append(f'AO HWCG {regs.name(offset)}: KGSL {value:#010x} (used), Linux {linux_hwcg[offset]:#010x}')
    # Bicubic weights: identical
    if t['bicubic'] != t['bicubic_linux']:
        fail(f'bicubic weights differ: KGSL {t["bicubic"]}, Linux {t["bicubic_linux"]}')
    # Power-up lists: Linux is used; record the set differences with KGSL (informational; IFPC only, ARCHITECTURE 4.17 P2)
    for label, ours, theirs in (('pwrup', t['pwrup'], t['pwrup_kgsl']), ('ifpc', t['ifpc'], t['ifpc_kgsl'])):
        if len(set(ours)) != len(ours):
            fail(f'{label} list has duplicates')
        only_linux = sorted(set(ours) - set(theirs))
        only_kgsl = sorted(set(theirs) - set(ours))
        if only_linux or only_kgsl:
            notes.append(f'{label} list: Linux only ' + (', '.join(regs.name(o) for o in only_linux) or '-') +
                         '; KGSL only ' + (', '.join(regs.name(o) for o in only_kgsl) or '-'))
    dyn_linux, dyn_kgsl = dict(t['dyn']), dict(t['dyn_kgsl'])
    only_linux = sorted(set(dyn_linux) - set(dyn_kgsl))
    only_kgsl = sorted(set(dyn_kgsl) - set(dyn_linux))
    differ = sorted(o for o in set(dyn_linux) & set(dyn_kgsl) if dyn_linux[o] != dyn_kgsl[o])
    if only_linux or only_kgsl or differ:
        notes.append('dyn list: Linux only ' + (', '.join(regs.name(o) for o in only_linux) or '-') + '; KGSL only ' +
                     (', '.join(regs.name(o) for o in only_kgsl) or '-') + '; pipes differ ' +
                     (', '.join(regs.name(o) for o in differ) or '-'))
    return notes


def pipes_text(mask):
    return '|'.join(PIPE_NAMES[p] for p in range(8) if mask >> p & 1) or '-'


def render(regs, t, notes, digests, variant):
    nonctxt = t['nonctxt_kgsl'] if variant == 'kgsl' else t['nonctxt_linux']
    gmem = [v for o, v, _ in nonctxt if o == 0x08e09]
    if len(gmem) != 1 or nonctxt[0][:2] != (0x00814, 0x00000101):
        fail('the non-context table must start with CP_SMMU_STREAM_ID_LPAC = 0x101 and hold RB_GC_GMEM_PROTECT once')
    dyn_triplets = sum(1 for p in range(1, 8) for _, mask in t['dyn'] if mask >> p & 1)
    out = []
    w = out.append
    w('/* SPDX-License-Identifier: MIT */')
    w(f'/* Generated by scripts/derive-a830-hwinit.py --variant {variant}; do not edit. Regenerate, then check with --check.')
    w(' *')
    w(' * Gen8 (A830, gen8_0_0/gen8_0_1) hardware-init tables (ARCHITECTURE 2.2, 4.8, 4.11, 4.17). Values are the numeric facts')
    w(' * of the pinned sources below, cross-checked between KGSL and Linux by the generator; register offsets are GPU-space')
    w(' * dwords as in A830Regs.h. Pipe masks: bit p = A8XX_PIPE_* (0 NONE, 1 BR, 2 BV, 3 LPAC, 4 AQE0, 5 AQE1, 6 DDE_BR, 7 DDE_BV).')
    w(' * Sources (sha256):')
    for key in SOURCES:
        w(f' *   {key:<14} {digests[key]}')
    w(' * Differences between the two drivers (pinned in the generator; the tables below use the selected variant / KGSL):')
    for note in notes:
        for i, line in enumerate(textwrap.wrap(note, 112)):
            w(f' *   {line}' if not i else f' *     {line}')
    w(' */')
    w('#ifndef A830_HWINIT_H')
    w('#define A830_HWINIT_H')
    w('#include <stdint.h>')
    w('')
    w(f'#define A830_HWINIT_VARIANT "{variant}"')
    w(f'#define A830_HWINIT_KGSL_GPULIST_SHA256 "{digests["kgsl_gpulist"]}"')
    w(f'#define A830_HWINIT_LINUX_CATALOG_SHA256 "{digests["linux_catalog"]}"')
    w('#define A830_HWINIT_PIPE(p) (1u << (p))')
    w('#if defined(__GNUC__)')
    w('#define A830_HWINIT_TABLE static const __attribute__((unused))')
    w('#else')
    w('#define A830_HWINIT_TABLE static const')
    w('#endif')
    w('')
    w('typedef struct A830HwInitReg {uint32_t offset, value;} A830HwInitReg;')
    w('typedef struct A830HwInitPipeReg {uint32_t offset, value, pipes;} A830HwInitPipeReg;')
    w('typedef struct A830HwInitProtect {uint32_t index, value;} A830HwInitProtect;   /* CP_PROTECT_GLOBAL[index] */')
    w('typedef struct A830HwInitPipeList {uint32_t offset, pipes;} A830HwInitPipeList;')
    w('')
    src = 'KGSL gen8_0_0_nonctxt_regs' if variant == 'kgsl' else 'Linux a830_nonctxt_regs'
    w(f'/* ARCHITECTURE 4.8 step 10: {src}, written pipe by pipe (PIPE_NONE with the aperture at 0). */')
    w(f'#define A830_HWINIT_NONCTXT_COUNT {len(nonctxt)}u')
    w('A830_HWINIT_TABLE A830HwInitPipeReg A830HwInitNonctxt[A830_HWINIT_NONCTXT_COUNT] = {')
    for offset, value, pipes in nonctxt:
        w(f'    {{0x{offset:05x}u, 0x{value:08x}u, 0x{pipes:02x}u}},   /* {regs.name(offset)} {pipes_text(pipes)} */')
    w('};')
    w(f'#define A830_HWINIT_GMEM_PROTECT_VALUE 0x{gmem[0]:08x}u   /* the table\'s RB_GC_GMEM_PROTECT, rewritten after the secure exit */')
    w('')
    w('/* ARCHITECTURE 4.8 step 13: KGSL gen8_0_0_protected_regs, value = start | (end - start) << 18 | noaccess << 31. Indices')
    w(' * 47..62 are not written; the last entry (63) is also written to CP_PROTECT_PIPE[15] of BR and BV (last span). */')
    w(f'#define A830_HWINIT_PROTECT_COUNT {len(t["protect"])}u')
    w('A830_HWINIT_TABLE A830HwInitProtect A830HwInitProtectRegs[A830_HWINIT_PROTECT_COUNT] = {')
    for index, value in t['protect']:
        start, count, noaccess = value & 0x3ffff, value >> 18 & 0x1fff, value >> 31
        w(f'    {{{index:2d}u, 0x{value:08x}u}},   /* 0x{start:05x}-0x{start + count:05x} {"no access" if noaccess else "read only"} */')
    w('};')
    w(f'#define A830_HWINIT_PROTECT_LAST_INDEX {t["protect"][-1][0]}u')
    w(f'#define A830_HWINIT_PROTECT_LAST_VALUE 0x{t["protect"][-1][1]:08x}u')
    w('')
    w('/* KGSL gen8_0_0_gbif_cx_regs: written by the GMU cold boot (ARCHITECTURE 4.7.3 B6). */')
    w(f'#define A830_HWINIT_GBIF_CX_COUNT {len(t["gbif"])}u')
    w('A830_HWINIT_TABLE A830HwInitReg A830HwInitGbifCx[A830_HWINIT_GBIF_CX_COUNT] = {')
    for offset, value in t['gbif']:
        w(f'    {{0x{offset:05x}u, 0x{value:08x}u}},   /* {regs.name(offset)} */')
    w('};')
    w('')
    w('/* KGSL gen8_ao_hwcg_regs: GMU AO clock gating (ARCHITECTURE 4.7.8, 4.11 step 11); 0 turns gating off. */')
    w(f'#define A830_HWINIT_AO_HWCG_COUNT {len(t["ao_hwcg"])}u')
    w('A830_HWINIT_TABLE A830HwInitReg A830HwInitAoHwcg[A830_HWINIT_AO_HWCG_COUNT] = {')
    for offset, value in t['ao_hwcg']:
        w(f'    {{0x{offset:05x}u, 0x{value:08x}u}},   /* {regs.name(offset)} */')
    w('};')
    w('')
    w('/* ARCHITECTURE 4.8 step 8: TPL1_BICUBIC_WEIGHTS_TABLE[0..19] (KGSL gen8_0_0_bicubic_regs = Linux hw_init). */')
    w('#define A830_HWINIT_BICUBIC_COUNT 20u')
    w('A830_HWINIT_TABLE uint32_t A830HwInitBicubic[A830_HWINIT_BICUBIC_COUNT] = {')
    for i in range(0, 20, 5):
        w('    ' + ' '.join(f'0x{v:08x}u,' for v in t['bicubic'][i:i + 5]))
    w('};')
    w('')
    w('/* ARCHITECTURE 4.11 step 10, 4.17 P2: the power-up restore lists of Linux a8xx_patch_pwrup_reglist (a830). The CP restores')
    w(' * the IFPC list after IFPC, the pwrup list after IFPC and preemption, and the dynamic list as {pipe << 12, offset, value}')
    w(' * triplets for every pipe BR..DDE_BV whose bit is set (LPAC included, as Linux reads it). */')
    w(f'#define A830_HWINIT_PWRUP_COUNT {len(t["pwrup"])}u')
    w('A830_HWINIT_TABLE uint32_t A830HwInitPwrup[A830_HWINIT_PWRUP_COUNT] = {')
    for offset in t['pwrup']:
        w(f'    0x{offset:05x}u,   /* {regs.name(offset)} */')
    w('};')
    w(f'#define A830_HWINIT_IFPC_COUNT {len(t["ifpc"])}u')
    w('A830_HWINIT_TABLE uint32_t A830HwInitIfpc[A830_HWINIT_IFPC_COUNT] = {')
    for offset in t['ifpc']:
        w(f'    0x{offset:05x}u,   /* {regs.name(offset)} */')
    w('};')
    w(f'#define A830_HWINIT_DYN_COUNT {len(t["dyn"])}u')
    w(f'#define A830_HWINIT_DYN_TRIPLETS {dyn_triplets}u   /* entries times pipes */')
    w('A830_HWINIT_TABLE A830HwInitPipeList A830HwInitDynPwrup[A830_HWINIT_DYN_COUNT] = {')
    for offset, pipes in t['dyn']:
        w(f'    {{0x{offset:05x}u, 0x{pipes:02x}u}},   /* {regs.name(offset)} {pipes_text(pipes)} */')
    w('};')
    w('')
    w('#endif')
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for key in SOURCES:
        parser.add_argument('--' + key.replace('_', '-'), dest=key, type=Path, help=f'default {SOURCES[key][0]}')
    parser.add_argument('--variant', choices=('kgsl', 'linux'), default='kgsl', help='non-context table (ARCHITECTURE 1.3 #8)')
    parser.add_argument('--out', type=Path, default=OUTPUT)
    parser.add_argument('--check', action='store_true', help='regenerate and compare with --out; write nothing')
    parser.add_argument('--allow-unpinned', action='store_true', help='accept sources whose sha256 differs from the pins')
    args = parser.parse_args()
    texts, digests = load(args)
    regs, tables = derive(texts)
    notes = cross_check(regs, tables)
    header = render(regs, tables, notes, digests, args.variant)
    if args.check:
        current = args.out.read_text() if args.out.is_file() else ''
        if current != header:
            print(f'derive-a830-hwinit: {args.out} differs from the regenerated {args.variant} tables', file=sys.stderr)
            sys.exit(1)
        print(f'PASS: {args.out.name} matches the pinned sources ({args.variant}: {len(tables["nonctxt_" + args.variant])} '
              f'non-context, {len(tables["protect"])} protect, {len(tables["gbif"])} GBIF, {len(tables["ao_hwcg"])} AO HWCG, '
              f'{len(tables["bicubic"])} bicubic, {len(tables["pwrup"])}/{len(tables["ifpc"])}/{len(tables["dyn"])} '
              f'pwrup/ifpc/dyn; {len(notes)} pinned differences)')
        return
    args.out.write_text(header)
    print(f'wrote {args.out} ({args.variant})')


if __name__ == '__main__':
    main()
