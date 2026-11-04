/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of BatteryI2c.c against a model of the SM8750 pieces it touches: the GCC branch votes and their CBCRs, the I2C hub engine's
 * RCG, the two TLMM pins, and a GENI I2C serial engine (FIFO commands, TX watermark, CMD_DONE with error bits, cancel and abort) with an
 * NFG8011B-like target at 0x55 behind it. The model refuses (counts) every engine register access while the engine's clocks are off,
 * also after another driver's unlocked update of the vote register dropped this driver's bits. */
#include <string.h>
#include "check.h"
#include "../BatteryGauge.h"
#include "../BatteryI2c.h"

typedef struct {
    /* GCC */
    uint32_t vote, rcg_cmd, rcg_cfg;
    int stuck_branch;           /* 1 S_AHB, 2 CORE, 3 S1 never runs */
    int ahb_hw_ctl;
    unsigned vote_writes, vote_reads;
    int lock_busy, vote_on_lock;    /* vote_on_lock: a user of the lock word votes these bits before this driver gets it */
    int vote_readonly;              /* the vote register ignores writes */
    unsigned lock_takes, lock_releases, lock_held;
    /* TLMM */
    uint32_t pin[2];
    int pin_readonly;
    /* engine */
    uint32_t se[0x1000];
    uint32_t proto, if_disable;
    unsigned dead_accesses;     /* engine registers touched with its clocks off */
    uint32_t status;            /* M_IRQ status */
    int active, op, stretch, stuck, cancel_ack, abort_ack, fail_command, empty_read;
    uint8_t address, tx[8];
    unsigned tx_need, tx_have;
    uint32_t rx[16];
    unsigned rx_count;
    unsigned commands, cancels, aborts;
    /* target */
    uint8_t target;
    uint8_t pointer;
    uint16_t regs[0x40];
    uint16_t subcommand, device_type;
    unsigned delays;
} Soc;

static int clocked(const Soc *s)
{
    return (s->vote & PBI_VOTE_BITS) == PBI_VOTE_BITS && !s->stuck_branch;
}

static uint32_t cbcr(const Soc *s, uint32_t bit, int which)
{
    const int on = (s->vote & bit) && s->stuck_branch != which;
    if (which == 1 && s->ahb_hw_ctl) return PBI_CBCR_HW_CTL | PBI_CBCR_OFF;   /* gated by the hardware while idle */
    return on ? 0u : PBI_CBCR_OFF;
}

static void finish_command(Soc *s, uint32_t bits)
{
    s->active = 0;
    s->status |= bits | PBI_M_CMD_DONE;
}

/* The bytes of a write reached the target. */
static void deliver(Soc *s)
{
    if (s->fail_command) {finish_command(s, PBI_M_CMD_FAILURE); return;}
    if (s->address != s->target) {finish_command(s, PBI_M_GP_NACK); return;}
    s->pointer = s->tx[0];
    if (s->tx_need == 3 && s->tx[0] == PgRegBlock) s->subcommand = (uint16_t)(s->tx[1] | s->tx[2] << 8);
    if (!s->stuck) finish_command(s, 0);
}

static void read_command(Soc *s)
{
    if (s->address != s->target) {finish_command(s, PBI_M_GP_NACK); return;}
    if (s->stuck) return;
    if (s->empty_read) {finish_command(s, 0); return;}
    const unsigned n = s->se[PBI_RX_LEN / 4];
    uint8_t bytes[4] = {0};
    if (s->pointer == PgRegBlock) {
        bytes[0] = (uint8_t)s->subcommand; bytes[1] = (uint8_t)(s->subcommand >> 8);
        bytes[2] = (uint8_t)s->device_type; bytes[3] = (uint8_t)(s->device_type >> 8);
    } else {
        const uint16_t v = s->regs[s->pointer & 0x3f];
        bytes[0] = (uint8_t)v; bytes[1] = (uint8_t)(v >> 8);
    }
    uint32_t word = 0;
    for (unsigned i = 0; i < n && i < 4; ++i) word |= (uint32_t)bytes[i] << (8 * i);
    CHECK(s->rx_count < 16);
    s->rx[s->rx_count++] = word;
    finish_command(s, 0);
}

static uint32_t se_read(Soc *s, uint32_t at)
{
    switch (at) {
    case PBI_FW_REVISION: return s->proto << 8 | 0x12;
    case PBI_IF_DISABLE: return s->if_disable;
    case PBI_GENI_STATUS: return s->active ? 1u : 0u;
    case PBI_M_IRQ_STATUS: return s->status;
    case PBI_RX_FIFO_STATUS: return s->rx_count;
    case PBI_RX_FIFO: {
        if (!s->rx_count) return 0xdeadbeef;
        const uint32_t word = s->rx[0];
        memmove(s->rx, s->rx + 1, --s->rx_count * sizeof s->rx[0]);
        return word;
    }
    default: return s->se[at / 4];
    }
}

static void se_write(Soc *s, uint32_t at, uint32_t value)
{
    switch (at) {
    case PBI_M_IRQ_CLEAR: s->status &= ~value; return;
    case PBI_M_CMD0:
        CHECK(!s->active);
        s->commands++;
        s->op = (int)(value >> 27);
        s->address = (uint8_t)((value >> 9) & 0x7f);
        s->stretch = (value & PBI_STOP_STRETCH) != 0;
        s->active = 1;
        if (s->op == PBI_OP_WRITE) {
            s->tx_need = s->se[PBI_TX_LEN / 4];
            s->tx_have = 0;
            CHECK(s->tx_need >= 1 && s->tx_need <= 8);
        } else {
            CHECK(s->op == PBI_OP_READ);
            read_command(s);
        }
        return;
    case PBI_TX_WATERMARK:
        s->se[at / 4] = value;
        if (value && s->active && s->op == PBI_OP_WRITE && s->tx_have < s->tx_need) s->status |= PBI_M_TX_WATERMARK;
        return;
    case PBI_TX_FIFO:
        CHECK(s->active && s->op == PBI_OP_WRITE && s->tx_have < s->tx_need);
        for (unsigned i = 0; i < 4 && s->tx_have < s->tx_need; ++i) s->tx[s->tx_have++] = (uint8_t)(value >> (8 * i));
        if (s->tx_have == s->tx_need) deliver(s);
        return;
    case PBI_M_CMD_CTRL:
        if (value & PBI_CTRL_CANCEL) {
            s->cancels++;
            if (s->cancel_ack) {s->active = 0; s->status |= PBI_M_CMD_CANCEL;}
        }
        if (value & PBI_CTRL_ABORT) {
            s->aborts++;
            if (s->abort_ack) {s->active = 0; s->status |= PBI_M_CMD_ABORT;}
        }
        return;
    default:
        s->se[at / 4] = value;
    }
}

static uint32_t io_read(void *ctx, unsigned window, uint32_t at)
{
    Soc *s = ctx;
    switch (window) {
    case PbiWinGccVote: CHECK(at == PBI_VOTE); s->vote_reads++; return s->vote;
    case PbiWinGccSe:
        if (at == PBI_CBCR_S1) return cbcr(s, PBI_VOTE_S1, 3);
        if (at == PBI_RCG_CMD) return s->rcg_cmd;
        CHECK(at == PBI_RCG_CFG);
        return s->rcg_cfg;
    case PbiWinGccAhb:
        if (at == PBI_CBCR_S_AHB) return cbcr(s, PBI_VOTE_S_AHB, 1);
        CHECK(at == PBI_CBCR_CORE);
        return cbcr(s, PBI_VOTE_CORE, 2);
    case PbiWinSda: case PbiWinScl: CHECK(at == 0); return s->pin[window - PbiWinSda];
    case PbiWinSe:
        CHECK(at < PBI_SE_SIZE && !(at & 3));
        if (!clocked(s)) {s->dead_accesses++; return 0xffffffffu;}
        return se_read(s, at);
    default: CHECK(0); return 0;
    }
}

static void io_write(void *ctx, unsigned window, uint32_t at, uint32_t value)
{
    Soc *s = ctx;
    switch (window) {
    case PbiWinGccVote:
        CHECK(at == PBI_VOTE && s->lock_held);   /* the shared vote word is written only under its lock */
        if (!s->vote_readonly) s->vote = value;
        s->vote_writes++;
        return;
    case PbiWinSda: case PbiWinScl:
        CHECK(at == 0);
        if (!s->pin_readonly) s->pin[window - PbiWinSda] = value;
        return;
    case PbiWinSe:
        CHECK(at < PBI_SE_SIZE && !(at & 3));
        if (!clocked(s)) {s->dead_accesses++; return;}
        se_write(s, at, value);
        return;
    default: CHECK(0);
    }
}

static void io_delay(void *ctx, unsigned us)
{
    Soc *s = ctx;
    s->delays++;
    (void)us;
}

static int io_lock(void *ctx, int take)
{
    Soc *s = ctx;
    if (!take) {CHECK(s->lock_held); s->lock_held = 0; s->lock_releases++; return 0;}
    if (s->lock_busy) return -16;
    CHECK(!s->lock_held);
    if (s->vote_on_lock) s->vote |= PBI_VOTE_BITS;
    s->lock_held = 1;
    s->lock_takes++;
    return 0;
}

static void soc_cold(Soc *s)
{
    memset(s, 0, sizeof *s);
    s->vote = 0x043c0000u;          /* the touch engine's votes, none of ours */
    s->rcg_cfg = 0;                 /* XO, no divider */
    s->pin[0] = 1u << 10 | 0x3;     /* gpio function, pull-up, an unrelated high bit */
    s->pin[1] = 0x1;
    s->proto = PBI_PROTO_I2C;
    s->cancel_ack = s->abort_ack = 1;
    s->target = PGAUGE_ADDRESS;
    s->regs[PgRegVoltage] = 3950;
    s->regs[PgRegCurrent] = (uint16_t)(int16_t)-812;
    s->regs[PgRegTemperature] = 2731 + 295;
    s->regs[PgRegSoc] = 87;
    s->regs[PgRegFull] = 6100;
    s->regs[PgRegRemaining] = 3200;
    s->regs[PgRegTimeToFull] = 0xffff;
    s->regs[PgRegCycles] = 37;
    s->regs[PgRegSoh] = 98;
    s->device_type = PGAUGE_DEVICE_NFG8011B;
}

static const PbiIo kIo = {0, io_read, io_write, io_delay, io_lock};

static PbiIo io_for(Soc *s)
{
    PbiIo io = kIo;
    io.ctx = s;
    return io;
}

static int read16(PbiBus *b, uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    const int rc = pagani_bi2c_xfer(b, PGAUGE_ADDRESS, &reg, 1, data, 2);
    if (!rc) *value = (uint16_t)(data[0] | data[1] << 8);
    return rc;
}

static void test_bringup(void)
{
    Soc s;
    PbiBus b;
    PbiReport r;
    soc_cold(&s);
    PbiIo io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == 0);
    CHECK(r.stage == PbiStReady && r.error == 0 && b.ready && !s.dead_accesses);
    CHECK(s.vote == (0x043c0000u | PBI_VOTE_BITS) && s.vote_writes == 1 && r.voted == 1);   /* the touch votes stay */
    CHECK(s.lock_takes == 1 && s.lock_releases == 1 && !s.lock_held);
    CHECK(r.vote_before == 0x043c0000u && r.vote_after == s.vote && !(r.cbcr[2] & PBI_CBCR_OFF));
    CHECK(s.pin[0] == (1u << 10 | PBI_PIN_I2C) && s.pin[1] == PBI_PIN_I2C);
    CHECK(r.pin_before[0] == (1u << 10 | 0x3) && r.pin_after[1] == PBI_PIN_I2C);
    CHECK(s.se[PBI_M_CLK_CFG / 4] == PBI_CLK_100K && s.se[PBI_SCL_COUNTERS / 4] == ((10u << 20) | (12u << 10) | 26u));
    CHECK(s.se[PBI_TX_PACK0 / 4] == 0x7f8fe && s.se[PBI_TX_PACK1 / 4] == 0xffefe);
    CHECK(s.se[PBI_RX_PACK0 / 4] == 0x7f8fe && s.se[PBI_RX_PACK1 / 4] == 0xffefe);
    CHECK(s.se[PBI_M_IRQ_EN / 4] == PBI_M_WANTED && s.se[PBI_SE_IRQ_EN / 4] == 0 && s.se[PBI_DMA_MODE / 4] == 0);
    CHECK(s.se[PBI_CGC_CTRL / 4] == 0x7f && s.se[PBI_OUTPUT_CTRL / 4] == 0x7f && s.se[PBI_FORCE_DEFAULT / 4] == 1);
    CHECK(s.se[PBI_CLK_SEL / 4] == 0 && s.se[PBI_BYTE_GRAN / 4] == 0 && s.se[PBI_RX_WATERMARK / 4] == 15);
    CHECK(r.fw == (PBI_PROTO_I2C << 8 | 0x12) && r.clk_after == PBI_CLK_100K);

    /* Already voted (a boot loader that read the gauge): the shared register is not written, its lock not taken. */
    soc_cold(&s);
    s.vote |= PBI_VOTE_BITS;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == 0 && !s.vote_writes && !s.lock_takes && r.voted == 0);

    /* The vote word stays owned by another driver: nothing is written, the engine is not touched. */
    soc_cold(&s);
    s.lock_busy = 1;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -16 && r.stage == PbiStNone && !s.vote_writes && !s.dead_accesses && !b.ready);

    /* A branch that never runs: the engine is never touched. */
    for (int which = 1; which <= 3; ++which) {
        soc_cold(&s);
        s.stuck_branch = which;
        io = io_for(&s);
        const unsigned before = s.delays;
        CHECK(pagani_bi2c_bringup(&b, &io, &r) == -110 && r.stage == PbiStNone && !s.dead_accesses && !b.ready);
        CHECK(s.delays > before);
    }
    /* S_AHB under hardware gating counts as running. */
    soc_cold(&s);
    s.ahb_hw_ctl = 1;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == 0 && (r.cbcr[0] & PBI_CBCR_HW_CTL));

    /* The root: off, or not on the XO, is refused and never reprogrammed. */
    soc_cold(&s);
    s.rcg_cmd = PBI_RCG_ROOT_OFF;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -95 && r.stage == PbiStClocks && !s.dead_accesses);
    soc_cold(&s);
    s.rcg_cfg = 1u << 8;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -95 && s.rcg_cfg == 1u << 8 && s.se[PBI_M_CLK_CFG / 4] == 0);

    /* The engine: another protocol, no FIFO, a command left running. */
    soc_cold(&s);
    s.proto = 1;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -71 && r.stage == PbiStRoot && s.pin[0] == (1u << 10 | 0x3));   /* pins untouched */
    soc_cold(&s);
    s.if_disable = 1;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -95);
    soc_cold(&s);
    s.active = 1;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == 0 && s.cancels == 1 && !s.aborts && (r.geni_status & 1));
    soc_cold(&s);
    s.active = 1;
    s.cancel_ack = 0;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == 0 && s.cancels == 1 && s.aborts == 1);
    soc_cold(&s);
    s.active = 1;
    s.cancel_ack = s.abort_ack = 0;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -16 && r.stage == PbiStRoot);

    /* Pins that do not take the function. */
    soc_cold(&s);
    s.pin_readonly = 1;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -5 && r.stage == PbiStEngine && !b.ready);

    /* Arguments. */
    CHECK(pagani_bi2c_bringup(0, &io, &r) == -22 && pagani_bi2c_bringup(&b, 0, &r) == -22);
    CHECK(pagani_bi2c_bringup(&b, 0, 0) == -22);
}

static void test_transfers(void)
{
    Soc s;
    PbiBus b;
    soc_cold(&s);
    PbiIo io = io_for(&s);
    uint8_t reg = PgRegVoltage, data[4] = {0};
    memset(&b, 0, sizeof b);
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, &reg, 1, data, 2) == -19);   /* not brought up */
    CHECK(pagani_bi2c_bringup(&b, &io, 0) == 0);
    uint16_t value = 0;
    CHECK(read16(&b, PgRegVoltage, &value) == 0 && value == 3950);
    CHECK(read16(&b, PgRegCurrent, &value) == 0 && (int16_t)value == -812);
    CHECK(s.commands == 4 && b.transfers == 2 && !b.failures && !s.active);
    /* the write held the stop condition for the read */
    CHECK(s.op == PBI_OP_READ);
    /* not acknowledged */
    s.target = 0x34;
    CHECK(read16(&b, PgRegVoltage, &value) == -6 && b.nacks == 1 && (b.last_status & PBI_M_GP_NACK) && !s.active);
    s.target = PGAUGE_ADDRESS;
    /* an engine failure */
    s.fail_command = 1;
    CHECK(read16(&b, PgRegVoltage, &value) == -5 && b.bus_errors == 1);
    s.fail_command = 0;
    /* a stuck command: cancelled; one whose cancel is not acknowledged: aborted */
    s.stuck = 1;
    CHECK(read16(&b, PgRegVoltage, &value) == -110 && b.timeouts == 1 && s.cancels == 1 && !s.aborts && !s.active);
    s.cancel_ack = 0;
    CHECK(read16(&b, PgRegVoltage, &value) == -110 && b.timeouts == 2 && s.cancels == 2 && s.aborts == 1 && !s.active);
    s.stuck = 0;
    s.cancel_ack = 1;
    CHECK(read16(&b, PgRegSoc, &value) == 0 && value == 87);
    /* words left in the RX FIFO by a transfer cut short are not taken for this one */
    s.rx[0] = 0x11111111; s.rx[1] = 0x22222222; s.rx_count = 2;
    CHECK(read16(&b, PgRegFull, &value) == 0 && value == 6100 && s.rx_count == 0);
    /* a read that ends without data */
    s.empty_read = 1;
    CHECK(read16(&b, PgRegFull, &value) == -61 && b.last_error == -61);
    s.empty_read = 0;
    /* write only (the gauge's block command), then the block read */
    const uint8_t sub[3] = {PgRegBlock, 0x01, 0x00};
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, sub, 3, 0, 0) == 0 && s.subcommand == 1 && s.op == PBI_OP_WRITE);
    reg = PgRegBlock;
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, &reg, 1, data, 4) == 0);
    CHECK(data[0] == 1 && data[1] == 0 && (data[2] | data[3] << 8) == PGAUGE_DEVICE_NFG8011B);
    /* eight bytes go out in two FIFO words */
    const uint8_t eight[8] = {PgRegBlock, 1, 2, 3, 4, 5, 6, 7};
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, eight, 8, 0, 0) == 0 && s.tx_have == 8 && s.tx[7] == 7);
    /* arguments */
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, &reg, 0, data, 2) == -22);
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, eight, 9, 0, 0) == -22);
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, &reg, 1, data, 5) == -22);
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, &reg, 1, 0, 2) == -22);
    CHECK(pagani_bi2c_xfer(&b, 0x80, &reg, 1, data, 2) == -22);
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, 0, 1, data, 2) == -22);
    CHECK(pagani_bi2c_xfer(0, PGAUGE_ADDRESS, &reg, 1, data, 2) == -19);
    CHECK(!s.dead_accesses);
}

/* The gauge reader over the engine model. */
static PbiBus g_bus;
static int bus_read16(void *ctx, uint8_t reg, uint16_t *value) {(void)ctx; return read16(&g_bus, reg, value);}
static int bus_xfer(void *ctx, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn)
{
    (void)ctx;
    return pagani_bi2c_xfer(&g_bus, PGAUGE_ADDRESS, tx, txn, rx, rxn);
}

static void test_gauge_over_engine(void)
{
    Soc s;
    soc_cold(&s);
    PbiIo io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&g_bus, &io, 0) == 0);
    const PgBus bus = {0, bus_read16, bus_xfer, 0};
    PgReading r;
    memset(&r, 0, sizeof r);
    CHECK(pagani_gauge_read(&bus, &r, 1) == PgCount);
    CHECK(r.voltage_mv == 3950 && r.current_ma == -812 && r.temperature_centi == 2950 && r.soc == 87 && r.cycles == 37);
    uint16_t type = 0;
    CHECK(pagani_gauge_device_type(&bus, &type) == 0 && type == PGAUGE_DEVICE_NFG8011B);
    s.target = 0x0b;   /* the gauge gone from the bus */
    CHECK(pagani_gauge_read(&bus, &r, 0) == -6 && !pagani_gauge_has(&r, PgVoltage) && g_bus.nacks == PGAUGE_TRIES);
    CHECK(!s.dead_accesses);
}

/* The touch (sm8750_touch_clock) and the speaker amplifiers update the vote register without the lock word: one racing this driver's
 * vote drops its bits after the bring-up checked them. Every transfer reads the register first and votes again before the engine. */
static void test_votes_dropped(void)
{
    Soc s;
    PbiBus b;
    soc_cold(&s);
    PbiIo io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, 0) == 0 && s.vote_writes == 1 && s.lock_takes == 1);
    uint16_t value = 0;
    /* kept: one read of the register per transfer, nothing written, the lock word not taken */
    unsigned reads = s.vote_reads;
    CHECK(read16(&b, PgRegVoltage, &value) == 0 && value == 3950);
    CHECK(s.vote_reads == reads + 1 && s.vote_writes == 1 && s.lock_takes == 1 && !b.revotes);   /* one transfer: write, then read */
    /* the speaker amplifiers' read-modify-write from a word read before this driver voted: bits 7, 8, 11 gone, 14 set */
    const uint32_t stale = 0x043c0000u | 1u << 14;
    s.vote = stale;
    unsigned commands = s.commands;
    CHECK(read16(&b, PgRegSoc, &value) == 0 && value == 87);
    CHECK(b.revotes == 1 && b.dropped_vote == stale && b.revote_error == 0 && !s.dead_accesses);
    CHECK(s.vote == (stale | PBI_VOTE_BITS) && s.vote_writes == 2 && s.lock_takes == 2 && !s.lock_held && s.commands == commands + 2);
    /* the touch writing back a word without the engine's own clock: the same */
    s.vote &= ~PBI_VOTE_S1;
    CHECK(read16(&b, PgRegSoc, &value) == 0 && b.revotes == 2 && s.vote_writes == 3 && !s.dead_accesses);
    /* dropped while the lock word stays owned: refused with the engine untouched; the next transfer, the word free, votes again */
    s.vote &= ~PBI_VOTE_BITS;
    s.lock_busy = 1;
    commands = s.commands;
    unsigned failures = b.failures;
    CHECK(read16(&b, PgRegSoc, &value) == -16 && b.last_error == -16 && b.revote_error == -16 && b.failures == failures + 1);
    CHECK(s.commands == commands && !(s.vote & PBI_VOTE_BITS) && !s.dead_accesses);
    s.lock_busy = 0;
    CHECK(read16(&b, PgRegSoc, &value) == 0 && b.revotes == 4 && b.revote_error == 0 && !s.dead_accesses);
    /* voted again but a branch stays off: refused, the engine untouched */
    s.vote &= ~PBI_VOTE_BITS;
    s.stuck_branch = 3;
    commands = s.commands;
    CHECK(read16(&b, PgRegSoc, &value) == -16 && b.revote_error == -110 && s.commands == commands && !s.dead_accesses);
    s.stuck_branch = 0;
    /* the gauge's device type query (write, then read) meets dropped votes too */
    s.vote &= ~PBI_VOTE_CORE;
    const uint8_t sub[3] = {PgRegBlock, 0x01, 0x00};
    CHECK(pagani_bi2c_xfer(&b, PGAUGE_ADDRESS, sub, 3, 0, 0) == 0 && b.revotes == 6 && !s.dead_accesses);
    CHECK(!s.lock_held && s.lock_takes == s.lock_releases);

    /* a user of the lock word voted the bits while this driver waited for it: read again under the lock, nothing written */
    soc_cold(&s);
    s.vote_on_lock = 1;
    io = io_for(&s);
    PbiReport r;
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == 0 && s.lock_takes == 1 && !s.vote_writes && !r.voted);
    CHECK(r.vote_before == 0x043c0000u && r.vote_after == (0x043c0000u | PBI_VOTE_BITS));
    /* bits that do not stay set: the bring-up stops before the engine */
    soc_cold(&s);
    s.vote_readonly = 1;
    io = io_for(&s);
    CHECK(pagani_bi2c_bringup(&b, &io, &r) == -5 && r.stage == PbiStNone && !s.dead_accesses && !b.ready && !s.lock_held);
}

int main(void)
{
    test_bringup();
    test_transfers();
    test_gauge_over_engine();
    test_votes_dropped();
    printf("PASS i2c: %u checks\n", checks);
    return 0;
}
