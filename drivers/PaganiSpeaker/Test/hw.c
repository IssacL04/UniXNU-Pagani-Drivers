/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Host tests of SpeakerHw.c against the models of Test/model.h: the pin states (function, drive, pull, the strong I2C pull, eGPIO
 * ownership, glitch-free outputs, readback failures), the reset pulse (high 1 ms, low, 2 ms), the I2C hub clocks (votes, halts, a gated
 * AHB branch, the RCG moved to the XO, a branch that never runs, a stuck vote or RCG), the serial engine setup (400 kHz, kept settings,
 * other firmware, a disabled FIFO, a command left running) and register transfers to the amplifiers (identity, NACK, timeouts). */
#include "model.h"

static void pins(void)
{
    Model m;
    model_init(&m);
    const SpeakerIo *io = model_io(&m);
    uint32_t ctl[4];
    /* I2C: function 1, 2 mA (drive 0), pull-up, strong pull; no OE. */
    CHECK(speaker_pins_i2c(io, 1, ctl) == 0);
    CHECK(m.ctl[SpeakerPinSda] == (3u | 1u << 2 | 0u << 6 | SpeakerCtlI2cPull) && m.ctl[SpeakerPinScl] == m.ctl[SpeakerPinSda]);
    CHECK(ctl[0] == m.ctl[SpeakerPinSda]);
    CHECK(speaker_pins_i2c(io, 0, ctl) == 0 && !(m.ctl[SpeakerPinSda] & SpeakerCtlI2cPull));
    /* Unknown bits of the register are kept. */
    m.ctl[SpeakerPinSda] |= 1u << 20;
    CHECK(speaker_pins_i2c(io, 1, ctl) == 0 && (m.ctl[SpeakerPinSda] & (1u << 20)));
    /* MI2S active: function 1, 4 mA (drive 1), no pull; data1 pulled down. Sleep: gpio, 2 mA, pull-down, input. */
    CHECK(speaker_pins_i2s(io, 1, ctl) == 0);
    CHECK(m.ctl[SpeakerPinSck] == (0u | 1u << 2 | 1u << 6) && m.ctl[SpeakerPinData0] == m.ctl[SpeakerPinSck] &&
          m.ctl[SpeakerPinWs] == m.ctl[SpeakerPinSck] && m.ctl[SpeakerPinData1] == (1u | 1u << 2 | 1u << 6));
    CHECK(speaker_pins_i2s(io, 0, ctl) == 0);
    for (int i = 0; i < 4; ++i) CHECK(ctl[i] == 1u);
    /* A pin that does not take its configuration is reported. */
    m.stuck_pin = SpeakerPinWs;
    CHECK(speaker_pins_i2s(io, 1, ctl) == -5);
    m.stuck_pin = SpeakerPinScl;
    m.ctl[SpeakerPinScl] = 1;                                 /* still in its reset state: the write is lost */
    CHECK(speaker_pins_i2c(io, 1, ctl) == -5 && ctl[1] == 1);
    m.stuck_pin = -1;

    /* Resets: the level is written before the output is enabled; eGPIO pin 2 is claimed for the apps TLMM (bit 12). */
    m.io_logged = 0;
    const uint64_t t0 = m.us;
    uint32_t rst[2];
    CHECK(speaker_reset_amps(io, 3, rst) == 0);
    CHECK((m.ctl[SpeakerPinResetTop] & (SpeakerCtlEgpioEnable | SpeakerCtlOe | 0x3cu)) == (SpeakerCtlEgpioEnable | SpeakerCtlOe));
    CHECK((m.ctl[SpeakerPinResetBottom] & (SpeakerCtlEgpioEnable | SpeakerCtlOe)) == SpeakerCtlOe);
    CHECK(m.io_logged == 6 && m.io_log[0].gpio == SpeakerPinResetTop && m.io_log[0].value == 0 && m.io_log[1].gpio == SpeakerPinResetBottom &&
          m.io_log[1].value == 0);
    CHECK(m.io_log[2].value == SpeakerIoOut && m.io_log[3].value == SpeakerIoOut && m.io_log[2].us == t0);
    CHECK(m.io_log[4].value == 0 && m.io_log[5].value == 0 && m.io_log[4].us - m.io_log[2].us >= 1000 && m.us - m.io_log[4].us >= 2000);
    CHECK(m.io[SpeakerPinResetTop] == 0 && m.io[SpeakerPinResetBottom] == 0);
    /* Only the bottom one; a stuck reset pin pulses nothing. */
    m.io_logged = 0;
    CHECK(speaker_reset_amps(io, 2, rst) == 0 && m.io_logged == 3 && rst[0] == 0);
    m.io_logged = 0;
    m.stuck_pin = SpeakerPinResetTop;
    m.ctl[SpeakerPinResetTop] = 1u | SpeakerCtlEgpioPresent;  /* an input still: the output enable is lost */
    CHECK(speaker_reset_amps(io, 3, rst) == -5 && m.io_logged == 2);
    for (unsigned i = 0; i < m.io_logged; ++i) CHECK(m.io_log[i].value == 0);
}

static void clocks(void)
{
    Model m;
    model_init(&m);
    const SpeakerIo *io = model_io(&m);
    SpeakerClocks s;
    m.vote = 1u << 3;                                         /* another driver's vote is kept */
    CHECK(speaker_clocks_on(io, &s) == 0 && m.vote == ((1u << 3) | SpeakerGccVoteBits) && s.vote == m.vote && !s.rcg_fixed);
    CHECK(s.polls >= 3 && !(s.core & 0x80000000u) && !(s.s4 & 0x80000000u) && (s.sahb & 0x80000000u) && (s.sahb & 2u));
    /* The shared register is written once, under the lock word held as 'SPKR', which is free again afterwards. */
    CHECK(m.vote_writes == 1 && m.vote_owner_at_write == SPEAKER_GCC_OWNER && s.vote_wrote && s.vote_locked && s.vote_waits == 0);
    CHECK(sm8750_gcc_vote_owner == 0 && sm8750_gcc_vote_unlocked == 0);
    /* Already voted: no write, the lock word not even taken. */
    const uint32_t before = m.vote;
    sm8750_gcc_vote_owner = 0x42415454u;                      /* held by the gauge ('BATT'): no wait either */
    CHECK(speaker_clocks_on(io, &s) == 0 && m.vote == before && m.vote_writes == 1 && !s.vote_wrote && s.vote_waits == 0);
    CHECK(sm8750_gcc_vote_owner == 0x42415454u);
    /* The gauge holds the lock word for 200 us: the speaker waits for it (10 us steps), then writes under it. */
    model_init(&m);
    m.vote = 1u << 11;
    m.lock_release_us = m.us + 200;
    uint64_t t1 = m.us;
    CHECK(speaker_clocks_on(io, &s) == 0 && s.vote_locked && s.vote_waits == 20 && m.us - t1 >= 200);
    CHECK(m.vote == ((1u << 11) | SpeakerGccVoteBits) && m.vote_owner_at_write == SPEAKER_GCC_OWNER && sm8750_gcc_vote_owner == 0);
    /* A holder that never gives it back: 50 ms (5000 steps), then the update is written without it and counted; the holder's word
     * is left alone. The bring-up goes on (the vote reads back). */
    model_init(&m);
    sm8750_gcc_vote_owner = 0x57494649u;                      /* 'WIFI' */
    t1 = m.us;
    CHECK(speaker_clocks_on(io, &s) == 0 && !s.vote_locked && s.vote_wrote && s.vote_waits == SM8750_GCC_VOTE_STEPS);
    CHECK(m.us - t1 >= 50000 && m.us - t1 < 50000 + 25000 && m.vote == SpeakerGccVoteBits && m.vote_owner_at_write == 0x57494649u);
    CHECK(sm8750_gcc_vote_owner == 0x57494649u && sm8750_gcc_vote_unlocked == 1);
    sm8750_gcc_vote_owner = 0;
    sm8750_gcc_vote_unlocked = 0;
    /* The RCG on another source: moved to the XO (root forced on for the update, then released). */
    model_init(&m);
    m.rcg_cfg = 0x0103;                                       /* source 1, divider 2 */
    CHECK(speaker_clocks_on(io, &s) == 0 && s.rcg_fixed && s.rcg_cfg == 0x0103 && m.rcg_cfg == 0 && !(m.rcg_cmd & (SpeakerRcgRootEn | 1u)));
    /* A branch whose root never runs: -110 after 20 ms, nothing else touched. */
    model_init(&m);
    m.core_off = 1;
    const uint64_t t0 = m.us;
    CHECK(speaker_clocks_on(io, &s) == -110 && m.us - t0 >= 20000 && (s.core & 0x80000000u));
    /* A vote that does not read back; an RCG whose update never completes. */
    model_init(&m);
    m.vote_stuck = 1;
    CHECK(speaker_clocks_on(io, &s) == -5);
    model_init(&m);
    m.rcg_cfg = 0x0100;
    m.rcg_stuck = 1;
    CHECK(speaker_clocks_on(io, &s) == -5 && s.rcg_fixed);
}

static void engine(void)
{
    Model m;
    model_init(&m);
    const SpeakerIo *io = model_io(&m);
    SpeakerClocks k;
    CHECK(speaker_clocks_on(io, &k) == 0);
    SpeakerSe s;
    /* Unconfigured: 400 kHz from 19.2 MHz (divider 2, counters 5/11/22), clock gating and output control set, no GSI events. */
    m.se[GENI_GSI_EVENT_EN / 4] = 0xff;
    CHECK(speaker_se_setup(io, &s) == 0 && s.configured && !s.aborted && s.fw == (GENI_PROTO_I2C << 8 | 0x23));
    CHECK(m.se[GENI_CLK_CFG / 4] == 0x21 && m.se[GENI_SCL_COUNTERS / 4] == (5u << 20 | 11u << 10 | 22u) && m.se[GENI_CLK_SEL / 4] == 0);
    CHECK(m.se[GENI_CGC_CTRL / 4] == 0x7f && m.se[GENI_DMA_GENERAL_CFG / 4] == 0xf && m.se[GENI_OUTPUT_CTRL / 4] == 0x7f &&
          m.se[GENI_FORCE_DEFAULT / 4] == 1 && m.se[GENI_GSI_EVENT_EN / 4] == 0);
    /* A boot loader's setting is kept. */
    m.se[GENI_CLK_CFG / 4] = 0x41; m.se[GENI_SCL_COUNTERS / 4] = 0x00b31e2a;
    CHECK(speaker_se_setup(io, &s) == 0 && !s.configured && m.se[GENI_CLK_CFG / 4] == 0x41);
    /* A command left running is aborted first. */
    m.active = 1;
    CHECK(speaker_se_setup(io, &s) == 0 && s.aborted && !m.active && m.status == 0);
    /* Not usable: another protocol, a disabled FIFO interface (the I2C hub has no DMA). */
    m.se[GENI_FW_REVISION / 4] = 2u << 8;
    CHECK(speaker_se_setup(io, &s) == -19);
    m.se[GENI_FW_REVISION / 4] = GENI_PROTO_I2C << 8;
    m.se[GENI_IF_DISABLE / 4] = 1;
    CHECK(speaker_se_setup(io, &s) == -19);
}

static void transfers(void)
{
    Model m;
    model_init(&m);
    const SpeakerIo *io = model_io(&m);
    SpeakerClocks k;
    SpeakerSe s;
    SpeakerBus b;
    uint32_t config = 0;
    CHECK(speaker_clocks_on(io, &k) == 0 && speaker_se_setup(io, &s) == 0 && speaker_bus_init(&b, io, &config) == 0);
    CHECK(config == (3u << 24 | 0x21) && m.se[GENI_DMA_MODE / 4] == 0);
    /* Identity of both amplifiers. */
    Tfa9873Step steps[TFA9873_MAX_STEPS];
    Tfa9873Run r;
    const unsigned n = tfa9873_identify_program(steps);
    m.amp[1].por[0x03] = m.amp[1].reg[0x03] = 0x0a73;
    CHECK(speaker_amp_run(&b, 0x34, steps, n, &r) == 0 && r.reads[0] == 0x0b73);
    CHECK(speaker_amp_run(&b, 0x35, steps, n, &r) == 0 && r.reads[0] == 0x0a73);
    /* A register round trip. */
    const uint8_t w[3] = {0x26, 0x00, 0x13}, a[1] = {0x26};
    uint32_t rx = 0;
    CHECK(speaker_bus_transfer(&b, 0x34, w, 3, 0, NULL) == 0 && m.amp[0].reg[0x26] == 0x0013);
    CHECK(speaker_bus_transfer(&b, 0x34, a, 1, 2, &rx) == 0 && tfa9873_wire_value(rx) == 0x0013);
    /* No answer: NACK (-5), counted, the next transfer works. */
    m.amp[1].present = 0;
    CHECK(speaker_amp_run(&b, 0x35, steps, n, &r) == -5 && b.failures == 1 && b.last_error == GeniErrBus && (b.last_status & (1u << 10)));
    CHECK(speaker_bus_transfer(&b, 0x34, a, 1, 2, &rx) == 0);
    /* A silent engine: cancelled after 10 ms (-110), the engine usable afterwards. */
    m.mute = 1;
    const uint64_t t0 = m.us;
    CHECK(speaker_bus_transfer(&b, 0x34, w, 3, 0, NULL) == -110 && b.timeouts == 1 && m.us - t0 >= 10000);
    m.mute = 0;
    m.active = 0;
    m.status = 0;
    CHECK(speaker_bus_transfer(&b, 0x34, a, 1, 2, &rx) == 0 && tfa9873_wire_value(rx) == 0x0013);
    /* The engine was not usable: no bus. */
    model_init(&m);
    m.se[GENI_FW_REVISION / 4] = 1u << 8;
    CHECK(speaker_bus_init(&b, io, &config) == -19);
}

int main(void)
{
    pins();
    clocks();
    engine();
    transfers();
    printf("PASS hw: %u checks\n", checks);
    return 0;
}
