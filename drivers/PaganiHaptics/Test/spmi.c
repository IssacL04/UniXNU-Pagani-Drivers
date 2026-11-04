/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of HapticsSpmi.c against the arbiter model: discovery and the owned-mapping preference, the observer read command and its
 * status bits, the write whitelist (exhaustively), opening only owned and identified CFG/PTN channels, the exact write command and
 * data packing, per-write re-verification of the mapping and the owner, closing on errors, the one emergency stop. */
#include "model.h"

static const uint16_t kPpid[PGH_PERIPHS] = {0x7f0, 0x7f1, 0x7f2, 0x09d};

static void discovery(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io = m_io(&m);
    PghSpmi s;
    /* not v7: only the two id registers are read */
    m.version = 0x50000000u;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == -19 && m.core_reads == 2 && s.closed);
    m.version = 0x80000000u;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == -19);
    m.version = 0x70060000u;
    m.channels = 0;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == -19);
    m.channels = M_CH;
    CHECK(pgh_spmi_discover(&s, &io, 8, kPpid, 149) == -22 && pgh_spmi_discover(&s, 0, 0, kPpid, 149) == -22);
    /* the 13T layout: CFG/PTN owned by EE 0, the boost by EE 3 */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0);
    CHECK(s.p[PGH_CFG].apid == A_CFG && s.p[PGH_CFG].owner == 0 && s.p[PGH_CFG].irq == 1 && s.p[PGH_CFG].mappings == 1);
    CHECK(s.p[PGH_PTN].apid == A_PTN && s.p[PGH_HBST].apid == A_HBST && s.p[PGH_HBST].owner == 3);
    CHECK(s.p[PGH_SDAM].apid == A_SDAM && !s.p[PGH_CFG].writable);
    /* a PPID mapped twice: the mapping this EE owns wins, wherever it is in the table */
    m.map[A_CFG + 100] = 0x7f0u << 8;
    m.owner[A_CFG + 100] = 0;
    m.owner[A_CFG] = 2;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && s.p[PGH_CFG].apid == A_CFG + 100 && s.p[PGH_CFG].owner == 0);
    CHECK(s.p[PGH_CFG].mappings == 2);
    m.owner[A_CFG + 100] = 4;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && s.p[PGH_CFG].apid == A_CFG && s.p[PGH_CFG].owner == 2);
    /* a missing PPID */
    m.map[A_PTN] = 0;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && s.p[PGH_PTN].apid == -1);
    uint8_t v = 0;
    CHECK(pgh_spmi_read(&s, PGH_PTN, 4, &v, 1) == -19 && pgh_spmi_identify(&s, PGH_PTN) == -19);
}

static void reads(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io = m_io(&m);
    PghSpmi s;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0);
    CHECK(pgh_spmi_identify(&s, PGH_CFG) == 0 && s.p[PGH_CFG].type == 0x15 && s.p[PGH_CFG].subtype == 0x0a);
    CHECK(s.p[PGH_CFG].rev[1] == 5 && s.p[PGH_CFG].identified && m.bad_obs == 0 && m.obs_cmds == 2);
    uint8_t d[4] = {0};
    CHECK(pgh_spmi_read(&s, PGH_HBST, PGH_HBST_HW_CTRL_FOLLOW, d, 1) == 0 && d[0] == PGH_FOLLOW_HW_EN);
    CHECK(pgh_spmi_read(&s, PGH_CFG, 0, d, 5) == -22 && pgh_spmi_read(&s, PGH_CFG, 0xfe, d, 4) == -22);
    CHECK(pgh_spmi_read(&s, 4, 0, d, 1) == -22);
    m.obs_fail = 1;
    CHECK(pgh_spmi_read(&s, PGH_CFG, 4, d, 1) == -5 && s.failures == 1 && s.last_status == 0x3u);
    m.obs_fail = 0;
    m.never_done = 1;
    const unsigned delays = m.delays;
    CHECK(pgh_spmi_read(&s, PGH_CFG, 4, d, 1) == -110 && s.timeouts == 1 && m.delays - delays == PGH_ARB_POLLS);
    m.never_done = 0;
    CHECK(s.writes == 0 && m.ch_cmds == 0);   /* reads never touch the RW channels */
}

/* Every (periph, reg, len) the whitelist admits, with one value it admits. */
static void whitelist(void)
{
    PghSpmi s;
    memset(&s, 0, sizeof s);
    s.vmax_code_max = 149;
    const uint8_t zero[8] = {0};
    unsigned allowed = 0;
    for (unsigned p = 0; p < PGH_PERIPHS; ++p)
        for (unsigned reg = 0; reg < 256; ++reg)
            for (unsigned len = 1; len <= 8; ++len)
                if (pgh_spmi_allowed(&s, p, (uint8_t)reg, zero, len)) {
                    allowed++;
                    CHECK(p == PGH_CFG || p == PGH_PTN);
                }
    /* CFG: 16 one-byte registers + TLRA (1 or 2 bytes); PTN: FIFO 4-byte, 6 one-byte registers, the brake (8 start points) */
    unsigned brake = 0;
    for (unsigned r = 0; r < 8; ++r) brake += 8 - r;
    CHECK(allowed == 16 + 2 + 1 + 6 + brake);
    const uint8_t v80 = 0x80, v40 = 0x40, v81 = 0x81, v89 = 0x89, v84 = 0x84, v90 = 0x90, vff = 0xff, v8b = 0x8b, v01 = 0x01;
    CHECK(pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_EN_CTL, &v80, 1) && !pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_EN_CTL, &v40, 1));
    CHECK(pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_SPMI_PLAY, &v81, 1) && pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_SPMI_PLAY, &v89, 1));
    CHECK(!pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_SPMI_PLAY, &v84, 1));   /* SWR source */
    CHECK(!pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_SPMI_PLAY, &v90, 1));   /* pattern memory select bits */
    const uint8_t code149 = 149, code150 = 150, hdrm30 = 30, hdrm31 = 31;
    CHECK(pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_VMAX, &code149, 1) && !pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_VMAX, &code150, 1));
    CHECK(pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_VMAX_HDRM, &hdrm30, 1) && !pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_VMAX_HDRM, &hdrm31, 1));
    const uint8_t tl_ok[2] = {0x04, 0x98}, tl_bad[2] = {0x14, 0x98};
    CHECK(pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_TLRA_OL_HIGH, tl_ok, 2) && !pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_TLRA_OL_HIGH, tl_bad, 2));
    CHECK(!pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_FIFO_DIN_0, zero, 1) && pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_FIFO_DIN_0, zero, 4));
    CHECK(!pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_BRAKE_AMP + 1, zero, 8));
    CHECK(pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_MEM_OP_ACCESS, &v01, 1) && !pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_MEM_OP_ACCESS, &v80, 1));
    CHECK(pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_MMAP_FIFO, &vff, 1) && !pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_MMAP_FIFO, &v8b, 1));
    const uint8_t rate7 = 7, rate13 = 0x5d, rate14 = 14;
    CHECK(!pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_FIFO_PLAY_RATE, &rate7, 1) && pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_FIFO_PLAY_RATE, &rate13, 1));
    CHECK(!pgh_spmi_allowed(&s, PGH_PTN, PGH_PTN_FIFO_PLAY_RATE, &rate14, 1));
    CHECK(!pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_VSET_CFG, zero, 1) && !pgh_spmi_allowed(&s, PGH_CFG, PGH_CFG_HPWR_INTF, zero, 1));
    CHECK(!pgh_spmi_allowed(&s, PGH_HBST, PGH_HBST_VREG_EN, zero, 1) && !pgh_spmi_allowed(&s, PGH_SDAM, 0x42, zero, 1));
}

static void writes(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io = m_io(&m);
    PghSpmi s;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0);
    /* not opened: refused, and the refusal closes the bus */
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0x80) == -1 && s.closed && s.refused == 1 && m.ch_cmds == 0);
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0);
    CHECK(pgh_spmi_open(&s, PGH_CFG) == -1);                  /* not identified */
    CHECK(pgh_spmi_identify(&s, PGH_CFG) == 0 && pgh_spmi_identify(&s, PGH_PTN) == 0 && pgh_spmi_identify(&s, PGH_HBST) == 0);
    CHECK(pgh_spmi_open(&s, PGH_HBST) == -1 && pgh_spmi_open(&s, PGH_SDAM) == -1 && m.map_calls == 0);
    CHECK(pgh_spmi_open(&s, PGH_CFG) == 0 && pgh_spmi_open(&s, PGH_PTN) == 0 && m.map_calls == 2);
    CHECK(m.mapped[PGH_CFG] == A_CFG && m.mapped[PGH_PTN] == A_PTN);
    /* the command word and the data packing */
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0x80) == 0 && m.ch_cmds == 1 && m.reg[PGH_CFG][PGH_CFG_EN_CTL] == 0x80);
    CHECK(m.log[0].reg == PGH_CFG_EN_CTL && m.log[0].len == 1 && m.log[0].data[0] == 0x80);
    const uint8_t brake[8] = {0xff, 0x3f, 0x1f, 1, 2, 3, 4, 5};
    m.wdata1[A_PTN] = 0xdeadbeefu;
    CHECK(pgh_spmi_write(&s, PGH_PTN, PGH_PTN_BRAKE_AMP, brake, 8) == 0);
    CHECK(m.wdata0[A_PTN] == 0x011f3fffu && m.wdata1[A_PTN] == 0x05040302u && !memcmp(&m.reg[PGH_PTN][PGH_PTN_BRAKE_AMP], brake, 8));
    m.wdata1[A_PTN] = 0xdeadbeefu;
    const uint8_t four[4] = {1, 2, 3, 4};
    CHECK(pgh_spmi_write(&s, PGH_PTN, PGH_PTN_FIFO_DIN_0, four, 4) == 0 && m.wdata1[A_PTN] == 0xdeadbeefu);   /* WDATA1 untouched */
    CHECK(m.fifo_count == 4 && m.fifo[0] == 1 && m.fifo[3] == 4);
    /* refused values close the bus; nothing reaches the channel after */
    const unsigned cmds = m.ch_cmds;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_VMAX, 150) == -1 && s.closed && m.ch_cmds == cmds);
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_VMAX, 10) == -1 && m.ch_cmds == cmds);
    /* the emergency stop still goes out, once */
    CHECK(pgh_spmi_emergency_stop(&s) == 0 && m.ch_cmds == cmds + 1 && m.log[m.log_n - 1].reg == PGH_CFG_SPMI_PLAY);
    CHECK(pgh_spmi_emergency_stop(&s) == -1 && m.ch_cmds == cmds + 1);

    /* re-verification before every write: the mapping changed, or the owner */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0);
    CHECK(!pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    m.owner[A_CFG] = 2;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -1 && s.closed && pgh_spmi_emergency_stop(&s) == -1);
    m.owner[A_CFG] = 0;
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0);
    CHECK(!pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    m.map[A_CFG] = 0x7f3u << 8;
    const unsigned before = m.ch_cmds;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -1 && m.ch_cmds == before);
    m.map[A_CFG] = 0x7f0u << 8;
    /* the arbiter denies (owner check in hardware), a failure, a timeout: each closes the bus */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    m.fail_writes_after = 0;
    m.fail_status = 0x5u;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -5 && s.closed && s.last_status == 0x5u);
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -1);
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    m.never_done = 1;
    const unsigned delays = m.delays;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -110 && s.closed && m.delays - delays == PGH_ARB_POLLS);
    m.never_done = 0;
    /* a transaction the arbiter dropped: default as any failure; with retry_dropped it goes again, bounded, the channel checked each time */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    m.fail_writes_after = 0;
    m.fail_status = 0x9u;
    unsigned sent = m.ch_cmds;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -5 && s.closed && s.last_status == 0x9u && s.dropped == 0 && m.ch_cmds == sent + 1);
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    s.retry_dropped = 1;
    m.fail_writes_after = 0;
    m.fail_repeat = 2;
    sent = m.ch_cmds;
    unsigned waits = m.delays;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, PGH_HAPTICS_EN) == 0 && !s.closed && s.dropped == 3 && m.ch_cmds == sent + 4);
    CHECK(m.reg[PGH_CFG][PGH_CFG_EN_CTL] == PGH_HAPTICS_EN && m.delays - waits >= 3);
    /* one drop more than the retries: the bus closes, as for a failure */
    m.fail_writes_after = 0;
    m.fail_repeat = PGH_ARB_DROP_RETRIES;
    sent = m.ch_cmds;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -5 && s.closed && m.ch_cmds == sent + 1 + PGH_ARB_DROP_RETRIES);
    m.fail_writes_after = -1;
    m.fail_repeat = 0;
    /* only a drop: failed (bit 1), denied (bit 2) or failed and dropped together go on closing it at the first report */
    const uint32_t hard[] = {0x3u, 0x5u, 0xbu, 0xdu};
    for (unsigned i = 0; i < sizeof hard / sizeof hard[0]; ++i) {
        CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
        s.retry_dropped = 1;
        m.fail_writes_after = 0;
        m.fail_status = hard[i];
        sent = m.ch_cmds;
        CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -5 && s.closed && s.dropped == 0 && m.ch_cmds == sent + 1);
    }
    /* the owner changes between the attempts: the repeat is not sent */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    s.retry_dropped = 1;
    m.fail_writes_after = 0;
    m.fail_status = 0x9u;
    m.owner_after_drop = 1;
    sent = m.ch_cmds;
    CHECK(pgh_spmi_write1(&s, PGH_CFG, PGH_CFG_EN_CTL, 0) == -1 && s.closed && m.ch_cmds == sent + 1);
    m.owner[A_CFG] = 0;
    m.owner_after_drop = 0;
    /* the emergency stop gets through a drop as well */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG) && !pgh_spmi_open(&s, PGH_CFG));
    s.retry_dropped = 1;
    m.fail_writes_after = 0;
    m.reg[PGH_CFG][PGH_CFG_SPMI_PLAY] = 0x89u;
    CHECK(pgh_spmi_emergency_stop(&s) == 0 && m.reg[PGH_CFG][PGH_CFG_SPMI_PLAY] == 0 && s.dropped == 1);
    m.reg[PGH_CFG][PGH_CFG_EN_CTL] = 0;
    /* a read too: default it reports the failure, with retry_dropped it reads */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG));
    uint8_t type = 0;
    m.obs_drops = 1;
    CHECK(pgh_spmi_read(&s, PGH_CFG, PGH_REG_TYPE, &type, 1) == -5);
    s.retry_dropped = 1;
    m.obs_drops = 2;
    CHECK(pgh_spmi_read(&s, PGH_CFG, PGH_REG_TYPE, &type, 1) == 0 && type == 0x15 && s.dropped == 2);
    m.obs_drops = PGH_ARB_DROP_RETRIES + 1;
    CHECK(pgh_spmi_read(&s, PGH_CFG, PGH_REG_TYPE, &type, 1) == -5);
    m.obs_drops = 0;
    /* opening requires this EE to own the channel at the time */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && !pgh_spmi_identify(&s, PGH_CFG));
    m.owner[A_CFG] = 1;
    const unsigned maps = m.map_calls;
    CHECK(pgh_spmi_open(&s, PGH_CFG) == -1 && m.map_calls == maps);
    /* the owner found at discovery counts: a channel that became this EE's only afterwards is not opened either */
    CHECK(pgh_spmi_discover(&s, &io, 0, kPpid, 149) == 0 && s.p[PGH_CFG].owner == 1 && !pgh_spmi_identify(&s, PGH_CFG));
    m.owner[A_CFG] = 0;
    CHECK(pgh_spmi_open(&s, PGH_CFG) == -1 && m.map_calls == maps);
    CHECK(m.bad_ch == 0 && m.bad_obs == 0 && m.overflow == 0);
}

int main(void)
{
    discovery();
    reads();
    whitelist();
    writes();
    printf("PASS spmi: %u checks\n", checks);
    return 0;
}
