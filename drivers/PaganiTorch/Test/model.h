/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* A model of the SPMI PMIC arbiter v7 (channel table, ownership table, observer and RW channels, the arbiter's own write-ownership
 * enforcement) with the PM8550 4-channel flash module behind it: a register file with [L]'s fields, the LED it drives (a channel is lit
 * while the module is enabled, its CHAN_EN bit is set and its strobe is software), the current of each lit channel from IRES and ITARGET,
 * the highest current and the longest time ever lit on the simulated clock, and fault injection. Every RW command is logged. */
#ifndef PAGANI_TORCH_TEST_MODEL_H
#define PAGANI_TORCH_TEST_MODEL_H
#include <stdint.h>
#include <string.h>
#include "check.h"
#include "../TorchCore.h"
#include "../TorchRegs.h"

enum { M_CH = 600, M_LOG = 4096, A_FLASH = 300, M_PPID = 0x1ee };

typedef struct {
    uint8_t reg, value;
    uint64_t t;
} MWrite;

typedef struct {
    uint32_t version, channels;
    uint32_t map[1024], owner[1024];
    uint32_t obs_status[1024], obs_rdata[1024];
    uint32_t wdata0[1024], wdata1[1024], ch_status[1024];
    int mapped;
    unsigned map_calls, obs_cmds, ch_cmds, bad_obs, bad_ch, core_reads, delays;
    uint8_t reg[256];
    /* injection */
    int fail_writes_after;        /* the write after this many more fails (-1: never) */
    uint32_t fail_status;         /* the status word a failing write gets */
    int never_done, obs_fail, chan_map_fail;
    int fail_reads_after;         /* the observer read after this many more fails (-1: never) */
    uint8_t stuck_reg, stuck_on;  /* writes to this register are acknowledged and do nothing */
    uint8_t fault_s1, fault_s2, fault_s3;   /* shown in STATUS1..3 from fault_at_us on */
    uint64_t fault_at_us;
    /* the LED */
    uint64_t now_us, lit_since, lit_longest;
    uint32_t lit_ma_max;          /* the highest current any channel was ever driven at */
    uint8_t lit_mask_all;         /* every channel ever lit */
    uint8_t lit;                  /* lit channels now */
    MWrite log[M_LOG];
    unsigned log_n;
    char lines[80][224];
    unsigned line_n;
} Model;

/* mA of channel ch from the registers (12.5 mA steps rounded up: a wrong resolution must show). */
static inline uint32_t m_channel_ma(const Model *m, unsigned ch)
{
    const uint32_t code = m->reg[PGT_FLASH_ITARGET(ch)] & 0x7fu;
    return (m->reg[PGT_FLASH_IRES] >> ch & 1u) ? (code + 1u) * 5u : ((code + 1u) * 25u + 1u) / 2u;
}

static inline void m_settle(Model *m)
{
    uint8_t lit = 0;
    if (m->reg[PGT_FLASH_EN_CTL] & PGT_MODULE_EN)
        for (unsigned ch = 0; ch < 4; ++ch)
            if ((m->reg[PGT_FLASH_CHAN_EN] >> ch & 1u) && !(m->reg[PGT_FLASH_CHAN_STROBE(ch)] & PGT_STROBE_HW_SEL)) {
                lit |= (uint8_t)(1u << ch);
                const uint32_t ma = m_channel_ma(m, ch);
                if (ma > m->lit_ma_max) m->lit_ma_max = ma;
            }
    if (lit && !m->lit) m->lit_since = m->now_us;
    if (!lit && m->lit && m->now_us - m->lit_since > m->lit_longest) m->lit_longest = m->now_us - m->lit_since;
    m->lit = lit;
    m->lit_mask_all |= lit;
}

static inline void m_advance(Model *m, uint64_t us)
{
    m->now_us += us;
    if (m->lit && m->now_us - m->lit_since > m->lit_longest) m->lit_longest = m->now_us - m->lit_since;
}

static inline uint8_t m_read(Model *m, uint8_t reg)
{
    const int on = m->fault_at_us && m->now_us >= m->fault_at_us;
    if (reg == PGT_FLASH_STATUS1) return (uint8_t)(m->reg[reg] | (on ? m->fault_s1 : 0));
    if (reg == PGT_FLASH_STATUS2) return (uint8_t)(m->reg[reg] | (on ? m->fault_s2 : 0));
    if (reg == PGT_FLASH_STATUS3) return (uint8_t)(m->reg[reg] | (on ? m->fault_s3 : 0));
    return m->reg[reg];
}

/* ---- PgtSpmiIo ---- */
static inline uint32_t m_core(void *ctx, uint32_t off)
{
    Model *m = ctx;
    m->core_reads++;
    if (off == 0) return m->version;
    if (off == 4) return 0xf800u | m->channels;   /* bits above the count are not the count */
    CHECK(off >= 0x2000u && off < 0x2000u + 4u * 1024u && !(off & 3u));
    return m->map[(off - 0x2000u) / 4u];
}
static inline uint32_t m_cnfg(void *ctx, uint32_t off)
{
    Model *m = ctx;
    CHECK(off < 4u * 1024u && !(off & 3u));
    return m->owner[off / 4u] | 0xfff0u;   /* only bits 2:0 are the owner */
}
static inline void m_obs_write(void *ctx, uint32_t off, uint32_t v)
{
    Model *m = ctx;
    const unsigned apid = off / 0x20u;
    if (off % 0x20u != PGT_ARB_CMD || (v >> 27) != PGT_ARB_OP_EXT_READL || apid >= M_CH) {
        m->bad_obs++;
        return;
    }
    m->obs_cmds++;
    const uint8_t reg = (uint8_t)((v >> 4) & 0xffu);
    const unsigned len = (v & 7u) + 1u;
    uint32_t data = 0;
    if (((m->map[apid] >> 8) & 0xfffu) == M_PPID)
        for (unsigned i = 0; i < len && i < 4; ++i) data |= (uint32_t)m_read(m, (uint8_t)(reg + i)) << (8 * i);
    m->obs_rdata[apid] = data;
    int fail = m->obs_fail;
    if (m->fail_reads_after == 0) {
        m->fail_reads_after = -1;
        fail = 1;
    } else if (m->fail_reads_after > 0) {
        m->fail_reads_after--;
    }
    m->obs_status[apid] = m->never_done ? 0 : (fail ? 0x3u : 0x1u);
}
static inline uint32_t m_obs_read(void *ctx, uint32_t off)
{
    Model *m = ctx;
    const unsigned apid = off / 0x20u;
    if (off % 0x20u == PGT_ARB_STATUS) return m->obs_status[apid];
    if (off % 0x20u == PGT_ARB_RDATA0) return m->obs_rdata[apid];
    return 0;
}
static inline int m_map_channel(void *ctx, unsigned periph, unsigned apid)
{
    Model *m = ctx;
    m->map_calls++;
    CHECK(periph == PGT_FLASH);
    if (m->chan_map_fail) return -12;
    m->mapped = (int)apid;
    return 0;
}
static inline uint32_t m_chan_read(void *ctx, unsigned periph, uint32_t off)
{
    Model *m = ctx;
    CHECK(periph == PGT_FLASH && m->mapped >= 0);
    return off == PGT_ARB_STATUS ? m->ch_status[m->mapped] : 0;
}
static inline void m_chan_write(void *ctx, unsigned periph, uint32_t off, uint32_t v)
{
    Model *m = ctx;
    CHECK(periph == PGT_FLASH && m->mapped >= 0);
    const unsigned apid = (unsigned)m->mapped;
    if (off == PGT_ARB_WDATA0) {m->wdata0[apid] = v; return;}
    if (off == PGT_ARB_WDATA1) {m->wdata1[apid] = v; return;}
    if (off != PGT_ARB_CMD || (v >> 27) != PGT_ARB_OP_EXT_WRITEL) {m->bad_ch++; return;}
    m->ch_cmds++;
    const uint8_t reg = (uint8_t)((v >> 4) & 0xffu);
    CHECK((v & 7u) == 0);   /* single bytes only */
    const uint8_t value = (uint8_t)m->wdata0[apid];
    CHECK(m->log_n < M_LOG);
    MWrite *w = &m->log[m->log_n++];
    w->reg = reg;
    w->value = value;
    w->t = m->now_us;
    if (m->never_done) {m->ch_status[apid] = 0; return;}
    if (m->fail_writes_after == 0) {
        m->fail_writes_after = -1;
        m->ch_status[apid] = m->fail_status;
        return;
    }
    if (m->fail_writes_after > 0) m->fail_writes_after--;
    if ((m->owner[apid] & 7u) != 0) {m->ch_status[apid] = 0x5u; return;}   /* the arbiter: done | denied */
    if (!(m->stuck_on && m->stuck_reg == reg)) m->reg[reg] = value;
    m_settle(m);
    m->ch_status[apid] = 1u;
}
static inline void m_delay(void *ctx, unsigned us) {Model *m = ctx; m->delays++; m_advance(m, us);}

/* ---- PgtOps ---- */
static inline uint64_t m_now(void *ctx) {return ((Model *)ctx)->now_us;}
static inline void m_trace(void *ctx, const char *line)
{
    Model *m = ctx;
    CHECK(strlen(line) < 200);
    if (m->line_n < 80) {
        strncpy(m->lines[m->line_n], line, sizeof m->lines[0] - 1);
        m->line_n++;
    }
}
static inline int m_logged(const Model *m, const char *needle)
{
    for (unsigned i = 0; i < m->line_n; ++i) if (strstr(m->lines[i], needle)) return 1;
    return 0;
}

static const PgtSpmiIo kModelIo = {0, m_core, m_cnfg, m_obs_read, m_obs_write, m_map_channel, m_chan_read, m_chan_write, m_delay};

/* The 13T as the sources describe it; the registers at plausible boot values (some deliberately off the targets). */
static inline void m_init(Model *m)
{
    memset(m, 0, sizeof *m);
    m->version = 0x70060000u;
    m->channels = M_CH;
    m->fail_writes_after = -1;
    m->fail_reads_after = -1;
    m->fail_status = 0x3u;
    m->mapped = -1;
    for (unsigned n = 0; n < M_CH; ++n) {   /* other peripherals of slaves 0..9, some owned by other EEs */
        m->map[n] = (((n % 10u) << 8 | (n & 0xffu)) << 8) | (n % 7u == 0 ? 1u << 24 : 0);
        m->owner[n] = n % 3u;
        if (((m->map[n] >> 8) & 0xfffu) == M_PPID) m->map[n] = 0;
    }
    m->map[A_FLASH] = (uint32_t)M_PPID << 8 | 1u << 24;
    m->owner[A_FLASH] = 0;
    uint8_t *r = m->reg;
    r[0] = 0; r[1] = 1; r[2] = 0; r[3] = 2;   /* REVISION1..4 */
    r[PGT_REG_TYPE] = PGT_FLASH_TYPE;
    r[PGT_REG_SUBTYPE] = PGT_FLASH_SUBTYPE_4CH;
    r[PGT_FLASH_IRES] = 0x00;                 /* 12.5 mA steps: the flash setting */
    r[PGT_FLASH_ITARGET(0)] = 0x7f;           /* a flash current left in the register */
    r[PGT_FLASH_ITARGET(1)] = 0x7f;
    r[PGT_FLASH_CHAN_TIMER(0)] = 0x80 | 128;
    r[PGT_FLASH_CHAN_TIMER(1)] = 0x80 | 128;
    r[PGT_FLASH_CHAN_STROBE(0)] = PGT_STROBE_HW_SEL | PGT_STROBE_ACTIVE_HIGH;
    r[PGT_FLASH_CHAN_STROBE(1)] = PGT_STROBE_HW_SEL | PGT_STROBE_ACTIVE_HIGH;
}

static inline PgtConfig m_config(unsigned mode, unsigned max_ma)
{
    PgtConfig c;
    CHECK(pgt_config_make(&c, mode, 0, M_PPID, 0x3, 200, max_ma, 2) == 1);
    return c;
}

static inline PgtOps m_ops(Model *m)
{
    PgtOps o = {m, m_now, m_trace};
    return o;
}

static inline PgtSpmiIo m_io(Model *m)
{
    PgtSpmiIo io = kModelIo;
    io.ctx = m;
    return io;
}

/* Runs the core's poll loop on the simulated clock until it reports nothing to do (or the limit). */
static inline void m_run(Model *m, PgtCore *c, uint64_t limit_us)
{
    const uint64_t end = m->now_us + limit_us;
    while (m->now_us < end) {
        const uint32_t ms = pgt_core_poll(c);
        if (!ms) break;
        m_advance(m, (uint64_t)ms * 1000u);
    }
}

/* index of the first logged write of reg at or after `from`, -1 none */
static inline int m_find(const Model *m, unsigned from, uint8_t reg)
{
    for (unsigned i = from; i < m->log_n; ++i) if (m->log[i].reg == reg) return (int)i;
    return -1;
}

#endif
