/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The PMIC arbiter survey and the charger's state (BatterySpmi.h). */
#include "BatterySpmi.h"

int pagani_spmi_survey(const PbsIo *io, unsigned sid, int types, PbsSurvey *s)
{
    pbat_zero(s, sizeof *s);
    if (!io || !io->core || !io->cnfg || sid > 15) return -22;
    s->version = io->core(io->ctx, 0);
    s->channels = io->core(io->ctx, 4) & 0x7ffu;
    if (s->version < 0x70000000u || s->version >= 0x80000000u || !s->channels || s->channels > 1024) return -19;
    for (uint32_t n = 0; n < s->channels; ++n) {
        const uint32_t map = io->core(io->ctx, 0x2000u + 4u * n);
        const uint32_t ppid = (map >> 8) & 0xfffu;
        if (!map || (ppid >> 8) != sid) continue;
        const uint8_t owner = (uint8_t)(io->cnfg(io->ctx, 4u * n) & 7u);
        s->per_owner[owner]++;
        if (map & (1u << 24)) s->irq_owned++;
        if (s->count < PBS_MAX) {
            const uint32_t k = s->count;
            s->ppid[k] = (uint16_t)ppid;
            s->apid[k] = (uint16_t)n;
            s->owner[k] = owner;
            if (types && io->read8 && !io->read8(io->ctx, (int)n, 4, &s->type[k]) && !io->read8(io->ctx, (int)n, 5, &s->subtype[k]))
                s->typed[k] = 1;
        }
        s->count++;
    }
    return 0;
}

static void hex2(PbatText *t, unsigned v)
{
    static const char hex[] = "0123456789abcdef";
    pbat_text_char(t, hex[(v >> 4) & 15]);
    pbat_text_char(t, hex[v & 15]);
}

size_t pagani_spmi_line(const PbsSurvey *s, unsigned sid, unsigned *next, char *line, size_t size)
{
    PbatText t;
    pbat_text_start(&t, line, size);
    pbat_text_str(&t, "[battery] spmi sid");
    pbat_text_udec(&t, sid);
    if (!*next) {
        pbat_text_kx(&t, "v", s->version);
        pbat_text_kd(&t, "ch", s->channels);
        pbat_text_kd(&t, "n", s->count);
        for (unsigned ee = 0; ee < 8; ++ee) {
            if (!s->per_owner[ee]) continue;
            pbat_text_str(&t, " ee");
            pbat_text_udec(&t, ee);
            pbat_text_char(&t, '=');
            pbat_text_udec(&t, s->per_owner[ee]);
        }
        pbat_text_kd(&t, "irq0", s->irq_owned);
        *next = s->count ? 1 : 0;
        return t.used;
    }
    /* pid:owner[:type/subtype] entries, as many as fit */
    pbat_text_str(&t, " +");
    const uint32_t kept = s->count < PBS_MAX ? s->count : PBS_MAX;
    unsigned k = *next - 1;
    for (; k < kept; ++k) {
        const size_t need = s->typed[k] ? 12 : 6;
        if (t.used + need + 1 >= size) break;
        pbat_text_char(&t, ' ');
        hex2(&t, s->ppid[k] & 0xffu);
        pbat_text_char(&t, ':');
        pbat_text_udec(&t, s->owner[k]);
        if (s->typed[k]) {
            pbat_text_char(&t, ':');
            hex2(&t, s->type[k]);
            pbat_text_char(&t, '/');
            hex2(&t, s->subtype[k]);
        }
    }
    if (k == *next - 1 && k < kept) k++;   /* a line too short for one entry still moves on */
    *next = k < kept ? k + 1 : 0;
    return t.used;
}

const uint16_t pagani_spmi_peripherals[PBS_PERIPHERALS] = {0x726, 0x727, 0x728, 0x729, 0x72b, 0x72c, 0x7fd, 0x3c2, 0x6c3, 0x5c1, 0x1cf, 0x1c5};

int pagani_spmi_map(const PbsIo *io, int apid[PBS_PERIPHERALS])
{
    for (unsigned i = 0; i < PBS_PERIPHERALS; ++i) apid[i] = -19;
    if (!io || !io->core || !io->cnfg) return -22;
    const uint32_t version = io->core(io->ctx, 0), channels = io->core(io->ctx, 4) & 0x7ffu;
    if (version < 0x70000000u || version >= 0x80000000u || !channels || channels > 1024) return -19;
    for (uint32_t n = 0; n < channels; ++n) {
        const uint32_t map = io->core(io->ctx, 0x2000u + 4u * n);
        if (!map) continue;
        const uint32_t ppid = (map >> 8) & 0xfffu;
        for (unsigned i = 0; i < PBS_PERIPHERALS; ++i) {
            if (pagani_spmi_peripherals[i] != ppid) continue;
            if (apid[i] < 0 || (io->cnfg(io->ctx, 4u * n) & 7u) == 0) apid[i] = (int)n;
        }
    }
    return 0;
}

int pagani_spmi_index(unsigned ppid)
{
    for (unsigned i = 0; i < PBS_PERIPHERALS; ++i) if (pagani_spmi_peripherals[i] == ppid) return (int)i;
    return -19;
}

int pagani_spmi_charger(PbsRead read, void *ctx, PbsCharger *out)
{
    /* in the order of the Pbs* registers */
    static const struct {uint16_t ppid; uint8_t offset;} regs[PbsChargerRegs] = {
        {0x729, 0x10}, {0x729, 0x06}, {0x729, 0x08}, {0x72b, 0x06}, {0x72b, 0x0b}, {0x726, 0x06}, {0x729, 0x52}, {0x727, 0x06}, {0x727, 0x09},
    };
    if (!read || !out) return -22;
    for (unsigned i = 0; i < PbsChargerRegs; ++i) {
        const int rc = read(ctx, regs[i].ppid, regs[i].offset, &out->reg[i]);
        if (rc) return rc;
    }
    return 0;
}

void pagani_spmi_charger_text(PbatText *t, const PbsCharger *c)
{
    pbat_text_kd(t, "stage", pagani_spmi_stage(c));
    pbat_text_kx(t, "rt", c->reg[PbsUsbinRt]);
    pbat_text_kx(t, "in", c->reg[PbsUsbinStatus]);
    pbat_text_kx(t, "det", c->reg[PbsUsbinDetect]);
    pbat_text_kx(t, "rp", c->reg[PbsTypecSink]);
    pbat_text_kx(t, "tc", c->reg[PbsTypecMisc]);
    pbat_text_kx(t, "st", c->reg[PbsChgrStatus]);
    pbat_text_kd(t, "icl", c->reg[PbsUsbinLimit] * 50);
    pbat_text_kd(t, "imax", c->reg[PbsAiclMax] * 50);
    pbat_text_kd(t, "inow", c->reg[PbsAiclNow] * 50);
}
