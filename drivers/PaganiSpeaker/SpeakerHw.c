/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * See SpeakerHw.h; the engine setup values and the transfer timeout ladder are sheng's (UniXNU-sheng package/AudioProbe.cpp, commit
 * 08cde20e), the pins, clocks and amplifier programs are the 13T's. */
#include "SpeakerHw.h"
#include "SpeakerBoard.h"
#include <PeripheralIO.h>   /* SoC kit (Sm8750Platform): sm8750_gcc_vote_set, the shared update of the GCC vote register */

static uint32_t rd(const SpeakerIo *io, unsigned window, uint32_t offset) {return io->read(io->context, window, offset);}
static void wr(const SpeakerIo *io, unsigned window, uint32_t offset, uint32_t value) {io->write(io->context, window, offset, value);}
static void delay(const SpeakerIo *io, uint32_t us) {io->delay_us(io->context, us);}

/* The GCC window as the kit's register interface (offsets in the GCC, as SpeakerWinGcc takes them). */
static uint32_t gcc_read(void *context, uint32_t offset) {return rd((const SpeakerIo *)context, SpeakerWinGcc, offset);}
static void gcc_write(void *context, uint32_t offset, uint32_t value) {wr((const SpeakerIo *)context, SpeakerWinGcc, offset, value);}
static void gcc_delay(void *context, unsigned us) {delay((const SpeakerIo *)context, us);}

/* ---- pins ---------------------------------------------------------------------------------------------------------------------- */

uint32_t speaker_pin_apply(const SpeakerIo *io, const SpeakerPin *p, int *ok)
{
    const unsigned w = SPEAKER_WIN_PIN(p->gpio);
    const uint32_t before = rd(io, w, 0);
    const uint32_t drive = p->drive_ma >= 2 && p->drive_ma <= 16 ? (uint32_t)p->drive_ma / 2u - 1u : 0u;
    uint32_t ctl = (before & ~(uint32_t)SpeakerCtlOwned) | (p->pull & 3u) | ((uint32_t)p->function & 15u) << SpeakerCtlFuncShift |
                   drive << SpeakerCtlDriveShift;
    if (p->output) ctl |= SpeakerCtlOe;
    if (before & SpeakerCtlEgpioPresent) ctl |= SpeakerCtlEgpioPresent | SpeakerCtlEgpioEnable;   /* claim it for the apps TLMM */
    if (p->i2c_pull) ctl |= SpeakerCtlI2cPull;
    if (p->output) wr(io, w, 4, p->value ? SpeakerIoOut : 0);   /* the level first: switching to output does not glitch */
    wr(io, w, 0, ctl);
    const uint32_t after = rd(io, w, 0);
    /* bit 11 is read-only; bit 13 (the strong I2C pull) may not exist on every pin: only what is functional must read back */
    const uint32_t check = (uint32_t)SpeakerCtlOwned & ~((uint32_t)SpeakerCtlEgpioPresent | (uint32_t)SpeakerCtlI2cPull);
    if (ok) *ok = (after & check) == (ctl & check);
    return after;
}

int speaker_pins_i2c(const SpeakerIo *io, int strong_pull, uint32_t ctl[2])
{
    const uint16_t pins[2] = {SpeakerPinSda, SpeakerPinScl};
    int all = 1;
    for (unsigned i = 0; i < 2; ++i) {
        const SpeakerPin p = {pins[i], 1, 2, SpeakerCtlPullUp, 0, 0, (uint8_t)(strong_pull != 0)};
        int ok = 0;
        ctl[i] = speaker_pin_apply(io, &p, &ok);
        all &= ok;
    }
    return all ? 0 : -5;
}

int speaker_pins_i2s(const SpeakerIo *io, int active, uint32_t ctl[4])
{
    const uint16_t pins[4] = {SpeakerPinSck, SpeakerPinData0, SpeakerPinWs, SpeakerPinData1};
    int all = 1;
    for (unsigned i = 0; i < 4; ++i) {
        SpeakerPin p = {pins[i], 0, 2, SpeakerCtlPullDown, 0, 0, 0};
        if (active) {
            p.function = 1;
            p.drive_ma = 4;
            p.pull = pins[i] == SpeakerPinData1 ? SpeakerCtlPullDown : SpeakerCtlPullNone;
        }
        int ok = 0;
        ctl[i] = speaker_pin_apply(io, &p, &ok);
        all &= ok;
    }
    return all ? 0 : -5;
}

int speaker_reset_amps(const SpeakerIo *io, unsigned mask, uint32_t ctl[2])
{
    const uint16_t pins[2] = {SpeakerPinResetTop, SpeakerPinResetBottom};
    int all = 1;
    for (unsigned i = 0; i < 2; ++i) {
        ctl[i] = 0;
        if (!(mask & (1u << i))) continue;
        const SpeakerPin p = {pins[i], 0, 2, SpeakerCtlPullNone, 1, 0, 0};
        int ok = 0;
        ctl[i] = speaker_pin_apply(io, &p, &ok);
        all &= ok;
    }
    if (!all) return -5;
    for (unsigned i = 0; i < 2; ++i) if (mask & (1u << i)) wr(io, SPEAKER_WIN_PIN(pins[i]), 4, SpeakerIoOut);
    delay(io, 1000);
    for (unsigned i = 0; i < 2; ++i) if (mask & (1u << i)) wr(io, SPEAKER_WIN_PIN(pins[i]), 4, 0);
    delay(io, 2000);
    return 0;
}

/* ---- clocks -------------------------------------------------------------------------------------------------------------------- */

static int branch_running(uint32_t cbcr, int gated) {return !(cbcr & SPEAKER_CBCR_CLK_OFF) || (gated && (cbcr & SpeakerCbcrHwCtl));}

int speaker_clocks_on(const SpeakerIo *io, SpeakerClocks *out)
{
    SpeakerClocks s = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    s.rcg_cmd = rd(io, SpeakerWinGcc, SpeakerGccRcgCmd);
    s.rcg_cfg = rd(io, SpeakerWinGcc, SpeakerGccRcgCfg);
    /* CFG: source 10:8 (0 = XO), divider 4:0 (0/1 = bypass), mode 13:12. Its only table entry is the XO at 19.2 MHz. */
    if ((s.rcg_cfg & 0x371fu) > 1u) {
        /* Linux clk_rcg2_shared: force the root on so the update completes even while no branch asks for the clock. */
        wr(io, SpeakerWinGcc, SpeakerGccRcgCmd, s.rcg_cmd | SpeakerRcgRootEn);
        wr(io, SpeakerWinGcc, SpeakerGccRcgCfg, s.rcg_cfg & ~0x371fu);
        wr(io, SpeakerWinGcc, SpeakerGccRcgCmd, rd(io, SpeakerWinGcc, SpeakerGccRcgCmd) | SpeakerRcgUpdate);
        unsigned n = 0;
        while ((rd(io, SpeakerWinGcc, SpeakerGccRcgCmd) & SpeakerRcgUpdate) && n++ < 100) delay(io, 10);
        wr(io, SpeakerWinGcc, SpeakerGccRcgCmd, rd(io, SpeakerWinGcc, SpeakerGccRcgCmd) & ~(uint32_t)SpeakerRcgRootEn);
        s.rcg_fixed = 1;
        if (n > 100) {*out = s; return -5;}
    }
    /* The shared vote register: the kit's locked update (one read when the bits are there already; else, under the lock word, read,
     * write the missing bits, read back; a lock held elsewhere for 50 ms gives an unlocked update, counted by the kit). */
    SpeakerIo self = *io;
    const struct peripheral_io gcc = {&self, gcc_read, gcc_write, gcc_delay};
    struct sm8750_gcc_vote_report vote;
    const int voted = sm8750_gcc_vote_set(&gcc, SpeakerGccVoteBits, SPEAKER_GCC_OWNER, &vote);
    s.vote = vote.after;
    s.vote_waits = vote.waits;
    s.vote_wrote = vote.wrote;
    s.vote_locked = vote.locked;
    if (voted) {*out = s; return -5;}
    for (s.polls = 0; s.polls < 2000; ++s.polls) {
        s.sahb = rd(io, SpeakerWinGcc, SpeakerGccHaltSahb);
        s.core = rd(io, SpeakerWinGcc, SpeakerGccHaltCore);
        s.s4 = rd(io, SpeakerWinGcc, SpeakerGccHaltS4);
        if (branch_running(s.sahb, 1) && branch_running(s.core, 0) && branch_running(s.s4, 0)) {*out = s; return 0;}
        delay(io, 10);
    }
    *out = s;
    return -110;   /* never touch an engine without running clocks */
}

/* ---- the serial engine --------------------------------------------------------------------------------------------------------- */

int speaker_se_setup(const SpeakerIo *io, SpeakerSe *out)
{
    SpeakerSe s = {0, 0, 0, 0, 0, 0, 0};
    s.fw = rd(io, SpeakerWinSe, GENI_FW_REVISION);
    s.if_disable = rd(io, SpeakerWinSe, GENI_IF_DISABLE);
    s.clk = rd(io, SpeakerWinSe, GENI_CLK_CFG);
    s.scl = rd(io, SpeakerWinSe, GENI_SCL_COUNTERS);
    s.status = rd(io, SpeakerWinSe, GENI_STATUS);
    if (((s.fw >> 8) & 0xffu) != GENI_PROTO_I2C || (s.if_disable & 1u)) {*out = s; return -19;}
    if (s.status & 1u) {                                     /* M_GENI_CMD_ACTIVE: a command the boot loader left running */
        wr(io, SpeakerWinSe, GENI_M_CMD_CTRL, GENI_CMD_CTRL_ABORT);
        for (unsigned n = 0; n < 100 && (rd(io, SpeakerWinSe, GENI_STATUS) & 1u); ++n) delay(io, 10);
        wr(io, SpeakerWinSe, GENI_M_IRQ_CLEAR, 0xffffffffu);
        s.aborted = 1;
    }
    if (!(s.clk & 1u) || !s.scl) {
        wr(io, SpeakerWinSe, GENI_CGC_CTRL, rd(io, SpeakerWinSe, GENI_CGC_CTRL) | 0x7fu);                /* clock gating controls on */
        wr(io, SpeakerWinSe, GENI_DMA_GENERAL_CFG, rd(io, SpeakerWinSe, GENI_DMA_GENERAL_CFG) | 0xfu);   /* DMA clock gating on */
        wr(io, SpeakerWinSe, GENI_OUTPUT_CTRL, 0x7fu);                                                   /* default output control */
        wr(io, SpeakerWinSe, GENI_FORCE_DEFAULT, 1u);
        wr(io, SpeakerWinSe, GENI_GSI_EVENT_EN, 0);                                                      /* no GSI events */
        wr(io, SpeakerWinSe, GENI_CLK_SEL, 0);                                                           /* the RCG's only level */
        wr(io, SpeakerWinSe, GENI_CLK_CFG, SpeakerI2cClkCfg);
        wr(io, SpeakerWinSe, GENI_SCL_COUNTERS, SpeakerI2cScl);
        s.configured = 1;
    }
    *out = s;
    return 0;
}

/* ---- transfers ----------------------------------------------------------------------------------------------------------------- */

static uint32_t se_read(void *context, uint32_t reg)
{
    const SpeakerBus *b = (const SpeakerBus *)context;
    return b->io->read(b->io->context, SpeakerWinSe, reg);
}
static void se_write(void *context, uint32_t reg, uint32_t value)
{
    const SpeakerBus *b = (const SpeakerBus *)context;
    b->io->write(b->io->context, SpeakerWinSe, reg, value);
}

int speaker_bus_init(SpeakerBus *b, const SpeakerIo *io, uint32_t *config)
{
    b->io = io;
    b->transfers = b->failures = b->timeouts = b->last_error = b->last_status = 0;
    b->last_address = 0;
    const GeniIo gio = {se_read, se_write, b};
    return pagani_geni_i2c_prepare(&b->geni, &gio, 0, config) ? -19 : 0;
}

int speaker_bus_transfer(SpeakerBus *b, uint8_t address, const uint8_t *tx, uint32_t length, uint32_t read_length, uint32_t *rx)
{
    b->geni.address = address;
    b->last_address = address;
    b->transfers++;
    const int started = read_length ? pagani_geni_i2c_start_read_bytes(&b->geni, tx, length, read_length)
                                    : pagani_geni_i2c_start_write_bytes(&b->geni, tx, length);
    if (started) {b->failures++; b->last_error = GeniErrBusy; return -5;}
    for (unsigned stage = 0; stage < 3; ++stage) {
        for (unsigned i = 0; i < 1000; ++i) {
            const int r = pagani_geni_i2c_poll(&b->geni);
            if (r == GeniDone) {
                if (rx) *rx = b->geni.rx;
                return 0;
            }
            if (r == GeniFailed) {
                b->failures++;
                b->last_error = b->geni.error;
                b->last_status = b->geni.error_status;
                if (b->geni.error != GeniErrTimeout) return -5;
                b->timeouts++;
                return -110;
            }
            delay(b->io, 10);
        }
        const int r = pagani_geni_i2c_timeout(&b->geni);
        if (r == GeniFailed) break;
    }
    b->failures++;
    b->timeouts++;
    b->last_error = b->geni.error ? b->geni.error : GeniErrTimeout;
    b->last_status = b->geni.error_status;
    return -110;
}

int speaker_amp_run(SpeakerBus *b, uint8_t address, const Tfa9873Step *steps, unsigned count, Tfa9873Run *run)
{
    tfa9873_run_start(run, steps, count);
    for (;;) {
        Tfa9873Action a;
        const int k = tfa9873_run_next(run, &a);
        if (k == Tfa9873Finished) return 0;
        if (k == Tfa9873Wait) {
            delay(b->io, a.delay_ms * 1000u);
            continue;
        }
        uint32_t value = 0;
        const int rc = speaker_bus_transfer(b, address, a.bytes, a.length, a.read_length, &value);
        if (rc) return rc;
        tfa9873_run_result(run, tfa9873_wire_value(value));
    }
}
