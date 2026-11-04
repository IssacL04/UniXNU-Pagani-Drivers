/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * CamPmic.h 说明了来源。 */
#include "CamPmic.h"

enum {kCmd = 0x00, kStatus = 0x08, kRdata0 = 0x18, kObsStride = 0x20, kOpRead = 1, kPolls = 1000};

static int find(const PcamSpmiIo *io, unsigned ppid)
{
    if (!io || !io->core || !io->cnfg || ppid > 0xfff) return PcamSpmiArgument;
    const uint32_t version = io->core(io->ctx, 0), count = io->core(io->ctx, 4) & 0x7ffu;
    if (version < 0x70000000u || version >= 0x80000000u || !count || count > 1024) return PcamSpmiLayout;
    int found = PcamSpmiNotFound;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t map = io->core(io->ctx, 0x2000 + 4 * i);
        if (!map || ((map >> 8) & 0xfffu) != ppid) continue;
        const int owner = (int)(io->cnfg(io->ctx, 4 * i) & 7u);
        if (found < 0 || owner == 0) found = (int)i;
    }
    return found;
}

static int read8(const PcamSpmiIo *io, int apid, uint8_t reg, uint8_t *value)
{
    if ((uint32_t)apid * kObsStride + kRdata0 >= PCAM_SPMI_OBS_BYTES) return PcamSpmiArgument;
    const uint32_t ch = (uint32_t)apid * kObsStride;
    io->obs_write(io->ctx, ch + kCmd, (uint32_t)kOpRead << 27 | (uint32_t)reg << 4);
    for (unsigned n = 0; n < kPolls; ++n) {
        const uint32_t status = io->obs_read(io->ctx, ch + kStatus);
        if (status & 0xeu) return PcamSpmiIoError;
        if (status & 1u) {
            *value = (uint8_t)io->obs_read(io->ctx, ch + kRdata0);
            return PcamSpmiOk;
        }
        io->delay_us(io->ctx, 1);
    }
    return PcamSpmiTimeout;
}

int pcam_pmic_rail(const PcamSpmiIo *io, unsigned ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts)
{
    const int apid = find(io, ppid);
    if (apid < 0) return apid;
    uint8_t st = 0, en = 0, lo = 0, hi = 0, md = 0xee;
    int rc = read8(io, apid, 0x08, &st);
    if (!rc) rc = read8(io, apid, 0x46, &en);
    if (!rc) rc = read8(io, apid, 0x40, &lo);
    if (!rc) rc = read8(io, apid, 0x41, &hi);
    if (rc) return rc;
    if (read8(io, apid, 0x45, &md)) md = 0xee;
    if (status) *status = st;
    if (enable) *enable = en;
    if (mode) *mode = md;
    if (millivolts) *millivolts = (uint16_t)(hi << 8 | lo);
    return PcamSpmiOk;
}
