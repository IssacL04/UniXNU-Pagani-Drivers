/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host models of the 13T speaker hardware for Test/hw.c and Test/core.c (one translation unit each): the TLMM pages of the pins, the GCC
 * (I2C hub votes, branch halts, the SE4 RCG), the I2C hub serial engine 4 running the I2C firmware in FIFO mode, and the two TFA9873
 * behind it (16-bit registers, I2CR, keys, the CLKS/PLL behaviour on the bit clock, the manager state). Time is a microsecond counter
 * every delay advances; events are logged with it so the tests can check orders (reset pulse, clocks before the amplifiers). */
#ifndef PAGANI_SPEAKER_TEST_MODEL_H
#define PAGANI_SPEAKER_TEST_MODEL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GeniI2c.h"
#include "SpeakerBoard.h"
#include "SpeakerHw.h"
#include <PeripheralIO.h>   /* the shared GCC vote lock word (sm8750_gcc_vote_owner) the speaker's clock bring-up takes */

#ifndef CHECK
static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
#endif

typedef struct {
    int present;
    uint16_t reg[256], por[256];
    int ignore;                       /* writes to this register are dropped; -1 none */
    int keys_open, key1_ok, key2_ok;
    int clks_reads;                   /* STATUS reads since the bit clock and the power state allowed CLKS */
    uint64_t ampe_on_us, ampe_off_us; /* when AMPE last went 1 / 0 */
    unsigned power_ups, power_downs, writes, reads;
} ModelAmp;

typedef struct {
    uint64_t us;                      /* the clock */
    /* TLMM */
    uint32_t ctl[256], io[256];
    int egpio[256];                   /* bit 11 reads set */
    int stuck_pin;                    /* CTL writes to this pin are lost; -1 none */
    struct {uint16_t gpio; uint32_t value; uint64_t us;} io_log[64];
    unsigned io_logged;
    /* GCC */
    uint32_t vote, rcg_cmd, rcg_cfg;
    int vote_stuck;                   /* the vote register ignores writes */
    int core_off;                     /* the I2C core branch never runs (its root is off) */
    int rcg_stuck;                    /* the RCG update never completes */
    uint64_t vote_us;                 /* when the vote went in; the branches run 30 us later */
    unsigned vote_writes;             /* writes to the vote register */
    uint32_t vote_owner_at_write;     /* the shared lock word when the vote register was last written */
    uint64_t lock_release_us;         /* a foreign holder of the lock word gives it back at this time (0: never) */
    /* the serial engine */
    uint32_t se[0x1000 / 4];
    uint32_t status;
    int active, op, hold, length, address, mute;
    uint32_t rx, rx_count;
    uint8_t tx[8]; int tx_bytes;
    uint8_t pointer[2];
    unsigned commands;
    ModelAmp amp[2];
    int bit_clock;                    /* the secondary MI2S runs (the DSP's device graph started) */
} Model;

static Model *gModel;

static void model_amp_por(ModelAmp *a, uint16_t rev)
{
    memset(a, 0, sizeof *a);
    for (unsigned i = 0; i < 256; ++i) a->por[i] = (uint16_t)(i * 0x0101u ^ 0x5a00u);
    a->por[0x00] = 0x0001; a->por[0x01] = 0; a->por[0x02] = 0x0008; a->por[0x03] = rev; a->por[0x04] = 0x0100; a->por[0x20] = 0x0001;
    a->por[0x10] = a->por[0x11] = a->por[0x13] = a->por[0x14] = 0; a->por[0x15] = 0x0123; a->por[0x16] = 0x0021; a->por[0x40] = 0x0001;
    a->por[0xfb] = 0x4321;
    memcpy(a->reg, a->por, sizeof a->reg);
    a->ignore = -1;
    a->present = 1;
}

static void model_init(Model *m)
{
    memset(m, 0, sizeof *m);
    gModel = m;
    m->stuck_pin = -1;
    for (unsigned g = 0; g < 256; ++g) m->ctl[g] = 0x00000001u;   /* gpio, pull-down, input */
    m->egpio[SpeakerPinResetTop] = 1;                               /* gpio2 is eGPIO capable */
    m->ctl[SpeakerPinResetTop] |= SpeakerCtlEgpioPresent;
    m->rcg_cfg = 0;                                                 /* XO, bypass */
    m->se[GENI_FW_REVISION / 4] = GENI_PROTO_I2C << 8 | 0x23;
    m->se[GENI_CLK_CFG / 4] = 0;                                    /* the boot loader left it alone */
    m->se[GENI_SCL_COUNTERS / 4] = 0;
    m->se[GENI_DMA_MODE / 4] = 1;
    model_amp_por(&m->amp[0], 0x0b73);
    model_amp_por(&m->amp[1], 0x0b73);
}

static int amp_running(const ModelAmp *a) {return !(a->reg[0x00] & 1) && (a->reg[0x01] & 4);}

static void amp_write(Model *m, ModelAmp *a, uint8_t r, uint16_t v)
{
    a->writes++;
    if (r == a->ignore) return;
    if (r == 0x00 && (v & 2)) {memcpy(a->reg, a->por, sizeof a->reg); a->keys_open = 0; a->clks_reads = 0; return;}
    if (r == 0x0f) a->keys_open = v == 0x5a6b;
    if (r == 0xa0) a->key1_ok = a->keys_open && v == (a->reg[0xfb] ^ 0x5a);
    if (r == 0xa1) a->key2_ok = a->keys_open && v == 0x5a;
    if (r == 0x44) {a->reg[0x40] &= (uint16_t)~v; return;}
    const uint16_t before = a->reg[0x00];
    a->reg[r] = v;
    if (r == 0x00 && (v & 8) && !(before & 8)) {a->ampe_on_us = m->us; a->power_ups++;}
    if (r == 0x00 && !(v & 8) && (before & 8)) {a->ampe_off_us = m->us; a->power_downs++;}
    if ((r == 0x00 || r == 0x01) && !amp_running(a)) a->clks_reads = 0;
}

static uint16_t amp_read(Model *m, ModelAmp *a, uint8_t r)
{
    a->reads++;
    const int clocked = amp_running(a) && m->bit_clock;
    if (r == 0x10) {
        if (clocked) a->clks_reads++;
        return (uint16_t)(clocked && a->clks_reads > 1 ? 0x0040 : 0x0000);   /* the PLL locks after a moment */
    }
    if (r == 0x11) return (uint16_t)((clocked && a->clks_reads > 1 ? 0x0040 : 0) | (clocked && (a->reg[0x00] & 8) ? 0x6000 : 0));
    if (r == 0x14) return (uint16_t)(clocked && (a->reg[0x00] & 8) ? 9u << 3 : amp_running(a) ? 1u << 3 : 0);
    return a->reg[r];
}

static ModelAmp *amp_at(Model *m, int address)
{
    if (address == 0x34) return m->amp[0].present ? &m->amp[0] : NULL;
    if (address == 0x35) return m->amp[1].present ? &m->amp[1] : NULL;
    return NULL;
}

static uint32_t se_read(Model *m, uint32_t r)
{
    if (r == GENI_M_IRQ_STATUS) return m->status;
    if (r == GENI_RX_FIFO_STATUS) return m->rx_count;
    if (r == GENI_RX_FIFO) {m->rx_count = 0; return m->rx;}
    if (r == GENI_STATUS) return m->active ? 1u : 0u;
    CHECK(r < sizeof m->se && !(r & 3));
    return m->se[r / 4];
}

static void se_write(Model *m, uint32_t r, uint32_t v)
{
    switch (r) {
    case GENI_M_IRQ_CLEAR: m->status &= ~v; return;
    case GENI_M_CMD0: {
        m->commands++;
        if (m->active) {m->status |= GENI_M_ILLEGAL_CMD | GENI_M_CMD_DONE; return;}
        CHECK(m->se[GENI_CLK_CFG / 4] & 1);                  /* never a command on an unclocked engine */
        m->op = (int)(v >> 27); m->address = (int)((v >> 9) & 0x7f); m->hold = (v & GENI_STOP_STRETCH) != 0; m->active = 1;
        if (m->op == GENI_OP_WRITE) {
            m->length = (int)m->se[GENI_TX_LEN / 4];
            if (m->se[GENI_TX_WATERMARK / 4] && !m->mute) m->status |= GENI_M_TX_WATERMARK;
            return;
        }
        CHECK(m->op == GENI_OP_READ && m->se[GENI_RX_LEN / 4] == 2);
        if (m->mute) return;
        ModelAmp *a = amp_at(m, m->address);
        m->active = 0;
        if (!a) {m->status |= (1u << 10) | GENI_M_CMD_DONE; return;}
        const uint16_t w = amp_read(m, a, m->pointer[m->address & 1]);
        m->rx = (uint32_t)(w >> 8) | (uint32_t)(w & 0xff) << 8;
        m->rx_count = 1;
        m->status |= GENI_M_CMD_DONE;
        return;
    }
    case GENI_TX_WATERMARK:
        m->se[r / 4] = v;
        if (v && m->active && m->op == GENI_OP_WRITE && !m->mute) m->status |= GENI_M_TX_WATERMARK;
        return;
    case GENI_TX_FIFO: {
        CHECK(m->active && m->op == GENI_OP_WRITE);
        for (int i = 0; i < 4 && m->tx_bytes < m->length; ++i) m->tx[m->tx_bytes++] = (uint8_t)(v >> (8 * i));
        if (m->tx_bytes < m->length) return;
        m->tx_bytes = 0;
        m->active = 0;
        ModelAmp *a = amp_at(m, m->address);
        if (!a) {m->status |= (1u << 10) | GENI_M_CMD_DONE; return;}
        CHECK(m->length == 1 || m->length == 3);
        m->pointer[m->address & 1] = m->tx[0];
        if (m->length == 3) amp_write(m, a, m->tx[0], (uint16_t)(m->tx[1] << 8 | m->tx[2]));
        m->status |= GENI_M_CMD_DONE;
        return;
    }
    case GENI_M_CMD_CTRL:
        if (v & (GENI_CMD_CTRL_CANCEL | GENI_CMD_CTRL_ABORT)) {
            m->active = 0;
            m->status |= (v & GENI_CMD_CTRL_CANCEL) ? GENI_M_CMD_CANCEL : GENI_M_CMD_ABORT;
        }
        return;
    default:
        CHECK(r < sizeof m->se && !(r & 3));
        m->se[r / 4] = v;
    }
}

static int branch_on(const Model *m, uint32_t bit) {return (m->vote & bit) && m->us >= m->vote_us + 30;}

static uint32_t gcc_read(Model *m, uint32_t r)
{
    switch (r) {
    case SpeakerGccVote: return m->vote;
    case SpeakerGccHaltSahb: return 0x80000002u;   /* under hardware clock gating it reads off while idle */
    case SpeakerGccHaltCore: return branch_on(m, 1u << 8) && !m->core_off ? 0x1u : 0x80000001u;
    case SpeakerGccHaltS4: return branch_on(m, 1u << 14) && !(m->rcg_cfg & 0x700) ? 0x1u : 0x80000001u;
    case SpeakerGccRcgCmd: return m->rcg_cmd;
    case SpeakerGccRcgCfg: return m->rcg_cfg;
    default: return 0;
    }
}

static void gcc_write(Model *m, uint32_t r, uint32_t v)
{
    switch (r) {
    case SpeakerGccVote:
        m->vote_writes++;
        m->vote_owner_at_write = sm8750_gcc_vote_owner;
        if (m->vote_stuck) return;
        if ((v & ~m->vote) && !m->vote) m->vote_us = m->us;
        if (v & ~m->vote) m->vote_us = m->us;
        m->vote = v;
        return;
    case SpeakerGccRcgCmd: m->rcg_cmd = m->rcg_stuck ? v : v & ~(uint32_t)SpeakerRcgUpdate; return;
    case SpeakerGccRcgCfg: m->rcg_cfg = v; return;
    default: CHECK(0);
    }
}

static uint32_t io_read(void *context, unsigned window, uint32_t offset)
{
    Model *m = (Model *)context;
    if (window == SpeakerWinGcc) return gcc_read(m, offset);
    if (window == SpeakerWinSe) return se_read(m, offset);
    CHECK(window >= 0x100 && window < 0x200 && (offset == 0 || offset == 4));
    const unsigned g = window - 0x100;
    return offset ? m->io[g] : m->ctl[g];
}

static void io_write(void *context, unsigned window, uint32_t offset, uint32_t value)
{
    Model *m = (Model *)context;
    if (window == SpeakerWinGcc) {gcc_write(m, offset, value); return;}
    if (window == SpeakerWinSe) {se_write(m, offset, value); return;}
    CHECK(window >= 0x100 && window < 0x200 && (offset == 0 || offset == 4));
    const unsigned g = window - 0x100;
    if (offset) {
        m->io[g] = value & SpeakerIoOut;
        if (m->io_logged < 64) {m->io_log[m->io_logged].gpio = (uint16_t)g; m->io_log[m->io_logged].value = value; m->io_log[m->io_logged++].us = m->us;}
        return;
    }
    if ((int)g == m->stuck_pin) return;
    uint32_t keep = m->egpio[g] ? SpeakerCtlEgpioPresent : 0;
    uint32_t v = (value & ~(uint32_t)SpeakerCtlEgpioPresent) | keep;
    if (!m->egpio[g]) v &= ~(uint32_t)SpeakerCtlEgpioEnable;
    if (g != SpeakerPinSda && g != SpeakerPinScl) v &= ~(uint32_t)SpeakerCtlI2cPull;   /* only the I2C pins have the strong pull */
    m->ctl[g] = v;
}

static void io_delay(void *context, uint32_t us)
{
    Model *m = (Model *)context;
    m->us += us;
    if (m->lock_release_us && m->us >= m->lock_release_us) {   /* the other driver's update ends: it gives the lock word back */
        __atomic_store_n(&sm8750_gcc_vote_owner, 0u, __ATOMIC_RELEASE);
        m->lock_release_us = 0;
    }
}

static const SpeakerIo *model_io(Model *m)
{
    static SpeakerIo io;
    io.context = m;
    io.read = io_read;
    io.write = io_write;
    io.delay_us = io_delay;
    return &io;
}

#endif
