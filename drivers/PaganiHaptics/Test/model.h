/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* A model of the SPMI PMIC arbiter v7 (channel table, ownership table, observer and RW channels, the arbiter's own write-ownership
 * enforcement) with a HAP530 haptics module behind it (CFG, PTN, HBOOST, SDAM register files; the FIFO with its memory map, flush and
 * fill status; FIFO playback consuming samples at the programmed rate on a simulated clock; HPWR status around plays and brakes;
 * the closed-loop status selections; fault injection). Every RW channel command is logged for sequence checks. */
#ifndef PAGANI_HAPTICS_TEST_MODEL_H
#define PAGANI_HAPTICS_TEST_MODEL_H
#include <stdint.h>
#include <string.h>
#include "check.h"
#include "../HapticsCore.h"
#include "../HapticsRegs.h"

enum { M_CH = 600, M_OUT = 1 << 20, M_LOG = 8192 };

typedef struct {
    uint8_t periph, reg, len;
    uint8_t data[8];
    uint64_t t;
} MWrite;

typedef struct {
    uint32_t version, channels;
    uint32_t map[1024], owner[1024];
    uint32_t obs_status[1024], obs_rdata[1024];
    uint32_t wdata0[1024], wdata1[1024], ch_status[1024];
    int mapped[4];
    unsigned map_calls, obs_cmds, ch_cmds, bad_obs, bad_ch, core_reads;
    uint16_t ppid[4];
    uint8_t reg[4][256];
    /* injection */
    int fail_writes_after;        /* the write after this many more fails (-1: never) */
    int fail_repeat;              /* that many more writes after it fail too */
    uint32_t fail_status;         /* the status word a failing write gets */
    int never_done, obs_fail, hpwr_stuck, sc_persistent;
    int obs_drops;                /* the next observer reads the arbiter drops */
    int owner_after_drop;         /* a dropped write hands the channel to another EE */
    /* FIFO and playback */
    uint8_t fifo[8192];
    uint32_t fifo_len, fifo_head, fifo_count, overflow, max_fill;
    int playing, brake;
    uint64_t next_sample_ns, brake_until_us;
    int8_t out[M_OUT];
    uint32_t out_n;
    uint32_t t_lra_us;
    uint8_t cal[2], good[2];
    uint64_t now_us;
    MWrite log[M_LOG];
    unsigned log_n;
    unsigned waits, delays;
    char lines[64][224];
    unsigned line_n;
    int battery_percent, battery_dc;
} Model;

static inline int m_periph_of(const Model *m, unsigned apid)
{
    const uint32_t ppid = (m->map[apid] >> 8) & 0xfffu;
    for (int k = 0; k < 4; ++k) if (m->ppid[k] == ppid) return k;
    return -1;
}

static inline uint32_t m_rate_ns(const Model *m)
{
    switch (m->reg[PGH_PTN][PGH_PTN_FIFO_PLAY_RATE] & 0x0fu) {
    case PGH_RATE_T_LRA: return m->t_lra_us * 1000u;
    case PGH_RATE_F_8KHZ: return 125000u;
    case PGH_RATE_F_16KHZ: return 62500u;
    case PGH_RATE_F_24KHZ: return 41667u;
    case PGH_RATE_F_32KHZ: return 31250u;
    case PGH_RATE_F_44P1KHZ: return 22676u;
    case PGH_RATE_F_48KHZ: return 20833u;
    default: return 1000000u;
    }
}

/* The clock: FIFO samples are consumed while a FIFO play runs. */
static inline void m_advance(Model *m, uint64_t us)
{
    const uint64_t end = m->now_us + us;
    if (m->playing && (m->reg[PGH_CFG][PGH_CFG_SPMI_PLAY] & PGH_PAT_SRC_MASK) == PGH_SRC_FIFO) {
        const uint64_t step = m_rate_ns(m);
        while (m->next_sample_ns / 1000u <= end) {
            if (!m->fifo_count) {   /* dry: the next sample plays one period after data comes */
                m->next_sample_ns = end * 1000u + step;
                break;
            }
            CHECK(m->out_n < M_OUT);
            m->out[m->out_n++] = (int8_t)m->fifo[m->fifo_head];
            m->fifo_head = (m->fifo_head + 1) % 8192u;
            m->fifo_count--;
            m->next_sample_ns += step;
        }
    }
    m->now_us = end;
}

static inline void m_push(Model *m, uint8_t v)
{
    if (m->fifo_count >= m->fifo_len) {
        m->overflow++;
        return;
    }
    m->fifo[(m->fifo_head + m->fifo_count) % 8192u] = v;
    m->fifo_count++;
    if (m->fifo_count > m->max_fill) m->max_fill = m->fifo_count;
}

static inline uint8_t m_read(Model *m, int p, uint8_t reg)
{
    if (p == PGH_CFG) {
        const uint8_t sel = m->reg[PGH_CFG][PGH_CFG_MOD_STATUS_SEL], xt = m->reg[PGH_CFG][PGH_CFG_MOD_STATUS_XT];
        if (reg == PGH_CFG_STATUS_DATA_MSB || reg == PGH_CFG_STATUS_DATA_LSB) {
            uint8_t msb = 0, lsb = 0;
            if (xt == 0x80 && sel == 5) {
                msb = (uint8_t)(((m->fifo_count >> 8) & 0x1fu) | (m->fifo_count ? 0 : 0x40u) |
                                (m->fifo_count >= m->fifo_len ? 0x20u : 0));
                lsb = (uint8_t)m->fifo_count;
            } else if (xt == 0 && sel == 0) {
                msb = m->cal[0];
                lsb = m->cal[1];
            } else if (xt == 0 && sel == 3) {
                msb = m->good[0];
                lsb = m->good[1];
            }
            return reg == PGH_CFG_STATUS_DATA_MSB ? msb : lsb;
        }
        if (reg == PGH_CFG_HPWR_INTF) {
            if (m->hpwr_stuck) return 1;
            return (m->playing || m->now_us < m->brake_until_us) ? PGH_HPWR_READY : PGH_HPWR_DISABLED;
        }
    }
    return m->reg[p][reg];
}

static inline void m_write(Model *m, int p, uint8_t reg, uint8_t v)
{
    if (p == PGH_CFG && reg == PGH_CFG_SPMI_PLAY) {
        const int was = m->playing;
        m->reg[p][reg] = v;
        if ((v & PGH_PLAY_EN) && !was) {
            m->playing = 1;
            m->brake = (v & PGH_BRAKE_EN) != 0;
            m->next_sample_ns = m->now_us * 1000u + m_rate_ns(m);
        } else if (!(v & PGH_PLAY_EN) && was) {
            m->playing = 0;
            m->brake_until_us = m->now_us + ((v & PGH_BRAKE_EN) ? 3u * m->t_lra_us / 2u : 0) + 200u;
        }
        return;
    }
    if (p == PGH_CFG && reg == PGH_CFG_FAULT_CLR) {
        if (!m->sc_persistent) m->reg[p][PGH_CFG_FAULT_STATUS] = (uint8_t)(m->reg[p][PGH_CFG_FAULT_STATUS] & ~v);
        return;
    }
    if (p == PGH_PTN && ((reg >= PGH_PTN_FIFO_DIN_0 && reg <= PGH_PTN_FIFO_DIN_0 + 3) || reg == PGH_PTN_FIFO_DIN_1B)) {
        m_push(m, v);
        return;
    }
    if (p == PGH_PTN && reg == PGH_PTN_MEM_OP_ACCESS && (v & PGH_MEM_FLUSH_RELOAD)) m->fifo_count = 0;
    if (p == PGH_PTN && reg == PGH_PTN_MMAP_FIFO)
        m->fifo_len = (v & PGH_MMAP_FIFO_EXIST) ? ((uint32_t)(v & PGH_MMAP_FIFO_LEN_MASK) + 1u) * 64u : 0;
    m->reg[p][reg] = v;
}

/* ---- PghSpmiIo ---- */
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
    if (off % 0x20u != PGH_ARB_CMD || (v >> 27) != PGH_ARB_OP_EXT_READL || apid >= M_CH) {
        m->bad_obs++;
        return;
    }
    m->obs_cmds++;
    const int p = m_periph_of(m, apid);
    const uint8_t reg = (uint8_t)((v >> 4) & 0xffu);
    const unsigned len = (v & 7u) + 1u;
    uint32_t data = 0;
    for (unsigned i = 0; i < len && i < 4 && p >= 0; ++i) data |= (uint32_t)m_read(m, p, (uint8_t)(reg + i)) << (8 * i);
    m->obs_rdata[apid] = data;
    m->obs_status[apid] = m->never_done ? 0 : (m->obs_fail ? 0x3u : 0x1u);
    if (m->obs_drops > 0) {
        m->obs_drops--;
        m->obs_status[apid] = 0x9u;
    }
}
static inline uint32_t m_obs_read(void *ctx, uint32_t off)
{
    Model *m = ctx;
    const unsigned apid = off / 0x20u;
    if (off % 0x20u == PGH_ARB_STATUS) return m->obs_status[apid];
    if (off % 0x20u == PGH_ARB_RDATA0) return m->obs_rdata[apid];
    return 0;
}
static inline int m_map_channel(void *ctx, unsigned periph, unsigned apid)
{
    Model *m = ctx;
    m->map_calls++;
    CHECK(periph == PGH_CFG || periph == PGH_PTN);
    m->mapped[periph] = (int)apid;
    return 0;
}
static inline uint32_t m_chan_read(void *ctx, unsigned periph, uint32_t off)
{
    Model *m = ctx;
    CHECK(periph < 4 && m->mapped[periph] >= 0);
    return off == PGH_ARB_STATUS ? m->ch_status[m->mapped[periph]] : 0;
}
static inline void m_chan_write(void *ctx, unsigned periph, uint32_t off, uint32_t v)
{
    Model *m = ctx;
    CHECK(periph < 4 && m->mapped[periph] >= 0);
    const unsigned apid = (unsigned)m->mapped[periph];
    if (off == PGH_ARB_WDATA0) {m->wdata0[apid] = v; return;}
    if (off == PGH_ARB_WDATA1) {m->wdata1[apid] = v; return;}
    if (off != PGH_ARB_CMD || (v >> 27) != PGH_ARB_OP_EXT_WRITEL) {m->bad_ch++; return;}
    m->ch_cmds++;
    const uint8_t reg = (uint8_t)((v >> 4) & 0xffu);
    const unsigned len = (v & 7u) + 1u;
    MWrite *w = m->log_n < M_LOG ? &m->log[m->log_n++] : 0;
    if (w) {
        w->periph = (uint8_t)periph;
        w->reg = reg;
        w->len = (uint8_t)len;
        w->t = m->now_us;
        for (unsigned i = 0; i < len; ++i) w->data[i] = (uint8_t)((i < 4 ? m->wdata0[apid] >> (8 * i) : m->wdata1[apid] >> (8 * (i - 4))));
    }
    if (m->never_done) {m->ch_status[apid] = 0; return;}
    if (m->fail_writes_after == 0) {
        if (m->fail_repeat > 0) m->fail_repeat--;
        else m->fail_writes_after = -1;
        m->ch_status[apid] = m->fail_status;
        if (m->owner_after_drop) m->owner[apid] = 1;
        return;
    }
    if (m->fail_writes_after > 0) m->fail_writes_after--;
    if ((m->owner[apid] & 7u) != 0) {m->ch_status[apid] = 0x5u; return;}   /* the arbiter: done | denied */
    const int p = m_periph_of(m, apid);
    CHECK(p == (int)periph);
    for (unsigned i = 0; i < len; ++i) m_write(m, p, (uint8_t)(reg + i), w ? w->data[i] : 0);
    m->ch_status[apid] = 1u;
}
static inline void m_delay(void *ctx, unsigned us) {Model *m = ctx; m->delays++; m_advance(m, us);}

/* ---- PghOps ---- */
static inline uint64_t m_now(void *ctx) {return ((Model *)ctx)->now_us;}
static inline void m_wait(void *ctx, unsigned us) {Model *m = ctx; m->waits++; m_advance(m, us);}
static inline void m_trace(void *ctx, const char *line)
{
    Model *m = ctx;
    if (m->line_n < 64) {
        strncpy(m->lines[m->line_n], line, sizeof m->lines[0] - 1);
        m->line_n++;
    }
}
static inline void m_battery(void *ctx, int *percent, int *dc) {Model *m = ctx; *percent = m->battery_percent; *dc = m->battery_dc;}
static inline int m_logged(const Model *m, const char *needle)
{
    for (unsigned i = 0; i < m->line_n; ++i) if (strstr(m->lines[i], needle)) return 1;
    return 0;
}

static const PghSpmiIo kModelIo = {0, m_core, m_cnfg, m_obs_read, m_obs_write, m_map_channel, m_chan_read, m_chan_write, m_delay};

enum { A_CFG = 300, A_PTN = 301, A_HBST = 302, A_SDAM = 120 };

/* The 13T as the sources describe it, register files at plausible boot values (some deliberately off the init's targets). */
static inline void m_init(Model *m)
{
    memset(m, 0, sizeof *m);
    m->version = 0x70060000u;
    m->channels = M_CH;
    m->fail_writes_after = -1;
    m->fail_status = 0x3u;
    m->t_lra_us = 5882;
    m->battery_percent = -1;
    m->battery_dc = 250;
    for (int k = 0; k < 4; ++k) m->mapped[k] = -1;
    for (unsigned n = 0; n < M_CH; ++n) {   /* other peripherals of slaves 0..9, some owned by other EEs */
        m->map[n] = (((n % 10u) << 8 | (n & 0xffu)) << 8) | (n % 7u == 0 ? 1u << 24 : 0);
        m->owner[n] = n % 3u;
    }
    m->ppid[PGH_CFG] = 0x7f0;
    m->ppid[PGH_PTN] = 0x7f1;
    m->ppid[PGH_HBST] = 0x7f2;
    m->ppid[PGH_SDAM] = 0x09d;
    for (unsigned n = 0; n < M_CH; ++n)
        for (int k = 0; k < 4; ++k) if (((m->map[n] >> 8) & 0xfffu) == m->ppid[k]) m->map[n] = 0;
    m->map[A_CFG] = 0x7f0u << 8 | 1u << 24; m->owner[A_CFG] = 0;
    m->map[A_PTN] = 0x7f1u << 8; m->owner[A_PTN] = 0;
    m->map[A_HBST] = 0x7f2u << 8; m->owner[A_HBST] = 3;   /* the boost need not be ours: it is only read */
    m->map[A_SDAM] = 0x09du << 8; m->owner[A_SDAM] = 0;
    uint8_t *cfg = m->reg[PGH_CFG], *ptn = m->reg[PGH_PTN], *hb = m->reg[PGH_HBST], *sd = m->reg[PGH_SDAM];
    cfg[0] = 0; cfg[1] = 5; cfg[4] = 0x15; cfg[5] = 0x0a;
    cfg[PGH_CFG_HW_CONFIG] = PGH_HW_HV_DRIVER;
    cfg[PGH_CFG_DRV_WF_SEL] = 0x11;      /* FMT set: the init clears it */
    cfg[PGH_CFG_BRAKE_MODE_CFG] = 0xc0;
    cfg[PGH_CFG_CAL_EN] = 0x0c;
    cfg[PGH_CFG_AUTO_SHUTDOWN] = PGH_EN_WFHOLD_FIFO;
    cfg[PGH_CFG_VMAX] = 0x48;
    ptn[0] = 0; ptn[1] = 5; ptn[4] = 0x15; ptn[5] = 0x0b;
    ptn[PGH_PTN_MMAP_FIFO] = 0x8b;       /* a pattern partition left by firmware: 768 bytes of FIFO */
    m->fifo_len = 768;
    hb[0] = 1; hb[1] = 0; hb[4] = 0x15; hb[5] = 0x0c;
    hb[PGH_HBST_HW_CTRL_FOLLOW] = PGH_FOLLOW_HW_EN;
    sd[4] = 0x2e; sd[5] = 0x02; sd[0x75] = 0x24;
    /* a closed-loop result: last good == calibrated = 6960 steps of 0.833333 us = 5799 us (172.4 Hz) */
    m->cal[0] = (uint8_t)(PGH_AUTO_RES_CAL_DONE | (6960u >> 8)); m->cal[1] = (uint8_t)(6960u & 0xffu);
    m->good[0] = (uint8_t)(6960u >> 8); m->good[1] = (uint8_t)(6960u & 0xffu);
}

static inline PghConfig m_config(unsigned mode)
{
    PghConfig c;
    memset(&c, 0, sizeof c);
    c.mode = mode;
    c.ee = 0;
    c.ppid[PGH_CFG] = 0x7f0; c.ppid[PGH_PTN] = 0x7f1; c.ppid[PGH_HBST] = 0x7f2; c.ppid[PGH_SDAM] = 0x09d;
    c.t_lra_us = 5882;
    c.vmax_mv = 1260; c.effect_mv = 3600; c.fifo_mv = 7450;
    const uint8_t brake[8] = {0xff, 0x3f, 0x1f, 0, 0, 0, 0, 0};
    memcpy(c.brake, brake, 8);
    c.brake_len = 3; c.brake_mode = 0; c.brake_gain = 0; c.drv_shape = 1; c.brake_shape = 1;
    c.drv_duty = 0x56; c.autores_cfg = 0x32;
    c.f0_min_dhz = 1600; c.f0_max_dhz = 1800;
    const uint8_t cycles[15] = {4, 6, 2, 3, 3, 6, 0, 2, 2, 2, 2, 2, 2, 2, 2};
    memcpy(c.effect_cycles, cycles, 15);
    c.effects = 15;
    c.vbat_low_mv = 6800; c.vbat_low_soc = 5; c.vbat_cold_soc = 20; c.vbat_cold_dc = -50;
    c.hard_mv = 7450; c.cap_mv = 3600;
    c.max_play_ms = 2000; c.max_stream_ms = 30000;
    c.budget_ms = 5000; c.refill_permille = 500;
    c.autores = 1;
    c.log = 2;
    return c;
}

static inline PghOps m_ops(Model *m)
{
    PghOps o = {m, m_now, m_wait, m_trace, m_battery};
    return o;
}

static inline PghSpmiIo m_io(Model *m)
{
    PghSpmiIo io = kModelIo;
    io.ctx = m;
    return io;
}

/* Runs the core's poll loop on the simulated clock until it reports nothing to do (or the limit). */
static inline void m_run(Model *m, PghCore *c, uint64_t limit_us)
{
    const uint64_t end = m->now_us + limit_us;
    while (m->now_us < end) {
        const uint32_t ms = pgh_core_poll(c);
        if (!ms) break;
        m_advance(m, (uint64_t)ms * 1000u);
    }
}

/* index of the first logged write (periph, reg) at or after `from`, -1 none */
static inline int m_find(const Model *m, unsigned from, int periph, uint8_t reg)
{
    for (unsigned i = from; i < m->log_n; ++i) if (m->log[i].periph == periph && m->log[i].reg == reg) return (int)i;
    return -1;
}

#endif
