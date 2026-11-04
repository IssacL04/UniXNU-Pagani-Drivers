#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiBt 的主机检查：可移植源码先严格编译一遍，再逐项用 ASan/UBSan 和 -Werror 跑测试。

  tlv       0x31 帧、HCI EDL 帧、243 字节分段、NVM 改写（越界全拒绝且不留半截）、事件解析、启动参数、本机地址
  h4        成帧（IBS 只在帧边界、帧里同值是数据、超长帧）、接收和发送的过滤矩阵（OGF 0x3f 就地回 0x01）
  bridge    阶段 4 的桥对假 bluetoothd 和控制器模型：真初始化序列（流式切分）、厂商命令当场回、控制器没起来时留着、流控不丢帧、守卫、轮询档位、替身
  bridge-threads  同一个桥用真线程在 ThreadSanitizer 下跑
  glue      PaganiBtHost.cpp 对内核替身（Test/kstub）：属性类型和字节序、控制名、连接被拒时的回调、先撤后杀、半路失败不留东西
  fixups    integration/text-fixups.txt 对只读根卷的 bluetoothd：偏移、原字节、指令的含义、payload.py 的格式（没有根卷时跳过并说明）
  ibs       两个方向互相独立、10 ms 重发、次数用完、默认常醒
  geni      UART 寄存器层对 GENI 模型：配置值、打包、部分字、流控、超时取消、错误位
  pmic      SPMI 仲裁器模型：只写 PPID 0x58a、owner 和子类型检查、读回、低高电平
  tables    PaganiBtBoard.h 和 PaganiWifiTables.h 的轨道一致，速率表自洽
  bringup   上电序列对模型：顺序、数值、S7I 规则、每条失败路径都停在 BT_EN 之前、勘察不写寄存器
  download  完整引导序列对控制器模型：逐段等待规则、Tag 改写、换速、错误注入
  firmware  mtools 做的 FAT 卷（4 KiB 扇区）上读五个文件，缺文件、坏卷、分区后到
  args      PaganiBt.cpp 读的启动参数都写在 README.md 和 PaganiBt.h 里
  replay    真固件和 Android hsuart 跟踪逐条比对（缺文件时跳过并说明，文件不进被跟踪路径）

  check-pagani-bt-host.py [--tree TREE] [--firmware DIR] [--trace FILE] [--root DIR] [--keep DIR]

--firmware、--trace 也可以用环境变量 PAGANI_BT_FIRMWARE、PAGANI_BT_TRACE，默认在 build-userspace/a830-inputs/round-1101/bt-local。
--root 也可以用环境变量 PAGANI_BT_ROOT，默认是 build-userspace/a830-inputs/osmnt-21H461。

--tree 时再做一次内核归档构建，输出放临时目录。主机检查不能证明任何东西在手机上能工作。"""
import argparse
import hashlib
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
CORE = ['BtTlv.c', 'BtH4.c', 'BtIbs.c', 'BtGeni.c', 'BtPmic.c', 'BtFat.c', 'BtFirmware.c', 'BtDownload.c', 'BtBringup.c', 'BtBridge.c', 'BtStandIn.c']
BRIDGE = ['BtBridge.c', 'BtStandIn.c', 'BtH4.c', 'BtIbs.c', 'BtTlv.c']
TESTS = {
    'tlv': ['Test/tlv.c', 'BtTlv.c'],
    'h4': ['Test/h4.c', 'BtH4.c'],
    'ibs': ['Test/ibs.c', 'BtIbs.c'],
    'geni': ['Test/geni.c', 'BtGeni.c'],
    'pmic': ['Test/pmic.c', 'BtPmic.c'],
    'bringup': ['Test/bringup.c', 'BtBringup.c', 'BtGeni.c', 'BtDownload.c', 'BtH4.c', 'BtIbs.c', 'BtTlv.c'],
    'download': ['Test/download.c', 'BtDownload.c', 'BtH4.c', 'BtIbs.c', 'BtTlv.c'],
    'bridge': ['Test/bridge.c', *BRIDGE],
}
STRICT = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Wno-sign-conversion', '-Wcast-qual',
          '-Wstrict-prototypes', '-Wmissing-prototypes', '-O2']
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror'] + SANITIZE
CXXFLAGS = ['-std=c++17', '-Wall', '-Wextra', '-Werror'] + SANITIZE
LOCAL = SOURCE.parents[2] / 'build-userspace/a830-inputs/round-1101/bt-local'     # 不被跟踪的本地目录，固件和跟踪不进仓库
DEFAULT_FIRMWARE = LOCAL / 'btfw'
DEFAULT_TRACE = LOCAL / 'hsuart.trace'
DEFAULT_ROOT = SOURCE.parents[2] / 'build-userspace/a830-inputs/osmnt-21H461'     # 只读根卷，苹果文件不进仓库
BLUETOOTHD_SHA256 = '3f1e4493ea6a76976625f7af58a8f330c0c90d82d9020b11110b4f3f5b758df4'


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}')
    return result.stdout


def last_line(text):
    lines = text.strip().splitlines()
    return lines[-1] if lines else '(no output)'


def check_arguments():
    """PaganiBt.cpp 读的启动参数都写在 README.md 的表和 PaganiBt.h 里，也没有写多。"""
    read = set(re.findall(r'argument\("(pagani-[a-z0-9-]+)"', (SOURCE / 'PaganiBt.cpp').read_text()))
    if read != {'pagani-bt'}:
        raise SystemExit(f'FAIL args: PaganiBt.cpp reads {sorted(read)}')
    readme = (SOURCE / 'README.md').read_text().split('## 启动参数', 1)[-1].split('\n## ', 1)[0]
    header = (SOURCE / 'PaganiBt.h').read_text().split('#ifndef', 1)[0]
    problems = []
    for where, text in (('README.md', readme), ('PaganiBt.h', header)):
        named = set(re.findall(r'(?<![a-z-])(pagani-bt(?:-[a-z0-9]+)*)', text))
        problems += [f'{name} is read but not in {where}' for name in sorted(read - named)]
        problems += [f'{where} names {name}, which is not read' for name in sorted(named - read)]
    if problems:
        raise SystemExit('FAIL args: ' + '; '.join(problems))
    print('PASS args: 1 boot argument, documented')


def check_quiet():
    """UFS 日志写者的保持字是按位用的，PaganiPanel 占 bit 0，PaganiBattery 的 USB 探针占 bit 1：这里写成整字存储就放开了它们的保持。模型只看得到回调，接线靠这一条。"""
    glue = (SOURCE / 'PaganiBt.cpp').read_text()
    bit = re.search(r'#define PB_QUIET_BIT (\d+)u', (SOURCE / 'PaganiBtBoard.h').read_text())
    wanted = ('__atomic_fetch_or(&pagani_panel_probe_quiet, PB_QUIET_BIT, __ATOMIC_RELEASE)', '__atomic_fetch_and(&pagani_panel_probe_quiet, ~PB_QUIET_BIT, __ATOMIC_RELEASE)',
              'hwSleepClock, hwBtEn, hwLog, hwQuiet};')
    missing = [text for text in wanted if text not in glue]
    if missing or '__atomic_store_n(&pagani_panel_probe_quiet' in glue or not bit or int(bit.group(1)) != 4:
        raise SystemExit(f'FAIL quiet: {missing or "保持字里的位"}')
    print('PASS quiet: 第一次读 SE 用自己的一位持住写者')


def make_volume(tmp, name, files, rng):
    """FAT16、4 KiB 扇区的卷（手机分区就是这样），先放几个文件删掉一半，让后面的文件有碎片。"""
    image = tmp / name
    with open(image, 'wb') as f:
        f.truncate(128 << 20)
    run(['mformat', '-i', image, '-M', '4096', '-c', '4', '-T', '32768', '-h', '1', '-s', '32768', '::'])
    run(['mmd', '-i', image, '::image'])
    holes = []
    for i in range(6):
        hole = tmp / f'{name}.hole{i}'
        hole.write_bytes(rng.randbytes(9000))
        run(['mcopy', '-i', image, hole, f'::image/hole{i}.bin'])
        holes.append(f'::image/hole{i}.bin')
    for hole in holes[::2]:
        run(['mdel', '-i', image, hole])
    for path, data in files.items():
        source = tmp / (name + '.' + path)
        source.write_bytes(data)
        run(['mcopy', '-i', image, source, '::image/' + path])
    return image


def test_firmware(out, tmp):
    exe = out / 'firmware'
    run(['cc', *CFLAGS, SOURCE / 'Test/firmware.c', SOURCE / 'BtFirmware.c', SOURCE / 'BtFat.c', '-o', exe])
    rng = random.Random(1101)
    files = {'brhperifw20.mbn': b'\x7fELF' + rng.randbytes(25416), 'brhperinv20.bin': rng.randbytes(741),
             'brhbtfw20.mbn': b'\x7fELF' + rng.randbytes(228868), 'brhbtnv20.bin': rng.randbytes(13227)}
    modem = {'tmel_peach_20.elf': b'\x7fELF' + rng.randbytes(55200)}
    staged = tmp / 'staged'
    staged.mkdir()
    for name, data in {**files, **modem}.items():
        (staged / name).write_bytes(data)
    bt = make_volume(tmp, 'bluetooth_a.img', files, rng)
    md = make_volume(tmp, 'modem_a.img', modem, rng)
    text = run([exe, bt, md, staged])
    print(last_line(text))


def test_tables(out):
    exe = out / 'tables'
    run(['c++', *CXXFLAGS, SOURCE / 'Test/tables.cpp', '-o', exe])
    print(last_line(run([exe])))


def test_threads(out):
    exe = out / 'bridge-threads'
    run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=thread', '-fno-sanitize-recover=all', SOURCE / 'Test/bridge_threads.c',
         *(SOURCE / name for name in BRIDGE), '-o', exe])
    print(last_line(run([exe])))


def test_glue(out):
    objects = []
    for name in BRIDGE:
        objects.append(out / (name + '.glue.o'))
        run(['cc', *CFLAGS, '-c', SOURCE / name, '-o', objects[-1]])
    exe = out / 'glue'
    run(['c++', *CXXFLAGS, '-I', SOURCE / 'Test/kstub', '-I', SOURCE, SOURCE / 'Test/host_glue.cpp', SOURCE / 'PaganiBtHost.cpp', *objects, '-o', exe])
    print(last_line(run([exe])))


def fixup_entries():
    """integration/text-fixups.txt 的条目：名字、偏移、原字节、新字节。"""
    entries = []
    for line in (SOURCE / 'integration/text-fixups.txt').read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if not re.fullmatch(r'[A-Za-z0-9_.\-]{1,63},[0-9a-fA-F]{1,16},(?:[0-9a-fA-F]{2}){1,16},(?:[0-9a-fA-F]{2}){1,16}', line) or len(line) >= 160:
            raise SystemExit(f'FAIL fixups: {line} 不合 payload.py 的格式')
        name, offset, original, replacement = line.split(',')
        entries.append((name, int(offset, 16), bytes.fromhex(original), bytes.fromhex(replacement)))
    return entries


def check_fixups(root):
    """text-fixups 只在原字节吻合时才改，写错了不报错，只是悄悄不生效，所以这里对真文件核对；再看指令本身是不是说的那条。"""
    entries = fixup_entries()
    for name, offset, original, replacement in entries:
        if name != 'bluetoothd' or len(original) != len(replacement) or (offset & 0x3fff) + len(original) > 0x4000:
            raise SystemExit(f'FAIL fixups: {name} {offset:#x}')
    path = root / 'usr/sbin/bluetoothd'
    if not path.is_file():
        print(f'SKIP fixups: 没有只读根卷 {path}（用 --root 指定），只查了格式')
        return
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != BLUETOOTHD_SHA256:
        raise SystemExit('FAIL fixups: bluetoothd 不是核对过的那个版本')
    for name, offset, original, replacement in entries:
        if data[offset:offset + len(original)] != original:
            raise SystemExit(f'FAIL fixups: {name}+{offset:#x} 的原字节是 {data[offset:offset + len(original)].hex()}')
    by_offset = {offset: replacement for _, offset, _, replacement in entries}
    # sub_1002EF860 → return 0：mov w0, #0; ret
    if struct.unpack('<2I', by_offset[0x2ef860]) != (0x52800000, 0xd65f03c0):
        raise SystemExit('FAIL fixups: F2 不是 mov w0, #0; ret')
    # strcmp 后的 CBZ w0：往前一条是 BL，改成 NOP 之后落到 TIOCGETA，不走 Skywalk
    cbz, = struct.unpack_from('<I', data, 0xddd20)
    bl, = struct.unpack_from('<I', data, 0xddd1c)
    if cbz & 0xff00001f != 0x34000000 or bl >> 26 != 0x25 or by_offset[0xddd20] != struct.pack('<I', 0xd503201f):
        raise SystemExit(f'FAIL fixups: F3 的上下文不对 {bl:#x} {cbz:#x}')
    print(f'PASS fixups: {len(entries)} 条对 bluetoothd 核对过')


def check_host_glue():
    """kern_control 的规矩，模型只替得了接口，替不了这几条：连接被拒时回调也会来（unitinfo 空），链要自己放。"""
    glue = (SOURCE / 'PaganiBtHost.cpp').read_text()
    wanted = ('if (unitinfo) pb_bridge_disconnect(gBridge, unit);', 'mbuf_freem(chain);', 'control.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;')
    missing = [text for text in wanted if text not in glue]
    if missing or 'CTL_FLAG_PRIVILEGED' in glue.split('namespace {', 1)[1]:
        raise SystemExit(f'FAIL glue rules: {missing}')
    # PaganiBt.cpp 的线程没有主机测试：发布在起线程之前，替身不起线程，失败走撤销，成功才交给桥
    main = (SOURCE / 'PaganiBt.cpp').read_text()
    order = [main.find(text) for text in ('pagani_bt_host_publish(gAddress', 'if (gArg.stand_in) {', 'kernel_thread_start(btThread')]
    wiring = ('if (rc) pagani_bt_host_failed();\n        else pagani_bt_host_run(&gDl.h4, gDl.rx + gDl.rx_pos, gDl.rx_len - gDl.rx_pos);', 'if (!rc && arg.phase == 3) {')
    if -1 in order or order != sorted(order) or any(text not in main for text in wiring):
        raise SystemExit(f'FAIL glue rules: PaganiBt.cpp 的接线 {order}')
    print('PASS glue rules: 不设 PRIVILEGED，空 unitinfo 不断开，链自己放，发布在起线程之前')


def test_replay(out, firmware, trace):
    missing = [str(p) for p in (trace, firmware / 'brhbtfw20.mbn', firmware / 'brhbtnv20.bin', firmware / 'brhperifw20.mbn',
                                firmware / 'brhperinv20.bin', firmware / 'tmel_peach_20.elf') if not p.is_file()]
    if missing:
        print('SKIP replay: 缺本地文件（固件和跟踪不进被跟踪路径，用 --firmware、--trace 指定）: ' + ', '.join(missing))
        return False
    exe = out / 'replay'
    run(['cc', *CFLAGS, SOURCE / 'Test/replay.c', *(SOURCE / name for name in ('BtDownload.c', *BRIDGE)), '-o', exe])
    dump = out / 'replay.tx'
    run([exe, firmware, dump])
    text = run([sys.executable, SOURCE / 'Test/replay-hsuart.py', '--trace', trace, '--generated', dump, '--h4', exe])
    print(next(line for line in text.splitlines() if line.startswith('桥：')))
    print(last_line(text))
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, help='also build the kernel archive with this tree (output in a temporary directory)')
    parser.add_argument('--firmware', type=Path, default=Path(os.environ.get('PAGANI_BT_FIRMWARE', DEFAULT_FIRMWARE)), help='local firmware copies')
    parser.add_argument('--trace', type=Path, default=Path(os.environ.get('PAGANI_BT_TRACE', DEFAULT_TRACE)), help='local hsuart trace')
    parser.add_argument('--root', type=Path, default=Path(os.environ.get('PAGANI_BT_ROOT', DEFAULT_ROOT)), help='read-only OS volume, for the text fix-ups')
    parser.add_argument('--only', help='comma separated test names')
    parser.add_argument('--keep', type=Path, help='build and test files here instead of a temporary directory')
    args = parser.parse_args()
    only = set(args.only.split(',')) if args.only else None
    with tempfile.TemporaryDirectory() as temporary:
        out = args.keep or Path(temporary)
        out.mkdir(parents=True, exist_ok=True)
        for name in CORE:
            run(['cc', *STRICT, '-c', SOURCE / name, '-o', out / (name + '.o')])
        print(f'PASS strict: {len(CORE)} portable sources')
        for name, sources in TESTS.items():
            if only and name not in only:
                continue
            exe = out / name
            run(['cc', *CFLAGS, *(SOURCE / s for s in sources), '-o', exe])
            print(last_line(run([exe])))
        if not only or 'tables' in only:
            test_tables(out)
        if not only or 'bridge-threads' in only:
            test_threads(out)
        if not only or 'glue' in only:
            test_glue(out)
            check_host_glue()
        if not only or 'fixups' in only:
            check_fixups(args.root)
        if not only or 'firmware' in only:
            work = out / 'fw'
            work.mkdir(exist_ok=True)
            test_firmware(out, work)
        if not only or 'args' in only:
            check_arguments()
            check_quiet()
        replayed = test_replay(out, args.firmware, args.trace) if not only or 'replay' in only else None
        if args.tree:
            print(last_line(run([sys.executable, SOURCE / 'scripts/build-pagani-bt.py', '--tree', args.tree, '--out', out / 'kernel'])))
    print('PASS all' + ('' if replayed or replayed is None else ' (replay skipped)'))


if __name__ == '__main__':
    main()
