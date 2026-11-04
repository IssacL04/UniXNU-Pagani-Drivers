/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* SPMI arbiter v7 access of the haptics module (HapticsSpmi.h). */
#include "HapticsSpmi.h"
#include "HapticsRegs.h"
#include "HapticsText.h"

enum { E_PERM = -1, E_IO = -5, E_NODEV = -19, E_INVAL = -22, E_TIMEDOUT = -110 };

int pgh_spmi_discover(PghSpmi *s, const PghSpmiIo *io, unsigned ee, const uint16_t ppid[PGH_PERIPHS], unsigned vmax_code_max)
{
    if (!s) return E_INVAL;
    pgh_zero(s, sizeof *s);
    for (unsigned k = 0; k < PGH_PERIPHS; ++k) s->p[k].apid = -1;
    if (!io || !io->core || !io->cnfg || !io->obs_read || !io->obs_write || !io->delay_us || !ppid || ee > 7 || vmax_code_max > 0xff)
        return E_INVAL;
    s->io = io;
    s->ee = ee;
    s->vmax_code_max = (uint8_t)vmax_code_max;
    s->version = io->core(io->ctx, PGH_ARB_VERSION);
    s->channels = io->core(io->ctx, PGH_ARB_FEATURES) & 0x7ffu;
    if (s->version < 0x70000000u || s->version >= 0x80000000u || !s->channels || s->channels > 1024) {
        s->closed = 1;
        return E_NODEV;
    }
    for (unsigned k = 0; k < PGH_PERIPHS; ++k) s->p[k].ppid = (uint16_t)(ppid[k] & 0xfffu);
    for (uint32_t n = 0; n < s->channels; ++n) {
        const uint32_t map = io->core(io->ctx, PGH_ARB_CHAN_MAP(n));
        if (!map) continue;
        const uint16_t found = (uint16_t)((map >> 8) & 0xfffu);
        for (unsigned k = 0; k < PGH_PERIPHS; ++k) {
            PghPeriph *p = &s->p[k];
            if (found != p->ppid) continue;
            const uint8_t owner = (uint8_t)(io->cnfg(io->ctx, PGH_ARB_OWNER(n)) & 7u);
            if (p->mappings < 255) p->mappings++;
            /* the first mapping, replaced by the first one this EE owns */
            if (p->apid < 0 || (p->owner != ee && owner == ee)) {
                p->apid = (int16_t)n;
                p->owner = owner;
                p->irq = (uint8_t)((map >> 24) & 1u);
            }
        }
    }
    return 0;
}

/* One observer read transaction: command, bounded wait, data. */
static int obs_transaction(PghSpmi *s, unsigned periph, uint8_t reg, unsigned len, uint32_t *data)
{
    const PghSpmiIo *io = s->io;
    const PghPeriph *p = &s->p[periph];
    const uint32_t ch = (uint32_t)p->apid * PGH_ARB_OBS_STRIDE;
    io->obs_write(io->ctx, ch + PGH_ARB_CMD, PGH_ARB_OP_EXT_READL << 27 | (uint32_t)reg << 4 | (len - 1));
    s->reads++;
    for (unsigned n = 0; n < PGH_ARB_POLLS; ++n) {
        const uint32_t status = io->obs_read(io->ctx, ch + PGH_ARB_STATUS);
        if (status & PGH_ARB_FAILED) {
            s->failures++;
            s->last_status = status;
            s->last_periph = (uint8_t)periph;
            s->last_reg = reg;
            return s->last_rc = E_IO;
        }
        if (status & 1u) {
            *data = io->obs_read(io->ctx, ch + PGH_ARB_RDATA0);
            return 0;
        }
        io->delay_us(io->ctx, 1);
    }
    s->timeouts++;
    s->last_periph = (uint8_t)periph;
    s->last_reg = reg;
    return s->last_rc = E_TIMEDOUT;
}

/* The transaction that failed was dropped by the arbiter and may go again: counted, after the pause. */
static int drop_again(PghSpmi *s, int rc, unsigned tries)
{
    if (rc != E_IO || !s->retry_dropped || tries >= PGH_ARB_DROP_RETRIES || (s->last_status & PGH_ARB_FAILED) != PGH_ARB_DROPPED) return 0;
    s->dropped++;
    s->io->delay_us(s->io->ctx, PGH_ARB_DROP_PAUSE_US);
    return 1;
}

int pgh_spmi_read(PghSpmi *s, unsigned periph, uint8_t reg, uint8_t *data, unsigned len)
{
    if (!s || !s->io || periph >= PGH_PERIPHS || !data || !len || len > 4 || (unsigned)reg + len > 0x100u) return E_INVAL;
    if (s->p[periph].apid < 0) return E_NODEV;
    uint32_t word = 0;
    int rc = obs_transaction(s, periph, reg, len, &word);
    for (unsigned tries = 0; drop_again(s, rc, tries); ++tries) rc = obs_transaction(s, periph, reg, len, &word);
    if (rc) return rc;
    for (unsigned i = 0; i < len; ++i) data[i] = (uint8_t)(word >> (8 * i));
    return 0;
}

int pgh_spmi_identify(PghSpmi *s, unsigned periph)
{
    if (!s || !s->io || periph >= PGH_PERIPHS) return E_INVAL;
    PghPeriph *p = &s->p[periph];
    p->identified = 0;
    uint8_t rev[4], id[2];
    int rc = pgh_spmi_read(s, periph, PGH_REG_REVISION1, rev, 4);
    if (!rc) rc = pgh_spmi_read(s, periph, PGH_REG_TYPE, id, 2);
    if (rc) return rc;
    for (unsigned i = 0; i < 4; ++i) p->rev[i] = rev[i];
    p->type = id[0];
    p->subtype = id[1];
    p->identified = 1;
    return 0;
}

/* The channel table entry still maps the PPID and this EE still owns it. */
static int channel_ours(const PghSpmi *s, const PghPeriph *p)
{
    const PghSpmiIo *io = s->io;
    if (p->apid < 0 || (uint32_t)p->apid >= s->channels) return 0;
    const uint32_t map = io->core(io->ctx, PGH_ARB_CHAN_MAP((uint32_t)p->apid));
    const uint32_t owner = io->cnfg(io->ctx, PGH_ARB_OWNER((uint32_t)p->apid)) & 7u;
    return ((map >> 8) & 0xfffu) == p->ppid && owner == s->ee;
}

int pgh_spmi_open(PghSpmi *s, unsigned periph)
{
    if (!s || !s->io || (periph != PGH_CFG && periph != PGH_PTN)) return E_PERM;
    PghPeriph *p = &s->p[periph];
    if (s->closed || !p->identified || p->owner != s->ee || !s->io->map_channel || !s->io->chan_read || !s->io->chan_write ||
        !channel_ours(s, p))
        return E_PERM;
    const int rc = s->io->map_channel(s->io->ctx, periph, (unsigned)p->apid);
    if (rc) return rc;
    p->writable = 1;
    return 0;
}

int pgh_spmi_allowed(const PghSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len)
{
    if (!s || !data || !len || len > 8) return 0;
    if (periph == PGH_CFG) {
        if (reg == PGH_CFG_TLRA_OL_HIGH) return len <= 2 && !(data[0] & 0xf0u);
        if (len != 1) return 0;
        const uint8_t v = data[0];
        switch (reg) {
        case PGH_CFG_EN_CTL: return v == 0 || v == PGH_HAPTICS_EN;
        case PGH_CFG_VMAX: return v <= s->vmax_code_max;
        case PGH_CFG_SPMI_PLAY:   /* FIFO or DIRECT_PLAY only (no SWR, no pattern sources) */
            return !(v & 0x70u) && (v & PGH_PAT_SRC_MASK) <= PGH_SRC_DIRECT;
        case PGH_CFG_VMAX_HDRM: return v <= PGH_HDRM_DEFAULT_MV / PGH_VMAX_STEP_MV;
        case PGH_CFG_MOD_STATUS_XT: return v == 0 || v == 0x80u;
        case PGH_CFG_MOD_STATUS_SEL: return v <= 0x07u;
        case PGH_CFG_TLRA_OL_LOW:
        case PGH_CFG_DRV_WF_SEL:
        case PGH_CFG_AUTO_SHUTDOWN:
        case PGH_CFG_TRIG_PRIORITY:
        case PGH_CFG_BRAKE_MODE_CFG:
        case PGH_CFG_DRV_DUTY_CFG:
        case PGH_CFG_AUTORES_CFG:
        case PGH_CFG_FAULT_CLR:
        case PGH_CFG_ZX_SYNC_CFG:
        case PGH_CFG_CAL_EN:
            return 1;
        default: return 0;
        }
    }
    if (periph == PGH_PTN) {
        if (reg == PGH_PTN_FIFO_DIN_0) return len == 4;
        if (reg >= PGH_PTN_BRAKE_AMP && reg < PGH_PTN_BRAKE_AMP + PGH_BRAKE_SAMPLES)
            return (unsigned)reg + len <= PGH_PTN_BRAKE_AMP + PGH_BRAKE_SAMPLES;
        if (len != 1) return 0;
        const uint8_t v = data[0];
        switch (reg) {
        case PGH_PTN_FIFO_DIN_1B:
        case PGH_PTN_DIRECT_PLAY:
        case PGH_PTN_CL_VDRIVE_CTL:
            return 1;
        case PGH_PTN_FIFO_PLAY_RATE: return (v & PGH_FIFO_RATE_MASK) != 7u && (v & PGH_FIFO_RATE_MASK) <= PGH_RATE_F_48KHZ;
        case PGH_PTN_FIFO_EMPTY_CFG: return v <= PGH_FIFO_THRESH_MASK;
        case PGH_PTN_MEM_OP_ACCESS: return v == 0 || v == PGH_MEM_FLUSH_RELOAD;   /* never the pattern memory access bit */
        case PGH_PTN_MMAP_FIFO: return v == (PGH_MMAP_FIFO_EXIST | (PGH_MEM_BYTES / PGH_MMAP_UNIT - 1));
        default: return 0;
        }
    }
    return 0;   /* HBOOST, SDAM: never */
}

static int write_transaction(PghSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len)
{
    const PghSpmiIo *io = s->io;
    uint32_t w0 = 0, w1 = 0;
    for (unsigned i = 0; i < len && i < 4; ++i) w0 |= (uint32_t)data[i] << (8 * i);
    for (unsigned i = 4; i < len; ++i) w1 |= (uint32_t)data[i] << (8 * (i - 4));
    io->chan_write(io->ctx, periph, PGH_ARB_WDATA0, w0);
    if (len > 4) io->chan_write(io->ctx, periph, PGH_ARB_WDATA1, w1);
    io->chan_write(io->ctx, periph, PGH_ARB_CMD, PGH_ARB_OP_EXT_WRITEL << 27 | (uint32_t)reg << 4 | (len - 1));
    s->writes++;
    for (unsigned n = 0; n < PGH_ARB_POLLS; ++n) {
        const uint32_t status = io->chan_read(io->ctx, periph, PGH_ARB_STATUS);
        if (status & PGH_ARB_FAILED) {
            s->failures++;
            s->last_status = status;
            return E_IO;
        }
        if (status & 1u) return 0;
        io->delay_us(io->ctx, 1);
    }
    s->timeouts++;
    return E_TIMEDOUT;
}

static int refuse(PghSpmi *s, unsigned periph, uint8_t reg, int rc)
{
    s->refused++;
    s->closed = 1;
    s->last_rc = rc;
    s->last_periph = (uint8_t)periph;
    s->last_reg = reg;
    return rc;
}

/* The write and, while the arbiter only drops it, its repeats: the channel is checked before each. */
static int write_checked(PghSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len)
{
    for (unsigned tries = 0;; ++tries) {
        if (!channel_ours(s, &s->p[periph])) return E_PERM;
        const int rc = write_transaction(s, periph, reg, data, len);
        if (!drop_again(s, rc, tries)) return rc;
    }
}

int pgh_spmi_write(PghSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len)
{
    if (!s || !s->io) return E_INVAL;
    if (periph >= PGH_PERIPHS || !s->p[periph].writable || s->closed || !pgh_spmi_allowed(s, periph, reg, data, len))
        return refuse(s, periph, reg, E_PERM);
    const int rc = write_checked(s, periph, reg, data, len);
    if (rc == E_PERM) return refuse(s, periph, reg, E_PERM);
    if (rc) {
        s->closed = 1;
        s->last_rc = rc;
        s->last_periph = (uint8_t)periph;
        s->last_reg = reg;
    }
    return rc;
}

int pgh_spmi_emergency_stop(PghSpmi *s)
{
    if (!s || !s->io || !s->p[PGH_CFG].writable || s->emergency_used) return E_PERM;
    s->emergency_used = 1;
    const uint8_t zero = 0;
    return write_checked(s, PGH_CFG, PGH_CFG_SPMI_PLAY, &zero, 1);
}
