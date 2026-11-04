/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Transfer phases after UniXNU-sheng package/GeniI2c.c (commit 08cde20e); bring-up and register facts as listed in BatteryI2c.h. */
#include "BatteryI2c.h"
#include "BatteryText.h"

enum {
    kPollUs = 10,
    kBranchPolls = 500,          /* 5 ms for a branch clock to run */
    kPhaseUs = 25000,            /* one command at 100 kHz takes under 1 ms; the gauge may stretch the clock */
    kCancelUs = 2000,
};

static uint32_t rd(const PbiIo *io, unsigned w, uint32_t at) {return io->read(io->ctx, w, at);}
static void wr(const PbiIo *io, unsigned w, uint32_t at, uint32_t v) {io->write(io->ctx, w, at, v);}
static void delay(const PbiIo *io, unsigned us) {if (io->delay_us) io->delay_us(io->ctx, us);}

static int fail(PbiReport *r, int stage, int error)
{
    r->stage = stage;
    r->error = error;
    return error;
}

/* A voted branch runs when its CLK_OFF bit clears; one under hardware clock gating may read off while idle and counts as running. */
static int branch_on(const PbiIo *io, unsigned window, uint32_t at, uint32_t *value)
{
    for (unsigned n = 0; n < kBranchPolls; ++n) {
        const uint32_t v = rd(io, window, at);
        *value = v;
        if ((v & PBI_CBCR_HW_CTL) || !(v & PBI_CBCR_OFF)) return 1;
        delay(io, kPollUs);
    }
    return 0;
}

/* The three votes under the lock word, written only when one is missing, then read back (BatteryI2c.h). */
static int vote_bits(const PbiIo *io, uint32_t *wrote, uint32_t *after)
{
    if (io->vote_lock && io->vote_lock(io->ctx, 1)) return -16;
    /* read again under the lock: another user of the lock word may have voted meanwhile */
    const uint32_t v = rd(io, PbiWinGccVote, PBI_VOTE);
    if ((v & PBI_VOTE_BITS) != PBI_VOTE_BITS) {
        wr(io, PbiWinGccVote, PBI_VOTE, v | PBI_VOTE_BITS);
        *wrote = 1;
    }
    *after = rd(io, PbiWinGccVote, PBI_VOTE);
    if (io->vote_lock) io->vote_lock(io->ctx, 0);
    return (*after & PBI_VOTE_BITS) == PBI_VOTE_BITS ? 0 : -5;
}

static int branches(const PbiIo *io, uint32_t cbcr[3])
{
    const int ahb = branch_on(io, PbiWinGccAhb, PBI_CBCR_S_AHB, &cbcr[0]);
    const int core = branch_on(io, PbiWinGccAhb, PBI_CBCR_CORE, &cbcr[1]);
    const int se = branch_on(io, PbiWinGccSe, PBI_CBCR_S1, &cbcr[2]);
    return ahb && core && se ? 0 : -110;
}

static int clocks(const PbiIo *io, PbiReport *r)
{
    r->vote_before = r->vote_after = rd(io, PbiWinGccVote, PBI_VOTE);
    if ((r->vote_before & PBI_VOTE_BITS) != PBI_VOTE_BITS) {
        const int rc = vote_bits(io, &r->voted, &r->vote_after);
        if (rc) return rc;
    }
    return branches(io, r->cbcr);
}

/* Before every transfer: one read of the vote register. Bits an unlocked update elsewhere dropped since the bring-up (a lock holder that
 * overstayed) are voted again and the branches seen running before the engine is touched; until then no transfer touches it. */
static int clocks_kept(PbiBus *b)
{
    const uint32_t seen = rd(&b->io, PbiWinGccVote, PBI_VOTE);
    if ((seen & PBI_VOTE_BITS) == PBI_VOTE_BITS) return 0;
    b->revotes++;
    b->dropped_vote = seen;
    uint32_t wrote = 0, after = 0, cbcr[3];
    int rc = vote_bits(&b->io, &wrote, &after);
    if (!rc) rc = branches(&b->io, cbcr);
    b->revote_error = rc;
    return rc;
}

/* Waits for any of the bits; returns the status seen (cleared), 0 on timeout. */
static uint32_t wait_bits(const PbiIo *io, uint32_t bits, unsigned us)
{
    for (unsigned waited = 0;; waited += kPollUs) {
        const uint32_t status = rd(io, PbiWinSe, PBI_M_IRQ_STATUS);
        if (status) wr(io, PbiWinSe, PBI_M_IRQ_CLEAR, status);
        if (status & bits) return status;
        if (waited >= us) return 0;
        delay(io, kPollUs);
    }
}

/* Cancels the running command, then aborts it when the cancel is not acknowledged (the engine's documented order). */
static void stop_command(const PbiIo *io)
{
    wr(io, PbiWinSe, PBI_TX_WATERMARK, 0);
    wr(io, PbiWinSe, PBI_M_CMD_CTRL, PBI_CTRL_CANCEL);
    if (!wait_bits(io, PBI_M_CMD_CANCEL, kCancelUs)) {
        wr(io, PbiWinSe, PBI_M_CMD_CTRL, PBI_CTRL_ABORT);
        wait_bits(io, PBI_M_CMD_ABORT, kCancelUs);
    }
    wr(io, PbiWinSe, PBI_M_IRQ_CLEAR, 0xffffffffu);
}

static int engine(const PbiIo *io, PbiReport *r)
{
    r->fw = rd(io, PbiWinSe, PBI_FW_REVISION);
    r->if_disable = rd(io, PbiWinSe, PBI_IF_DISABLE);
    r->geni_status = rd(io, PbiWinSe, PBI_GENI_STATUS);
    r->clk_before = rd(io, PbiWinSe, PBI_M_CLK_CFG);
    r->scl_before = rd(io, PbiWinSe, PBI_SCL_COUNTERS);
    if (((r->fw >> 8) & 0xffu) != PBI_PROTO_I2C) return -71;
    if (r->if_disable & 1u) return -95;   /* FIFO interface off: a GPI DMA engine owns it */
    if (r->geni_status & 1u) {
        /* a command left running (a boot loader's transfer cut short) */
        stop_command(io);
        if (rd(io, PbiWinSe, PBI_GENI_STATUS) & 1u) return -16;
    }
    /* The common setup (interrupt state, clock gating, output defaults), then FIFO mode without an interrupt line. */
    wr(io, PbiWinSe, PBI_GSI_EVENT_EN, 0);
    wr(io, PbiWinSe, PBI_M_IRQ_CLEAR, 0xffffffffu);
    wr(io, PbiWinSe, PBI_S_IRQ_CLEAR, 0xffffffffu);
    wr(io, PbiWinSe, PBI_DMA_TX_IRQ_CLR, 0xffffffffu);
    wr(io, PbiWinSe, PBI_DMA_RX_IRQ_CLR, 0xffffffffu);
    wr(io, PbiWinSe, PBI_CGC_CTRL, rd(io, PbiWinSe, PBI_CGC_CTRL) | 0x7fu);
    wr(io, PbiWinSe, PBI_DMA_GENERAL_CFG, rd(io, PbiWinSe, PBI_DMA_GENERAL_CFG) | 0xfu);
    wr(io, PbiWinSe, PBI_OUTPUT_CTRL, 0x7fu);
    wr(io, PbiWinSe, PBI_FORCE_DEFAULT, 1u);
    wr(io, PbiWinSe, PBI_RX_WATERMARK, 15u);     /* the hub engines' 16-word FIFO */
    wr(io, PbiWinSe, PBI_RX_RFR, 16u);
    wr(io, PbiWinSe, PBI_DMA_MODE, 0);
    wr(io, PbiWinSe, PBI_SE_IRQ_EN, 0);
    wr(io, PbiWinSe, PBI_M_IRQ_EN, PBI_M_WANTED);
    wr(io, PbiWinSe, PBI_TX_PACK0, PBI_PACK0);
    wr(io, PbiWinSe, PBI_TX_PACK1, PBI_PACK1);
    wr(io, PbiWinSe, PBI_RX_PACK0, PBI_PACK0);
    wr(io, PbiWinSe, PBI_RX_PACK1, PBI_PACK1);
    wr(io, PbiWinSe, PBI_BYTE_GRAN, 0);
    wr(io, PbiWinSe, PBI_TX_WATERMARK, 0);
    wr(io, PbiWinSe, PBI_CLK_SEL, 0);
    wr(io, PbiWinSe, PBI_M_CLK_CFG, PBI_CLK_100K);
    wr(io, PbiWinSe, PBI_SCL_COUNTERS, PBI_SCL_100K);
    r->clk_after = rd(io, PbiWinSe, PBI_M_CLK_CFG);
    r->scl_after = rd(io, PbiWinSe, PBI_SCL_COUNTERS);
    if ((r->clk_after & 0xfff1u) != PBI_CLK_100K || (r->scl_after & 0x3fffffffu) != PBI_SCL_100K) return -5;
    return 0;
}

static int pins(const PbiIo *io, PbiReport *r)
{
    static const unsigned windows[2] = {PbiWinSda, PbiWinScl};
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t before = rd(io, windows[i], 0);
        r->pin_before[i] = before;
        const uint32_t want = (before & ~PBI_PIN_MASK) | PBI_PIN_I2C;
        if (want != before) wr(io, windows[i], 0, want);
        r->pin_after[i] = rd(io, windows[i], 0);
        if ((r->pin_after[i] & PBI_PIN_MASK) != PBI_PIN_I2C) return -5;
    }
    return 0;
}

int pagani_bi2c_bringup(PbiBus *b, const PbiIo *io, PbiReport *report)
{
    PbiReport scratch;
    PbiReport *r = report ? report : &scratch;
    pbat_zero(r, sizeof *r);
    if (!b || !io || !io->read || !io->write) return fail(r, PbiStNone, -22);
    pbat_zero(b, sizeof *b);
    b->io = *io;
    /* Nothing of the engine is touched before its clocks run (its registers would not answer). */
    int rc = clocks(io, r);
    if (rc) return fail(r, PbiStNone, rc);
    r->stage = PbiStClocks;
    r->rcg_cmd = rd(io, PbiWinGccSe, PBI_RCG_CMD);
    r->rcg_cfg = rd(io, PbiWinGccSe, PBI_RCG_CFG);
    if ((r->rcg_cmd & PBI_RCG_ROOT_OFF) || ((r->rcg_cfg >> 8) & 7u)) return fail(r, PbiStClocks, -95);
    r->stage = PbiStRoot;
    rc = engine(io, r);
    if (rc) return fail(r, PbiStRoot, rc);
    r->stage = PbiStEngine;
    /* The pins last: the engine drives its idle levels before it reaches the bus. */
    rc = pins(io, r);
    if (rc) return fail(r, PbiStEngine, rc);
    r->stage = PbiStPins;
    b->ready = 1;
    r->stage = PbiStReady;
    r->error = 0;
    return 0;
}

static int classify(uint32_t status)
{
    if (status & PBI_M_GP_NACK) return -6;
    if (status & PBI_M_GP_BUS) return -71;
    if (status & PBI_M_GP_ARB) return -11;
    return -5;
}

static int failed(PbiBus *b, int error, uint32_t status)
{
    b->failures++;
    b->last_error = error;
    b->last_status = status;
    if (error == -6) b->nacks++;
    else if (error == -110) b->timeouts++;
    else b->bus_errors++;
    return error;
}

/* One command to its CMD_DONE: the TX data go in on the watermark. Returns 0, or the error with the engine idle again. */
static int run(PbiBus *b, uint32_t command, const uint32_t *words, unsigned nwords)
{
    const PbiIo *io = &b->io;
    uint32_t errors = 0;
    unsigned filled = nwords == 0;
    wr(io, PbiWinSe, PBI_M_CMD0, command);
    if (!filled) wr(io, PbiWinSe, PBI_TX_WATERMARK, 1);
    for (unsigned waited = 0;; waited += kPollUs) {
        const uint32_t status = rd(io, PbiWinSe, PBI_M_IRQ_STATUS);
        if (status) wr(io, PbiWinSe, PBI_M_IRQ_CLEAR, status);
        errors |= status & PBI_M_ERRORS;
        if (!filled && (status & PBI_M_TX_WATERMARK) && !errors) {
            for (unsigned i = 0; i < nwords; ++i) wr(io, PbiWinSe, PBI_TX_FIFO, words[i]);
            wr(io, PbiWinSe, PBI_TX_WATERMARK, 0);
            filled = 1;
        }
        if (status & PBI_M_CMD_DONE) {
            /* a failed command also ends with CMD_DONE; its error bits decide */
            wr(io, PbiWinSe, PBI_TX_WATERMARK, 0);
            if (errors) return failed(b, classify(errors), errors);
            if (!filled) return failed(b, -5, status);   /* done before the data went in */
            return 0;
        }
        if (waited >= kPhaseUs) {
            stop_command(io);
            return failed(b, -110, errors | status);
        }
        delay(io, kPollUs);
    }
}

int pagani_bi2c_xfer(PbiBus *b, uint8_t address, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn)
{
    if (!b || !b->ready) return -19;
    if (!tx || !txn || txn > 8 || rxn > 4 || (rxn && !rx) || address > 0x7f) return -22;
    const PbiIo *io = &b->io;
    b->transfers++;
    if (clocks_kept(b)) return failed(b, -16, 0);
    uint32_t words[2] = {0, 0};
    for (unsigned i = 0; i < txn; ++i) words[i / 4] |= (uint32_t)tx[i] << (8 * (i % 4));
    wr(io, PbiWinSe, PBI_M_IRQ_CLEAR, 0xffffffffu);
    wr(io, PbiWinSe, PBI_TX_LEN, txn);
    int rc = run(b, PBI_OP_WRITE << 27 | (uint32_t)address << 9 | (rxn ? PBI_STOP_STRETCH : 0), words, (txn + 3) / 4);
    if (rc || !rxn) return rc;
    /* words a transfer cut short left behind would be read as this one's */
    for (unsigned n = 0; n < 16 && (rd(io, PbiWinSe, PBI_RX_FIFO_STATUS) & 0x1ffffffu); ++n) (void)rd(io, PbiWinSe, PBI_RX_FIFO);
    wr(io, PbiWinSe, PBI_RX_LEN, rxn);
    rc = run(b, PBI_OP_READ << 27 | (uint32_t)address << 9, 0, 0);
    if (rc) return rc;
    if (!(rd(io, PbiWinSe, PBI_RX_FIFO_STATUS) & 0x1ffffffu)) return failed(b, -61, 0);
    const uint32_t word = rd(io, PbiWinSe, PBI_RX_FIFO);
    for (unsigned i = 0; i < rxn; ++i) rx[i] = (uint8_t)(word >> (8 * i));
    return 0;
}
