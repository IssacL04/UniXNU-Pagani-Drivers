#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""从一次运行的日志判断声音和震动的链路走到哪一段，停在哪一段。只读日志，不碰手机。

  sound-verdict.py 运行目录或 merged.log [--args '启动参数']

adsp-rounds.py 判 ADSP、功放、后端交接；这里判它不看的那几段：iOS 路由、插件 IO、音量、后端里有没有真样本、功放输出级、震动。
内核提示音（C6、C7）也判到功放这一段，响没响要人听。每段一行：
  好    日志证实走通
  停    这一段没走通，后面的段看不了（第一条“停”就是要查的地方）
  缺    这一轮本来就没开这一段（启动参数没带）
  等人  日志走通，但要人做一下或者听一下才算数
退出码：0 没有“停”，1 有“停”。"""
import argparse
import re
import sys
from pathlib import Path

GOOD, STOP, OFF, HUMAN = '好', '停', '缺', '等人'
VA_SETTINGS = {0: '还没动', 1: '已在初始化之前给上', 2: '给上了，但设备管理器已存在，可能晚了', 3: '已经有，没写', 4: '拒绝：不是 21H461 的 VirtualAudio',
               5: '拒绝：产品不是 J120'}


def fields(text):
    return dict(re.findall(r'(\w+)=(\S+)', text))


def number(text):
    try:
        return int(str(text), 0)
    except (TypeError, ValueError):
        return None


class Log:
    """merged.log 的 U 行和启动参数。"""

    def __init__(self, text, arguments=None):
        self.lines = []
        for line in text.splitlines():
            match = re.match(r'\s*\d+\s+[\d.]+\s+(\w)\s+(.*)$', line)
            if match:
                self.lines.append(match.group(2))
                if match.group(1) == 'A' and arguments is None:
                    arguments = match.group(2)
        self.arguments = dict(w.split('=', 1) if '=' in w else (w, '1') for w in (arguments or '').split())

    def all(self, prefix):
        return [t for t in self.lines if t.startswith(prefix)]

    def last(self, prefix):
        found = self.all(prefix)
        return found[-1] if found else ''

    def arg(self, name):
        return number(self.arguments.get(name)) or 0


def route_part(log, out):
    """iOS 路由：VirtualAudio 初始化、串行化监听、Thunderbolt 端口。"""
    route, mx = log.last('audio route #'), fields(log.last('audio mx '))
    if not route:
        out.append(('路由', STOP, '没有 audio route 行：探针没跑（pagani-audio-probe=0？）或插件没加载'))
        return False
    r = fields(route)
    words = route.split()
    vad = number(r.get('vad')) or 0
    if 'va' in mx:
        settings = number(mx['va']) or 0
        status = GOOD if settings in (1, 3) else STOP if settings in (4, 5) else GOOD if settings == 2 and vad else STOP if settings == 2 else GOOD
        out.append(('VirtualAudio 马达设置', status, VA_SETTINGS.get(settings, str(settings))))
    if not vad:
        haptics = log.arg('pagani-haptics-ios')
        why = ('VirtualAudio 的延迟初始化没走完：产品 J120 没有马达设置，建路由库时抛 Invalid Product Type。带 pagani-haptics-ios 时 pagani-audio-srzl 要有 bit 1，'
               '或者去掉 pagani-haptics-ios') if haptics and 'va' not in mx else 'VirtualAudio 没发布默认虚拟设备：加 pagani-audio-trace=1 看 audio vaerr 的抛出点'
        out.append(('VirtualAudio 初始化', STOP, f'vad=0 cat={r.get("cat")} vcsp={r.get("vcsp")}（vc={r.get("vc")} 是读失败算成的，音量条因此能出来，后面什么都没有）。{why}'))
        return False
    out.append(('VirtualAudio 初始化', GOOD, f'vad={vad} cat={r.get("cat")} mode={r.get("mode")}'))
    ports = number(r.get('out')) or 0
    letters = re.search(r' out=\d+ (\S{4}) (\S{4}) ', route)
    if not ports:
        t1 = letters.group(2) if letters else '????'
        out.append(('路由', STOP, f'默认虚拟设备没有活动输出端口，t1={t1}：s 串行化监听丢了（pagani-audio-srzl bit 0），h 设备没被接管，0 没有端口，SH1T 路由没选它。audio mx srzl={mx.get("srzl")}'))
        return False
    first = letters.group(1) if letters else '?'
    vcsp = r.get('vcsp', '')
    if first != 'ptbo' or not vcsp.startswith('1/'):
        out.append(('路由', STOP, f'端口 {first} vcsp={vcsp}：要 ptbo 和 vcsp=1/0'))
        return False
    out.append(('路由', GOOD, f'out={ports} ptbo vcsp={vcsp}（{len(words)} 列）'))
    return True


def io_part(log, out):
    """插件 IO 和内核环。"""
    plug, svc = fields(log.last('audio plug ')), log.last('audio svc ')
    writes, sets = number(plug.get('writes')) or 0, number(plug.get('sets')) or 0
    starts = re.search(r' out=(\d+)/(\d+)/(\d+) ', svc)
    if not writes or not starts or not int(starts.group(1)):
        out.append(('IO', HUMAN, f'writes={writes}，没有 IO：路由通了但还没人放声音，让用户放一段音乐再看'))
        return False
    peak = re.search(r' peak=(\d+)/', svc)
    if peak and not int(peak.group(1)):
        out.append(('IO', STOP, f'writes={writes} 但环里全是 0（peak=0）：iOS 在写静音'))
        return False
    out.append(('IO', GOOD, f'writes={writes} out={starts.group(0).strip()} {peak.group(0).strip() if peak else ""}'))
    out.append(('音量', GOOD if sets else HUMAN, f'sets={sets} vol={plug.get("vol")}' + ('' if sets else '：让用户按一下音量键，sets 应该涨；不涨就是音量没到插件')))
    return True


def amps_part(log, out, name):
    """功放状态行。没进工作态写一条“停”，返回 None；否则返回附在结论后面的话。
    sws、ampen 是读的那一刻的状态；noclk、tdmerr 读一次清一次，可能是起时钟时留下的，只记下来，不算停。"""
    amps = [fields(t) for t in log.all('[spk] amp 0x3') if 'state=' in t]
    bad = [a for a in amps if (a.get('state'), a.get('clks'), a.get('plls')) != ('9', '1', '1') or a.get('sws', '1') != '1' or a.get('ampen', '1') != '1']
    if not amps or bad:
        out.append((name, STOP, f'功放没进工作态：{bad[-1] if bad else "没有 amp state 行"}'))
        return None
    sticky = [a for a in amps if a.get('noclk', '0') != '0' or a.get('tdmerr', '0') != '0']
    return f'（有过 noclk={sticky[-1].get("noclk")} tdmerr={sticky[-1].get("tdmerr")}，没声时先看它）' if sticky else ''


def tone_part(log, out):
    """内核提示音：不经过 iOS，日志到功放为止。"""
    tones = [fields(t) for t in log.all('[spk] tone rc=')]
    if not tones:
        out.append(('提示音', STOP, f'没有 tone 行：{log.last("[spk] ") or "没有 [spk] 行"}'))
        return
    bad = [t for t in tones if t.get('rc') != '0' or t.get('clks_failed') != '0' or not number(t.get('played')) or not number(t.get('out'))]
    if bad:
        out.append(('提示音', STOP, f'提示音没放完：{bad[-1]}'))
        return
    note = amps_part(log, out, '提示音')
    if note is not None:
        last = tones[-1]
        out.append(('提示音', HUMAN, f'{len(tones)} 声放完（played={last.get("played")} out={last.get("out")}），功放工作态{note}：问用户听到没有；日志全过却没声，下一张试 pagani-speaker-sd=2（只在 feedback=0 时）'))


def speaker_part(log, out):
    """adsp 后端里有没有真样本，功放开没开。"""
    if not log.last('[spk] backend adsp handed to the PaganiAudio service'):
        out.append(('后端', STOP, f'adsp 后端没交给 PaganiAudio：{log.last("[spk] ") or "没有 [spk] 行"}'))
        return False
    out.append(('后端', GOOD, 'adsp 后端已接管'))
    stops = [fields(t) for t in log.all('[spk] play stop ')]
    if not stops:
        out.append(('放音', HUMAN, '后端没跑过：让用户放一段音乐，等 5 秒停下才有 play stop 行'))
        return False
    loud = [s for s in stops if (number(s.get('in')) or 0) > 0]
    if not loud:
        out.append(('放音', STOP, f'play stop 的 in 全是 0：环里的静音送进了 DSP（{len(stops)} 次）'))
        return False
    worst = loud[-1]
    if (number(worst.get('played')) or 0) < (number(worst.get('sent')) or 0) - 8 or any(worst.get(k) != '0' for k in ('werr', 'stalls', 'clamp')):
        out.append(('放音', STOP, f'DSP 没把样本吃下去：{worst}'))
        return False
    note = amps_part(log, out, '放音')
    if note is None:
        return False
    out.append(('放音', HUMAN, f'样本进了 DSP（in={worst.get("in")} out={worst.get("out")} played={worst.get("played")}），功放工作态{note}：问用户听没听到；没听到先看 pagani-speaker-sd'))
    return True


def haptics_part(log, out):
    if not log.arg('pagani-haptics-ios'):
        out.append(('震动', OFF, 'pagani-haptics-ios 没开'))
        return
    if log.last('[haptics] FAULT') and not log.last('[haptics] ready'):
        out.append(('震动', STOP, f'{log.last("[haptics] FAULT")}：pagani-audio-srzl 的 bit 2 让 SPMI 丢包重发'))
        return
    plug, svc = fields(log.last('audio haptics plug ')), fields(log.last('audio haptics runs='))
    if not number(plug.get('vad')):
        out.append(('震动', STOP, f'VirtualAudioDevice_Actuator 不存在（vad=0 hog={plug.get("hog")}）：iOS 没有地方送触感，马达一次也不会动'))
        return
    runs = (svc.get('runs') or '0/0').split('/')[0]
    if runs == '0':
        out.append(('震动', HUMAN, f'虚拟设备在（vad={plug.get("vad")}），还没有客户端起 IO：让用户打字或者开关一下静音'))
        return
    streams = re.search(r' streams=(\d+)/', log.last('audio haptics runs=') or '')
    if not streams or not int(streams.group(1)) or not log.last('[haptics] stream'):
        out.append(('震动', STOP, f'有 IO 但马达流没起来：{log.last("audio haptics runs=")}'))
        return
    out.append(('震动', HUMAN, f'流起来了（{streams.group(0).strip()}）：问用户手上感觉到没有'))


def verdict(log):
    out = []
    if log.arg('pagani-speaker') == 1 and log.arg('pagani-audio-tone'):
        tone_part(log, out)
    if log.arg('pagani-audio') != 1:
        return out or [('服务', OFF, 'pagani-audio=1 没开')]
    if not log.last('audio: open '):
        out.append(('插件', STOP, '没有 audio: open 行：audiomxd 没打开服务（种子没带插件，或者 audiomxd 没起来）'))
        return out
    out.append(('插件', GOOD, log.last('audio: open ')))
    speaker = log.arg('pagani-speaker') == 1 and log.arg('pagani-adsp') == 1
    if not speaker:
        out.append(('后端', OFF, 'pagani-adsp=1 pagani-speaker=1 没开：后端是 silent，插件写的样本被内核清掉，这一轮没声音是必然的'))
    if not route_part(log, out):
        out.append(('声音', STOP, '路由没通，iOS 不会写样本：后面的段不用看'))
    elif io_part(log, out) and speaker:
        speaker_part(log, out)
    haptics_part(log, out)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('path', type=Path)
    parser.add_argument('--args', help='启动参数（日志里有 A 行时不用给）')
    options = parser.parse_args()
    path = options.path / 'merged.log' if options.path.is_dir() else options.path
    rows = verdict(Log(path.read_text(errors='replace'), options.args))
    for name, status, text in rows:
        print(f'{status:　<2} {name}：{text}')
    return 1 if any(status == STOP for _, status, _ in rows) else 0


if __name__ == '__main__':
    sys.exit(main())
