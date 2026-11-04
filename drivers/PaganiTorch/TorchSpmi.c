/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* SPMI arbiter v7 access of the torch module (TorchSpmi.h): PaganiHaptics' HapticsSpmi.c with this module's prefix, one peripheral,
 * the flash module's whitelist, the channel interlock and a two-write emergency stop. */
#include "TorchSpmi.h"
#include "TorchRegs.h"
#include "TorchText.h"

enum { E_PERM = -1, E_IO = -5, E_NODEV = -19, E_INVAL = -22, E_TIMEDOUT = -110 };

int pgt_spmi_discover(PgtSpmi *s, const PgtSpmiIo *io, unsigned ee, const uint16_t ppid[PGT_PERIPHS], unsigned chan_mask,
                      unsigned itarget_max)
{
    if (!s) return E_INVAL;
    pgt_zero(s, sizeof *s);
    for (unsigned k = 0; k < PGT_PERIPHS; ++k) s->p[k].apid = -1;
    if (!io || !io->core || !io->cnfg || !io->obs_read || !io->obs_write || !io->delay_us || !ppid || ee > 7 || !chan_mask ||
        chan_mask > (1u << PGT_CHANNELS) - 1u || !itarget_max || itarget_max > PGT_ITARGET_MASK)
        return E_INVAL;
    s->io = io;
    s->ee = ee;
    s->chan_mask = (uint8_t)chan_mask;
    s->itarget_max = (uint8_t)itarget_max;
    s->version = io->core(io->ctx, PGT_ARB_VERSION);
    s->channels = io->core(io->ctx, PGT_ARB_FEATURES) & 0x7ffu;
    if (s->version < 0x70000000u || s->version >= 0x80000000u || !s->channels || s->channels > 1024) {
        s->closed = 1;
        return E_NODEV;
    }
    for (unsigned k = 0; k < PGT_PERIPHS; ++k) s->p[k].ppid = (uint16_t)(ppid[k] & 0xfffu);
    for (uint32_t n = 0; n < s->channels; ++n) {
        const uint32_t map = io->core(io->ctx, PGT_ARB_CHAN_MAP(n));
        if (!map) continue;
        const uint16_t found = (uint16_t)((map >> 8) & 0xfffu);
        for (unsigned k = 0; k < PGT_PERIPHS; ++k) {
            PgtPeriph *p = &s->p[k];
            if (found != p->ppid) continue;
            const uint8_t owner = (uint8_t)(io->cnfg(io->ctx, PGT_ARB_OWNER(n)) & 7u);
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
static int obs_transaction(PgtSpmi *s, unsigned periph, uint8_t reg, unsigned len, uint32_t *data)
{
    const PgtSpmiIo *io = s->io;
    const PgtPeriph *p = &s->p[periph];
    const uint32_t ch = (uint32_t)p->apid * PGT_ARB_OBS_STRIDE;
    io->obs_write(io->ctx, ch + PGT_ARB_CMD, PGT_ARB_OP_EXT_READL << 27 | (uint32_t)reg << 4 | (len - 1));
    s->reads++;
    for (unsigned n = 0; n < PGT_ARB_POLLS; ++n) {
        const uint32_t status = io->obs_read(io->ctx, ch + PGT_ARB_STATUS);
        if (status & 0xeu) {
            s->failures++;
            s->last_status = status;
            s->last_periph = (uint8_t)periph;
            s->last_reg = reg;
            return s->last_rc = E_IO;
        }
        if (status & 1u) {
            *data = io->obs_read(io->ctx, ch + PGT_ARB_RDATA0);
            return 0;
        }
        io->delay_us(io->ctx, 1);
    }
    s->timeouts++;
    s->last_periph = (uint8_t)periph;
    s->last_reg = reg;
    return s->last_rc = E_TIMEDOUT;
}

int pgt_spmi_read(PgtSpmi *s, unsigned periph, uint8_t reg, uint8_t *data, unsigned len)
{
    if (!s || !s->io || periph >= PGT_PERIPHS || !data || !len || len > 4 || (unsigned)reg + len > 0x100u) return E_INVAL;
    if (s->p[periph].apid < 0) return E_NODEV;
    uint32_t word = 0;
    const int rc = obs_transaction(s, periph, reg, len, &word);
    if (rc) return rc;
    for (unsigned i = 0; i < len; ++i) data[i] = (uint8_t)(word >> (8 * i));
    return 0;
}

int pgt_spmi_identify(PgtSpmi *s, unsigned periph)
{
    if (!s || !s->io || periph >= PGT_PERIPHS) return E_INVAL;
    PgtPeriph *p = &s->p[periph];
    p->identified = 0;
    uint8_t rev[4], id[2];
    int rc = pgt_spmi_read(s, periph, PGT_REG_REVISION1, rev, 4);
    if (!rc) rc = pgt_spmi_read(s, periph, PGT_REG_TYPE, id, 2);
    if (rc) return rc;
    for (unsigned i = 0; i < 4; ++i) p->rev[i] = rev[i];
    p->type = id[0];
    p->subtype = id[1];
    p->identified = 1;
    return 0;
}

/* The channel table entry still maps the PPID and this EE still owns it. */
static int channel_ours(const PgtSpmi *s, const PgtPeriph *p)
{
    const PgtSpmiIo *io = s->io;
    if (p->apid < 0 || (uint32_t)p->apid >= s->channels) return 0;
    const uint32_t map = io->core(io->ctx, PGT_ARB_CHAN_MAP((uint32_t)p->apid));
    const uint32_t owner = io->cnfg(io->ctx, PGT_ARB_OWNER((uint32_t)p->apid)) & 7u;
    return ((map >> 8) & 0xfffu) == p->ppid && owner == s->ee;
}

int pgt_spmi_open(PgtSpmi *s, unsigned periph)
{
    if (!s || !s->io || periph != PGT_FLASH) return E_PERM;
    PgtPeriph *p = &s->p[periph];
    if (s->closed || !p->identified || p->owner != s->ee || !s->io->map_channel || !s->io->chan_read || !s->io->chan_write ||
        !channel_ours(s, p))
        return E_PERM;
    if (!p->writable) {
        const int rc = s->io->map_channel(s->io->ctx, periph, (unsigned)p->apid);
        if (rc) return rc;
    }
    p->writable = 1;
    s->ires_set = 0;
    s->itarget_set = s->strobe_set = s->timer_set = 0;
    return 0;
}

/* The channel of a per-channel register (base .. base + 3), or -1. */
static int channel_of(uint8_t reg, unsigned base)
{
    return reg >= base && reg < base + PGT_CHANNELS ? (int)(reg - base) : -1;
}

int pgt_spmi_allowed(const PgtSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len)
{
    if (!s || !data || len != 1 || periph != PGT_FLASH) return 0;   /* single bytes of the flash module only */
    const uint8_t v = data[0], mask = s->chan_mask;
    int ch;
    if ((ch = channel_of(reg, PGT_FLASH_ITARGET(0))) >= 0)        /* the field is 6:0; bit 7 is kept as read */
        return (mask >> ch & 1u) && (v & PGT_ITARGET_MASK) <= s->itarget_max;
    if ((ch = channel_of(reg, PGT_FLASH_CHAN_TIMER(0))) >= 0)     /* torch mode: the safety timer off */
        return (mask >> ch & 1u) && v == 0;
    if ((ch = channel_of(reg, PGT_FLASH_CHAN_STROBE(0))) >= 0)    /* software strobe, level, active high; bit 7 as read */
        return (mask >> ch & 1u) && (v & PGT_STROBE_FIELD) == PGT_STROBE_SW_TORCH;
    switch (reg) {
    case PGT_FLASH_EN_CTL:   /* the module enable bit only */
        return !(v & (uint8_t)~PGT_MODULE_EN);
    case PGT_FLASH_IRES:     /* 5 mA steps on every board channel; the others' bits as read */
        return (v & mask) == mask;
    case PGT_FLASH_CHAN_EN: {  /* board channels only, each switched on only after the interlock writes */
        if (v & (uint8_t)~mask) return 0;
        const uint8_t ready = s->ires_set ? (uint8_t)(s->itarget_set & s->strobe_set & s->timer_set) : 0;
        return (v & (uint8_t)~ready) == 0;
    }
    default:
        return 0;
    }
}

static int write_transaction(PgtSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len)
{
    const PgtSpmiIo *io = s->io;
    uint32_t w0 = 0, w1 = 0;
    for (unsigned i = 0; i < len && i < 4; ++i) w0 |= (uint32_t)data[i] << (8 * i);
    for (unsigned i = 4; i < len; ++i) w1 |= (uint32_t)data[i] << (8 * (i - 4));
    io->chan_write(io->ctx, periph, PGT_ARB_WDATA0, w0);
    if (len > 4) io->chan_write(io->ctx, periph, PGT_ARB_WDATA1, w1);
    io->chan_write(io->ctx, periph, PGT_ARB_CMD, PGT_ARB_OP_EXT_WRITEL << 27 | (uint32_t)reg << 4 | (len - 1));
    s->writes++;
    for (unsigned n = 0; n < PGT_ARB_POLLS; ++n) {
        const uint32_t status = io->chan_read(io->ctx, periph, PGT_ARB_STATUS);
        if (status & 0xeu) {
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

static int refuse(PgtSpmi *s, unsigned periph, uint8_t reg, int rc)
{
    s->refused++;
    s->closed = 1;
    s->last_rc = rc;
    s->last_periph = (uint8_t)periph;
    s->last_reg = reg;
    return rc;
}

/* What a written register adds to the interlock. */
static void note_written(PgtSpmi *s, uint8_t reg)
{
    int ch;
    if ((ch = channel_of(reg, PGT_FLASH_ITARGET(0))) >= 0) s->itarget_set |= (uint8_t)(1u << ch);
    else if ((ch = channel_of(reg, PGT_FLASH_CHAN_TIMER(0))) >= 0) s->timer_set |= (uint8_t)(1u << ch);
    else if ((ch = channel_of(reg, PGT_FLASH_CHAN_STROBE(0))) >= 0) s->strobe_set |= (uint8_t)(1u << ch);
    else if (reg == PGT_FLASH_IRES) s->ires_set = 1;
}

int pgt_spmi_write(PgtSpmi *s, unsigned periph, uint8_t reg, const uint8_t *data, unsigned len)
{
    if (!s || !s->io) return E_INVAL;
    if (periph >= PGT_PERIPHS || !s->p[periph].writable || s->closed || !pgt_spmi_allowed(s, periph, reg, data, len))
        return refuse(s, periph, reg, E_PERM);
    if (!channel_ours(s, &s->p[periph])) return refuse(s, periph, reg, E_PERM);
    const int rc = write_transaction(s, periph, reg, data, len);
    if (rc) {
        s->closed = 1;
        s->last_rc = rc;
        s->last_periph = (uint8_t)periph;
        s->last_reg = reg;
        return rc;
    }
    note_written(s, reg);
    return 0;
}

int pgt_spmi_emergency_stop(PgtSpmi *s)
{
    if (!s || !s->io || !s->p[PGT_FLASH].writable || s->emergency_used) return E_PERM;
    s->emergency_used = 1;
    s->closed = 1;
    if (!channel_ours(s, &s->p[PGT_FLASH])) return E_PERM;
    const uint8_t zero = 0;
    const int rc = write_transaction(s, PGT_FLASH, PGT_FLASH_CHAN_EN, &zero, 1);
    const int rc2 = write_transaction(s, PGT_FLASH, PGT_FLASH_EN_CTL, &zero, 1);
    return rc ? rc : rc2;
}
