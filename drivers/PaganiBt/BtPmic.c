/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * BtPmic.h 说明了来源。 */
#include "BtPmic.h"
#include "PaganiBtBoard.h"

enum {kCmd = 0x00, kStatus = 0x08, kWdata0 = 0x10, kRdata0 = 0x18, kObsStride = 0x20, kOpWrite = 0, kOpRead = 1, kPolls = 1000};
enum {kGpioType = 0x10, kRegType = 0x04, kRegSubtype = 0x05, kRegMode = 0x40, kRegVin = 0x41, kRegPull = 0x42, kRegSource = 0x44,
      kRegOut = 0x45, kRegEnable = 0x46, kModeOutput = 1, kPullNone = 5, kSourceHigh = 0x80, kEnable = 0x80};
static const uint8_t kGpioRegs[PbGpioRegs] = {kRegType, kRegSubtype, kRegMode, kRegVin, kRegPull, kRegSource, kRegOut, kRegEnable};
enum {kIdxType, kIdxSubtype, kIdxMode, kIdxVin, kIdxPull, kIdxSource, kIdxOut, kIdxEnable};

int pb_spmi_find(const PbSpmiIo *io, unsigned ppid, int *owner)
{
    if (!io || !io->core || !io->cnfg || ppid > 0xfff) return PbSpmiArgument;
    const uint32_t version = io->core(io->ctx, 0), count = io->core(io->ctx, 4) & 0x7ffu;
    if (version < 0x70000000u || version >= 0x80000000u || !count || count > 1024) return PbSpmiLayout;
    int found = PbSpmiNotFound, found_owner = -1;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t map = io->core(io->ctx, 0x2000 + 4 * i);
        if (!map || ((map >> 8) & 0xfffu) != ppid) continue;
        const int o = (int)(io->cnfg(io->ctx, 4 * i) & 7u);
        if (found < 0 || o == 0) {found = (int)i; found_owner = o;}
    }
    if (owner) *owner = found_owner;
    return found;
}

int pb_spmi_read8(const PbSpmiIo *io, int apid, uint8_t reg, uint8_t *value)
{
    if (!io || !io->obs_read || !io->obs_write || apid < 0 || (uint32_t)apid * kObsStride + kRdata0 >= PB_SPMI_OBS_BYTES || !value)
        return PbSpmiArgument;
    const uint32_t ch = (uint32_t)apid * kObsStride;
    io->obs_write(io->ctx, ch + kCmd, (uint32_t)kOpRead << 27 | (uint32_t)reg << 4);
    for (unsigned n = 0; n < kPolls; ++n) {
        const uint32_t status = io->obs_read(io->ctx, ch + kStatus);
        if (status & 0xeu) return PbSpmiIoError;
        if (status & 1u) {
            *value = (uint8_t)io->obs_read(io->ctx, ch + kRdata0);
            return PbSpmiOk;
        }
        io->delay_us(io->ctx, 1);
    }
    return PbSpmiTimeout;
}

int pb_pmic_rail(const PbSpmiIo *io, unsigned ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts)
{
    int owner = -1;
    const int apid = pb_spmi_find(io, ppid, &owner);
    if (apid < 0) return apid;
    uint8_t st = 0, en = 0, lo = 0, hi = 0;
    int rc = pb_spmi_read8(io, apid, 0x08, &st);
    if (!rc) rc = pb_spmi_read8(io, apid, 0x46, &en);
    if (!rc) rc = pb_spmi_read8(io, apid, 0x40, &lo);
    if (!rc) rc = pb_spmi_read8(io, apid, 0x41, &hi);
    if (rc) return rc;
    uint8_t md = 0xee;
    if (pb_spmi_read8(io, apid, 0x45, &md)) md = 0xee;
    if (status) *status = st;
    if (enable) *enable = en;
    if (mode) *mode = md;
    if (millivolts) *millivolts = (uint16_t)(hi << 8 | lo);
    return PbSpmiOk;
}

static void read_pin(const PbSpmiIo *io, int apid, uint8_t out[PbGpioRegs])
{
    for (unsigned i = 0; i < PbGpioRegs; ++i) {
        uint8_t v = 0xee;
        if (pb_spmi_read8(io, apid, kGpioRegs[i], &v)) v = 0xee;
        out[i] = v;
    }
}

static int chan_write8(const PbSpmiIo *io, unsigned apid, uint8_t reg, uint8_t value)
{
    io->chan_write(io->ctx, apid, kWdata0, value);
    io->chan_write(io->ctx, apid, kCmd, (uint32_t)kOpWrite << 27 | (uint32_t)reg << 4);
    for (unsigned n = 0; n < kPolls; ++n) {
        const uint32_t status = io->chan_read(io->ctx, apid, kStatus);
        if (status & 0xeu) return PbSpmiIoError;
        if (status & 1u) return PbSpmiOk;
        io->delay_us(io->ctx, 1);
    }
    return PbSpmiTimeout;
}

/* 共用的开头：找通道、读一遍寄存器。 */
static int pin_open(const PbSpmiIo *io, unsigned ppid, PbGpioState *s)
{
    for (unsigned i = 0; i < PbGpioRegs; ++i) s->regs[i] = s->after[i] = 0;
    s->written = 0;
    s->owner = -1;
    s->apid = pb_spmi_find(io, ppid, &s->owner);
    if (s->apid < 0) return s->apid;
    read_pin(io, s->apid, s->regs);
    for (unsigned i = 0; i < PbGpioRegs; ++i) s->after[i] = s->regs[i];
    return s->regs[kIdxType] == 0xee ? PbSpmiIoError : PbSpmiOk;
}
static int pin_writable(const PbSpmiIo *io, const PbGpioState *s)
{
    if (s->owner != 0 || !io->chan_read || !io->chan_write) return PbSpmiNotOwner;
    if (s->regs[kIdxType] != kGpioType || s->regs[kIdxSubtype] < 0x10 || s->regs[kIdxSubtype] > 0x13) return PbSpmiLayout;
    return PbSpmiOk;
}
static int clock_on(const uint8_t r[PbGpioRegs]) {return (r[kIdxEnable] & kEnable) && (r[kIdxMode] & 3) == kModeOutput && (r[kIdxSource] & 0xf) == 2;}

int pb_pmic_sleep_clock(const PbSpmiIo *io, int write, PbGpioState *out)
{
    PbGpioState local;
    PbGpioState *s = out ? out : &local;
    int rc = pin_open(io, PB_SLEEP_CLOCK_PPID, s);
    if (rc) return rc;
    if (clock_on(s->regs)) return 1;
    if (!write) return 0;
    if ((rc = pin_writable(io, s))) return rc;
    static const uint8_t seq[5][2] = {{kRegPull, kPullNone}, {kRegVin, 0}, {kRegSource, 2}, {kRegMode, kModeOutput}, {kRegEnable, kEnable}};
    for (unsigned i = 0; i < 5; ++i) {
        if ((rc = chan_write8(io, (unsigned)s->apid, seq[i][0], seq[i][1]))) return rc;
        s->written++;
    }
    read_pin(io, s->apid, s->after);
    return clock_on(s->after) ? 2 : PbSpmiIoError;
}

int pb_pmic_bt_en(const PbSpmiIo *io, int level, PbGpioState *out)
{
    PbGpioState local;
    PbGpioState *s = out ? out : &local;
    int rc = pin_open(io, PB_BT_EN_PPID, s);
    if (rc || level < 0) return rc;
    if ((rc = pin_writable(io, s))) return rc;
    const uint8_t source = level ? kSourceHigh : 0;
    /* 配置已经对时只写电平，每次脉冲不重复写配置 */
    const int configured = (s->regs[kIdxEnable] & kEnable) && (s->regs[kIdxMode] & 3) == kModeOutput && (s->regs[kIdxPull] & 7) == kPullNone &&
                           (s->regs[kIdxVin] & 7) == PbBtEnVin && !(s->regs[kIdxOut] & 0x30);
    if (!configured) {
        /* 先定上下拉和电源，再定输出类型，再给电平，最后开输出和使能：pinctrl-spmi-gpio 的顺序，使能前电平已经确定 */
        const uint8_t seq[5][2] = {{kRegPull, kPullNone}, {kRegVin, PbBtEnVin}, {kRegOut, (uint8_t)(s->regs[kIdxOut] & 0xcfu)}, {kRegSource, source},
                                   {kRegMode, kModeOutput}};
        for (unsigned i = 0; i < 5; ++i) {
            if ((rc = chan_write8(io, (unsigned)s->apid, seq[i][0], seq[i][1]))) return rc;
            s->written++;
        }
        if ((rc = chan_write8(io, (unsigned)s->apid, kRegEnable, kEnable))) return rc;
        s->written++;
    } else {
        if ((rc = chan_write8(io, (unsigned)s->apid, kRegSource, source))) return rc;
        s->written++;
    }
    read_pin(io, s->apid, s->after);
    if ((s->after[kIdxSource] & kSourceHigh) != source || !(s->after[kIdxEnable] & kEnable) || (s->after[kIdxMode] & 3) != kModeOutput)
        return PbSpmiIoError;
    return PbSpmiOk;
}
