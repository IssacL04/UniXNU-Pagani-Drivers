/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * See WifiPmic.h for the sources this follows. */
#include "WifiPmic.h"

enum {kCmd = 0x00, kStatus = 0x08, kWdata0 = 0x10, kRdata0 = 0x18, kObsStride = 0x20, kOpWrite = 0, kOpRead = 1, kPolls = 1000};

int pw_spmi_find(const PwSpmiIo *io, unsigned ppid, int *owner)
{
    if (!io || !io->core || !io->cnfg || ppid > 0xfff) return PwSpmiArgument;
    const uint32_t version = io->core(io->ctx, 0), count = io->core(io->ctx, 4) & 0x7ffu;
    if (version < 0x70000000u || version >= 0x80000000u || !count || count > 1024) return PwSpmiLayout;
    int found = PwSpmiNotFound, found_owner = -1;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t map = io->core(io->ctx, 0x2000 + 4 * i);
        if (!map || ((map >> 8) & 0xfffu) != ppid) continue;
        const int o = (int)(io->cnfg(io->ctx, 4 * i) & 7u);
        if (found < 0 || o == 0) {found = (int)i; found_owner = o;}
    }
    if (owner) *owner = found_owner;
    return found;
}

int pw_spmi_read8(const PwSpmiIo *io, int apid, uint8_t reg, uint8_t *value)
{
    if (!io || !io->obs_read || !io->obs_write || apid < 0 || (uint32_t)apid * kObsStride + kRdata0 >= PW_SPMI_OBS_BYTES || !value)
        return PwSpmiArgument;
    const uint32_t ch = (uint32_t)apid * kObsStride;
    io->obs_write(io->ctx, ch + kCmd, (uint32_t)kOpRead << 27 | (uint32_t)reg << 4);
    for (unsigned n = 0; n < kPolls; ++n) {
        const uint32_t status = io->obs_read(io->ctx, ch + kStatus);
        if (status & 0xeu) return PwSpmiIoError;
        if (status & 1u) {
            *value = (uint8_t)io->obs_read(io->ctx, ch + kRdata0);
            return PwSpmiOk;
        }
        io->delay_us(io->ctx, 1);
    }
    return PwSpmiTimeout;
}

int pw_pmic_rail(const PwSpmiIo *io, unsigned ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts)
{
    int owner = -1;
    const int apid = pw_spmi_find(io, ppid, &owner);
    if (apid < 0) return apid;
    uint8_t st = 0, en = 0, lo = 0, hi = 0;
    int rc = pw_spmi_read8(io, apid, 0x08, &st);
    if (!rc) rc = pw_spmi_read8(io, apid, 0x46, &en);
    if (!rc) rc = pw_spmi_read8(io, apid, 0x40, &lo);
    if (!rc) rc = pw_spmi_read8(io, apid, 0x41, &hi);
    if (rc) return rc;
    uint8_t md = 0xee;
    if (pw_spmi_read8(io, apid, 0x45, &md)) md = 0xee;
    if (status) *status = st;
    if (enable) *enable = en;
    if (mode) *mode = md;
    if (millivolts) *millivolts = (uint16_t)(hi << 8 | lo);
    return PwSpmiOk;
}

static void read_pin(const PwSpmiIo *io, int apid, uint8_t out[10])
{
    static const uint8_t regs[10] = {0x04, 0x05, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x4a};
    for (unsigned i = 0; i < 10; ++i) {
        uint8_t v = 0xee;
        if (pw_spmi_read8(io, apid, regs[i], &v)) v = 0xee;
        out[i] = v;
    }
}
static int pin_on(const uint8_t regs[10]) {return (regs[8] & 0x80) && (regs[2] & 3) == 1 && (regs[6] & 0xf) == 2;}

static int chan_write8(const PwSpmiIo *io, unsigned apid, uint8_t reg, uint8_t value)
{
    io->chan_write(io->ctx, apid, kWdata0, value);
    io->chan_write(io->ctx, apid, kCmd, (uint32_t)kOpWrite << 27 | (uint32_t)reg << 4);
    for (unsigned n = 0; n < kPolls; ++n) {
        const uint32_t status = io->chan_read(io->ctx, apid, kStatus);
        if (status & 0xeu) return PwSpmiIoError;
        if (status & 1u) return PwSpmiOk;
        io->delay_us(io->ctx, 1);
    }
    return PwSpmiTimeout;
}

int pw_pmic_sleep_clock(const PwSpmiIo *io, int write, PwSleepClock *out)
{
    PwSleepClock local;
    PwSleepClock *s = out ? out : &local;
    for (unsigned i = 0; i < 10; ++i) s->regs[i] = s->after[i] = 0;
    s->written = 0;
    s->owner = -1;
    s->apid = pw_spmi_find(io, PW_SLEEP_CLOCK_PPID, &s->owner);
    if (s->apid < 0) return s->apid;
    read_pin(io, s->apid, s->regs);
    for (unsigned i = 0; i < 10; ++i) s->after[i] = s->regs[i];
    if (s->regs[0] == 0xee) return PwSpmiIoError;
    if (pin_on(s->regs)) return 1;
    if (!write) return 0;
    if (s->owner != 0 || !io->chan_read || !io->chan_write) return PwSpmiNotOwner;
    if (s->regs[1] < 0x10 || s->regs[1] > 0x13) return PwSpmiLayout;
    static const uint8_t seq[5][2] = {{0x42, 5}, {0x41, 0}, {0x44, 2}, {0x40, 1}, {0x46, 0x80}};
    for (unsigned i = 0; i < 5; ++i) {
        const int rc = chan_write8(io, (unsigned)s->apid, seq[i][0], seq[i][1]);
        if (rc) return rc;
        s->written++;
    }
    read_pin(io, s->apid, s->after);
    return pin_on(s->after) ? 2 : PwSpmiIoError;
}
