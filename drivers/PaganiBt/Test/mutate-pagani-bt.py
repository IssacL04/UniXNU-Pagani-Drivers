#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""PaganiBt 关键判据的变异检查：每个变异是一处源码改动，必须让某个主机测试失败。源码拷到临时目录，这里什么都不改。
最后列出每个测试抓到了多少个，replay 一列是只有真实跟踪才抓得到的（缺本地文件时跳过 replay，并说明）。

  mutate-pagani-bt.py [名字里含这段文字的变异]    不带参数全跑"""
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
LOCAL = SOURCE.parents[2] / 'build-userspace/a830-inputs/round-1101/bt-local'     # 不被跟踪的本地目录，固件和跟踪不进仓库
FIRMWARE = Path(os.environ.get('PAGANI_BT_FIRMWARE', LOCAL / 'btfw'))
TRACE = Path(os.environ.get('PAGANI_BT_TRACE', LOCAL / 'hsuart.trace'))
FLAGS = ['-std=c11', '-w', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
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
B, G, T, D, H, I, P, K = 'BtBringup.c', 'BtGeni.c', 'BtTlv.c', 'BtDownload.c', 'BtH4.c', 'BtIbs.c', 'BtPmic.c', 'PaganiBtBoard.h'
R, S, O = 'BtBridge.c', 'BtStandIn.c', 'PaganiBtHost.cpp'
MUTANTS = [
    # S7I 和轨道规则
    (B, 'if (!before.read) {', 'if (0) {', 'S7I 读不到不投'),
    (B, 'if (r->read && r->millivolts >= rail->chosenMv) set_voltage = 0;', '', 'S7I 够高不再投电压'),
    (B, 'if (r->read && (r->enable & 0x80)) set_enable = 0;', '', 'S7I 开着不再投使能'),
    (B, 'int rc = hw->vrm(hw->ctx, rail->resource, PbVrmMode, PbVrmModeHigh, &address);', 'int rc = 0;', '总投高功率模式'),
    (B, 'hw->vrm(hw->ctx, rail->resource, PbVrmEnable, 1, &address)', 'hw->vrm(hw->ctx, rail->resource, PbVrmEnable, 0, &address)', '绝不 enable=0'),
    (B, 'if (r->read && r->millivolts > mv) {', 'if (0) {', '电压只升不降'),
    (B, 'else set_voltage = 0;                                   /*', 'else mv = rail->maxMv;                                  /*', '超范围不投电压'),
    (B, 'if (!after->read) return failed(b, PbStepPower, PbErrRailRead, "S7I not read back");', '', '投后读回'),
    (B, 'if (after->millivolts < before.millivolts || ((before.enable & 0x80) && !(after->enable & 0x80)))\n        return failed(b, PbStepPower, PbErrRail, "S7I dropped");', '', 'S7I 掉了就停（比原来低）'),
    (B, 'if (after->millivolts < kPbRails[ufs].chosenMv || !(after->enable & 0x80)) return failed(b, PbStepPower, PbErrRail, "S7I below what the chip needs");', '', 'S7I 不够就停'),
    (B, 'int rc = hw->vrm(hw->ctx, PB_RF_CLOCK, PbRfClockEnable, 1, &b->rf_clock);', 'int rc = 0;', '投 rfclka1'),
    (B, 'hw->vrm(hw->ctx, PB_RF_CLOCK, PbRfClockEnable, 1, &b->rf_clock)', 'hw->vrm(hw->ctx, PB_RF_CLOCK, 0, 1, &b->rf_clock)', 'rfclka1 写 +4'),
    (K, '{"smpd4", 0x3a4, 876, 1036, 4},', '{"smpd4", 0x3a4, 800, 1036, 4},', '轨道表和 Wi-Fi 一致'),
    # 时钟和 SE
    (B, 'if (b->halts) return failed(b, PbStepClocks, PbErrBranch, "a branch did not start: the SE was not read");', '', '分支没起来绝不读 SE'),
    (B, 'if ((v & PB_CBCR_OFF) && !(v & PB_CBCR_HW_CTL)) off |= 1u << i;', 'if (v & PB_CBCR_OFF) off |= 1u << i;', '硬件门控的分支'),
    (B, '    hw->quiet(hw->ctx, 1);\n', '', '第一次读 SE 时持住 UFS 日志的写者'),
    (B, '    hw->msleep(hw->ctx, kQuietMs);\n', '', '写者手里的一轮收尾'),
    (B, '    hw->msleep(hw->ctx, kCrumbMs);\n', '', '上一行先落盘'),
    (B, '    line(b, text);\n    let_writer_go(b);\n    if (rc) return failed(b, PbStepClocks', '    line(b, text);\n    if (rc) return failed(b, PbStepClocks', '读完放开写者'),
    (B, '    hold_writer(b);\n    const PbGeniIo io', '    const PbGeniIo io', '第一次读 SE 这一段'),
    (B, '    line(b, "[bt] guard rails");\n    hold_writer(b);\n', '', '投票这一段持住写者：S7I 带着 UFS'),
    (B, '    line(b, "[bt] guard bt_en");\n    hold_writer(b);\n', '', 'BT_EN 这一段持住写者：芯片的负载落到 S7I'),
    (B, '    let_writer_go(b);\n    return done(b, PbStepPower);', '    return done(b, PbStepPower);', '投完票放开写者'),
    (B, '    let_writer_go(b);\n    return done(b, PbStepBtEn);', '    return done(b, PbStepBtEn);', 'BT_EN 走完放开写者'),
    (B, '    line(b, text);\n    let_writer_go(b);\n    return error;', '    line(b, text);\n    return error;', '失败时放开写者'),
    (B, '    if (!b->held) return;\n', '', '只放开自己持着的'),
    (B, 'wr(b, PbWinGcc, PbGccVote, now | PbVoteBits);', 'wr(b, PbWinGcc, PbGccVote, PbVoteBits);', '投票保留别的位'),
    (B, 'if (hw->vote_lock && hw->vote_lock(hw->ctx, 1)) return failed(b, PbStepClocks, PbErrLock, "GCC vote lock busy");', '', '投票拿锁'),
    (K, 'PbVoteSe6 = 1u << 10', 'PbVoteSe6 = 1u << 9', 'SE6 投票位'),
    (K, 'PbRcg = 0x1e77c,', 'PbRcg = 0x1e77e,', 'RCG 地址对齐（访问检查）'),
    (B, '*d = ~(uint32_t)r->n & 0xffffu;', '*d = ~(uint32_t)(r->n - r->m) & 0xffffu;', 'RCG D 值'),
    (B, '*n = ~(uint32_t)(r->n - r->m) & 0xffffu;', '*n = ~(uint32_t)r->n & 0xffffu;', 'RCG N 值'),
    (B, 'if (b->rcg_dfs & 1u) {', 'if (0) {', '动态频率表不写 CFG'),
    (B, 'if (level < 0) return PbErrRcg;', '', '频率表里没有就失败'),
    (B, 'for (unsigned i = 0; i < kRcgPolls; ++i) {\n        if (!(rd(b, PbWinGcc, PbRcg) & PbRcgUpdate)) {\n            b->level = 0;\n            return 0;\n        }\n        hw->udelay(hw->ctx, kRcgStepUs);\n    }\n    return PbErrRcg;', 'b->level = 0;\n    return 0;', 'RCG 更新要完成'),
    (K, '{PbPinCts, 0, "cts"}, {PbPinRts, -1, "rts"}, {PbPinTx, 1, "tx"}', '{PbPinCts, 0, "cts"}, {PbPinRts, 0, "rts"}, {PbPinTx, 1, "tx"}', 'RTS 下拉'),
    (K, 'PbPinFunction = 1,', 'PbPinFunction = 2,', '串口脚功能 1'),
    (B, 'pb_geni_flow(&b->geni, 1, 1);\n    /* 清掉', 'pb_geni_flow(&b->geni, 0, 1);\n    /* 清掉', '开硬件流控'),
    # BT_EN
    (B, '    hw->msleep(hw->ctx, PbBtEnLowMs);\n    /* 一次睡眠可能提前醒，低电平时间按时钟算够 */\n    while (hw->now_ms(hw->ctx) - low_at < PbBtEnLowMs) hw->msleep(hw->ctx, 1);\n', '', 'BT_EN 低够 54 ms'),
    (B, 'hw->msleep(hw->ctx, PbBtEnHighMs);', '', 'BT_EN 高后等 54 ms'),
    (B, 'if (rc) return failed(b, PbStepBtEn, PbErrBtEn, "BT_EN not driven low");', '', '拉低失败不拉高'),
    # GENI
    (G, '#include "BtUtil.h"', '#include "BtUtil.h"\n#undef PB_PACK0_4X8\n#define PB_PACK0_4X8 0x7f8feu', 'TX 打包（UART 低位先发）'),
    (G, 'uint32_t room = used < g->tx_depth ? g->tx_depth - used : 0;', 'uint32_t room = g->tx_depth;', 'TX FIFO 不写爆'),
    (G, 'const uint32_t tail = (fifo & PB_RX_FIFO_LAST) && last && last < PB_BYTES_PER_WORD ? last : PB_BYTES_PER_WORD;', 'const uint32_t tail = PB_BYTES_PER_WORD;', 'RX 半个字'),
    (G, '    if (!(fifo & PB_RX_FIFO_LAST) && (last & 3u)) g->rx_odd_tails++;\n', '', '半字异常要计数'),
    (G, 'const uint32_t tail = (fifo & PB_RX_FIFO_LAST) && last && last < PB_BYTES_PER_WORD ? last : PB_BYTES_PER_WORD;', 'const uint32_t tail = (last & 3u) ? last : PB_BYTES_PER_WORD;', '半个字要看最高位'),
    (G, 'wr(g, PB_SE_RFR_WATERMARK, g->rx_depth - PB_RFR_WATERMARK_GAP);', 'wr(g, PB_SE_RFR_WATERMARK, g->rx_depth + 2u);', 'RFR 水线'),
    (G, 'wr(g, PB_SE_TX_WATERMARK, PB_TX_WATERMARK);', 'wr(g, PB_SE_TX_WATERMARK, 0);', 'TX 水线'),
    (G, 'wr(g, PB_SE_RX_WATERMARK, g->rx_depth - PB_RX_WATERMARK_GAP);', 'wr(g, PB_SE_RX_WATERMARK, 2u);', 'RX 水线'),
    (G, 'wr(g, PB_SE_UART_RX_STALE, PB_STALE_BITS);', 'wr(g, PB_SE_UART_RX_STALE, 160u);', 'stale 取 HSUartDxe 的值'),
    (G, 'return depth >= 16 && depth <= 128 ? depth : 16;', 'return depth >= 4 && depth <= 128 ? depth : 16;', 'FIFO 深度下限（水线不下溢）'),
    (G, 'wr(g, PB_SE_UART_TX_TRANS_CFG, cts ? 0 : PB_UART_CTS_MASK);', 'wr(g, PB_SE_UART_TX_TRANS_CFG, cts ? PB_UART_CTS_MASK : 0);', 'CTS 掩码方向'),
    (G, 'wr(g, PB_SE_UART_MANUAL_RFR, rfr_auto ? 0 : PB_UART_RFR_MANUAL | PB_UART_RFR_NOT_READY);', 'wr(g, PB_SE_UART_MANUAL_RFR, 0);', '手动 RFR'),
    (G, 'if (rc == PbGeniErrTimeout || rc == PbGeniErrBus) {', 'if (0) {', '超时要取消命令'),
    (G, 'if (status & PB_M_IRQ_ERRORS) {\n            g->m_sticky |= status;\n            wr(g, PB_SE_M_IRQ_CLEAR, status);\n            return PbGeniErrBus;\n        }\n        if (status & PB_IRQ_DONE) {', 'if (status & PB_IRQ_DONE) {', '发送错误位'),
    (G, 'wr(g, PB_SE_S_CMD0, PB_UART_START_RX);', 'wr(g, PB_SE_S_CMD0, PB_UART_START_RX | 1u << 7);', 'FIFO 接收不带 RFR_OPEN'),
    (G, 'if (i.proto != PB_PROTO_UART) return PbGeniErrProto;', '', '协议不是 UART 就停'),
    (G, 'if (i.if_disable & 1u) return PbGeniErrNoFifo;', '', 'FIFO 被关就停'),
    (G, 'const uint32_t cfg = (uint32_t)div << 4 | 1u;', 'const uint32_t cfg = (uint32_t)div << 4;', 'SER_CLK_EN'),
    # 帧和 NVM
    (T, 'out[0] = PbH4PeriCmd;', 'out[0] = PbH4Peri;', '发送包类型 0x31'),
    (T, 'peri_header(out, PbOpPeriEdl, (uint8_t)(bytes + 3), PbSubDownload);', 'peri_header(out, PbOpPeriEdl, (uint8_t)(bytes + 2), PbSubDownload);', '外设段的长度字段'),
    (T, 'out[3] = (uint8_t)(bytes + 2);', 'out[3] = (uint8_t)(bytes + 3);', 'EDL 段的长度字段'),
    (T, 'if (edit && edit->baud_code >= 0) data[1] = (uint8_t)edit->baud_code;', 'if (edit && edit->baud_code >= 0) data[0] = (uint8_t)edit->baud_code;', 'Tag 17 改 data[1]'),
    (T, 'if (edit && edit->bdaddr) pb_copy(data, edit->bdaddr, 6);', '', 'Tag 2 写地址'),
    (T, 'int rc = nvm_walk(nvm, bytes, NULL, NULL);      /* 全部校验过才动字节 */', 'int rc = 0;', 'NVM 先校验再改'),
    (T, 'if (size > stop - q - 12) return -4;', '', 'NVM tag 越界'),
    (T, 'if (length > end - p - 4) return -3;', '', 'NVM 块越界'),
    (T, 'if (type == PbTlvNvm) {', 'if (1) {', '只改类型 2 的块'),
    (T, 'if (bytes >= 4 && data[0] == 0x7f && data[1] == \'E\' && data[2] == \'L\' && data[3] == \'F\') return 0;', '', 'ELF 不逐段等'),
    (T, 'e->status = a[9];\n        e->sub = a[10];', 'e->status = a[10];\n        e->sub = a[9];', '完成事件的字段'),
    (T, 'if (m[2] == 0x9e && m[1] == 0x8b && m[0] < 0x40) m[2] ^= 1;', '', '避开保留 LAP'),
    ('BtTlv.h', 'PbSegmentBytes = 243,', 'PbSegmentBytes = 240,', '分段 243 字节'),
    # 序列
    (D, 'if ((rc = wake_soc(d))) return fail(d, PbStepTmel, rc, "second wake");', '', 'TME-L 前再唤醒'),
    (D, 'if ((rc = get_version(d, PbSsTmel, &d->tmel_version))) return rc;', '', 'TME-L 取版本'),
    (D, 'if ((rc = send(d, p, pb_peri_reset(p), kWriteMs))) return rc;\n    if ((rc = peri_wait(d, PbOpPeriGeneral, PbSubReset, &e, kReplyMs))) return rc;', '', '外设复位'),
    (D, 'if ((rc = send(d, p, pb_peri_board_id(p), kWriteMs))) return rc;\n    rc = peri_wait(d, PbOpPeriEdl, PbSubBoardId, &e, kReplyMs);\n    if (rc && rc != PbErrTimeout) return rc;                 /* HAL 取不到板号也继续，只是用默认 NVM */\n    if (!rc && d->h4.have >= 14) say_kx(d, "peri board id", "hi", d->h4.buf[12], "lo", d->h4.buf[13]);', '', '外设取板号'),
    (D, 'if ((rc = send(d, p, pb_peri_build_info(p, PbSsPeri), kWriteMs))) return rc;\n    if ((rc = peri_wait(d, PbOpPeriEdl, PbSubBuildInfo, &e, kReplyMs))) return rc;', '', '外设取构建信息'),
    (D, 'best_effort(d, PbOpOtp, -1, otp, sizeof otp);', '', '和 Android 一样读 OTP'),
    (D, 'best_effort(d, PbOpLog, PbLogSet, logs, sizeof logs);', '', '和 Android 一样发日志设置'),
    (D, 'best_effort(d, PbOpAddOn, -1, addon, sizeof addon);\n    best_effort(d, PbOpOtp, -1, otp, sizeof otp);', 'best_effort(d, PbOpOtp, -1, otp, sizeof otp);\n    best_effort(d, PbOpAddOn, -1, addon, sizeof addon);', '可选命令的顺序'),
    (D, 'if (x->rfr(x->ctx, 0)) return PbErrBaud;\n', '', '换速前关 RFR'),
    (D, 'x->sleep_ms(x->ctx, kBaudSettleMs);', '', '换速等 20 ms'),
    (D, 'if (each || i + 1 == count) {\n            PbPeriEvent e;', 'if (each) {\n            PbPeriEvent e;', '外设最后一段要等应答'),
    (D, 'if (e.status != 0) {\n                say_kx(d, "segment refused", "status", e.status, "segment", i);\n                return PbErrStatus;\n            }\n            if (ss == PbSsPeri', 'if (ss == PbSsPeri', '外设段的状态'),
    (D, 'if (status != 0) {\n                say_kx(d, "segment refused", "status", status, "segment", i);\n                return PbErrStatus;\n            }\n', '', 'HCI 段的状态'),
    (D, 'if (action & PbIbsSendAck) {', 'if (0) {', '0xfd 回 0xfc'),
    (D, 'if (e.status != 0 || e.ss != ss) return PbErrStatus;', '', '版本应答的状态和子系统'),
    (D, 'if (e.ss != ss) return PbErrProtocol;', '', '仲裁应答的子系统'),
    (D, 'if (d->peri_status != PbPatchNone) return PbErrStatus;', '', '未知补丁状态'),
    (D, 'if (d->peri_status == PbPatchDone) return 0;', '', '已打过补丁不重打'),
    (D, 'if (d->tmel_status == PbTmelAbsent1 || d->tmel_status == PbTmelAbsent2) return 0;', '', 'TME-L 不存在就跳过'),
    (D, 'if (e.status == PbPatchDone) return 0;                  /* ActivateSS', '/* ActivateSS', 'ActivateSS 状态 23 不再等'),
    (D, 'return activated_wait(d, kNotifyMs);', 'return 0;', 'ActivateSS 等完成事件'),
    (D, 'if (f[1] == PbEvtHardwareError) {', 'if (0) {', '硬件错误事件'),
    (D, 'return *flag == 1 ? 0 : PbErrStatus;', 'return 0;', '补丁通知的状态'),
    (D, 'if (d->local_version[0] != 0x0e || d->local_version[4] != 0x1d || d->local_version[5] != 0x00)\n        return fail(d, PbStepCheck, PbErrIdentity, "not the expected controller");', '', '控制器身份'),
    (D, 'if (same != 6) return fail(d, PbStepCheck, PbErrIdentity, "BD address is not the one in tag 2");', '', '地址生效'),
    (D, 'if (!d->label_ok) return fail(d, PbStepCheck, PbErrIdentity, "version string lacks " PB_BUILD_LABEL);', '', '版本串'),
    (D, 'if (!report.saw_hci) return fail(d, PbStepPeri, PbErrNvm, "peri NVM without tag 17");', '', '外设 NVM 要有 Tag 17'),
    (D, 'PbNvmEdit peri = {d->code, 1, -1, NULL}, bt = {d->code, 1, 0, d->cfg.bdaddr};', 'PbNvmEdit peri = {0x15, 1, -1, NULL}, bt = {0x15, 1, 0, d->cfg.bdaddr};', 'Tag 17 写实际波特率'),
    (D, 'if (f[1] == PbEvtLeMeta && n >= 5 && f[3] == PbLeAdvReport) d->adverts += f[4];', '', '数广播'),
    (D, '(void)hci_command(d, PbOpLeScanEnable, off, sizeof off, &status);', '', '扫描结束要关'),
    # H4
    (H, 'if ((p[2] & 0xfc) != 0xfc) return PbFilterPass;', 'if (p[2] != 0xfc) return PbFilterPass;', 'OGF 0x3f 全部挡'),
    (H, '== PbDebugHandle ? PbFilterDrop : PbFilterPass;', '== PbDebugHandle ? PbFilterPass : PbFilterPass;', '丢控制器日志'),
    (H, 'return f[1] == PbEvtVendor ? PbFilterDrop : PbFilterPass;', 'return PbFilterPass;', '丢厂商事件'),
    (H, 'case PbH4Peri:\n        return PbFilterDrop;', 'case PbH4Peri:\n        return PbFilterPass;', '丢 0x34'),
    (H, 'if (!r->have) {\n        if (byte == PbIbsWakeAck', 'if (1) {\n        if (byte == PbIbsWakeAck', 'IBS 只在帧边界'),
    (H, 'if (r->need > sizeof r->buf) {', 'if (0) {', '超长帧'),
    (H, 'const uint8_t answer[7] = {PbH4Evt, PbEvtCommandComplete, 4, 1, p[1], p[2], 1};', 'const uint8_t answer[7] = {PbH4Evt, PbEvtCommandComplete, 4, 1, p[1], p[2], 0};', '厂商命令回失败'),
    (H, 'case PbH4Acl: return 5u + (uint32_t)(b[3] | (uint32_t)b[4] << 8);', 'case PbH4Acl: return 5u + b[3];', 'ACL 长度 16 位'),
    # IBS
    ('BtIbs.h', 'PbIbsRetryMs = 10,', 'PbIbsRetryMs = 20,', '0xfd 重发间隔'),
    ('BtIbs.h', 'PbIbsTriesBoot = 10}', 'PbIbsTriesBoot = 4}', 'WakeUpSoC 次数'),
    (I, 'if (s->tries >= s->max_tries) {', 'if (0) {', '次数用完就失败'),
    (I, 's->peer_awake = 0;\n        return 0;', 's->peer_awake = 0;\n        s->tx = PbIbsAsleep;\n        return 0;', '对端 0xfe 不影响主机发送方向'),
    (I, 'if (s->tx == PbIbsAwake && s->sleep_enabled && s->idle_at && now_ms >= s->idle_at) {', 'if (s->tx == PbIbsAwake && s->idle_at && now_ms >= s->idle_at) {', '默认常醒'),
    # PMIC
    (P, 'if (s->owner != 0 || !io->chan_read || !io->chan_write) return PbSpmiNotOwner;', '', 'BT_EN 查 owner'),
    (P, 'if (s->regs[kIdxType] != kGpioType || s->regs[kIdxSubtype] < 0x10 || s->regs[kIdxSubtype] > 0x13) return PbSpmiLayout;', '', 'BT_EN 查类型和子类型'),
    (P, 'const uint8_t source = level ? kSourceHigh : 0;', 'const uint8_t source = level ? 1 : 0;', 'LV/MV 的电平在 bit 7'),
    (P, 'if ((s->after[kIdxSource] & kSourceHigh) != source || !(s->after[kIdxEnable] & kEnable) || (s->after[kIdxMode] & 3) != kModeOutput)\n        return PbSpmiIoError;', '', 'BT_EN 读回'),
    (P, 'if (rc || level < 0) return rc;', 'if (rc) return rc;', 'BT_EN 只读不写'),
    (K, '#define PB_BT_EN_PPID 0x58au', '#define PB_BT_EN_PPID 0x58bu', 'BT_EN 的 PPID'),
    # 阶段 4：参数
    (T, 'value == 43 || value == 48', 'value == 43 || value == 44', '48 是 8 Mbps 的阶段 4'),
    (T, 'a->stand_in = 1;', 'a->stand_in = 0;', '40 是替身'),
    (T, '    a->stop = 0;\n    a->stand_in = 0;\n', '    a->stop = 0;\n', '替身标记不留到下一次'),
    (T, 'if (value >= 1 && value <= 4) {', 'if (value >= 1 && value <= 5) {', '阶段只到 4'),
    (T, 'written[i] = wire[5 - i];', 'written[i] = wire[i];', '书写顺序是线上的反序'),
    (T, 'return f[0] == PbH4Evt && bytes >= 5 && f[1] == PbEvtVendor && f[3] == 1 && (f[4] & 0xfe) == 8;', 'return f[0] == PbH4Evt && bytes >= 5 && f[1] == PbEvtVendor && f[3] == 1;', '崩溃转储的判法'),
    (D, 'if (rc && rc != PbErrTimeout) return fail(d, PbStepHandover, rc, "controller failed at the handover");', '', '交接时控制器崩了不能交'),
    (D, 'if (rc && rc != PbErrTimeout) return fail(d, PbStepHandover, rc, "controller failed at the handover");', 'if (rc) return fail(d, PbStepHandover, rc, "controller failed at the handover");', '交接时没有 0xfc 也往下走'),
    # 阶段 4：发送成帧和过滤的接线
    (H, 't->need = t->buf[0] == PbH4Acl ? 5u + (uint32_t)(t->buf[3] | (uint32_t)t->buf[4] << 8) : 4u + t->buf[3];', 't->need = 4u + t->buf[3];', 'ACL 长度是 16 位'),
    (H, '        if (t->need > sizeof t->buf) {\n            t->skip', '        if (0) {\n            t->skip', '发送方向的超长包'),
    (H, 'if (!t->have && !tx_header_bytes(byte)) {', 'if (!t->have && 0) {', '发送方向不认识的类型字节'),
    (R, '        remember_vsc(b, vendor);\n        answer(b, reply, reply_bytes);', '        remember_vsc(b, vendor);', '厂商命令当场回'),
    (R, 'if (pb_h4_filter_tx(p, bytes, reply, &reply_bytes, &vendor) == PbFilterAnswered) {', 'if (0) {', '厂商命令不下发'),
    (R, 'if (p[0] == PbH4Cmd) {\n            if (p[1] == (uint8_t)PbOpErroneousData', 'if (0) {\n            if (p[1] == (uint8_t)PbOpErroneousData', '替身要回命令'),
    (R, 'stack_ready(b, now_ms(b));', '', '替身也记初始化走完'),
    (R, '} else if (b->link == PbLinkDead) {\n        b->tx_dropped++;\n    } else if (room_for', '} else if (room_for', 'trip 之后不再下发'),
    (R, 'for (uint32_t i = 0; i < bytes && b->client && b->unit == unit; ++i)', 'for (uint32_t i = 0; i < bytes && b->client; ++i)', '别的连接写来的字节不算'),
    (R, 'b->io->kick(b->io->ctx);\n    } else {\n        b->tx_dropped++;', '(void)0;\n    } else {\n        b->tx_dropped++;', '有包要发时叫醒工作线程'),
    # 阶段 4：接收
    (R, 'if (pb_ibs_rx(&b->ibs, b->rx.buf[0], now) & PbIbsSendAck) b->ack_due = 1;', '(void)pb_ibs_rx(&b->ibs, b->rx.buf[0], now);', '0xfd 回 0xfc'),
    (R, 'if (verdict == PbFilterDrop) {\n        b->frames_dropped++;\n    } else if (verdict != PbFilterPass) {', 'if (verdict != PbFilterPass) {', '过滤层丢的和坏帧分开数'),
    (R, '    if (pb_frame_crashed(f, bytes)) {\n        trip(b, PbGuardCrash);\n        return;\n    }\n', '', '控制器崩溃转储'),
    (R, 'if (rc > 0) {\n            b->put_full++;\n            return;\n        }', 'if (rc > 0) {\n            b->put_full++;\n        }', '放不下的帧留着'),
    (R, 'const int room = ring_free(&b->to_client) >= PbBridgeReadRoom;', 'const int room = 1;', '上行环快满时不取串口'),
    (R, 'for (unsigned round = 0; round < PbBridgeReadRounds; ++round) {', 'for (unsigned round = 0; round < 1000000u; ++round) {', '一步读的轮数有上限'),
    (R, 'if (rx) b->rx = *rx;', '', '接过下载路径的成帧状态'),
    (R, 'feed(b, rest, rest_bytes, now);', '', '下载路径多读的字节'),
    (R, 'if (b->link == PbLinkStarting) {\n        const uint64_t now = now_ms(b);', 'if (1) {\n        const uint64_t now = now_ms(b);', '只起来一次'),
    (R, 'if (b->link != PbLinkUp) {\n        const int stop = b->link == PbLinkDead;', 'if (b->link == PbLinkDead) {\n        const int stop = b->link == PbLinkDead;', '没起来不碰串口'),
    # 阶段 4：连接
    (R, 'if (b->link == PbLinkDead) {\n        b->refused++;', 'if (0) {\n        b->refused++;', 'trip 之后拒绝连接'),
    (R, '} else if (b->client) {\n        rc = PbBridgeBusy;', '} else if (0) {\n        rc = PbBridgeBusy;', '只有一个客户端'),
    (R, 'if (!b->client || b->unit != unit) {\n        unlock(b);', 'if (!b->client) {\n        unlock(b);', '被拒的连接断开不影响现在的'),
    (R, 'b->own_reset = (uint8_t)ring_put(&b->to_uart, reset, sizeof reset);', '(void)ring_put(&b->to_uart, reset, sizeof reset);', '内核自己的 HCI_Reset 应答不上交'),
    (R, 'b->own_reset = (uint8_t)ring_put(&b->to_uart, reset, sizeof reset);', '', '断开时复位控制器'),
    (R, 'b->client = 0;\n    ring_clear(&b->to_client);\n    ring_clear(&b->to_uart);\n', 'b->client = 0;\n    ring_clear(&b->to_client);\n', '断开时没发的包作废'),
    (R, '        b->ready = b->ready_noted = 0;\n', '', '新连接重新算初始化'),
    # 阶段 4：守卫
    (R, 'if (b->pending && now >= b->pending_at + PbBridgeCommandMs) trip(b, PbGuardCommand);', '', '命令期限'),
    (R, 'if (b->pending && now >= b->pending_at + PbBridgeCommandMs)', 'if (b->pending && now > b->pending_at + PbBridgeCommandMs)', '命令期限的边界'),
    (R, 'if (opcode == PbOpHostCompleted) return;', '', 'Host_Number_Of_Completed_Packets 没有应答'),
    (R, 'if (!opcode) return 0;\n', '', 'opcode 0 不是应答'),
    (R, '} else if (f[1] == PbEvtCommandStatus && bytes >= 7) {', '} else if (0) {', 'Command Status 也结清'),
    (R, '    b->pending_at = now;\n    if (opcode == PbOpReset) {', '    if (opcode == PbOpReset) {', '应答接着来时期限往后走'),
    (R, 'b->pending = 0;                 /* 控制器复位了', '/* 控制器复位了', 'HCI_Reset 清账'),
    (R, 'if (b->client && !b->ready && now >= from + PbBridgeInitMs) trip(b, PbGuardInit);', '', '初始化期限'),
    (R, 'const uint64_t from = b->connected_at > b->up_at ? b->connected_at : b->up_at;', 'const uint64_t from = b->connected_at;', '控制器起来得晚时从起来算'),
    (R, 'if (b->failed_starts >= PbBridgeFailedStarts) trip(b, PbGuardStarts);', '', '崩溃循环'),
    (R, 'if (!b->ready) b->failed_starts++;', '', '走不完初始化就退了要计数'),
    (R, '    b->ready = 1;\n    b->failed_starts = 0;\n', '    b->ready = 1;\n', '走完初始化清零'),
    (R, 'if (opcode == PbOpErroneousData) stack_ready(b, now);', '', '初始化表最后一步'),
    (R, 'if (status && b->refused_logged < PbBridgeLogRefusals) {', 'if (0) {', '控制器拒绝的命令要留一行'),
    (R, 'if (status && b->refused_logged < PbBridgeLogRefusals) {', 'if (status) {', '拒绝的日志有上限'),
    (R, '        status = f[3];\n', '', 'Command Status 的状态字节'),
    (R, 'if (rc) trip(b, PbGuardWrite);\n        else after_write(b, now);', 'after_write(b, now);', '串口写失败'),
    (R, '            if (rc) {\n                lock(b);\n                trip(b, PbGuardWrite);\n                unlock(b);\n            }\n', '', '0xfc 写失败'),
    (R, 'b->kill_pid = b->client ? b->pid : 0;', 'b->kill_pid = b->pid;', '没人连着不杀'),
    (R, 'b->kill_pid = b->client ? b->pid : 0;', 'b->kill_pid = 0;', 'trip 时连着的进程要杀'),
    (R, '    if (b->guard) return;\n    b->guard = (uint8_t)why;', '    b->guard = (uint8_t)why;', '第一个原因留着'),
    (R, 'if (b->link == PbLinkUp) {\n        if (!b->ready) b->failed_starts++;', 'if (1) {\n        if (!b->ready) b->failed_starts++;', 'Starting 时的断开不算失败'),
    # 阶段 4：等待和轮询
    (R, '        ring_drop(&b->to_uart, bytes);\n        io->wake_room(io->ctx);\n', '        ring_drop(&b->to_uart, bytes);\n', '腾出地方叫醒写的线程'),
    (R, 'if (ring_free(r) < bytes + 2) return 0;', 'if (0) return 0;', '环写满不覆盖'),
    (R, 'else if (ring_used(&b->to_uart)) sleep_ms = 0;', '', '还有包要发不睡'),
    (R, 'else if (b->pending || now < b->active_at + PbBridgeActiveMs) sleep_ms = PbBridgeBusyPollMs;', 'else if (now < b->active_at + PbBridgeActiveMs) sleep_ms = PbBridgeBusyPollMs;', '等应答时勤快'),
    (R, 'else if (b->pending || now < b->active_at + PbBridgeActiveMs) sleep_ms = PbBridgeBusyPollMs;', 'else if (b->pending || 1) sleep_ms = PbBridgeBusyPollMs;', '空闲时降下轮询'),
    (R, 'else sleep_ms = b->client ? PbBridgeIdlePollMs : PbBridgeNoClientPollMs;', 'else sleep_ms = PbBridgeIdlePollMs;', '没有客户端时更懒'),
    (R, '    if (moved) b->active_at = now;\n', '', '动过才算活跃'),
    # 替身
    (S, 'if (opcode == PbOpHostCompleted) return 0;', '', '替身也不回 Host_Number_Of_Completed_Packets'),
    (S, 'if ((opcode >> 10) == 1) return opcode != kOpInquiryCancel;', 'if ((opcode >> 10) == 1) return 0;', 'OGF 1 回 Command Status'),
    (S, 'return opcode != kOpInquiryCancel;', 'return 1;', 'Inquiry_Cancel 例外'),
    (S, 'PbH4Evt, PbEvtCommandStatus, 4, PbStandInDisallowed, 1,', 'PbH4Evt, PbEvtCommandStatus, 4, 0, 1,', 'Command Status 不是成功'),
    (S, 'static const uint8_t kVersion[] = {0x00, 0x0e, 0x00, 0x00, 0x0e, 0x1d, 0x00, 0x21, 0x7e};', 'static const uint8_t kVersion[] = {0x00, 0x0e, 0x00, 0x00, 0x0e, 0x0f, 0x00, 0x21, 0x7e};', '版本是跟踪里的'),
    (S, 'pb_copy(params + 1, address, 6);', '', 'Read_BD_ADDR 的地址'),
    (S, 'for (uint32_t i = sizeof kCommandsHead; i < kCommandBytes; ++i) params[1 + i] = 0xff;', '', '支持的命令要覆盖初始化用到的'),
    (S, 'page == 1 ? kPage1 : page == 2 ? kPage2 : kFeatures + 1', 'kFeatures + 1', '扩展特性按页回'),
    (S, 'params[2] = 2;', 'params[2] = 0;', '扩展特性的最大页'),
    (S, 'return complete(out, opcode, params, 1 + kCommandBytes);', 'return complete(out, opcode, params, kCommandBytes);', 'Read_Local_Supported_Commands 的长度'),
    # 胶水
    (O, 'if (unitinfo) pb_bridge_disconnect(gBridge, unit);', 'pb_bridge_disconnect(gBridge, unit);', '连接被拒时的 disconnect 回调'),
    (O, '    if (rc == PbBridgeBusy) return EBUSY;\n    if (rc) return ECONNREFUSED;\n', '', '连接被拒要返回错误'),
    (O, '    *unitinfo = gBridge;\n', '', '连接成功留下 unitinfo'),
    (O, '    mbuf_freem(chain);\n', '', 'mbuf 链自己放'),
    (O, 'for (mbuf_t m = chain; m; m = mbuf_next(m))', 'for (mbuf_t m = chain; m; m = nullptr)', '走完整条 mbuf 链'),
    (O, 'return error == ENOBUFS || error == ENOMEM ? 1 : -1;', 'return -1;', 'ENOBUFS 留着重试'),
    (O, 'return error == ENOBUFS || error == ENOMEM ? 1 : -1;', 'return 1;', '客户端没了不重试'),
    (O, 'control.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;', 'control.ctl_flags = CTL_FLAG_REG_SOCK_STREAM | CTL_FLAG_PRIVILEGED;', '控制名不设 PRIVILEGED'),
    (O, 'control.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;', 'control.ctl_flags = 0;', '控制名是流式'),
    (O, 'pb_bdaddr_written(address, written);', 'memcpy(written, address, 6);', '服务里的地址是书写顺序'),
    (O, 'putU32(node, "transport-encoding", PbTransportH4)', 'putU32(node, "transport-encoding", 3)', 'transport-encoding 是 H4'),
    (O, '&& putU32(node, "transport-speed", baud)', '', '服务带 transport-speed'),
    (O, 'node->registerService();\n', '', '服务要 registerService'),
    (O, 'void finish()\n{\n    retract();\n    logStats();', 'void finish()\n{\n    logStats();', 'trip 之后撤销'),
    (O, 'if (pid > 0) proc_signal(pid, SIGKILL);', 'if (pid > 0) proc_signal(pid, SIGTERM);', '用 SIGKILL'),
    (O, 'if (pid > 0) proc_signal(pid, SIGKILL);', '', 'trip 之后杀连着的进程'),
    (O, '        chosen->removeProperty(PB_KEY_ADDRESS);\n', '', '撤掉地址'),
    (O, '        product->removeProperty(PB_KEY_LE);\n', '', '撤掉 bluetooth-le'),
    (O, '        gService->terminate();\n', '', '撤掉服务'),
    (O, 'if (!registerControl()) return -13;', 'registerControl();', '控制名注册失败就停'),
    (O, '        retract();\n        (void)ctl_deregister(gControl);', '        retract();', '半路失败撤掉控制名'),
    (O, '        retract();\n        (void)ctl_deregister(gControl);', '        (void)ctl_deregister(gControl);', '半路失败撤掉键'),
]


def run_test(work, name, sources):
    exe = work / name
    built = subprocess.run(['cc', *FLAGS, *(str(work / s) for s in sources), '-o', str(exe)], capture_output=True, text=True, cwd=work)
    if built.returncode:
        return True
    try:
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=120).returncode != 0
    except subprocess.TimeoutExpired:
        return True


def run_replay(work):
    exe = work / 'replay'
    built = subprocess.run(['cc', *FLAGS, *(str(work / s) for s in ('Test/replay.c', 'BtDownload.c', *BRIDGE)), '-o', str(exe)],
                           capture_output=True, text=True, cwd=work)
    if built.returncode:
        return True
    dump = work / 'replay.tx'
    if subprocess.run([str(exe), str(FIRMWARE), str(dump)], capture_output=True, text=True, timeout=120).returncode:
        return True
    return subprocess.run([sys.executable, str(work / 'Test/replay-hsuart.py'), '--trace', str(TRACE), '--generated', str(dump), '--h4', str(exe)],
                          capture_output=True, text=True, timeout=120).returncode != 0


def run_glue(work):
    objects = []
    for name in BRIDGE:
        objects.append(work / (name + '.glue.o'))
        if subprocess.run(['cc', *FLAGS, '-c', str(work / name), '-o', str(objects[-1])], capture_output=True, text=True, cwd=work).returncode:
            return True
    exe = work / 'glue'
    built = subprocess.run(['c++', '-std=c++17', *FLAGS[2:], '-I', str(work / 'Test/kstub'), '-I', str(work), str(work / 'Test/host_glue.cpp'),
                            str(work / 'PaganiBtHost.cpp'), *map(str, objects), '-o', str(exe)], capture_output=True, text=True, cwd=work)
    if built.returncode:
        return True
    try:
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=120).returncode != 0
    except subprocess.TimeoutExpired:
        return True


def run_tables(work):
    exe = work / 'tables'
    built = subprocess.run(['c++', '-std=c++17', '-w', str(work / 'Test/tables.cpp'), '-o', str(exe)], capture_output=True, text=True,
                           cwd=work)
    return built.returncode != 0 or subprocess.run([str(exe)], capture_output=True, text=True).returncode != 0


def one(args):
    index, tmp, replay = args
    name, old, new, what = MUTANTS[index]
    base = tmp / f'm{index}'
    work = base / 'PaganiBt'
    shutil.copytree(SOURCE, work, ignore=shutil.ignore_patterns('*.o', '__pycache__'))
    # Test/tables.cpp 用相对路径找 Wi-Fi 的表
    (base / 'PaganiWifi').symlink_to(SOURCE.parent / 'PaganiWifi')
    text = (work / name).read_text()
    if old not in text:
        return index, what, 'NOT APPLIED'
    (work / name).write_text(text.replace(old, new, 1))
    killers = []
    for test, sources in TESTS.items():
        if run_test(work, test, sources):
            killers.append(test)
    if run_tables(work):
        killers.append('tables')
    if name in (R, S, O, H, T) and run_glue(work):
        killers.append('glue')
    if replay and run_replay(work):
        killers.append('replay')
    shutil.rmtree(work)
    return index, what, killers


def main():
    replay = FIRMWARE.is_dir() and TRACE.is_file()
    if not replay:
        print(f'说明：缺本地固件或跟踪（{FIRMWARE}，{TRACE}；可用 PAGANI_BT_FIRMWARE、PAGANI_BT_TRACE 指定），跳过 replay 这一列')
    pick = sys.argv[1] if len(sys.argv) > 1 else ''
    chosen = [i for i, mutant in enumerate(MUTANTS) if pick in mutant[3]]
    survivors, broken, tally, only_replay = [], [], {}, []
    with tempfile.TemporaryDirectory() as tmp:
        with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
            for index, what, killers in pool.map(one, [(i, Path(tmp), replay) for i in chosen]):
                if killers == 'NOT APPLIED':
                    broken.append(what)
                    print(f'不适用   {what}')
                    continue
                print(f'{"抓到" if killers else "漏网"}  {what}  <- {",".join(killers) if killers else "-"}')
                if not killers:
                    survivors.append(what)
                for test in killers:
                    tally[test] = tally.get(test, 0) + 1
                if killers == ['replay']:
                    only_replay.append(what)
    print('各测试抓到的数量：' + '，'.join(f'{k} {v}' for k, v in sorted(tally.items())))
    if only_replay:
        print('只有真实跟踪抓到的：' + '，'.join(only_replay))
    if broken:
        raise SystemExit('FAIL: 变异没有套上（源码改过了）: ' + ', '.join(broken))
    if survivors:
        raise SystemExit('FAIL: 漏网的变异: ' + ', '.join(survivors))
    print(f'PASS mutate: {len(chosen)} 个变异全部抓到')


if __name__ == '__main__':
    main()
