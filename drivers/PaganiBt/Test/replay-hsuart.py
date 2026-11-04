#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""把 Test/replay.c 用真固件生成的发送字节，和 Android 的 hsuart 跟踪逐条比对。

跟踪的局限决定了这里能证明什么，所以逐项算清楚，不混在一起：
  - 每条 DMA 记录只留前 21 字节和总长度。比对的是这 21 字节和位置，不是全部字节；其余字节来自同一个文件（sha256 在报告里）。
  - 每 CPU 环形缓冲回绕，有的记录丢了，表现为两条记录之间有缺口。缺口不算错，但缺口字节没被比过，单独统计。
  - DMA 边界和包边界不一致：一条记录可能从包中间开始，也可能跨几个包。
  - 带校准的 NVM 段：校准文件在手机 data 分区，这里只有全零占位，那些记录只比 6 字节的帧头。
  - 0xfc 由读路径按接收到的 0xfd 发出，序列里的位置取决于跟踪丢失的 RX，单字节 IBS 记录单独统计。

规则：跟踪里 HCI_Reset 之前的每一条发送记录都必须在生成流里按顺序找到；记录以帧头开始时必须落在生成的包边界上；记录不能超出生成流。
任何一条对不上都失败。"""
import argparse
import bisect
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RECORD = re.compile(r'\s([0-9]+\.[0-9]+): serial_info: 898000\.qcom,qup_uart: dump_ipc : DMA (Tx|Rx)\[0x0+:(\d+)\] : ([0-9a-f ]+)')
HEAD_BYTES = 21
FRAME_STARTS = (b'\x31\x00\xf0\xff', b'\x31\x00\xf1\xff', b'\x01\x00\xfc')


def parse_trace(path):
    records = []
    for line in open(path, errors='replace'):
        match = RECORD.search(line)
        if match:
            records.append((float(match.group(1)), match.group(2), int(match.group(3)), bytes.fromhex(match.group(4).strip())))
    return records


def parse_generated(path):
    writes, stream, wild, starts, events = [], bytearray(), bytearray(), [], []
    for line in open(path):
        kind, *rest = line.split()
        if kind == 'W':
            limit, text = int(rest[0]), rest[1]
            data = bytes.fromhex(text)
            starts.append(len(stream))
            writes.append(data)
            stream += data
            wild += bytes(min(i >= limit, 1) for i in range(len(data)))
        else:
            events.append((kind, rest[0], len(stream)))
    return writes, stream, wild, starts, events


def matches(stream, wild, at, head):
    if at + len(head) > len(stream):
        return False
    for i, byte in enumerate(head):
        if not wild[at + i] and stream[at + i] != byte:
            return False
    return True


def find(stream, wild, cursor, head, stats):
    """cursor 之后第一个吻合的位置。先用 bytes.find 快速找，落在占位区时逐字节找。"""
    plain = bytes(head)
    at = stream.find(plain, cursor)
    wild_start = wild.find(1, cursor)
    if wild_start >= 0 and (at < 0 or at + len(plain) > wild_start):
        for p in range(max(cursor, wild_start - HEAD_BYTES), len(stream)):
            if matches(stream, wild, p, head):
                stats['wild'] += 1
                return p
    return at


def compare(records, writes, stream, wild, starts, stop_time):
    stats = {'records': 0, 'ibs_inline': 0, 'ibs_extra': 0, 'wild': 0, 'gap_bytes': 0, 'gaps': 0, 'head_bytes': 0, 'covered': 0, 'one_byte': 0}
    write_starts = set(starts)
    cursor, problems, seen_writes, log = 0, [], set(), []
    for t, direction, length, head in records:
        if direction != 'Tx' or t > stop_time:
            continue
        stats['records'] += 1
        if length == 1 and head[0] in (0xfc, 0xfd, 0xfe):
            # 0xfd 是序列的一部分，前面可能有丢掉的短命令；0xfc 是读路径发的，只能算额外
            at = stream.find(head, cursor, cursor + 16) if head[0] == 0xfd else (cursor if stream[cursor:cursor + 1] == head else -1)
            if at >= 0 and at in write_starts:
                if at > cursor:
                    stats['gaps'] += 1
                    stats['gap_bytes'] += at - cursor
                stats['ibs_inline'] += 1
                seen_writes.add(bisect.bisect_right(starts, at) - 1)
                cursor = at + 1
            else:
                stats['ibs_extra'] += 1
                log.append((t, head.hex(), cursor))
            continue
        at = find(stream, wild, cursor, head, stats)
        if at < 0:
            problems.append(f'{t:.6f}: 记录 len={length} {head.hex()} 在 {cursor} 之后找不到')
            continue
        if at + length > len(stream):
            problems.append(f'{t:.6f}: 记录 len={length} 超出生成流（{at} + {length} > {len(stream)}）')
            continue
        if head.startswith(FRAME_STARTS) and at not in write_starts:
            problems.append(f'{t:.6f}: 记录以帧头开始，却落在 {at}，不是包边界')
            continue
        if at > cursor:
            stats['gaps'] += 1
            stats['gap_bytes'] += at - cursor
        if length == 1:
            stats['one_byte'] += 1
        stats['head_bytes'] += len(head)
        stats['covered'] += length
        for i in range(bisect.bisect_right(starts, at) - 1, len(starts)):
            if starts[i] >= at + length:
                break
            seen_writes.add(i)
        cursor = at + length
    if cursor != len(stream):
        problems.append(f'比完之后停在 {cursor}，生成流共 {len(stream)} 字节')
    return stats, problems, seen_writes, log


def h4_check(records, exe, stop_time):
    """跟踪里记全了的接收记录（不超过 21 字节）过成帧和过滤；复位之后的发送合并成包，过发送过滤。"""
    lines, tx, pending = [], [], None
    skipped = 0
    for t, direction, length, head in records:
        if direction == 'Rx':
            if length <= HEAD_BYTES and length == len(head):
                lines.append('R ' + head.hex())
            else:
                skipped += 1
        elif t > stop_time:
            if length == 1 and head[0] in (0x01, 0x02):
                pending = head
            elif pending is not None:
                if length == len(head):
                    tx.append(pending + head)
                pending = None
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'h4.txt'
        path.write_text('\n'.join(lines + ['T ' + p.hex() for p in tx]) + '\n')
        result = subprocess.run([str(exe), '--h4', str(path)], capture_output=True, text=True, check=True)
    fields = dict(item.split('=') for item in result.stdout.split()[1:])
    fields = {k: int(v) for k, v in fields.items()}
    vendor = sum(1 for p in tx if len(p) >= 3 and (p[2] & 0xfc) == 0xfc)
    return fields, skipped, len(tx), vendor


def bridge_check(records, exe, stop_time):
    """同样的记录按时间顺序走整个桥：发送记录当协议栈写来的，接收记录当串口上来的。返回桥的统计和厂商命令种类。"""
    lines, pending, sent = [], None, []
    for t, direction, length, head in records:
        if direction == 'Rx':
            if length <= HEAD_BYTES and length == len(head):
                lines.append('R ' + head.hex())
        elif t > stop_time:
            if length == 1 and head[0] in (0x01, 0x02):
                pending = head
            elif pending is not None:
                if length == len(head):
                    lines.append('T ' + (pending + head).hex())
                    sent.append(pending + head)
                pending = None
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'bridge.txt'
        path.write_text('\n'.join(lines) + '\n')
        result = subprocess.run([str(exe), '--bridge', str(path)], capture_output=True, text=True, check=True)
    rows = result.stdout.splitlines()
    fields = {k: int(v) for k, v in (item.split('=') for item in rows[0].split()[1:])}
    return fields, [row.split()[1] for row in rows[1:]], sent


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--generated', type=Path, required=True, help='Test/replay.c 的输出')
    parser.add_argument('--h4', type=Path, help='Test/replay.c 的可执行文件：同时回放接收记录')
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()
    records = parse_trace(args.trace)
    writes, stream, wild, starts, events = parse_generated(args.generated)
    # 比到第一条 HCI_Reset 为止：之后是协议栈的流量
    reset = bytes.fromhex('01030c00')
    stop = next(t for t, d, n, h in records if d == 'Tx' and h == reset)
    stats, problems, seen, log = compare(records, writes, stream, wild, starts, stop)
    unseen = [i for i in range(len(writes)) if i not in seen]
    print(f'生成：{len(writes)} 次写，{len(stream)} 字节；占位 {sum(wild)} 字节')
    print(f'跟踪：HCI_Reset 之前发送记录 {stats["records"]} 条，其中单字节 IBS 在序列里 {stats["ibs_inline"]} 条、额外 {stats["ibs_extra"]} 条')
    print(f'比对：记录覆盖 {stats["covered"]} 字节（{stats["covered"] * 100 / len(stream):.1f}%），其中逐字节比过 {stats["head_bytes"]} 字节')
    print(f'缺口：{stats["gaps"]} 处共 {stats["gap_bytes"]} 字节没有记录（环形缓冲丢失），占位段 {stats["wild"]} 条只比帧头，单字节非 IBS 记录 {stats["one_byte"]} 条（判别力弱）')
    short = [writes[i].hex() for i in unseen if len(writes[i]) <= 8]
    print(f'没有被任何记录碰到的包：{len(unseen)} 个，其中短命令 {short}')
    for t, byte, cursor in log:
        print(f'  额外 IBS：{t:.6f} {byte}（读路径对收到的 0xfd 的应答，序列里的位置取决于丢失的 RX）')
    baud = [e for e in events if e[0] == 'B']
    if baud != [('B', '8000000', baud[0][2] if baud else 0)]:
        problems.append(f'主机换速记录不对：{baud}')
    if args.h4:
        fields, skipped, tx, vendor = h4_check(records, args.h4, stop)
        print(f'H4：接收记录全的走成帧（截断的 {skipped} 条不喂），IBS {fields["ibs"]}，帧 {fields["frames"]}，上交 {fields["pass"]}，丢弃 {fields["drop"]}，'
              f'坏包 {fields["bad"]}，杂字节 {fields["junk"]}；发送 {tx} 包，透传 {fields["tx_pass"]}，就地应答 {fields["tx_answered"]}')
        if fields['junk'] or fields['oversize'] or fields['bad']:
            problems.append(f'H4 回放有杂字节或坏包：{fields}')
        if fields['tx_answered'] != vendor or fields['tx_bad'] or fields['tx_pass'] + fields['tx_answered'] != tx:
            problems.append(f'发送过滤不对：{fields}，厂商命令 {vendor} 条')
        bridge, kinds, sent = bridge_check(records, args.h4, stop)
        forwarded = [p for p in sent if not (p[0] == 1 and (p[2] & 0xfc) == 0xfc)]
        print(f'桥：协议栈写 {len(sent)} 包，下发 {bridge["out"]}（{bridge["wire"]} 字节），就地回 {bridge["answered"]}（{bridge["kinds"]} 种：{" ".join(kinds)}），'
              f'上交 {bridge["up"]} 帧，丢弃 {bridge["dropped"]}，0xfc {bridge["acks"]} 个')
        if bridge['guard'] or bridge['bad'] or bridge['junk']:
            problems.append(f'桥的回放不干净：{bridge}')
        if bridge['out'] != len(forwarded) or bridge['wire'] != sum(map(len, forwarded)) or bridge['answered'] != vendor:
            problems.append(f'桥下发的和发送记录对不上：{bridge}，应下发 {len(forwarded)} 包')
        if bridge['dropped'] != fields['drop'] or bridge['up'] != fields['pass'] + bridge['answered']:     # 上交的还包括就地回的
            problems.append(f'桥上交的和接收记录对不上：{bridge}，{fields}')
    if problems:
        print('\n'.join(problems[:40]))
        print(f'FAIL replay: {len(problems)} 处对不上')
        sys.exit(1)
    print(f'PASS replay: HCI_Reset 之前 {stats["records"]} 条发送记录全部吻合，逐字节比过 {stats["head_bytes"]} 字节，缺口 {stats["gap_bytes"]} 字节未覆盖')


if __name__ == '__main__':
    main()
