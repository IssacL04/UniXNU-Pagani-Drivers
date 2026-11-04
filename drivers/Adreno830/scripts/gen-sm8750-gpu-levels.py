#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate platform/sm8750/Sm8750GpuLevels.h from the PKX110 running device tree.

The kernel never parses a device tree. Everything the SM8750 platform layer needs from it
is turned into constant C tables here, once, at build time:

  * the GPU power-level bins of qcom,kgsl-3d0 (qcom,gpu-pwrlevel-bins, in DT order, with
    qcom,speed-bin / qcom,sku-codes / qcom,initial-pwrlevel), each table stored ascending;
  * the levels every bin shares (the default table of the driver, see
    docs/ARCHITECTURE.md 4.1.4), computed with the same rule as Sm8750GpuLevelsCommon();
  * qcom,bus-table-ddr, qcom,bus-table-cnoc and the GMU's qcom,gmu-freq-table;
  * the speed-bin nvmem cell (qfprom base, cell offset, bit position and width);
  * the SMEM region, the qcom_scm wait-queue interrupt and queue count, the SoC ids of
    qcom,msm-id.

The source is the PKX110 runtime device tree blob kept with the phone's stock backup (the capture the
device repository pins for its own GPU inputs: scripts/data/sm8750-gpu-inputs.json there), decompiled
with dtc; --dts reads an already decompiled tree instead. A runtime tree differs from boot to boot
(/chosen and friends), so only that one capture is accepted (DTB_PIN), and the header pins the
normalised inputs: they are turned into JSON and its sha256 is written into the header; --check
regenerates from the source and fails when the committed header differs.

Usage:
  gen-sm8750-gpu-levels.py [--dtb PATH | --dts PATH] [--out PATH] [--json PATH] [--check] [--self-test]
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DTB = ROOT.parent / '一加13T原厂备份' / '设备信息' / '运行中设备树.dtb'
# PKX110 ColorOS 16.0.8.300 runtime DT (/sys/firmware/fdt), the same pin as the device repository's
# scripts/data/sm8750-gpu-inputs.json; not the author's capture d00767f2 (proprietary-files.txt).
DTB_PIN = {'bytes': 1427077, 'sha256': '13b8b74a8dba343b96fd34910858f66501ce7ab4eb9ce2eec1787f55fc2ffe60'}
DEFAULT_OUT = ROOT / 'platform/sm8750/Sm8750GpuLevels.h'
MAX_LEVELS = 16          # KGSL_MAX_PWRLEVELS; Sm8750GpuMaxLevels
MAX_SKUS = 4             # Sm8750GpuMaxSkus


class DtsError(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# A small DTS reader. It understands what dtc emits when it decompiles a blob: nodes with
# labels, properties with <cells>, "strings" and [bytes], comments, /dts-v1/ and /memreserve/.

class Node:
    def __init__(self, name, parent):
        self.name = name
        self.parent = parent
        self.labels = []
        self.props = {}
        self.children = []

    def path(self):
        parts = []
        node = self
        while node is not None and node.parent is not None:
            parts.append(node.name)
            node = node.parent
        return '/' + '/'.join(reversed(parts))

    def cells(self, name, default=None):
        value = self.props.get(name)
        if value is None:
            return default
        out = []
        for kind, item in value:
            if kind != 'cells':
                raise DtsError('%s: %s is not a cell list' % (self.path(), name))
            out.extend(item)
        return out

    def strings(self, name):
        value = self.props.get(name)
        if value is None:
            return []
        if any(kind != 'str' for kind, _ in value):
            raise DtsError('%s: %s is not a string list' % (self.path(), name))
        return [item for _, item in value]

    def u32(self, name, default=None):
        value = self.cells(name)
        if value is None:
            return default
        if len(value) != 1:
            raise DtsError('%s: %s has %d cells, expected 1' % (self.path(), name, len(value)))
        return value[0]

    def child(self, name):
        found = [c for c in self.children if c.name == name]
        if len(found) != 1:
            raise DtsError('%s: expected one child %s, found %d' % (self.path(), name, len(found)))
        return found[0]

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


def tokenize(text):
    tokens = []
    i, n = 0, len(text)
    value_context = False
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        if text.startswith('/*', i):
            end = text.find('*/', i + 2)
            if end < 0:
                raise DtsError('unterminated comment')
            i = end + 2
            continue
        if text.startswith('//', i):
            end = text.find('\n', i)
            i = n if end < 0 else end
            continue
        if c == '"':
            j, out = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == '\\' and j + 1 < n:
                    out.append(text[j:j + 2].encode().decode('unicode_escape'))
                    j += 2
                else:
                    out.append(text[j])
                    j += 1
            if j >= n:
                raise DtsError('unterminated string')
            tokens.append(('str', ''.join(out)))
            i = j + 1
            continue
        if c == '<':
            end = text.find('>', i)
            if end < 0:
                raise DtsError('unterminated cell list')
            cells = []
            for word in text[i + 1:end].split():
                if word.startswith('&'):
                    raise DtsError('label references in cells are not supported: %s' % word)
                cells.append(int(word, 0) & 0xffffffff)
            tokens.append(('cells', cells))
            i = end + 1
            continue
        if c == '[':
            end = text.find(']', i)
            if end < 0:
                raise DtsError('unterminated byte string')
            tokens.append(('bytes', bytes.fromhex(''.join(text[i + 1:end].split()))))
            i = end + 1
            continue
        if c in '{};=':
            tokens.append((c, c))
            value_context = c == '='
            if c == ';':
                value_context = False
            i += 1
            continue
        if c == ',' and value_context:
            tokens.append((',', ','))
            i += 1
            continue
        j = i
        while j < n and not text[j].isspace() and text[j] not in '{};=<>"[':
            if value_context and text[j] == ',':
                break
            j += 1
        if j == i:
            raise DtsError('unexpected character %r at offset %d' % (c, i))
        tokens.append(('word', text[i:j]))
        i = j
    return tokens


def parse(text):
    tokens = tokenize(text)
    pos = 0
    root = None

    def take(kind=None):
        nonlocal pos
        if pos >= len(tokens):
            raise DtsError('unexpected end of file')
        tok = tokens[pos]
        if kind is not None and tok[0] != kind:
            raise DtsError('expected %s, found %r' % (kind, tok))
        pos += 1
        return tok

    def body(node):
        while True:
            kind, value = tokens[pos]
            if kind == '}':
                take('}')
                take(';')
                return
            labels = []
            while kind == 'word' and value.endswith(':'):
                labels.append(value[:-1])
                take()
                kind, value = tokens[pos]
            name = take('word')[1]
            kind, _ = tokens[pos]
            if kind == '{':
                take('{')
                child = Node(name, node)
                child.labels = labels
                node.children.append(child)
                body(child)
            elif kind == ';':
                take(';')
                node.props[name] = []
            elif kind == '=':
                take('=')
                items = []
                while True:
                    tok = take()
                    if tok[0] not in ('cells', 'str', 'bytes', 'word'):
                        raise DtsError('%s/%s: bad value %r' % (node.path(), name, tok))
                    items.append(tok)
                    sep = take()[0]
                    if sep == ';':
                        break
                    if sep != ',':
                        raise DtsError('%s/%s: expected , or ;' % (node.path(), name))
                node.props[name] = items
            else:
                raise DtsError('%s: unexpected %r after %s' % (node.path(), tokens[pos], name))

    while pos < len(tokens):
        kind, value = tokens[pos]
        if kind == 'word' and value in ('/dts-v1/', '/plugin/'):
            take()
            take(';')
            continue
        if kind == 'word' and value == '/memreserve/':
            while take()[0] != ';':
                pass
            continue
        if kind == 'word' and value == '/':
            take()
            take('{')
            if root is None:
                root = Node('', None)
            body(root)
            continue
        raise DtsError('unexpected top-level token %r' % (tokens[pos],))
    if root is None:
        raise DtsError('no root node')
    return root


# ---------------------------------------------------------------------------------------------
# Sources

def check_pin(data, pin):
    """Refuse any blob but the pinned capture: a runtime tree is only the right input if it is that one."""
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != pin['bytes'] or digest != pin['sha256']:
        raise DtsError('not the pinned PKX110 runtime DTB (%d bytes, sha256 %s; pinned %d bytes, %s)'
                       % (len(data), digest, pin['bytes'], pin['sha256']))
    return digest


def decompile(data):
    """dtc -I dtb -O dts of a blob, as text (what /tmp/claude-501/live.dts was made with)."""
    dtc = shutil.which('dtc')
    if dtc is None:
        raise DtsError('dtc not found (Homebrew: brew install dtc)')
    with tempfile.TemporaryDirectory() as directory:
        blob = Path(directory) / 'tree.dtb'
        blob.write_bytes(data)
        result = subprocess.run([dtc, '-q', '-I', 'dtb', '-O', 'dts', '-o', '-', str(blob)], capture_output=True)
    if result.returncode:
        raise DtsError('dtc failed: %s' % result.stderr.decode(errors='replace').strip()[-500:])
    return result.stdout.decode('utf-8')


def compile_dts(text):
    """dtc -I dts -O dtb (self test only)."""
    dtc = shutil.which('dtc')
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / 'tree.dts'
        source.write_text(text)
        result = subprocess.run([dtc, '-q', '-I', 'dts', '-O', 'dtb', '-o', '-', str(source)], capture_output=True)
    if result.returncode:
        raise DtsError('dtc failed: %s' % result.stderr.decode(errors='replace').strip()[-500:])
    return result.stdout


# ---------------------------------------------------------------------------------------------
# Extraction

def one(nodes, what):
    nodes = list(nodes)
    if len(nodes) != 1:
        raise DtsError('expected exactly one %s, found %d' % (what, len(nodes)))
    return nodes[0]


def reg_pairs(node):
    parent = node.parent
    acells = parent.u32('#address-cells', 2)
    scells = parent.u32('#size-cells', 1)
    raw = node.cells('reg', [])
    step = acells + scells
    if not raw or len(raw) % step:
        raise DtsError('%s: reg does not match #address-cells/#size-cells' % node.path())
    out = []
    for i in range(0, len(raw), step):
        addr = 0
        for c in raw[i:i + acells]:
            addr = (addr << 32) | c
        size = 0
        for c in raw[i + acells:i + step]:
            size = (size << 32) | c
        out.append((addr, size))
    return out


LEVEL_PROPS = {'reg', 'qcom,gpu-freq', 'qcom,level', 'qcom,bus-min', 'qcom,bus-freq', 'qcom,bus-max',
               'qcom,acd-level'}
BIN_PROPS = {'qcom,speed-bin', 'qcom,sku-codes', 'qcom,initial-pwrlevel', '#address-cells', '#size-cells'}


def extract(root):
    nodes = list(root.walk())
    by_phandle = {}
    for node in nodes:
        ph = node.u32('phandle')
        if ph is not None:
            by_phandle[ph] = node

    gpu = one((n for n in nodes if 'qcom,kgsl-3d0' in n.strings('compatible')), 'qcom,kgsl-3d0 node')
    bins_node = gpu.child('qcom,gpu-pwrlevel-bins')
    ddr = gpu.cells('qcom,bus-table-ddr')
    cnoc = gpu.cells('qcom,bus-table-cnoc')
    if not ddr or not cnoc:
        raise DtsError('GPU bus tables missing')

    bins = []
    for bnode in bins_node.children:
        extra = set(bnode.props) - BIN_PROPS
        if extra:
            raise DtsError('%s: unhandled properties %s' % (bnode.path(), sorted(extra)))
        levels = {}
        for lnode in bnode.children:
            extra = set(lnode.props) - LEVEL_PROPS
            if extra:
                # qcom,cx-level, DDR-type variants (qcom,bus-freq-ddrN) and the like change the meaning
                # of a level; refuse them instead of silently dropping them.
                raise DtsError('%s: unhandled properties %s' % (lnode.path(), sorted(extra)))
            index = lnode.u32('reg')
            freq = lnode.u32('qcom,gpu-freq')
            if index is None or freq is None:
                raise DtsError('%s: reg or qcom,gpu-freq missing' % lnode.path())
            if freq == 0:
                continue                             # KGSL ignores "zero" power levels
            bus = lnode.u32('qcom,bus-freq')
            if bus is None or lnode.u32('qcom,level') is None:
                raise DtsError('%s: qcom,level or qcom,bus-freq missing' % lnode.path())
            if freq % 1000:
                raise DtsError('%s: frequency is not a whole kHz' % lnode.path())
            if index in levels:
                raise DtsError('%s: duplicate reg %d' % (lnode.path(), index))
            levels[index] = {
                'khz': freq // 1000,
                'vlvl': lnode.u32('qcom,level'),
                'bus_min': lnode.u32('qcom,bus-min', bus),
                'bus': bus,
                'bus_max': lnode.u32('qcom,bus-max', bus),
                'acd': lnode.u32('qcom,acd-level', 0),
            }
        count = len(levels)
        if sorted(levels) != list(range(count)):
            raise DtsError('%s: level indices are not 0..%d' % (bnode.path(), count - 1))
        if not 0 < count <= MAX_LEVELS:
            raise DtsError('%s: %d levels' % (bnode.path(), count))
        ordered = [levels[i] for i in range(count)]          # DT order: index 0 is the fastest
        for a, b in zip(ordered, ordered[1:]):
            if b['khz'] > a['khz']:
                raise DtsError('%s: levels are not in descending frequency order' % bnode.path())
        for level in ordered:
            if not level['bus_min'] <= level['bus'] <= level['bus_max'] < len(ddr):
                raise DtsError('%s: bus votes out of order or past qcom,bus-table-ddr' % bnode.path())
        initial = bnode.u32('qcom,initial-pwrlevel')
        if initial is None or initial >= count:
            raise DtsError('%s: bad qcom,initial-pwrlevel' % bnode.path())
        skus = bnode.cells('qcom,sku-codes')
        if skus is not None and not 0 < len(skus) <= MAX_SKUS:
            raise DtsError('%s: %d sku codes' % (bnode.path(), len(skus)))
        bins.append({
            'name': bnode.name,
            'speed_bin': bnode.u32('qcom,speed-bin'),
            'sku_codes': skus,
            'initial': initial,
            'levels': list(reversed(ordered)),                # stored ascending
        })
    if not bins:
        raise DtsError('no power-level bins')

    names = gpu.strings('nvmem-cell-names')
    phandles = gpu.cells('nvmem-cells', [])
    if 'speed_bin' not in names or len(names) != len(phandles):
        raise DtsError('GPU speed_bin nvmem cell missing')
    cell = by_phandle.get(phandles[names.index('speed_bin')])
    if cell is None:
        raise DtsError('speed_bin nvmem phandle does not resolve')
    (cell_offset, cell_bytes), = reg_pairs(cell)
    bits = cell.cells('bits', [0, cell_bytes * 8])
    qfprom = cell.parent
    (qfprom_base, qfprom_bytes), = reg_pairs(qfprom)
    if len(bits) != 2 or bits[0] + bits[1] > cell_bytes * 8 or cell_offset + cell_bytes > qfprom_bytes:
        raise DtsError('speed_bin cell geometry')

    gmu = one((n for n in nodes if n.props.get('qcom,gmu-freq-table')), 'GMU node with qcom,gmu-freq-table')
    table = gmu.cells('qcom,gmu-freq-table')
    if len(table) % 2 or table[0::2] != sorted(table[0::2]):
        raise DtsError('qcom,gmu-freq-table')
    gmu_levels = [{'khz': table[i] // 1000, 'vlvl': table[i + 1]} for i in range(0, len(table), 2)]

    smem = one((n for n in nodes if 'qcom,smem' in n.strings('compatible') and n.props.get('reg') is not None),
               'qcom,smem reserved region')
    (smem_base, smem_bytes), = reg_pairs(smem)

    scm = one((n for n in nodes if 'qcom,scm' in n.strings('compatible')), 'qcom,scm node')
    irq = scm.cells('interrupts', [])
    if len(irq) != 3 or irq[0] != 0:
        raise DtsError('qcom_scm interrupt is not one GIC SPI')
    scm_queues = scm.u32('qcom,max-queues', 1)

    msm = root.cells('qcom,msm-id', [])
    if not msm or len(msm) % 2:
        raise DtsError('qcom,msm-id')
    soc_ids = sorted({msm[i] & 0xffff for i in range(0, len(msm), 2)})

    return {
        'gpu': {'bins': bins, 'ddr_kbps': ddr, 'cnoc': cnoc},
        'gmu': gmu_levels,
        'speed_bin': {'qfprom': qfprom_base, 'qfprom_bytes': qfprom_bytes, 'offset': cell_offset,
                      'bytes': cell_bytes, 'shift': bits[0], 'bits': bits[1]},
        'smem': {'base': smem_base, 'bytes': smem_bytes},
        'scm_waitq': {'spi': irq[1], 'intid': irq[1] + 32, 'edge': 1 if irq[2] in (1, 2, 3) else 0,
                      'queues': scm_queues},
        'soc_ids': soc_ids,
    }


# ---------------------------------------------------------------------------------------------
# Derived tables (the C side computes the same; tests/test_sm8750_socinfo.c cross-checks them)

def common_levels(bins):
    """Ascending prefix on which every bin agrees on frequency, voltage level and ACD value.
    Bus votes may differ (bin 0xBE votes 11 at 900 MHz, the others 10): take the lowest. A level that
    repeats the previous one's frequency and voltage level is dropped (Sm8750GpuLevelsCommon)."""
    out = []
    for i in range(min(len(b['levels']) for b in bins)):
        column = [b['levels'][i] for b in bins]
        first = column[0]
        if any((l['khz'], l['vlvl'], l['acd']) != (first['khz'], first['vlvl'], first['acd']) for l in column):
            break
        if out and (out[-1]['khz'], out[-1]['vlvl']) == (first['khz'], first['vlvl']):
            continue
        out.append({'khz': first['khz'], 'vlvl': first['vlvl'], 'acd': first['acd'],
                    'bus_min': min(l['bus_min'] for l in column),
                    'bus': min(l['bus'] for l in column),
                    'bus_max': min(l['bus_max'] for l in column)})
    if not out:
        raise DtsError('the bins share no level')
    return out


def common_initial(bins, common):
    """The fastest common level not faster than the slowest level any bin starts at (index 0 if none)."""
    start = min(initial_khz(b) for b in bins)
    return max([i for i, l in enumerate(common) if l['khz'] <= start] or [0])


def initial_khz(b):
    count = len(b['levels'])
    return b['levels'][count - 1 - b['initial']]['khz']


# ---------------------------------------------------------------------------------------------
# Output

def c_level(l):
    return '{%7u, %#5x, %2u, %2u, %2u, 0, %#010x}' % (l['khz'], l['vlvl'], l['bus_min'], l['bus'], l['bus_max'],
                                                       l['acd'])


INFORMATIONAL = '(informational'      # header lines --check does not compare


def render(inputs, digest, source):
    gpu = inputs['gpu']
    bins = gpu['bins']
    common = common_levels(bins)
    start = common_initial(bins, common)
    sb = inputs['speed_bin']
    w = inputs['scm_waitq']
    lines = [
        '/* SPDX-License-Identifier: MIT */',
        '/* Generated by scripts/gen-sm8750-gpu-levels.py from the PKX110 running device tree. Do not edit;',
        ' * regenerate, and check with --check.',
        ' *   normalised GPU inputs sha256 %s' % digest,
        ' *   source %s sha256 %s %s: --check compares the inputs)' % (source[0], source[1], INFORMATIONAL),
        ' * Level tables are ascending (index 0 is the slowest; GMU level = index + 1, GMU level 0 is off).',
        ' * initial is the DT qcom,initial-pwrlevel index (0 = fastest) of the bin, as KGSL reads it. */',
        '#ifndef SM8750_GPU_LEVELS_H',
        '#define SM8750_GPU_LEVELS_H',
        '#include "Sm8750SocInfo.h"',
        '',
        '#define SM8750_DT_GPU_INPUTS_SHA256 "%s"' % digest,
        '/* Physical windows (reserved-memory smem region; qfprom block that holds the speed-bin cell). */',
        '#define SM8750_DT_SMEM_PHYS %#xull' % inputs['smem']['base'],
        '#define SM8750_DT_SMEM_BYTES %#xu' % inputs['smem']['bytes'],
        '#define SM8750_DT_QFPROM_PHYS %#xull' % sb['qfprom'],
        '#define SM8750_DT_QFPROM_BYTES %#xu' % sb['qfprom_bytes'],
        '',
        'enum {',
        '    Sm8750DtSpeedBinOffset = %#x, Sm8750DtSpeedBinShift = %u, Sm8750DtSpeedBinBits = %u,'
        % (sb['offset'], sb['shift'], sb['bits']),
        '    Sm8750DtScmWaitqIntid = %u, Sm8750DtScmWaitqEdge = %u, Sm8750DtScmWaitqQueues = %u,'
        % (w['intid'], w['edge'], w['queues']),
        '    Sm8750DtGpuBinCount = %u, Sm8750DtGpuCommonCount = %u, Sm8750DtGpuCommonInitial = %u,'
        % (len(bins), len(common), start),
        '    Sm8750DtDdrLevels = %u, Sm8750DtCnocLevels = %u, Sm8750DtGmuLevels = %u, Sm8750DtSocIdCount = %u,'
        % (len(gpu['ddr_kbps']), len(gpu['cnoc']), len(inputs['gmu']), len(inputs['soc_ids'])),
        '};',
        '',
        '/* qcom,msm-id SoC ids (low 16 bits) the tree was built for. */',
        'static const uint32_t Sm8750DtSocIds[%u] = {%s};' % (len(inputs['soc_ids']),
                                                         ', '.join(str(x) for x in inputs['soc_ids'])),
        '/* qcom,bus-table-ddr (kBps per DDR level) and qcom,bus-table-cnoc. */',
        'static const uint32_t Sm8750DtDdrKbps[%u] = {%s};' % (len(gpu['ddr_kbps']),
                                                          ', '.join(str(x) for x in gpu['ddr_kbps'])),
        'static const uint32_t Sm8750DtCnoc[%u] = {%s};' % (len(gpu['cnoc']), ', '.join(str(x) for x in gpu['cnoc'])),
        '/* qcom,gmu-freq-table: GMU clock (kHz) and its RPMh level. */',
        'static const uint32_t Sm8750DtGmuKhz[%u] = {%s};' % (len(inputs['gmu']),
                                                         ', '.join(str(g['khz']) for g in inputs['gmu'])),
        'static const uint32_t Sm8750DtGmuVlvl[%u] = {%s};' % (len(inputs['gmu']),
                                                          ', '.join('%#x' % g['vlvl'] for g in inputs['gmu'])),
        '',
        '/* {khz, vlvl, bus_min, bus, bus_max, reserved, acd} */',
    ]
    for i, b in enumerate(bins):
        lines.append('static const Sm8750GpuLevel Sm8750DtGpuBin%uLevels[%u] = {   /* %s */'
                     % (i, len(b['levels']), b['name']))
        lines += ['    %s,' % c_level(l) for l in b['levels']]
        lines.append('};')
    lines.append('/* The levels every bin shares: the driver default (pagani-gpu-speedbin=0). */')
    lines.append('static const Sm8750GpuLevel Sm8750DtGpuCommonLevels[%u] = {' % len(common))
    lines += ['    %s,' % c_level(l) for l in common]
    lines.append('};')
    lines.append('')
    lines.append('/* {flags, speed_bin, sku[4], sku_count, initial, count, reserved, levels}, in DT order (KGSL matches the')
    lines.append(' * first bin whose speed bin equals the fuse or whose sku codes hold 0 or the SoC code). */')
    lines.append('static const Sm8750GpuBin Sm8750DtGpuBins[%u] = {' % len(bins))
    for i, b in enumerate(bins):
        flags = []
        if b['speed_bin'] is not None:
            flags.append('Sm8750GpuBinHasSpeed')
        if b['sku_codes'] is not None:
            flags.append('Sm8750GpuBinHasSku')
        skus = list(b['sku_codes'] or []) + [0] * (MAX_SKUS - len(b['sku_codes'] or []))
        lines.append('    {%s, %#x, {%s}, %u, %u, %u, 0, Sm8750DtGpuBin%uLevels},   /* max %u MHz */'
                     % (' | '.join(flags) or '0', b['speed_bin'] or 0, ', '.join(str(s) for s in skus),
                        len(b['sku_codes'] or []), b['initial'], len(b['levels']), i,
                        b['levels'][-1]['khz'] // 1000))
    lines.append('};')
    lines.append('')
    lines.append('#endif')
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------------------------------------
# Self test: a small tree in the shape dtc emits, plus the inputs the generator must refuse.

SELF_TEST_DTS = r'''/dts-v1/;
/memreserve/ 0x80000000 0x1000;
/ {
	qcom,msm-id = <0x100026a 0x20000 0x27f 0x10000>;
	#address-cells = <0x02>;
	#size-cells = <0x02>;
	model = "test \"tree\", with ; and { inside";
	reserved-memory {
		#address-cells = <0x02>;
		#size-cells = <0x02>;
		smem_mem: smem_region@81d00000 {
			compatible = "qcom,smem";
			reg = <0x00 0x81d00000 0x00 0x200000>;
			no-map;
		};
	};
	firmware {
		qcom_scm {
			compatible = "qcom,scm-v1.1", "qcom,scm";
			qcom,max-queues = <0x02>;
			interrupts = <0x00 0x3c2 0x01>;
		};
	};
	soc: soc {
		#address-cells = <0x01>;
		#size-cells = <0x01>;
		/* a comment */
		qfprom@221c8000 {
			#address-cells = <0x01>;
			#size-cells = <0x01>;
			reg = <0x221c8000 0x1000>;
			mac = [00 11 22 33];
			gpu_speed_bin: gpu_speed_bin@138 {
				reg = <0x138 0x02>;
				bits = <0x00 0x09>;
				phandle = <0x47d>;
			};
		};
		gpu: qcom,kgsl-3d0@3d00000 {
			compatible = "qcom,adreno-gpu-gen8-0-1", "qcom,kgsl-3d0";
			nvmem-cell-names = "speed_bin";
			nvmem-cells = <0x47d>;
			qcom,bus-table-ddr = <0x00 0x64 0xc8 0x12c>;
			qcom,bus-table-cnoc = <0x00 0x64>;
			qcom,gpu-pwrlevel-bins {
				#address-cells = <0x01>;
				#size-cells = <0x00>;
				qcom,gpu-pwrlevels-0 {
					qcom,speed-bin = <0xaa>;
					qcom,sku-codes = <0x05>;
					qcom,initial-pwrlevel = <0x01>;
					qcom,gpu-pwrlevel@0 { reg = <0x00>; qcom,gpu-freq = <0x11e1a300>; qcom,level = <0x80>; qcom,bus-freq = <0x03>; qcom,bus-min = <0x02>; qcom,bus-max = <0x03>; qcom,acd-level = <0x882a5ffd>; };
					qcom,gpu-pwrlevel@1 { reg = <0x01>; qcom,gpu-freq = <0x5f5e100>; qcom,level = <0x40>; qcom,bus-freq = <0x01>; };
					qcom,gpu-pwrlevel@2 { reg = <0x02>; qcom,gpu-freq = <0x00>; };
				};
				qcom,gpu-pwrlevels-1 {
					qcom,sku-codes = <0x00>;
					qcom,initial-pwrlevel = <0x01>;
					qcom,gpu-pwrlevel@1 { reg = <0x01>; qcom,gpu-freq = <0x5f5e100>; qcom,level = <0x40>; qcom,bus-freq = <0x02>; };
					qcom,gpu-pwrlevel@0 { reg = <0x00>; qcom,gpu-freq = <0x17d78400>; qcom,level = <0xc0>; qcom,bus-freq = <0x03>; };
				};
			};
		};
		gmu: qcom,gmu@3d37000 {
			compatible = "qcom,gen8-gmu";
			qcom,gmu-freq-table = <0x1dcd6500 0x40 0x26be3680 0x80>;
		};
	};
};
'''


def self_test():
    inputs = extract(parse(SELF_TEST_DTS))
    bins = inputs['gpu']['bins']
    assert [b['speed_bin'] for b in bins] == [0xaa, None] and bins[0]['sku_codes'] == [5], bins
    assert [l['khz'] for l in bins[0]['levels']] == [100000, 300000], 'zero level skipped, ascending'
    assert bins[0]['levels'][0]['bus_min'] == 1 and bins[0]['levels'][0]['bus_max'] == 1, 'bus defaults'
    assert [l['khz'] for l in bins[1]['levels']] == [100000, 400000], 'children in any order'
    common = common_levels(bins)
    assert len(common) == 1 and common[0]['bus'] == 1 and common_initial(bins, common) == 0, common
    assert inputs['speed_bin'] == {'qfprom': 0x221c8000, 'qfprom_bytes': 0x1000, 'offset': 0x138, 'bytes': 2,
                                   'shift': 0, 'bits': 9}, inputs['speed_bin']
    assert inputs['scm_waitq'] == {'spi': 962, 'intid': 994, 'edge': 1, 'queues': 2}, inputs['scm_waitq']
    assert inputs['smem'] == {'base': 0x81d00000, 'bytes': 0x200000} and inputs['soc_ids'] == [618, 639], inputs
    text = render(inputs, '0' * 64, ('DTS', '1' * 64))
    assert text.count('Sm8750GpuBinHasSpeed | Sm8750GpuBinHasSku') == 1
    assert [l for l in text.splitlines() if INFORMATIONAL in l] == [
        ' *   source DTS sha256 %s (informational: --check compares the inputs)' % ('1' * 64)], 'source line'
    # Only the pinned capture is accepted as a blob.
    try:
        check_pin(b'\xd0\x0d\xfe\xed' + bytes(60), DTB_PIN)
    except DtsError as error:
        assert 'not the pinned PKX110 runtime DTB' in str(error), error
    else:
        raise AssertionError('accepted an unpinned DTB')
    pin = {'bytes': 3, 'sha256': hashlib.sha256(b'abc').hexdigest()}
    assert check_pin(b'abc', pin) == pin['sha256'], 'pinned blob'
    # The reader takes what dtc emits when it decompiles a blob (the default source), not only hand-written text.
    blobs = 'dtc absent: blob round trip skipped'
    if shutil.which('dtc'):
        assert extract(parse(decompile(compile_dts(SELF_TEST_DTS)))) == inputs, 'dtc round trip'
        blobs = 'reads dtc-decompiled blobs'
    refused = {   # what: (text, replacement, reason the reader must give)
        'DDR-type variant': ('qcom,bus-max = <0x03>;', 'qcom,bus-max = <0x03>; qcom,bus-freq-ddr8 = <0x02>;',
                             "unhandled properties ['qcom,bus-freq-ddr8']"),
        'cx level': ('qcom,bus-freq = <0x01>;', 'qcom,bus-freq = <0x01>; qcom,cx-level = <0x40>;',
                     "unhandled properties ['qcom,cx-level']"),
        'ascending DT order': ('qcom,gpu-freq = <0x17d78400>', 'qcom,gpu-freq = <0x2faf080>',
                               'not in descending frequency order'),
        'fractional kHz': ('qcom,gpu-freq = <0x17d78400>', 'qcom,gpu-freq = <0x17d78401>', 'not a whole kHz'),
        'initial past the table': ('qcom,sku-codes = <0x00>;\n\t\t\t\t\tqcom,initial-pwrlevel = <0x01>;',
                                   'qcom,sku-codes = <0x00>;\n\t\t\t\t\tqcom,initial-pwrlevel = <0x02>;',
                                   'bad qcom,initial-pwrlevel'),
        'bus past the DDR table': ('qcom,bus-freq = <0x02>;', 'qcom,bus-freq = <0x04>;', 'past qcom,bus-table-ddr'),
        'unknown bin property': ('qcom,speed-bin = <0xaa>;',
                                 'qcom,speed-bin = <0xaa>; qcom,throttle-pwrlevel = <0x00>;',
                                 "unhandled properties ['qcom,throttle-pwrlevel']"),
        'duplicate level index': ('reg = <0x01>; qcom,gpu-freq = <0x5f5e100>; qcom,level = <0x40>; qcom,bus-freq = <0x02>;',
                                  'reg = <0x00>; qcom,gpu-freq = <0x5f5e100>; qcom,level = <0x40>; qcom,bus-freq = <0x02>;',
                                  'duplicate reg 0'),
        'two GPU nodes': ('gmu: qcom,gmu@3d37000 {', 'gpu2 { compatible = "qcom,kgsl-3d0"; };\n\t\tgmu: qcom,gmu@3d37000 {',
                          'expected exactly one qcom,kgsl-3d0 node'),
        'unterminated string': ('"qcom,gen8-gmu";', '"qcom,gen8-gmu;', 'unterminated string'),
    }
    for what, (old, new, reason) in refused.items():
        assert SELF_TEST_DTS.count(old) == 1, what
        try:
            extract(parse(SELF_TEST_DTS.replace(old, new)))
        except DtsError as error:
            assert reason in str(error), '%s refused for another reason: %s' % (what, error)
            continue
        raise AssertionError('accepted: ' + what)
    print('PASS: gen-sm8750-gpu-levels.py reads dtc output (%s), refuses unpinned blobs and %d malformed or '
          'unhandled inputs' % (blobs, len(refused)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--dtb', type=Path, default=DEFAULT_DTB, help='pinned PKX110 runtime DTB (default: the backup)')
    source.add_argument('--dts', type=Path, help='an already decompiled tree instead of the DTB')
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--json', type=Path, help='also write the normalised inputs')
    parser.add_argument('--check', action='store_true', help='compare with --out instead of writing it')
    parser.add_argument('--self-test', action='store_true', help='run the reader and generator self test')
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    path = args.dts or args.dtb
    if not path.is_file():
        if args.check:
            print('SKIP: %s is missing; Sm8750GpuLevels.h not re-derived' % path)
            return 0
        sys.exit('device tree %s is missing' % path)
    raw = path.read_bytes()
    try:
        if args.dts:
            text, kind = raw.decode('utf-8'), 'DTS'
        else:
            check_pin(raw, DTB_PIN)
            text, kind = decompile(raw), 'DTB'
        inputs = extract(parse(text))
    except DtsError as error:
        sys.exit('%s: %s' % (path, error))
    normal = json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()
    digest = hashlib.sha256(normal).hexdigest()
    text = render(inputs, digest, (kind, hashlib.sha256(raw).hexdigest()))
    if args.json:
        args.json.write_text(json.dumps(inputs, sort_keys=True, indent=1) + '\n')
    if args.check:
        current = args.out.read_text() if args.out.is_file() else ''
        # The source digest is informational (a DTS of the same tree may be formatted differently); everything
        # else, the inputs digest included, must match.
        strip = lambda t: '\n'.join(l for l in t.splitlines() if INFORMATIONAL not in l)
        if strip(current) != strip(text):
            sys.exit('FAIL: %s differs from what %s gives (inputs sha256 %s)' % (args.out, path, digest))
        print('PASS: %s matches %s (inputs sha256 %s)' % (args.out.name, path, digest))
        return 0
    args.out.write_text(text)
    print('wrote %s (inputs sha256 %s, %d bins, %d common levels)'
          % (args.out, digest, len(inputs['gpu']['bins']), len(common_levels(inputs['gpu']['bins']))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
