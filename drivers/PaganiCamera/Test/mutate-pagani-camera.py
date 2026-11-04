#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiCamera 关键判据的变异检查：每个变异是一处源码改动，必须让某个主机测试失败。源码拷到临时目录，这里什么都不改。最后列出每个测试抓到了多少个。"""
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
DT = SOURCE.parents[2] / 'build-userspace/a830-inputs/dt/running.dts'
FLAGS = ['-std=c11', '-w', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
C_TESTS = {
    'bringup': ['Test/bringup.c', 'CamBringup.c', 'CamCci.c'],
    'cci': ['Test/cci.c', 'CamCci.c'],
    'pmic': ['Test/pmic.c', 'CamPmic.c'],
}
OBJC = ['-w', '-fno-objc-arc', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DPAGANI_CAMERA_PLUGIN_HOST']
FRAMEWORKS = [flag for name in ('Foundation', 'CoreFoundation', 'CoreMedia', 'CoreVideo', 'CoreImage', 'IOSurface', 'CoreMediaIO') for flag in ('-framework', name)]
B, C, P, K, Q, R, G = 'CamBringup.c', 'CamCci.c', 'CamPmic.c', 'PaganiCameraBoard.h', 'ios/CamPattern.c', 'ios/CamProvider.m', 'PaganiCamera.cpp'
MUTANTS = [
    # 没上电不碰
    (B, 'if (off) return failed(b, PcamStepCamcc, PcamErrGcc, "a GCC branch did not start: CAMCC was not read");', '', 'GCC 分支没起来不读 CAMCC'),
    (B, 'if (branch_off(b->gcc[i]) || !(b->gcc[i] & PCAM_CBCR_ENABLE)) off |= 1u << i;', '', 'GCC 分支要轮询'),
    (B, 'if (!(now & PCAM_CBCR_ENABLE)) wr(b, PcamWinGcc, kGccBranches[i], now | PCAM_CBCR_ENABLE);', '', 'GCC 分支要打开'),
    (B, 'if (!cci_ready(b)) return domain_fail(b, PcamErrBranch, "state check failed: CCI was not read");', '', '碰 CCI 之前总检查'),
    (B, 'if (n == kGdscPolls) return domain_fail(b, PcamErrGdsc, "the GDSC did not power up: CCI was not read");', '', 'GDSC 没上电不碰 CCI'),
    (B, 'if (branch_on(b, PcamWinCamcc, PcamCcTopAhb) || branch_on(b, PcamWinCamcc, PcamCcTopFastAhb) || branch_on(b, PcamWinCamcc, PcamCcCci0))\n        return domain_fail(b, PcamErrBranch, "a branch did not start: CCI was not read");', '', '分支没起来不碰 CCI'),
    (B, 'if ((v & PCAM_CBCR_ENABLE) && !branch_off(v)) return 0;', 'return 0;', '分支要等 CLK_OFF 清掉'),
    (B, 'for (unsigned i = 0; i < 4; ++i) {\n        const uint32_t now = rd(b, PcamWinCamcc, kAlwaysOn[i]);\n        if (!(now & PCAM_CBCR_ENABLE)) wr(b, PcamWinCamcc, kAlwaysOn[i], now | PCAM_CBCR_ENABLE);\n    }\n', '', 'GDSC 之前开常开分支'),
    (B, 'if (gdscr & PCAM_GDSC_HW_CONTROL) return domain_fail(b, PcamErrGdsc, "the GDSC is under hardware control");', '', '步 201 的 GDSC 硬件控制'),
    (B, 'if (b->gdscr & PCAM_GDSC_HW_CONTROL) return failed(b, PcamStepCamcc, PcamErrGdsc, "the GDSC is under hardware control");', '', '步 102 的 GDSC 硬件控制'),
    (B, 'if (b->gdscr == 0xffffffffu || b->cci_cbcr == 0xffffffffu || b->mclk_cbcr == 0xffffffffu) return failed(b, PcamStepCamcc, PcamErrCamccValue, "CAMCC reads all ones");', '', 'CAMCC 读到全 1 就停'),
    (B, 'if (!b->cci_version || b->cci_version == 0xffffffffu) return failed(b, PcamStepDomain, PcamErrCciVersion, "CCI version is not plausible");', '', 'CCI 版本不合理就停'),
    (B, 'wr(b, PcamWinCamcc, PcamCcGdscr, gdscr & ~PCAM_GDSC_SW_COLLAPSE);', 'wr(b, PcamWinCamcc, PcamCcGdscr, 0);', 'GDSC 只改折叠位'),
    # 落盘和写者
    (B, '    b->hw->msleep(b->hw->ctx, kCrumbMs);\n', '', '上一行先落盘'),
    (B, '    b->hw->quiet(b->hw->ctx, 1);\n', '', '第一次碰时持住写者'),
    (B, '    b->hw->msleep(b->hw->ctx, kQuietMs);\n', '', '写者手里的一轮收尾'),
    (B, '    b->hw->quiet(b->hw->ctx, 0);\n', '', '读完放开写者'),
    (B, '    guard_off(b);\n    return failed(b, PcamStepDomain, error, what);', '    return failed(b, PcamStepDomain, error, what);', '失败时放开写者'),
    (B, '    guard_on(b);\n    rc = rails_on(b);', '    rc = rails_on(b);', '级 4 上电之前持住写者'),
    (B, '    const int off = power_off(b);\n    guard_off(b);', '    const int off = power_off(b);', '级 4 走完放开写者'),
    (K, '#define PCAM_QUIET_BIT 16u', '#define PCAM_QUIET_BIT 8u', '保持字用 bit 4'),
    # 投票
    (K, '#define PCAM_ARC_MXC "mxc.lvl"', '#define PCAM_ARC_MXC "mx.lvl"', '绝不投 mx.lvl'),
    (K, 'PcamArcLevel = 0x180', 'PcamArcLevel = 0x40', 'ARC 投 Android 开机的 0x180'),
    (B, 'if (rc) return failed(b, PcamStepDomain, PcamErrVote, "ARC vote");', '', '投票失败就停'),
    (B, 'if (!rc) rc = hw->arc(hw->ctx, PCAM_ARC_MXC, PcamArcLevel, &b->mxc_hlvl);', '', '投 mxc'),
    # 电压
    (B, 'case PcamVrmVoltage: return value == kPcamRails[i].millivolts;', 'case PcamVrmVoltage: return 1;', '闸门只放白名单电压'),
    (B, 'case PcamVrmEnable: return value <= 1;', 'case PcamVrmEnable: return 1;', '闸门的使能值'),
    (B, 'case PcamVrmMode: return value == PcamVrmModeHigh;', 'case PcamVrmMode: return 1;', '闸门的模式值'),
    (K, '{"ldom4", 0xc43, 2200, 2, "vana"}', '{"ldom4", 0xc43, 2800, 2, "vana"}', '长焦 vana 是 2.2 V'),
    (K, '{"ldoi1", 0x8c1, 1000, 3, "vdig"}', '{"ldoi1", 0x8c1, 1100, 3, "vdig"}', '长焦 vdig 是 1.0 V（1.1 V 是主摄的）'),
    (K, '{"ldom5", 0xc44, 1800, 1, "vio"}', '{"ldom5", 0xc45, 1800, 1, "vio"}', 'vio 的 PPID'),
    (B, 'if (b->rails[i].read && (b->rails[i].millivolts != kPcamRails[i].millivolts || !(b->rails[i].enable & 0x80)))\n            return failed(b, PcamStepRails, PcamErrRail, kPcamRails[i].resource);', '', '投后读回，读到别的值就下电'),
    (B, 'if (r->read && (r->enable & 0x80) && r->millivolts != rail->millivolts) {', 'if (0) {', '已经开在别的电压上不碰'),
    (B, 'int rc = vrm(b, rail, PcamVrmMode, PcamVrmModeHigh, &address);', 'int rc = 0;', '投高功率模式'),
    (B, 'if (rc) {\n            log_rail(b, "vote failed", i);\n            return failed(b, PcamStepRails, PcamErrRail, rail->resource);\n        }\n', '', '投票失败就停'),
    (B, 'b->rails_touched |= (uint8_t)(1u << i);', '', '记下投过的轨道，下电要用'),
    # 复位和时序
    (B, 'wr(b, PcamWinTlmm, reset + 4, 0);\n    wr(b, PcamWinTlmm, reset, ctl);', 'wr(b, PcamWinTlmm, reset, ctl);', '先定输出电平'),
    (B, 'wr(b, PcamWinTlmm, reset, ctl);\n    if ((rd', 'if ((rd', '复位脚要配成输出'),
    (B, '| (now & PcamCtlEgpioPresent ? PcamCtlEgpioEnable : 0)', '', 'egpio 脚要归 apps'),
    (B, '| (now & PcamCtlEgpioPresent ? PcamCtlEgpioEnable : 0)', '| PcamCtlEgpioEnable', '不是 egpio 的脚不置位'),
    (B, 'if (function != 0 && function != kPins[i].function) return failed(b, PcamStepRails, PcamErrPin, kPins[i].name);', '', '脚被别人占着就停'),
    (B, 'hw->msleep(hw->ctx, kResetSettleMs);', '', '放开复位后等'),
    (B, 'wr(b, PcamWinTlmm, pin_ctl_off(kPinReset) + 4, PcamIoOut);\n    hw->msleep', 'hw->msleep', '放开复位'),
    (B, 'wr(b, PcamWinTlmm, pin_ctl_off(kPinMclk), (ctl & ~(uint32_t)PcamCtlMask) | PcamCtlMclk);', '', 'MCLK 脚切功能'),
    (B, 'for (unsigned i = kPinSda; i <= kPinScl; ++i) wr(b, PcamWinTlmm, pin_ctl_off(i), (b->pin_ctl[i] & ~(uint32_t)PcamCtlMask) | PcamCtlCci);', '', 'CCI 脚切功能'),
    (B, 'if (branch_on(b, PcamWinBist, PcamBistMclk0Cbcr)) return failed(b, PcamStepSensor, PcamErrBranch, "MCLK branch did not start");', '(void)branch_on(b, PcamWinBist, PcamBistMclk0Cbcr);', 'MCLK 起不来就停'),
    # 下电
    (B, '    const int off = power_off(b);\n    guard_off(b);\n    return rc ? rc : off;', '    guard_off(b);\n    return rc;', '不论成败都下电'),
    (B, 'if (rc) error = PcamErrRail;\n', '', '关轨道失败要报'),
    (B, 'if (b->rails[i].read && (b->rails[i].enable & 0x80)) error = PcamErrRail;', '', '下电后读回'),
    (B, 'wr(b, PcamWinTlmm, pin_ctl_off(kPinReset) + 4, 0);\n        hw->msleep(hw->ctx, kResetLowMs);', '', '下电前先复位'),
    (B, 'wr(b, PcamWinBist, PcamBistMclk0Cbcr, v & ~PCAM_CBCR_ENABLE);', '', '下电关 MCLK'),
    (B, 'for (unsigned i = 0; i < PcamPins; ++i) wr(b, PcamWinTlmm, pin_ctl_off(i), b->pin_ctl[i]);', '', '脚还原'),
    (B, 'for (unsigned order = PCAM_RAIL_COUNT; order >= 1; --order) {', 'for (unsigned order = 1; order <= PCAM_RAIL_COUNT; ++order) {', '下电顺序相反'),
    (B, 'if (already) {                          /* 原来的失败更要紧：步号和错误码留原来的 */', 'if (0) {', '下电失败不盖掉原来的失败'),
    # 级
    (B, 'if (rc || b->level < 2) return rc;', 'if (rc) return rc;', '级 1 停在勘察'),
    (B, 'if ((rc = camcc_read(b)) || b->level < 3) return rc;', 'if ((rc = camcc_read(b))) return rc;', '级 2 停在读'),
    (B, 'if ((rc = domain_on(b)) || b->level < 4) return rc;', 'if ((rc = domain_on(b))) return rc;', '级 3 不上轨道'),
    (B, 'unsigned pcam_level(unsigned value) {return value >= 1 && value <= PcamLevelMax ? value : 0;}', 'unsigned pcam_level(unsigned value) {return value;}', '未知的值是关'),
    # RCG
    (B, 'if (!(cfg & (PCAM_RCG_SRC_MASK | PCAM_RCG_MODE_MASK)) && (cfg & PCAM_RCG_DIV_MASK) <= 1) return 0;', '', '已经是 XO 不写'),
    (B, 'wr(b, win, rcg + PcamRcgCfg, PCAM_RCG_CFG_XO);\n', '', 'RCG 切 XO'),
    (B, 'for (unsigned n = 0; n < kRcgPolls; ++n) {\n        if (!(rd(b, win, rcg) & PCAM_RCG_UPDATE)) return 0;\n        b->hw->udelay(b->hw->ctx, kRcgStepUs);\n    }\n    return -1;', 'return 0;', 'RCG 更新要完成'),
    # 闸门
    (B, 'return (offset == PcamGccBistAhb || offset == PcamGccCameraAhb || offset == PcamGccCameraXo) && cbcr_on(value);', 'return 1;', 'GCC 白名单'),
    (B, 'case PcamCcGdscr: return !(value & (PCAM_GDSC_SW_COLLAPSE | PCAM_GDSC_HW_CONTROL));', 'case PcamCcGdscr: return 1;', 'GDSC 只开不关'),
    (B, 'return value == PCAM_RCG_CFG_XO;\n        default:', 'return 1;\n        default:', 'RCG 只许 XO'),
    (B, 'return function == 0 || function == kPins[i].function;', 'return 1;', 'TLMM 功能白名单'),
    (B, 'return reg == 4 && i == kPinReset && (value == 0 || value == PcamIoOut);', 'return reg == 4;', '只有复位脚写电平'),
    (B, 'case PcamCciQueueStart: return value == 1u;', 'case PcamCciQueueStart: return 1;', '只有 master 0 queue 0'),
    (B, '    if (!pcam_write_allowed(win, off, value)) {\n        b->refused++;\n        return;\n    }\n', '', '核心的写过闸门'),
    (B, 'if (!pcam_vrm_allowed(rail->resource, reg, value)) {\n        b->refused++;\n        return -1;\n    }\n', '', '核心的 VRM 过闸门'),
    # ID
    (K, 'PcamTeleId = 0x38e5', 'PcamTeleId = 0x38e6', '长焦 ID'),
    (K, 'PcamTelePageValue = 0x4000', 'PcamTelePageValue = 0x2000', '长焦的页寄存器值'),
    (B, 'if (id == PcamTeleId) {', 'if (1) {', 'ID 不对不算'),
    (B, 'if (!e) e = pcam_cci_read16(&b->cci, &kPcamCciFast, addr, PcamTeleIdReg, &id);', 'e = pcam_cci_read16(&b->cci, &kPcamCciFast, addr, PcamTeleIdReg, &id);', '写失败不再读'),
    (B, 'int e = pcam_cci_write16(&b->cci, &kPcamCciFast, addr, PcamTelePageReg, PcamTelePageValue);', 'int e = 0;', '先写页寄存器'),
    (B, 'pcam_text_str(&t, " id ");', 'pcam_text_str(&t, " sensor ");', '判据那一行的写法'),
    (K, 'static const uint8_t kPcamTeleAddresses[2] = {0x2d, 0x10};', 'static const uint8_t kPcamTeleAddresses[2] = {0x2d, 0x2d};', '两个候选地址'),
    # CCI
    (C, 'wr(io, PcamCciExecWords, rd(io, PcamCciCurWords));', '', '抄 EXEC_WORD_CNT'),
    (C, 'wr(io, PcamCciQueueStart, 1u);', 'wr(io, PcamCciQueueStart, 2u);', '启动位'),
    (C, 'if (rc) recover(io, t);\n    return rc;', 'return rc;', '出错要恢复'),
    (C, 'wr(io, PcamCciHalt, PcamCciHaltM0);', '', '恢复先 HALT'),
    (C, '(void)pcam_cci_init(io, t);', '', '恢复要重写时序'),
    (C, 'if (have & PCAM_CCI_IRQ_ERROR) {', 'if (0) {', '错误位'),
    (C, 'if (!queue_empty(io)) return PcamCciBusy;\n    wr(io, PcamCciLoadData, set_param(addr));\n    wr(io, PcamCciLoadData, PcamCciCmdLock);', 'wr(io, PcamCciLoadData, set_param(addr));\n    wr(io, PcamCciLoadData, PcamCciCmdLock);', '队列不空不追加'),
    (C, '(uint32_t)(addr & 0x7fu) << 4', '(uint32_t)addr << 4', '7 位地址'),
    (C, '| (uint32_t)PcamCciRetries << PcamCciRetryShift', '', '重试次数'),
    (C, '*value = (uint16_t)(((word >> 8) & 0xffu) << 8 | ((word >> 16) & 0xffu));', '*value = (uint16_t)((word & 0xffu) << 8 | ((word >> 8) & 0xffu));', '第一个字节不是数据'),
    (C, 'if ((rd(io, PcamCciReadLevel) & 0xffu) != 1u) {', 'if (0) {', '读回字数'),
    (C, 'wr(io, PcamCciLoadData, PcamCciCmdLock);', '', 'LOCK'),
    (C, 'wr(io, PcamCciLoadData, PcamCciCmdUnlock);', '', 'UNLOCK'),
    (C, 'wr(io, PcamCciLoadData, value & 0xffu);', '', '写的低字节'),
    (C, 'PcamCciCmdWriteNoStop | 2u << 4', 'PcamCciCmdWrite | 2u << 4', '读之前不发 STOP'),
    (C, 'wr(io, PcamCciSclCtl, (uint32_t)t->thigh << 16 | t->tlow);', 'wr(io, PcamCciSclCtl, (uint32_t)t->tlow << 16 | t->thigh);', 'SCL 时序的高低'),
    (C, 'wr(io, PcamCciIrqMask0, PCAM_CCI_IRQ_ENABLE);', '', '中断掩码'),
    (K, '{0x26, 0x38, 0x28, 0x28, 0x16, 0x23, 0x3e, 0, 6, 3}', '{0x26, 0x38, 0x28, 0x28, 0x16, 0x23, 0x3e, 0, 3, 3}', '设备树的时序'),
    (K, 'PcamCciResetAll = 0x0f73f3f7', 'PcamCciResetAll = 0x0f73f3f6', 'CCI 复位值'),
    # PMIC
    (P, 'if (found < 0 || owner == 0) found = (int)i;', 'found = (int)i;', '优先 EE 0 的通道'),
    (P, 'if (version < 0x70000000u || version >= 0x80000000u || !count || count > 1024) return PcamSpmiLayout;', '', '只认 v7'),
    (P, 'if (status & 0xeu) return PcamSpmiIoError;', '', 'observer 的错误位'),
    (P, '*millivolts = (uint16_t)(hi << 8 | lo);', '*millivolts = (uint16_t)(lo << 8 | hi);', '电压的高低字节'),
    # 测试图
    (Q, 'if (x & 0x100u) x ^= 0x11du;', 'if (x & 0x100u) x ^= 0x12du;', 'GF(256) 的多项式'),
    (Q, 'root = gf_mul(root, 2);', 'root = gf_mul(root, 3);', 'RS 生成多项式的根'),
    (Q, '^ 0x5412u;', '^ 0x5413u;', '格式信息的掩码'),
    (Q, '* 0x537u', '* 0x536u', '格式信息的 BCH'),
    (Q, 'alignment(&c, 22, 22);', 'alignment(&c, 21, 22);', '校正图形的位置'),
    (Q, 'set(c, PcamQrSize - 8, 8, 1);', 'set(c, PcamQrSize - 8, 8, 0);', '固定的深色模块'),
    (Q, 'put_bits(out, &at, 4, 4);', 'put_bits(out, &at, 2, 4);', '字节模式'),
    (Q, 'if (right == 6) right = 5;', '', '跳过定时列'),
    (Q, 'memset(uv + (size_t)(p->tile_top / 2 + r) * uv_stride + p->tile_left, 128, p->tile);', '', '二维码的色度是中性'),
    (Q, 'const unsigned shift = (unsigned)(((uint64_t)frame * PcamScrollPixels) % p->width) & ~1u;', 'const unsigned shift = 0;', '滚动'),
    # 提供者
    (R, 'id vendor = gVendorInit(self, cmd, (void *)placeholder_create, cmio, session);', 'id vendor = gVendorInit(self, cmd, NULL, cmio, session);', '占位函数过厂商类的检查'),
    (R, 'if (vendor) *(void **)((char *)vendor + gDefaultOffset) = NULL;', '', '过了检查把默认函数清回空'),
    (R, 'if (create || !cmio) return gVendorInit(self, cmd, create, cmio, session);', '', '真有默认函数时原样'),
    (R, '[provider beginProviderContextRegistration:context];', '', '新连接要注册'),
    (R, '[providers setObject:provider forKey:@PCAM_BUNDLE];', '', '挂进发现会话'),
    (R, '[provider setBundleID:@PCAM_BUNDLE];', '', '设 bundle ID'),
    (R, ', (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};', '};', '帧靠 IOSurface'),
    (R, 'if (need[i].instance ? !class_getInstanceMethod(c, s) : !class_getClassMethod(c, s)) return PcamStartNoSelector;', '', '少一个方法什么都不建'),
    (R, 'if (!c) return PcamStartNoClass;', 'if (!c) continue;', '少一个类什么都不建'),
    (R, 'dispatch_sync(gFrameQueue, ^{timer_stop();});', '', '停流要停定时器'),
    (R, 'if (value >= PcamModes) return NO;\n    dispatch_sync(gFrameQueue, ^{\n        gActive = value;', 'if (value >= PcamModes) return NO;\n    dispatch_sync(gFrameQueue, ^{', '切格式'),
    (R, 'if ([properties containsObject:(id)gKeyDeviceMaker]) [out setPropertyState:[state propertyStateWithValue:@"UniXNU"] forProperty:(id)gKeyDeviceMaker];', '', '设备的厂商属性'),
    (R, '++gFrame;\n', '', '帧号递增'),
    (R, '#define PCAM_VENDOR_INIT "initWithDefaultDeviceCreateFunction:cmioExtensionDeviceCreateFunction:cmioDiscoverySession:"', '#define PCAM_VENDOR_INIT "initWithDefaultDeviceCreateFunction:"', '厂商类的方法名'),
    # 接线（check 脚本的静态检查）
    (G, 'if (!pcam_write_allowed(win, off, value)) {', 'if (0) {', '胶水的 MMIO 写过闸门'),
    (G, 'if (!pcam_vrm_allowed(resource, reg, value)) {', 'if (0) {', '胶水的 VRM 写过闸门'),
    (G, '__atomic_fetch_and(&pagani_panel_probe_quiet, ~PCAM_QUIET_BIT, __ATOMIC_RELEASE)', '__atomic_store_n(&pagani_panel_probe_quiet, 0, __ATOMIC_RELEASE)', '保持字按位放开'),
]


def run_c(work, name, sources):
    exe = work / name
    built = subprocess.run(['cc', *FLAGS, *(str(work / s) for s in sources), '-o', str(exe)], capture_output=True, text=True, cwd=work)
    if built.returncode:
        return True
    try:
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=120).returncode != 0
    except subprocess.TimeoutExpired:
        return True


def run_pattern(work):
    exe = work / 'pattern'
    built = subprocess.run(['clang', '-w', '-fobjc-arc', '-O1', '-g', 'Test/pattern.m', 'ios/CamPattern.c', '-framework', 'Foundation', '-framework', 'CoreImage', '-framework', 'CoreVideo',
                            '-framework', 'Vision', '-framework', 'CoreGraphics', '-o', str(exe)], capture_output=True, text=True, cwd=work)
    if built.returncode:
        return True
    try:
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=120).returncode != 0
    except subprocess.TimeoutExpired:
        return True


def run_provider(work):
    for name, define in (('provider', []), ('provider-no-class', ['-DNO_CLASS'])):
        exe = work / name
        built = subprocess.run(['clang', *OBJC, *define, 'Test/provider.m', 'ios/CamProvider.m', 'ios/CamPattern.c', *FRAMEWORKS, '-lobjc', '-Wl,-export_dynamic', '-o', str(exe)],
                               capture_output=True, text=True, cwd=work)
        if built.returncode:
            return True
        try:
            if subprocess.run([str(exe)], capture_output=True, text=True, timeout=30).returncode:
                return True
        except subprocess.TimeoutExpired:
            return True
    return False


def run_static(work):
    environment = dict(os.environ, PAGANI_CAM_DT=str(DT))
    return subprocess.run([sys.executable, '-B', str(work / 'Test/check-pagani-camera-host.py'), '--only', 'facts,args'], capture_output=True, text=True, env=environment).returncode != 0


def one(args):
    index, tmp = args
    name, old, new, what = MUTANTS[index]
    work = tmp / f'm{index}' / 'PaganiCamera'
    shutil.copytree(SOURCE, work, ignore=shutil.ignore_patterns('*.o', '__pycache__'))
    text = (work / name).read_text()
    if old not in text:
        return index, what, 'NOT APPLIED'
    (work / name).write_text(text.replace(old, new, 1))
    killers = []
    if name in (B, C, K):
        for test in ('bringup', 'cci'):
            if run_c(work, test, C_TESTS[test]):
                killers.append(test)
    if name == P and run_c(work, 'pmic', C_TESTS['pmic']):
        killers.append('pmic')
    if name == Q and run_pattern(work):
        killers.append('pattern')
    if name in (Q, R) and run_provider(work):
        killers.append('provider')
    if name in (K, G, B) and run_static(work):
        killers.append('static')
    shutil.rmtree(work)
    return index, what, killers


def main():
    chosen = list(range(len(MUTANTS)))
    survivors, broken, tally = [], [], {}
    with tempfile.TemporaryDirectory() as tmp:
        with ThreadPoolExecutor(max(4, (os.cpu_count() or 4) // 2)) as pool:
            for index, what, killers in pool.map(one, [(i, Path(tmp)) for i in chosen]):
                if killers == 'NOT APPLIED':
                    broken.append(what)
                    print(f'不适用   {what}')
                    continue
                print(f'{"抓到" if killers else "漏网"}  {what}  <- {",".join(killers) if killers else "-"}')
                if not killers:
                    survivors.append(what)
                for test in killers:
                    tally[test] = tally.get(test, 0) + 1
    print('各测试抓到的数量：' + '，'.join(f'{k} {v}' for k, v in sorted(tally.items())))
    if broken:
        raise SystemExit('FAIL: 变异没有套上（源码改过了）: ' + ', '.join(broken))
    if survivors:
        raise SystemExit('FAIL: 漏网的变异: ' + ', '.join(survivors))
    print(f'PASS mutate: {len(chosen)} 个变异全部抓到')


if __name__ == '__main__':
    main()
