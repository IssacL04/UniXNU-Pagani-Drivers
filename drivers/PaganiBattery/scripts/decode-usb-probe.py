#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""The lines of the USB probe (BatteryUsb.h) read out: what state USB was left in, the DWC version, the charger's watchdog and limits, and
where the run stopped when it did not finish. Reads a run's merged.log (or anything with "[usbp] " lines), writes the report to stdout.
Only the "[usbp]" lines are used; nothing is ever sent to the phone.

The charger's meanings are readings of Android's register dumps and of the SMB5 register map (BatterySpmi.h): what the phone has not
confirmed is marked 未证实.

  decode-usb-probe.py merged.log"""
import argparse
import re
import sys
from pathlib import Path

STAGES = ['inhibit（抑制）', 'trickle（涓流）', 'pre（预充）', 'fullon（快充）', 'taper（恒压收尾）', 'terminate（充满截止）', 'pause（暂停）', 'disabled（关闭）']
ROLE = {0: '未定', 1: '主机', 2: '设备', 3: 'OTG'}
ROUTE = {
    'inherit': 'P1 走继承路线：GDSC、时钟都开，设备模式，PHY 未掉电',
    'host': 'P1 要先停 xHCI：控制器在主机模式',
    'cold': 'P1 走冷启动路线：控制器未上电（报告估计工期加 2 天）',
    'mixed': '上电了但不是干净的设备模式，看下面各项',
    'noid': '控制器读回的不是 Synopsys ID，先查寄存器窗口',
}
SNPS = {0x5533: 'DWC_usb3', 0x3331: 'DWC_usb31', 0x3332: 'DWC_usb32'}


def fields(text):
    """key=value of a line: numbers where they are numbers."""
    out = {}
    for key, value in re.findall(r'(\w+)=(\S+)', text):
        try:
            out[key] = int(value, 0)
        except ValueError:
            out[key] = value
    return out


def parse(lines):
    probe, order = {}, []
    for line in lines:
        at = line.find('[usbp] ')
        if at < 0:
            continue
        text = line[at + 7:].rstrip()
        parts = text.split(' ', 1)
        name = parts[0]
        if name == 'smmu' and text.startswith('smmu bank'):
            name = 'bank'
        probe.setdefault(name, []).append(text)
        order.append(name)
    return probe, order


def window(text):
    """{offset: byte or None} of a peripheral line ("p726 04=.. 40=..")."""
    out = {}
    for first, data in re.findall(r' ([0-9a-f]{2})=((?:[0-9a-f]{2}|--)+)', text):
        for i in range(0, len(data), 2):
            pair = data[i:i + 2]
            out[int(first, 16) + i // 2] = None if pair == '--' else int(pair, 16)
    return out


def byte(page, offset):
    value = page.get(offset)
    return '不可读' if value is None else f'0x{value:02x}'


def report(probe, order, out):
    if not order:
        out('日志里没有 [usbp] 行：探针没开（pagani-battery 的第 1 位），或日志已被覆盖。')
        return 1
    done = fields(probe['done'][-1]) if 'done' in probe else None
    if done:
        route = done.get('route')
        out(f'结论：route={route}，{ROUTE.get(route, "?")}')
        if done.get('why') not in ('-', None):
            out(f'  闸门停在：{done["why"]}')
        entry = {'same': '与驱动启动时一致，所以这也是 XNU 入口时的状态', 'changed': '与驱动启动时不同，入口状态看 gcc0 行', '?': '没有 gcc0 行'}
        out(f'  GCC：{entry.get(done.get("gcc"), "?")}')
    else:
        last = order[-1]
        out(f'结论：探针没走完，最后一行是 {last}。')
        if last == 'gate' and 'next=dwc' in probe['gate'][-1]:
            out('  下一步是第一次读 DWC3：GCC 判据都过了仍然出事，说明这些判据还不够，这是最要紧的结果。')
        elif last == 'hw':
            out('  下一步是读 QSCRATCH，DWC3 本身读通了。')
        elif last == 'qs' and 'next=tcsr' in probe['qs'][-1]:
            out('  下一步是第一次读 TCSR（0xf206008）：控制器已读通，出事的是这个块。')
        elif last == 'ref' and 'next=phy' in probe['ref'][-1]:
            out('  下一步是读 M31 PHY：供电和参考时钟的判据过了仍然出事。')
        else:
            out('  这一步之前的读都是安全类，看日志是否被别的原因截断。')
    if 'gcc0' in probe and 'gcc' not in probe:
        g = fields(probe['gcc0'][-1])
        out(f'入口 GCC（gcc0）：GDSC={g["gdsc"]:#x}，master={g["mst"]:#x}，cfg_noc={g["noc"]:#x}')
    if 'gcc' in probe:
        g = fields(probe['gcc'][-1])
        out('电源与时钟：')
        out(f'  GDSC 0x139004={g["gdsc"]:#x}：PWR_ON={g["gdsc"] >> 31} SW_COLLAPSE={g["gdsc"] & 1}；CFG_GDSCR={g["cfg"]:#x}')
        names = (('master', 'mst'), ('aggre', 'agg'), ('cfg_noc', 'noc'), ('mock_utmi', 'utmi'), ('sleep', 'slp'))
        for name, key in names:
            v = g[key]
            out(f'  {name:9} {v:#010x} enable={v & 1} hw_ctl={(v >> 1) & 1} clk_off={v >> 31}')
        out(f'  master RCG {g["rcg"]:#x}，mock_utmi RCG {g["urcg"]:#x}，BCR {g["bcr"]:#x}')
    if 'gate' in probe:
        r = fields(probe['gate'][-1])
        out(f'  QUSB2PHY BCR={r["phybcr"]:#x}，USB3 PHY GDSC={r["phygdsc"]:#x}')
    if 'ref' in probe:
        r = fields(probe['ref'][-1])
        out(f'  TCSR_USB2_CLKREF_EN={r["tcsr"]:#x}')
    if 'rail' in probe:
        r = fields(probe['rail'][-1])
        states = []
        for name in ('l2d', 'l3g', 'l1f', 'l15b', 'l5b'):
            v = r.get(name)
            states.append(f'{name.upper()} ' + ('不可读' if not isinstance(v, int) or v < 0 else
                                                f'{"在稳压" if v >> 8 & 0x80 else "未稳压"}/{"使能" if v & 0x80 else "未使能"}'))
        out('供电：' + '，'.join(states))
    if 'dwc' in probe:
        d = fields(probe['dwc'][-1])
        product = d['id'] >> 16
        name = SNPS.get(product, f'未知 {product:#x}')
        if product == 0x5533:
            version = f'{(d["id"] >> 12) & 15:x}.{(d["id"] >> 4) & 255:02x}{chr(97 + (d["id"] & 15) - 10) if (d["id"] & 15) >= 10 else "?"}'
        elif 'ver' in d:
            number = d['ver'].to_bytes(4, 'big').decode('latin-1')
            kind = d['type'].to_bytes(4, 'big').decode('latin-1')
            version = f'{number[0]}.{number[1:3]}{number[3]}，类型 {kind}'
        else:
            version = '?'
        out(f'DWC：{name} {version}（GSNPSID={d["id"]:#x}）')
        if product != 0x5533:
            out('  不是 DWC_usb3：报告里 AppleUSBXDCI 路线的兼容性要重新评估。')
        if 'gctl' in d:
            out(f'  GCTL={d["gctl"]:#x}：PRTCAPDIR={ROLE[(d["gctl"] >> 12) & 3]}；GUSB2PHYCFG={d["u2"]:#x}，GUSB3PIPECTL={d["pipe"]:#x}')
    if 'dev' in probe:
        d = fields(probe['dev'][-1])
        out(f'  DCTL={d["dctl"]:#x}：RUN={d["dctl"] >> 31}；DSTS={d["dsts"]:#x}：HALTED={(d["dsts"] >> 22) & 1} 速度={d["dsts"] & 7} 链路={(d["dsts"] >> 18) & 15}')
        out(f'  事件环 {d["evt"]:#x} 大小 {d["evsz"]:#x} 计数 {d["evcnt"]:#x}（固件留下的地址，接管时要重置）')
    if 'hw' in probe:
        words = [int(v, 0) for v in re.findall(r'0x[0-9a-f]+', probe['hw'][-1])]
        if len(words) == 9:
            out(f'  GHWPARAMS3：端点 {(words[3] >> 12) & 63} 个，IN {(words[3] >> 18) & 31} 个；GHWPARAMS1：事件中断 {(words[1] >> 15) & 63} 个')
    if 'xhci' in probe:
        x = fields(probe['xhci'][-1])
        out(f'xHCI：cap={x.get("cap", 0):#x} USBCMD={x.get("cmd", 0):#x} USBSTS={x.get("sts", 0):#x}')
    if 'qs' in probe:
        q = fields(probe['qs'][-1])
        out(f'QSCRATCH：HS_PHY_CTRL={q["hs"]:#x}（bit20 VBUS 有效 {(q["hs"] >> 20) & 1}，bit28 软件选择 {(q["hs"] >> 28) & 1}），GENERAL_CFG={q["cfg"]:#x}，PWR_EVNT={q["pwr"]:#x}')
    if 'phy' in probe:
        p = dict(re.findall(r' ([0-9a-f]{4})=(0x[0-9a-f]+)', ' ' + probe['phy'][-1].split(' ', 1)[1]))
        common = int(p.get('0054', '0'), 0)
        out(f'M31 PHY：COMMON0={common:#x} SIDDQ={(common >> 2) & 1} FSEL={(common >> 4) & 7}；UTMI_CTRL5={p.get("0050")}，CTRL2={p.get("0064")}')
    if 'bank' in probe or 'smmu' in probe:
        s = fields(probe['smmu'][-1]) if 'smmu' in probe else {}
        if 'refused' in (probe.get('smmu') or [''])[-1]:
            out('SMMU：几何不符，没有读表。')
        else:
            out(f'SMMU：SID 0x40 命中 {s.get("found", "?")} 项' + (f'，SMR={s["smr"]:#x} S2CR={s["s2cr"]:#x}' if 'smr' in s else ''))
        for text in probe.get('bank', []):
            b = fields(text)
            out(f'  bank {b["n"]}：SCTLR={b["sctlr"]:#x}（M={b["sctlr"] & 1}），TTBR0={b["ttbr0"]:#x}，CBAR={b["cbar"]:#x}，FSR={b["fsr"]:#x}')
        if done and 'bank' not in probe and 's2cr' in s and not (s['s2cr'] >> 16) & 3:
            out('  这项是翻译类，但没有 smmu bank 行：读银行寄存器时出事，或日志已截断。结论行之前的数据不受影响。')
    pages = {name: window(probe[name][-1]) for name in ('p7fd', 'p726', 'p727', 'p729', 'p72b', 'p72c') if name in probe and ' none=' not in probe[name][-1]}
    if 'p7fd' in pages:
        p = pages['p7fd']
        rptr = p.get(8)
        out(f'中继 0x7fd：状态 08={byte(p, 8)}（RPTR_OK={"?" if rptr is None else rptr >> 7}），使能 46={byte(p, 0x46)}，'
            f'51-57={" ".join(byte(p, o)[2:] for o in range(0x51, 0x58))}（设备树设备模式 0f 01 07 04 03 .. 03）')
    if 'p729' in pages or 'p726' in pages:
        out('充电器（含义未证实，来自安卓转储与 SMB5 布局）：')
    if 'p729' in pages:
        p = pages['p729']
        rt = p.get(0x10)
        limit = p.get(0x52)
        if rt is not None:
            out(f'  USBIN 0x2910={rt:#x}：VBUS {"在" if (rt & 0x14) == 0x10 else "不在"}')
        if limit is not None:
            out(f'  输入限流 0x2952={limit:#x}：{limit * 50} mA')
    if 'p727' in pages:
        p = pages['p727']
        values = [p.get(o) for o in (6, 7, 8, 9)]
        out('  实际限流 0x2706-09：' + ' '.join('不可读' if v is None else f'{v * 50} mA' for v in values))
    if 'p726' in pages:
        p = pages['p726']
        status = p.get(6)
        if status is not None:
            out(f'  阶段 0x2606={status:#x}：{STAGES[status & 7]}')
        out(f'  充电使能类 0x2642={byte(p, 0x42)}，0x2650={byte(p, 0x50)}，快充电流类 0x2661={byte(p, 0x61)}，浮充类 0x2670={byte(p, 0x70)}（原始值）')
    if 'p72b' in pages:
        p = pages['p72b']
        sink = p.get(6)
        if sink is not None:
            names = {2: '3 A', 4: '1.5 A', 8: '默认'}
            out(f'  Type-C 0x2b06={sink:#x}：Rp {names.get(sink, "无或其他")}；0x2b0b={byte(p, 0xb)}')
    if 'p72c' in pages:
        p = pages['p72c']
        cfg, timing = p.get(0x51), p.get(0x53)
        out(f'  看门狗（按 SMB5 的 MISC 布局读，未证实）：0x2c51={byte(p, 0x51)}，0x2c53={byte(p, 0x53)}，状态 0x2c10={byte(p, 0x10)}')
        if cfg is not None:
            out(f'    定时器使能 bit0={cfg & 1}，插线即开 bit1={(cfg >> 1) & 1}')
        if timing is not None:
            out(f'    咬人停充 bit7={timing >> 7}，snarl/bark/bite 档 {(timing >> 4) & 7}/{(timing >> 2) & 3}/{timing & 3}')
        out('    是否真的停充，以中段电量实测的 [battery] pmic trend 行为准。')
    if done and done.get('pfail'):
        out(f'PMIC 读失败 {done["pfail"]} 字节。')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    probe, order = parse(args.log.read_text(errors='replace').splitlines())
    return report(probe, order, print)


if __name__ == '__main__':
    sys.exit(main())
