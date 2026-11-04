#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiCamera 的主机检查：可移植源码先严格编译一遍，再逐项用 ASan/UBSan 和 -Werror 跑测试。

  bringup   上电序列对硬件模型：没开时钟的块一碰就是挂总线；每一级的范围和顺序；第一次碰块时写者持住、上一行落了盘；每条失败路径都安全停下并下电；闸门的白名单逐值扫过
  cci       CCI 主机对寄存器模型：队列逐字比对，启动位，NACK 和超时的恢复
  pmic      SPMI observer 读：没有写路径，找不到 PPID，优先 EE 0 的通道
  pattern   测试图：RS 的伴随式全 0；macOS 的 CoreImage 和 Vision 从 NV12 帧里读回二维码（滚动的各个相位、两种分辨率、缩到 640 宽）
  provider  ios/CamProvider.m 跑在这台 Mac 真的 CoreMediaIO 上：匿名 XPC、会话提供者、起流、帧经 IOSurface 回来、切格式、停流；厂商类替身验证 init 的修正；
            少一个类或方法时什么都不建
  plugin    H6ISP.mediacapture 真的按 iOS 17 链一遍（不部署），导入的符号逐个在 21H461 的符号表里（表在才查）
  facts     PaganiCameraBoard.h 的数字和 running.dts 对得上（设备树在才查）
  args      PaganiCamera.cpp 读的启动参数都写在 README.md 和 PaganiCamera.h 里；保持字按位用

  check-pagani-camera-host.py [--tree TREE] [--only a,b]

--tree 时再做一次内核归档构建，输出放临时目录。主机检查不能证明任何东西在手机上能工作。"""
import argparse
import mmap
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
TOP = SOURCE.parents[2]
CORE = ['CamBringup.c', 'CamCci.c', 'CamPmic.c', 'ios/CamPattern.c']
TESTS = {
    'bringup': ['Test/bringup.c', 'CamBringup.c', 'CamCci.c'],
    'cci': ['Test/cci.c', 'CamCci.c'],
    'pmic': ['Test/pmic.c', 'CamPmic.c'],
}
STRICT = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Wno-sign-conversion', '-Wcast-qual', '-Wstrict-prototypes', '-Wmissing-prototypes', '-O2']
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror'] + SANITIZE
OBJC = ['-Wall', '-Wextra', '-Werror', '-Wno-objc-method-access', '-fno-objc-arc', *SANITIZE, '-DPAGANI_CAMERA_PLUGIN_HOST']
FRAMEWORKS = [flag for name in ('Foundation', 'CoreFoundation', 'CoreMedia', 'CoreVideo', 'CoreImage', 'IOSurface', 'CoreMediaIO') for flag in ('-framework', name)]
PROVIDER = ['Test/provider.m', 'ios/CamProvider.m', 'ios/CamPattern.c']
DT = Path(os.environ.get('PAGANI_CAM_DT', TOP / 'build-userspace/a830-inputs/dt/running.dts'))
SYMBOLS = TOP / 'build-userspace/a830-inputs/dsc-21H461.a2s'
CACHE = TOP / 'build-userspace/a830-inputs/systemos-21H461/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.stdout.write(result.stdout + result.stderr)
        raise SystemExit(f'FAIL: {" ".join(map(str, command))}')
    return result.stdout


def last_line(text):
    lines = text.strip().splitlines()
    return lines[-1] if lines else '(no output)'


def test_pattern(out):
    exe = out / 'pattern'
    run(['clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', *SANITIZE, SOURCE / 'Test/pattern.m', SOURCE / 'ios/CamPattern.c', '-framework', 'Foundation', '-framework', 'CoreImage',
         '-framework', 'CoreVideo', '-framework', 'Vision', '-framework', 'CoreGraphics', '-o', exe])
    print(last_line(run([exe])))


def test_provider(out):
    partial = ['-Wno-unused-function', '-Wno-unused-variable']      # 这两种构建只跑一小段
    for name, define in (('provider', []), ('provider-no-vendor', ['-DNO_VENDOR', *partial]), ('provider-no-class', ['-DNO_CLASS', *partial])):
        exe = out / name
        run(['clang', *OBJC, *define, *(SOURCE / s for s in PROVIDER), *FRAMEWORKS, '-lobjc', '-Wl,-export_dynamic', '-o', exe])
        print(last_line(run([exe], timeout=60)))


def test_plugin(out):
    built = out / 'plugin'
    print(last_line(run([sys.executable, '-B', SOURCE / 'scripts/build-pagani-camera-plugin.py', '--out', built])))
    if not SYMBOLS.is_file():
        print('SKIP plugin imports: 没有 21H461 的符号表')
        return
    names = [line.strip() for line in run(['xcrun', 'nm', '-u', built / 'H6ISP.mediacapture']).splitlines() if line.strip()]
    with SYMBOLS.open('rb') as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as table:
        missing = [name for name in names if table.find(name.encode() + b'\0') < 0]
    # 符号表里没有 __objc_data 里的数据符号：这两个直接问共享缓存。
    data = {'___CFConstantStringClassReference': 'CoreFoundation', '__xpc_type_connection': 'libxpc.dylib'}
    for name, image in data.items():
        if name in missing and CACHE.is_file() and name in subprocess.run(['ipsw', 'dyld', 'symaddr', str(CACHE), name, '--image', image], capture_output=True, text=True).stdout:
            missing.remove(name)
    if missing:
        raise SystemExit('FAIL plugin imports: not in 21H461: ' + ', '.join(missing))
    print(f'PASS plugin imports: {len(names)} 个符号都在 21H461 的符号表里')


def check_facts():
    if not DT.is_file():
        print('SKIP facts: 没有 running.dts')
        return
    dts = DT.read_text(errors='replace')
    board = (SOURCE / 'PaganiCameraBoard.h').read_text()

    def node(name):
        start = dts.index(name)
        return dts[start:dts.index('\n\t\t\t};', start)]

    def want(condition, what):
        if not condition:
            raise SystemExit('FAIL facts: ' + what)

    sensor = node('qcom,cam-sensor2 {')
    want('ftm_setting_compatible = <0x57 0xfcfc 0x02 0x02 0x4000 0x52 0x00 0x02 0x02 0x38e5>;' in sensor, '长焦的 ID 序列')
    want('cci-master = <0x00>;' in sensor and 'csiphy-sd-index = <0x05>;' in sensor, '长焦的 master 和 PHY')
    want('rgltr-max-voltage = <0x1b7740 0xf4240 0x00 0x2191c0 0x2ab980>;' in sensor and 'rgltr-min-voltage = <0x1b7740 0xf4240 0x00 0x2191c0 0x2ab980>;' in sensor, '长焦的电压')
    want('regulator-names = "cam_vio", "cam_vdig", "cam_clk", "cam_vana", "cam_vaf";' in sensor, '电压的次序')
    want('clock-rates = <0x124f800>;' in sensor, 'MCLK 19.2 MHz')
    want('gpios = <0x26 0x59 0x00 0x26 0xb2 0x00>;' in sensor, 'MCLK0 gpio89、复位 gpio178')
    for name, phandle, resource, microvolt in (('cam_vio', 0x432, 'ldom5', 1800000), ('cam_vdig', 0x420, 'ldoi1', 1000000), ('cam_vana', 0x431, 'ldom4', 2200000)):
        want(f'{name}-supply = <{phandle:#x}>;' in sensor, f'{name} 的 phandle')
        at = dts.index(f'phandle = <{phandle:#x}>;')
        block = dts[dts.rindex('qcom,resource-name', 0, at):at]
        want(f'"{resource}"' in block, f'{name} 的资源名')
        want(f'regulator-min-microvolt = <{microvolt:#x}>' in block and f'regulator-max-microvolt = <{microvolt:#x}>' in block, f'{name} 的电压范围')
        want(f'{{"{resource}",' in board and f', {microvolt // 1000}, ' in board, f'{name} 不在板级表里')
    for ppid, phandle in ((0xc44, 0x432), (0xc43, 0x431), (0x8c1, 0x420)):
        want(f'periph-{ppid:x}-supply = <{phandle:#x}>;' in dts, f'PPID {ppid:#x}')
    fast = dts[dts.index('i2c_freq_400Khz_cci0: qcom,i2c_fast_mode'):]
    fast = fast[:fast.index('};')]
    for key, value in (('thigh', 0x26), ('tlow', 0x38), ('tsu-sto', 0x28), ('tsu-sta', 0x28), ('thd-dat', 0x16), ('thd-sta', 0x23), ('tbuf', 0x3e), ('trdhld', 6), ('tsp', 3)):
        want(f'hw-{key} = <{value:#04x}>;' in fast, f'CCI {key}')
    want('{0x26, 0x38, 0x28, 0x28, 0x16, 0x23, 0x3e, 0, 6, 3}' in board, '板级表的 CCI 时序')
    want('PcamTelePageReg = 0xfcfc, PcamTelePageValue = 0x4000, PcamTeleIdReg = 0x0000, PcamTeleId = 0x38e5' in board, '板级表的长焦 ID 序列')
    want('cam_cci0: qcom,cci0@ac7b000' in dts and 'reg = <0xac7b000 0x1000>;' in dts, 'CCI0')
    want('camcc: clock-controller@ade0000' in dts and 'cambistmclkcc: clock-controller@1760000' in dts, '时钟控制器')
    want('cam_cc_titan_top_gdsc: qcom,gdsc@adf134c' in dts, 'titan GDSC')
    for label, pin, function in (('cci_i2c_sda0_active', 113, 'cci_i2c_sda0'), ('cci_i2c_scl0_active', 114, 'cci_i2c_scl0'), ('cam_sensor_mclk0_active', 89, 'cam_mclk')):
        block = dts[dts.index(label + ': ' + label):]
        block = block[:block.index('};\n\n')]
        want(f'function = "{function}"' in block and f'pins = "gpio{pin}"' in block, label)
    want('qcom,gpios-reserved = <0x24 0x25 0x26 0x27 0x4a>;' in dts, '保留脚')
    print('PASS facts: 板级数字和 running.dts 一致')


def check_arguments():
    """PaganiCamera.cpp 读的启动参数都写在 README.md 的表和 PaganiCamera.h 里，也没有写多。"""
    read = set(re.findall(r'argument\("(pagani-[a-z0-9-]+)"', (SOURCE / 'PaganiCamera.cpp').read_text()))
    if read != {'pagani-cam', 'pagani-torch-ios'}:
        raise SystemExit(f'FAIL args: PaganiCamera.cpp reads {sorted(read)}')
    readme = (SOURCE / 'README.md').read_text().split('## 启动参数', 1)[-1].split('\n## ', 1)[0]
    header = (SOURCE / 'PaganiCamera.h').read_text().split('#ifndef', 1)[0]
    problems = []
    for where, text in (('README.md', readme), ('PaganiCamera.h', header)):
        named = set(re.findall(r'(?<![a-z-])(pagani-(?:cam|torch-ios)(?:-[a-z0-9]+)*)', text))
        problems += [f'{name} is read but not in {where}' for name in sorted(read - named)]
        problems += [f'{where} names {name}, which is not read' for name in sorted(named - read)]
    if problems:
        raise SystemExit('FAIL args: ' + '; '.join(problems))
    print('PASS args: 2 boot arguments, documented')


def check_quiet():
    """UFS 日志写者的保持字是按位用的：写成整字存储就放开了别人的保持。模型只看得到回调，接线靠这一条。"""
    glue = (SOURCE / 'PaganiCamera.cpp').read_text()
    bit = re.search(r'#define PCAM_QUIET_BIT (\d+)u', (SOURCE / 'PaganiCameraBoard.h').read_text())
    wanted = ('__atomic_fetch_or(&pagani_panel_probe_quiet, PCAM_QUIET_BIT, __ATOMIC_RELEASE)', '__atomic_fetch_and(&pagani_panel_probe_quiet, ~PCAM_QUIET_BIT, __ATOMIC_RELEASE)',
              'hwPmic, hwLog, hwQuiet};')
    missing = [text for text in wanted if text not in glue]
    if missing or '__atomic_store_n(&pagani_panel_probe_quiet' in glue or not bit or int(bit.group(1)) != 16:
        raise SystemExit(f'FAIL quiet: {missing or "保持字里的位"}')
    print('PASS quiet: 第一次碰 CAMCC 和 CCI 用 bit 4 持住写者')


def check_gate_in_glue():
    """胶水的两个写路径各过一道闸门：核心的路径错了也写不出去。"""
    glue = (SOURCE / 'PaganiCamera.cpp').read_text()
    if 'if (!pcam_write_allowed(win, off, value))' not in glue or 'if (!pcam_vrm_allowed(resource, reg, value))' not in glue:
        raise SystemExit('FAIL gate: 胶水没有过闸门')
    # 两道闸门只有代码错了才拦得到东西，模型走不到这条路：接线靠这一条。
    core = (SOURCE / 'CamBringup.c').read_text()
    if 'if (!pcam_write_allowed(win, off, value)) {' not in core or 'if (!pcam_vrm_allowed(rail->resource, reg, value)) {' not in core:
        raise SystemExit('FAIL gate: 核心没有过闸门')
    if 'mx.lvl' in glue or '"mx.lvl"' in (SOURCE / 'PaganiCameraBoard.h').read_text():
        raise SystemExit('FAIL gate: 绝不投 mx.lvl')
    print('PASS gate: 胶水的 MMIO 和 VRM 写都过闸门，没有 mx.lvl')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, help='also build the kernel archive with this tree (output in a temporary directory)')
    parser.add_argument('--only', help='comma separated test names')
    parser.add_argument('--keep', type=Path, help='build and test files here instead of a temporary directory')
    args = parser.parse_args()
    only = set(args.only.split(',')) if args.only else None

    def chosen(name):
        return not only or name in only

    with tempfile.TemporaryDirectory() as temporary:
        out = args.keep or Path(temporary)
        out.mkdir(parents=True, exist_ok=True)
        for name in CORE:
            run(['cc', *STRICT, '-c', SOURCE / name, '-o', out / (Path(name).name + '.o')])
        print(f'PASS strict: {len(CORE)} portable sources')
        for name, sources in TESTS.items():
            if chosen(name):
                exe = out / name
                run(['cc', *CFLAGS, *(SOURCE / s for s in sources), '-o', exe])
                print(last_line(run([exe])))
        if chosen('pattern'):
            test_pattern(out)
        if chosen('provider'):
            test_provider(out)
        if chosen('plugin'):
            test_plugin(out)
        if chosen('facts'):
            check_facts()
        if chosen('args'):
            check_arguments()
            check_quiet()
            check_gate_in_glue()
        if args.tree:
            print(last_line(run([sys.executable, '-B', SOURCE / 'scripts/build-pagani-camera.py', '--tree', args.tree, '--out', out / 'kernel'])))
    print('PASS all')


if __name__ == '__main__':
    main()
