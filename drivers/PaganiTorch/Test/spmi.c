/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of TorchSpmi.c against the arbiter model: discovery and the owned-mapping preference, the observer read command and its
 * status bits, the write whitelist (every register and value), the interlock, opening only an owned and identified channel, the exact
 * write command, per-write re-verification of the mapping and the owner, closing on errors, the one emergency stop. */
#include "model.h"

enum { CODE_MAX = 39 };   /* 200 mA */
static const uint16_t kPpid[PGT_PERIPHS] = {M_PPID};

static void discovery(void)
{
    static Model m;
    m_init(&m);
    PgtSpmiIo io = m_io(&m);
    PgtSpmi s;
    /* not v7: only the two id registers are read */
    m.version = 0x50000000u;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == -19 && m.core_reads == 2 && s.closed);
    m.version = 0x80000000u;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == -19);
    m.version = 0x70060000u;
    m.channels = 0;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == -19);
    m.channels = M_CH;
    /* bad arguments */
    CHECK(pgt_spmi_discover(&s, &io, 8, kPpid, 3, CODE_MAX) == -22 && pgt_spmi_discover(&s, 0, 0, kPpid, 3, CODE_MAX) == -22);
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 0, CODE_MAX) == -22 && pgt_spmi_discover(&s, &io, 0, kPpid, 16, CODE_MAX) == -22);
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, 0) == -22 && pgt_spmi_discover(&s, &io, 0, kPpid, 3, 128) == -22);
    CHECK(pgt_spmi_discover(0, &io, 0, kPpid, 3, CODE_MAX) == -22);
    /* the 13T layout */
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0);
    CHECK(s.p[PGT_FLASH].apid == A_FLASH && s.p[PGT_FLASH].owner == 0 && s.p[PGT_FLASH].irq == 1 && s.p[PGT_FLASH].mappings == 1);
    CHECK(!s.p[PGT_FLASH].writable && s.chan_mask == 3 && s.itarget_max == CODE_MAX);
    /* a PPID mapped twice: the mapping this EE owns wins, wherever it is in the table */
    m.map[A_FLASH + 100] = (uint32_t)M_PPID << 8;
    m.owner[A_FLASH + 100] = 0;
    m.owner[A_FLASH] = 2;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && s.p[PGT_FLASH].apid == A_FLASH + 100 && s.p[PGT_FLASH].owner == 0);
    CHECK(s.p[PGT_FLASH].mappings == 2);
    m.owner[A_FLASH + 100] = 4;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && s.p[PGT_FLASH].apid == A_FLASH && s.p[PGT_FLASH].owner == 2);
    /* a missing PPID */
    m.map[A_FLASH] = m.map[A_FLASH + 100] = 0;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && s.p[PGT_FLASH].apid == -1);
    uint8_t v = 0;
    CHECK(pgt_spmi_read(&s, PGT_FLASH, 4, &v, 1) == -19 && pgt_spmi_identify(&s, PGT_FLASH) == -19);
}

static void reads(void)
{
    static Model m;
    m_init(&m);
    PgtSpmiIo io = m_io(&m);
    PgtSpmi s;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0);
    CHECK(pgt_spmi_identify(&s, PGT_FLASH) == 0 && s.p[PGT_FLASH].type == 0x18 && s.p[PGT_FLASH].subtype == 0x07);
    CHECK(s.p[PGT_FLASH].rev[1] == 1 && s.p[PGT_FLASH].rev[3] == 2 && s.p[PGT_FLASH].identified && m.bad_obs == 0 && m.obs_cmds == 2);
    uint8_t d[4] = {0};
    CHECK(pgt_spmi_read(&s, PGT_FLASH, PGT_FLASH_ITARGET(0), d, 4) == 0 && d[0] == 0x7f && d[1] == 0x7f);
    CHECK(pgt_spmi_read(&s, PGT_FLASH, 0, d, 5) == -22 && pgt_spmi_read(&s, PGT_FLASH, 0xfe, d, 4) == -22);
    CHECK(pgt_spmi_read(&s, 1, 0, d, 1) == -22 && pgt_spmi_read(&s, PGT_FLASH, 0, d, 0) == -22 && pgt_spmi_read(&s, PGT_FLASH, 0, 0, 1) == -22);
    m.obs_fail = 1;
    CHECK(pgt_spmi_read(&s, PGT_FLASH, 4, d, 1) == -5 && s.failures == 1 && s.last_status == 0x3u);
    m.obs_fail = 0;
    m.never_done = 1;
    const unsigned delays = m.delays;
    CHECK(pgt_spmi_read(&s, PGT_FLASH, 4, d, 1) == -110 && s.timeouts == 1 && m.delays - delays == PGT_ARB_POLLS);
    m.never_done = 0;
    CHECK(s.writes == 0 && m.ch_cmds == 0);   /* reads never touch the RW channels */
}

/* Every (register, value) the gate admits, with the interlock satisfied, against the list of the sequences. */
static void whitelist(void)
{
    PgtSpmi s;
    memset(&s, 0, sizeof s);
    s.chan_mask = 0x3;
    s.itarget_max = CODE_MAX;
    s.ires_set = 1;
    s.itarget_set = s.strobe_set = s.timer_set = 0xf;
    unsigned regs = 0;
    for (unsigned reg = 0; reg < 256; ++reg) {
        unsigned admitted = 0;
        for (unsigned value = 0; value < 256; ++value) {
            const uint8_t v = (uint8_t)value;
            const int yes = pgt_spmi_allowed(&s, PGT_FLASH, (uint8_t)reg, &v, 1);
            admitted += yes != 0;
            int expect = 0;
            if (reg == PGT_FLASH_ITARGET(0) || reg == PGT_FLASH_ITARGET(1)) expect = (value & 0x7fu) <= CODE_MAX;
            else if (reg == PGT_FLASH_CHAN_TIMER(0) || reg == PGT_FLASH_CHAN_TIMER(1)) expect = value == 0;
            else if (reg == PGT_FLASH_CHAN_STROBE(0) || reg == PGT_FLASH_CHAN_STROBE(1)) expect = (value & 0x7fu) == 1;
            else if (reg == PGT_FLASH_EN_CTL) expect = value == 0 || value == 0x80;
            else if (reg == PGT_FLASH_IRES) expect = (value & 3u) == 3;
            else if (reg == PGT_FLASH_CHAN_EN) expect = value <= 3;
            CHECK(!yes == !expect);
        }
        regs += admitted != 0;
    }
    CHECK(regs == 2 + 2 + 2 + 3);   /* nothing else: never the other channels, the status or the identity registers */
    /* wrong peripheral, length, null */
    const uint8_t z[8] = {0};
    CHECK(!pgt_spmi_allowed(&s, 1, PGT_FLASH_EN_CTL, z, 1) && !pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_EN_CTL, z, 2));
    CHECK(!pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_EN_CTL, z, 0) && !pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0, 1));
    CHECK(!pgt_spmi_allowed(0, PGT_FLASH, PGT_FLASH_EN_CTL, z, 1));
    /* other masks: exactly the channels named */
    s.chan_mask = 0x4;
    const uint8_t one = 1, four = 4, five = 5, two = 2;
    CHECK(pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_ITARGET(2), &one, 1) && !pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_ITARGET(1), &one, 1));
    CHECK(pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &four, 1) && !pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &five, 1));
    CHECK(!pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &two, 1));
    CHECK(pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_STROBE(2), &one, 1) && !pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_STROBE(3), &one, 1));
    /* 0x46 + 3 and 0x4a + 4 are not channels of anything: the ends of each range */
    const uint8_t zero = 0;
    CHECK(!pgt_spmi_allowed(&s, PGT_FLASH, 0x3d, &zero, 1) && !pgt_spmi_allowed(&s, PGT_FLASH, 0x47, &zero, 1) &&
          !pgt_spmi_allowed(&s, PGT_FLASH, 0x48, &zero, 1) && !pgt_spmi_allowed(&s, PGT_FLASH, 0x4f, &zero, 1));
}

/* Each of the interlock's four conditions alone keeps the channels off; off is always allowed. */
static void interlock(void)
{
    static Model m;
    m_init(&m);
    PgtSpmiIo io = m_io(&m);
    PgtSpmi s;
    const uint8_t on = 3, off = 0, ch0 = 1;
    const uint8_t order[7][2] = {{PGT_FLASH_IRES, 3}, {PGT_FLASH_ITARGET(0), 9}, {PGT_FLASH_ITARGET(1), 9}, {PGT_FLASH_CHAN_TIMER(0), 0},
                                 {PGT_FLASH_CHAN_TIMER(1), 0}, {PGT_FLASH_CHAN_STROBE(0), 1}, {PGT_FLASH_CHAN_STROBE(1), 1}};
    for (int skip = -1; skip < 7; ++skip) {
        CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && !pgt_spmi_identify(&s, PGT_FLASH) && !pgt_spmi_open(&s, PGT_FLASH));
        CHECK(pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &off, 1) && !pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &on, 1));
        for (int i = 0; i < 7; ++i)
            if (i != skip) CHECK(pgt_spmi_write1(&s, PGT_FLASH, order[i][0], order[i][1]) == 0);
        CHECK(!pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &on, 1) == (skip >= 0));
        if (skip == 2 || skip == 4 || skip == 6) CHECK(pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &ch0, 1));   /* channel 0 is ready */
        if (skip == 1 || skip == 3 || skip == 5) CHECK(!pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &ch0, 1));
        if (skip == 0) CHECK(!pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &ch0, 1));
        if (skip >= 0) {
            const unsigned cmds = m.ch_cmds;
            CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, 3) == -1 && s.closed && m.ch_cmds == cmds);
        } else {
            CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, 3) == 0);
        }
        /* opening again clears what was written */
        CHECK(!s.closed || skip >= 0);
        s.closed = 0;
        CHECK(pgt_spmi_open(&s, PGT_FLASH) == 0 && !pgt_spmi_allowed(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, &on, 1));
    }
    /* a failed write does not count */
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && !pgt_spmi_identify(&s, PGT_FLASH) && !pgt_spmi_open(&s, PGT_FLASH));
    m.fail_writes_after = 0;
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_IRES, 3) == -5 && !s.ires_set);
}

static void writes(void)
{
    static Model m;
    m_init(&m);
    PgtSpmiIo io = m_io(&m);
    PgtSpmi s;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0);
    /* not opened: refused, and the refusal closes the bus */
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0x80) == -1 && s.closed && s.refused == 1 && m.ch_cmds == 0);
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0);
    CHECK(pgt_spmi_open(&s, PGT_FLASH) == -1);                  /* not identified */
    CHECK(pgt_spmi_identify(&s, PGT_FLASH) == 0);
    CHECK(pgt_spmi_open(&s, 1) == -1 && m.map_calls == 0);
    m.chan_map_fail = 1;
    CHECK(pgt_spmi_open(&s, PGT_FLASH) == -12 && !s.p[PGT_FLASH].writable);
    m.chan_map_fail = 0;
    CHECK(pgt_spmi_open(&s, PGT_FLASH) == 0 && m.map_calls == 2 && m.mapped == A_FLASH);
    CHECK(pgt_spmi_open(&s, PGT_FLASH) == 0 && m.map_calls == 2);   /* the page is mapped once */
    /* the command word and the data */
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0x80) == 0 && m.ch_cmds == 1 && m.reg[PGT_FLASH_EN_CTL] == 0x80);
    CHECK(m.log[0].reg == PGT_FLASH_EN_CTL && m.log[0].value == 0x80 && s.writes == 1 && m.wdata1[A_FLASH] == 0);
    /* refused values close the bus; nothing reaches the channel after */
    const unsigned cmds = m.ch_cmds;
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_ITARGET(0), CODE_MAX + 1) == -1 && s.closed && m.ch_cmds == cmds);
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_ITARGET(0), 1) == -1 && m.ch_cmds == cmds);
    /* the emergency stop still goes out, once: all channels off, then the module; the bus stays closed */
    CHECK(pgt_spmi_emergency_stop(&s) == 0 && m.ch_cmds == cmds + 2);
    CHECK(m.log[m.log_n - 2].reg == PGT_FLASH_CHAN_EN && m.log[m.log_n - 2].value == 0);
    CHECK(m.log[m.log_n - 1].reg == PGT_FLASH_EN_CTL && m.log[m.log_n - 1].value == 0);
    CHECK(pgt_spmi_emergency_stop(&s) == -1 && m.ch_cmds == cmds + 2);
    /* the emergency stop closes an open bus, and tries the module when the channels' write fails */
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && !pgt_spmi_identify(&s, PGT_FLASH) && !pgt_spmi_open(&s, PGT_FLASH));
    m.reg[PGT_FLASH_EN_CTL] = 0x80;
    m.fail_writes_after = 0;
    CHECK(pgt_spmi_emergency_stop(&s) == -5 && m.reg[PGT_FLASH_EN_CTL] == 0 && s.closed);
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_CHAN_EN, 0) == -1);

    /* re-verification before every write: the mapping changed, or the owner */
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0);
    CHECK(!pgt_spmi_identify(&s, PGT_FLASH) && !pgt_spmi_open(&s, PGT_FLASH));
    m.owner[A_FLASH] = 2;
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0) == -1 && s.closed && pgt_spmi_emergency_stop(&s) == -1);
    m.owner[A_FLASH] = 0;
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0);
    CHECK(!pgt_spmi_identify(&s, PGT_FLASH) && !pgt_spmi_open(&s, PGT_FLASH));
    m.map[A_FLASH] = 0x1efu << 8;
    const unsigned before = m.ch_cmds;
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0) == -1 && m.ch_cmds == before);
    m.map[A_FLASH] = (uint32_t)M_PPID << 8;
    /* the arbiter denies (owner check in hardware), a failure, a timeout: each closes the bus */
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && !pgt_spmi_identify(&s, PGT_FLASH) && !pgt_spmi_open(&s, PGT_FLASH));
    m.fail_writes_after = 0;
    m.fail_status = 0x5u;
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0) == -5 && s.closed && s.last_status == 0x5u);
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0) == -1);
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && !pgt_spmi_identify(&s, PGT_FLASH) && !pgt_spmi_open(&s, PGT_FLASH));
    m.never_done = 1;
    const unsigned delays = m.delays;
    CHECK(pgt_spmi_write1(&s, PGT_FLASH, PGT_FLASH_EN_CTL, 0) == -110 && s.closed && m.delays - delays == PGT_ARB_POLLS);
    m.never_done = 0;
    /* opening requires this EE to own the channel at the time */
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && !pgt_spmi_identify(&s, PGT_FLASH));
    m.owner[A_FLASH] = 1;
    const unsigned maps = m.map_calls;
    CHECK(pgt_spmi_open(&s, PGT_FLASH) == -1 && m.map_calls == maps);
    /* the owner found at discovery counts: a channel that became this EE's only afterwards is not opened either */
    CHECK(pgt_spmi_discover(&s, &io, 0, kPpid, 3, CODE_MAX) == 0 && s.p[PGT_FLASH].owner == 1 && !pgt_spmi_identify(&s, PGT_FLASH));
    m.owner[A_FLASH] = 0;
    CHECK(pgt_spmi_open(&s, PGT_FLASH) == -1 && m.map_calls == maps);
    /* the emergency stop without an opened channel writes nothing */
    CHECK(pgt_spmi_emergency_stop(&s) == -1 && m.bad_ch == 0 && m.bad_obs == 0);
}

int main(void)
{
    discovery();
    reads();
    whitelist();
    interlock();
    writes();
    printf("PASS spmi: %u checks\n", checks);
    return 0;
}
