/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of HapticsCore.c on the arbiter + HAP530 model (model.h): the dry run writes nothing; every identity, revision, owner and
 * arbiter problem refuses before any write; the initialization's write sequence; DIRECT_PLAY start and stop order with the brake and
 * the closed-loop period; effects; streams (order, no FIFO overflow, refill, drain, abort, underrun); the voltage caps, the play
 * length cap and the on-time budget; faults (bus failure, timeout, short circuit, owner change) stop the play and keep the driver off;
 * an HPWR that does not settle and a boost in use refuse the play without a fault; every wait is bounded. */
#include "model.h"

static PghCore core;   /* large (the ring): static */

static int init_with(Model *m, PghConfig cfg, PghSpmiIo *io)
{
    PghOps ops = m_ops(m);
    *io = m_io(m);
    return pgh_core_init(&core, &cfg, &ops, io);
}

static int command(uint32_t op, uint32_t a, uint32_t b, uint32_t c)
{
    const PghCmd cmd = {op, a, b, c};
    return pgh_core_command(&core, &cmd);
}

static void dry_run(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io;
    CHECK(init_with(&m, m_config(PGH_MODE_DRY), &io) == 1);
    CHECK(core.state == PGH_ST_DRY && core.stage == 2);
    CHECK(m.ch_cmds == 0 && m.map_calls == 1 && m.mapped[PGH_CFG] == A_CFG && m.bad_obs == 0 && m.obs_cmds > 40);
    CHECK(m_logged(&m, "[haptics] chan cfg apid=300 map=0 status=0x0 (read only)") && m.log_n == 0);   /* a read, no command */
    CHECK(m_logged(&m, "[haptics] spmi rc=0 v=0x70060000 ch=600 ee=0 cfg=0x7f0:300:ee0:irq1:x1 ptn=0x7f1:301:ee0:irq0:x1 hbst=0x7f2:302:ee3"));
    CHECK(m_logged(&m, "[haptics] id cfg=15/0a:r5.0") && m_logged(&m, "verdict=ok"));
    CHECK(m_logged(&m, "[haptics] reg cfg 08=") && m_logged(&m, "[haptics] reg ptn") && m_logged(&m, " a0=8b"));
    CHECK(m_logged(&m, "[haptics] reg hbst") && m_logged(&m, "[haptics] reg sdam") && m_logged(&m, " 75=24"));
    CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 0) == -19 && pgh_core_poll(&core) == 0 && m.ch_cmds == 0);
    /* the dry run also reports a problem it would refuse for */
    m_init(&m);
    m.reg[PGH_CFG][5] = 0x09;
    CHECK(init_with(&m, m_config(PGH_MODE_DRY), &io) == 1 && m_logged(&m, "verdict=cfg-type") && m.ch_cmds == 0);
    CHECK(m.map_calls == 0 && !m_logged(&m, "[haptics] chan cfg"));   /* no channel probe for a peripheral it would refuse */
}

static void refusals(void)
{
    struct {const char *name; void (*apply)(Model *, PghConfig *); int rc; const char *verdict;} cases[] = {
        {"cfg owner", 0, -1, "cfg-owner"}, {"cfg subtype", 0, -1, "cfg-type"}, {"cfg type", 0, -1, "cfg-type"},
        {"cfg revision", 0, -1, "cfg-revision"}, {"cfg minor", 0, -1, "cfg-revision"}, {"ptn revision", 0, -1, "ptn-revision"},
        {"ptn pinned", 0, -1, "ptn-type"}, {"hbst absent", 0, -1, "hbst-absent"}, {"no ptn", 0, -1, "no-channel"},
        {"ptn owner", 0, -1, "ptn-owner"}, {"arbiter", 0, -19, 0}, {"unreadable", 0, -1, "unreadable"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        static Model m;
        m_init(&m);
        PghConfig cfg = m_config(PGH_MODE_ON);
        switch (i) {
        case 0: m.owner[A_CFG] = 2; break;
        case 1: m.reg[PGH_CFG][5] = 0x09; break;
        case 2: m.reg[PGH_CFG][4] = 0x16; break;
        case 3: m.reg[PGH_CFG][1] = 4; break;
        case 4: m.reg[PGH_CFG][0] = 2; break;
        case 5: m.reg[PGH_PTN][1] = 4; break;
        case 6: cfg.ptn_type = 0x15; cfg.ptn_subtype = 0x99; break;
        case 7: m.reg[PGH_HBST][4] = 0; break;
        case 8: m.map[A_PTN] = 0; break;
        case 9: m.owner[A_PTN] = 1; break;
        case 10: m.version = 0x50000000u; break;
        case 11: m.obs_fail = 1; break;
        }
        PghSpmiIo io;
        const int rc = init_with(&m, cfg, &io);
        if (rc != cases[i].rc) fprintf(stderr, "case %s: rc %d\n", cases[i].name, rc);
        CHECK(rc == cases[i].rc);
        CHECK(m.ch_cmds == 0 && m.map_calls == 0 && core.state == PGH_ST_OFF);   /* nothing written, no channel mapped */
        if (cases[i].verdict) {
            char want[64];
            snprintf(want, sizeof want, "verdict=%s", cases[i].verdict);
            CHECK(m_logged(&m, want) && m_logged(&m, "[haptics] refused: "));
        }
        CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 0) == -19 && m.ch_cmds == 0);
    }
    /* pinned identities that match are accepted */
    static Model m;
    m_init(&m);
    PghConfig cfg = m_config(PGH_MODE_ON);
    cfg.ptn_type = 0x15; cfg.ptn_subtype = 0x0b; cfg.hbst_type = 0x15; cfg.hbst_subtype = 0x0c;
    PghSpmiIo io;
    CHECK(init_with(&m, cfg, &io) == 0 && core.state == PGH_ST_IDLE);
}

static void init_sequence(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io;
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0 && core.state == PGH_ST_IDLE && core.stage == 3);
    const struct {uint8_t p, reg, len, v0;} want[] = {
        {0, PGH_CFG_FAULT_CLR, 1, 0x17}, {0, PGH_CFG_VMAX_HDRM, 1, 30}, {0, PGH_CFG_DRV_WF_SEL, 1, 0x01},
        {0, PGH_CFG_BRAKE_MODE_CFG, 1, 0x01}, {0, PGH_CFG_CAL_EN, 1, 0x04}, {0, PGH_CFG_TLRA_OL_HIGH, 2, 0x04},
        {0, PGH_CFG_DRV_DUTY_CFG, 1, 0x56}, {0, PGH_CFG_AUTORES_CFG, 1, 0x32}, {0, PGH_CFG_AUTO_SHUTDOWN, 1, 0x00},
        {1, PGH_PTN_FIFO_EMPTY_CFG, 1, 0}, {1, PGH_PTN_MMAP_FIFO, 1, 0xff}, {1, PGH_PTN_BRAKE_AMP, 8, 0xff},
        {0, PGH_CFG_ZX_SYNC_CFG, 1, PGH_EN_PDN_ZX_TO_FAULT}, {0, PGH_CFG_FAULT_CLR, 1, 0x17}, {0, PGH_CFG_VMAX, 1, 25},
        {0, PGH_CFG_ZX_SYNC_CFG, 1, 0},
    };
    CHECK(m.log_n == sizeof want / sizeof want[0]);
    for (unsigned i = 0; i < m.log_n; ++i)
        CHECK(m.log[i].periph == want[i].p && m.log[i].reg == want[i].reg && m.log[i].len == want[i].len && m.log[i].data[0] == want[i].v0);
    CHECK(m.log[5].data[1] == 0x98);   /* 5882 us / 5 = 0x498 */
    CHECK(m.log[11].data[1] == 0x3f && m.log[11].data[2] == 0x1f && m.log[11].data[3] == 0);
    CHECK(core.fifo_len == 8192 && m.fifo_len == 8192 && core.hw_max_mv == 10000 && core.vmax_code == 25);
    CHECK(m.reg[PGH_CFG][PGH_CFG_EN_CTL] == 0 && !core.en_on);   /* the module is enabled by the first play */
    CHECK(m.bad_ch == 0 && m.bad_obs == 0 && m_logged(&m, "[haptics] ready fifo=8192"));
    /* a play left running by firmware is stopped first */
    m_init(&m);
    m.reg[PGH_CFG][PGH_CFG_SPMI_PLAY] = 0x81;
    m.playing = 1;
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0 && m.log[0].reg == PGH_CFG_SPMI_PLAY && m.log[0].data[0] == 0 && !m.playing);
}

static void direct_play(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io;
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    unsigned mark = m.log_n;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 300, 0) == 0 && core.state == PGH_ST_DIRECT && m.playing && m.brake);
    /* start order: SWR ignored, amplitude, auto resonance on, faults cleared, the module enabled (after 500 us), SPMI_PLAY last */
    const int swr = m_find(&m, mark, 0, PGH_CFG_TRIG_PRIORITY), amp = m_find(&m, mark, 1, PGH_PTN_DIRECT_PLAY);
    const int ares = m_find(&m, mark, 0, PGH_CFG_AUTORES_CFG), clr = m_find(&m, mark, 0, PGH_CFG_FAULT_CLR);
    const int en = m_find(&m, mark, 0, PGH_CFG_EN_CTL), play = m_find(&m, mark, 0, PGH_CFG_SPMI_PLAY);
    CHECK(swr >= 0 && amp > swr && ares > amp && clr > ares && en > clr && play == (int)m.log_n - 1);
    CHECK(m.log[swr].data[0] == PGH_SWR_IGNORE && m.log[amp].data[0] == 255 && m.log[ares].data[0] == 0xb2);
    CHECK(m.log[en].data[0] == 0x80 && m.log[play].data[0] == 0x89 && m.log[play].t - m.log[en - 0].t <= 10);
    CHECK(m_find(&m, mark, 0, PGH_CFG_VMAX) < 0);   /* 1250 mV already set by the init */
    /* runs its time, then stops: PLAY_EN off with the brake, the brake waited for, headroom and SWR restored */
    mark = m.log_n;
    m_run(&m, &core, 1000000);
    CHECK(core.state == PGH_ST_IDLE && !m.playing && m.now_us >= 300000 && m.now_us < 340000);
    CHECK(m.log[mark].reg == PGH_CFG_MOD_STATUS_XT || m.log[mark].reg == PGH_CFG_MOD_STATUS_SEL);   /* closed-loop status read */
    const int stop = m_find(&m, mark, 0, PGH_CFG_SPMI_PLAY);
    CHECK(stop >= 0 && m.log[stop].data[0] == 0x09);
    const int hdrm = m_find(&m, stop, 0, PGH_CFG_VMAX_HDRM), swr2 = m_find(&m, stop, 0, PGH_CFG_TRIG_PRIORITY);
    CHECK(hdrm > stop && m.log[hdrm].data[0] == 30 && swr2 > hdrm && m.log[swr2].data[0] == 0);
    CHECK(m.now_us >= m.brake_until_us);   /* the brake was waited for */
    /* the closed-loop period (6960 x 0.833333 us = 5799 us, 172.4 Hz) became the open-loop period */
    CHECK(core.cl_t_lra_us == 5799 && core.f0_dhz == 1724);
    const int tlra = m_find(&m, stop, 0, PGH_CFG_TLRA_OL_HIGH);
    CHECK(tlra > stop && m.log[tlra].data[0] == 0x04 && m.log[tlra].data[1] == 0x87);   /* 5799 / 5 = 0x487 */
    CHECK(m_logged(&m, "closed-loop period us=5799"));
    /* a period outside 160..180 Hz is not taken */
    m.good[0] = (uint8_t)(5000u >> 8); m.good[1] = (uint8_t)(5000u & 0xffu);
    m.cal[0] = (uint8_t)(PGH_AUTO_RES_CAL_DONE | (5000u >> 8)); m.cal[1] = (uint8_t)(5000u & 0xffu);
    CHECK(command(PGH_CMD_CONSTANT, 500, 200, 0) == 0);
    m_run(&m, &core, 1000000);
    CHECK(core.cl_t_lra_us == 5799 && m_logged(&m, "outside the accepted range"));
    CHECK(m.reg[PGH_PTN][PGH_PTN_DIRECT_PLAY] == 128);   /* 500 permille of 255, rounded up */
    /* an explicit stop */
    CHECK(command(PGH_CMD_CONSTANT, 1000, 1500, 0) == 0);
    m_advance(&m, 50000);
    CHECK(pgh_core_poll(&core) > 0 && command(PGH_CMD_STOP, 0, 0, 0) == 0 && core.state == PGH_ST_IDLE && !m.playing);
    CHECK(command(PGH_CMD_CONSTANT, 0, 100, 0) == -22 && command(PGH_CMD_CONSTANT, 100, 0, 0) == -22 && command(99, 0, 0, 0) == -22);
    CHECK(m.bad_ch == 0 && m.overflow == 0);
}

static void caps(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io;
    PghConfig cfg = m_config(PGH_MODE_ON);
    CHECK(init_with(&m, cfg, &io) == 0);
    /* the runtime cap (3600 mV) */
    CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 9000) == 0 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 72 && core.clips >= 1);
    CHECK(command(PGH_CMD_STOP, 0, 0, 0) == 0);
    /* the length cap: 10 s asked, 2 s played */
    const uint64_t t0 = m.now_us;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 10000, 0) == 0);
    m_run(&m, &core, 20000000);
    CHECK(core.state == PGH_ST_IDLE && m.now_us - t0 >= 2000000 && m.now_us - t0 < 2100000);
    /* the budget: 5 s bucket, 50 % refill: back-to-back 2 s plays are clipped, then refused */
    uint64_t played = 0;
    const uint64_t start = m.now_us;
    int refused = 0;
    for (int i = 0; i < 12; ++i) {
        const uint64_t t = m.now_us;
        const int rc = command(PGH_CMD_CONSTANT, 1000, 2000, 0);
        if (rc == -16) {refused++; m_advance(&m, 20000); continue;}
        CHECK(rc == 0);
        m_run(&m, &core, 10000000);
        played += m.now_us - t;
    }
    const uint64_t elapsed = m.now_us - start;
    CHECK(refused > 0 && played <= 5000000u + elapsed / 2 + 200000u);
    /* hard cap: the bus refuses VMAX codes above 7450 mV even if asked directly */
    CHECK(core.spmi.vmax_code_max == 149);
    /* raising the runtime cap to the hard cap; the boost's 5 V clamp; low battery */
    m_init(&m);
    cfg.cap_mv = 9000;    /* clamped to hard 7450 */
    cfg.budget_ms = 100000;
    CHECK(init_with(&m, cfg, &io) == 0 && core.cfg.cap_mv == 7450);
    CHECK(command(PGH_CMD_CONSTANT, 1000, 50, 9000) == 0 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 149);
    m_run(&m, &core, 1000000);
    m.reg[PGH_HBST][PGH_HBST_CLAMP] = PGH_HBST_CLAMP_5V;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 50, 9000) == 0 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 100);
    m_run(&m, &core, 1000000);
    m.reg[PGH_HBST][PGH_HBST_CLAMP] = 0;
    m.battery_percent = 3;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 50, 9000) == 0 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 136);   /* 6800 mV */
    m_run(&m, &core, 1000000);
    m.battery_percent = 15;
    m.battery_dc = -60;   /* cold: 20 % applies */
    CHECK(command(PGH_CMD_CONSTANT, 1000, 50, 9000) == 0 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 136);
    m_run(&m, &core, 1000000);
    m.battery_dc = 250;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 50, 9000) == 0 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 149);
    m_run(&m, &core, 1000000);
    for (unsigned i = 0; i < m.log_n; ++i)
        if (m.log[i].reg == PGH_CFG_VMAX && m.log[i].periph == PGH_CFG) CHECK(m.log[i].data[0] <= 149);
}

static void effects(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io;
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    const unsigned mark = m.log_n;
    CHECK(command(PGH_CMD_EFFECT, 1, 1000, 0) == 0 && core.state == PGH_ST_EFFECT);
    CHECK(m.reg[PGH_CFG][PGH_CFG_VMAX] == 72 && (m.reg[PGH_PTN][PGH_PTN_FIFO_PLAY_RATE] & 15) == 0);   /* 3600 mV, T_LRA */
    const int flush = m_find(&m, mark, 1, PGH_PTN_MEM_OP_ACCESS), din = m_find(&m, mark, 1, PGH_PTN_FIFO_DIN_0);
    const int play = m_find(&m, mark, 0, PGH_CFG_SPMI_PLAY);
    CHECK(flush >= 0 && m.log[flush].data[0] == 1 && m.log[flush + 1].reg == PGH_PTN_MEM_OP_ACCESS && m.log[flush + 1].data[0] == 0);
    CHECK(din > flush && play > din && m.log[play].data[0] == 0x80 && play == (int)m.log_n - 1);
    m_run(&m, &core, 1000000);
    CHECK(core.state == PGH_ST_IDLE && m.out_n == 6);
    for (unsigned i = 0; i < m.out_n; ++i) CHECK(m.out[i] == 0x7f);
    CHECK(m.now_us < 200000);
    /* the silent primitive does nothing; an unknown effect is refused */
    const unsigned before = m.log_n;
    CHECK(command(PGH_CMD_EFFECT, 6, 1000, 0) == 0 && m.log_n == before && command(PGH_CMD_EFFECT, 15, 1000, 0) == -22);
    /* half strength: half the effect voltage */
    CHECK(command(PGH_CMD_EFFECT, 2, 500, 0) == 0 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 36);
    m_run(&m, &core, 1000000);
    CHECK(m.out_n == 8 && m.overflow == 0);
}

static unsigned fill_pattern(int8_t *buf, unsigned n, unsigned from)
{
    for (unsigned i = 0; i < n; ++i) buf[i] = (int8_t)((from + i) * 37u + 11u);
    return n;
}

static void streams(void)
{
    static Model m;
    m_init(&m);
    PghSpmiIo io;
    PghConfig cfg = m_config(PGH_MODE_ON);
    cfg.budget_ms = 60000;
    CHECK(init_with(&m, cfg, &io) == 0);
    static int8_t sent[200000];
    unsigned total = 0;
    CHECK(command(PGH_CMD_STREAM_BEGIN, 8000, 3600, pgh_ring_head(&core.ring)) == 0 && core.state == PGH_ST_STREAM_WAIT);
    /* 1.5 s at 8 kHz in 2 ms chunks (the iOS bridge's round) while the poll loop runs on its own schedule; the play starts at the 8 ms
     * prebuffer (64 samples), not before */
    uint64_t next_poll = m.now_us, next_push = m.now_us;
    unsigned max_cal = 0;
    while (total < 12000) {
        if (m.now_us >= next_push) {
            int8_t chunk[16];
            fill_pattern(chunk, 16, total);
            CHECK(pgh_ring_push(&core.ring, chunk, 16) == 16);
            memcpy(&sent[total], chunk, 16);
            total += 16;
            next_push += 2000;
        }
        if (m.now_us >= next_poll) {
            const unsigned before = pgh_ring_head(&core.ring) - core.ring.tail;
            const int waited = core.state == PGH_ST_STREAM_WAIT;
            const uint32_t ms = pgh_core_poll(&core);
            CHECK(ms > 0);
            CHECK(!waited || (core.state == PGH_ST_STREAM) == (before >= 64));
            next_poll = m.now_us + (uint64_t)ms * 1000u;
        }
        if (core.state == PGH_ST_STREAM && m.reg[PGH_CFG][PGH_CFG_CAL_EN] > max_cal) max_cal = m.reg[PGH_CFG][PGH_CFG_CAL_EN];
        m_advance(&m, 1000);
    }
    CHECK(core.state == PGH_ST_STREAM && (m.reg[PGH_PTN][PGH_PTN_FIFO_PLAY_RATE] & 15) == PGH_RATE_F_8KHZ);
    CHECK((m.reg[PGH_CFG][PGH_CFG_CAL_EN] & PGH_CAL_RC_CLK_MASK) == PGH_CAL_RC_CLK_DISABLED && max_cal == 0);
    /* three odd samples at the end: the 1-byte register takes them */
    int8_t tail[3];
    fill_pattern(tail, 3, total);
    CHECK(pgh_ring_push(&core.ring, tail, 3) == 3);
    memcpy(&sent[total], tail, 3);
    total += 3;
    CHECK(command(PGH_CMD_STREAM_END, 1, pgh_ring_head(&core.ring), 0) == 0);
    m_run(&m, &core, 5000000);
    CHECK(core.state == PGH_ST_IDLE && !m.playing);
    CHECK(m.out_n == total && !memcmp(m.out, sent, total) && m.overflow == 0 && m.max_fill <= 8192);
    CHECK(core.underruns == 0 && core.samples == total);
    CHECK((m.reg[PGH_PTN][PGH_PTN_FIFO_PLAY_RATE] & 15) == 0 && (m.reg[PGH_CFG][PGH_CFG_CAL_EN] & PGH_CAL_RC_CLK_MASK) == PGH_CAL_RC_CLK_AUTO);
    unsigned ones = 0;
    for (unsigned i = 0; i < m.log_n; ++i) {
        if (m.log[i].periph != PGH_PTN) continue;
        if (m.log[i].reg == PGH_PTN_FIFO_DIN_1B) ones++;
        if (m.log[i].reg == PGH_PTN_FIFO_DIN_0) CHECK(m.log[i].len == 4);
    }
    CHECK(ones == 3);
    CHECK(m_logged(&m, "[haptics] stop drained"));

    /* 44.1 kHz (176.4 samples per 4 ms poll: odd free space), written ahead in large blocks (the FIFO never overflows), then aborted */
    m.out_n = 0;
    const unsigned mark48 = m.log_n;
    CHECK(command(PGH_CMD_STREAM_BEGIN, 44100, 0, pgh_ring_head(&core.ring)) == 0);
    unsigned pushed = 0;
    for (int round = 0; round < 40; ++round) {
        int8_t block[4096];
        fill_pattern(block, sizeof block, pushed);
        pushed += pgh_ring_push(&core.ring, block, sizeof block);
        const uint32_t ms = pgh_core_poll(&core);
        CHECK(ms > 0 && m.overflow == 0 && m.fifo_count <= m.fifo_len);
        m_advance(&m, (uint64_t)ms * 1000u);
    }
    CHECK(core.state == PGH_ST_STREAM && m.max_fill <= 8192 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 72);   /* 0 mV: the cap */
    for (unsigned i = mark48; i < m.log_n; ++i)   /* the FIFO full: odd free space, still only 4-byte writes before the end */
        CHECK(!(m.log[i].periph == PGH_PTN && m.log[i].reg == PGH_PTN_FIFO_DIN_1B));
    CHECK(m.max_fill + 4 > m.fifo_len);   /* it was filled to the brim */
    CHECK(command(PGH_CMD_STREAM_END, 0, 0, 0) == 0 && core.state == PGH_ST_IDLE && !m.playing);
    CHECK(pgh_ring_head(&core.ring) == core.ring.tail && m.out_n < pushed);

    /* a producer that goes away: stopped after the FIFO runs dry for 100 ms */
    CHECK(command(PGH_CMD_STREAM_BEGIN, 16000, 2000, pgh_ring_head(&core.ring)) == 0);
    int8_t some[640];
    fill_pattern(some, sizeof some, 0);
    CHECK(pgh_ring_push(&core.ring, some, sizeof some) == sizeof some);
    const uint64_t t0 = m.now_us;
    m_run(&m, &core, 5000000);
    CHECK(core.state == PGH_ST_IDLE && core.underruns >= 1 && m.now_us - t0 < 400000 && m_logged(&m, "stop underrun"));

    /* samples pushed before the stream began are dropped; a begun stream without data is dropped after 500 ms */
    CHECK(pgh_ring_push(&core.ring, some, 100) == 100);
    const unsigned out_before = m.out_n;
    CHECK(command(PGH_CMD_STREAM_BEGIN, 8000, 2000, pgh_ring_head(&core.ring)) == 0 && core.ring.tail == pgh_ring_head(&core.ring));
    m_run(&m, &core, 5000000);
    CHECK(core.state == PGH_ST_IDLE && m.out_n == out_before);
    /* a short stream that ends before reaching the prebuffer still plays */
    CHECK(command(PGH_CMD_STREAM_BEGIN, 8000, 2000, pgh_ring_head(&core.ring)) == 0);
    CHECK(pgh_ring_push(&core.ring, some, 10) == 10 && command(PGH_CMD_STREAM_END, 1, pgh_ring_head(&core.ring), 0) == 0);
    m_run(&m, &core, 5000000);
    CHECK(core.state == PGH_ST_IDLE && m.out_n == out_before + 10);
    CHECK(command(PGH_CMD_STREAM_BEGIN, 12345, 2000, 0) == -22);
    /* the ring refuses what does not fit */
    static int8_t big[PGH_RING + 100];
    CHECK(pgh_ring_push(&core.ring, big, sizeof big) == PGH_RING);
    CHECK(m.bad_ch == 0 && m.overflow == 0);
}

static void faults(void)
{
    static Model m;
    PghSpmiIo io;
    /* a failing write while starting: the play is stopped (emergency SPMI_PLAY = 0) and the driver stays off */
    m_init(&m);
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    m.fail_writes_after = 3;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 500, 0) == -5 && core.state == PGH_ST_FAULT && core.stage == -9);
    CHECK(m.log[m.log_n - 1].reg == PGH_CFG_SPMI_PLAY && m.log[m.log_n - 1].data[0] == 0 && !m.playing);
    const unsigned n = m.ch_cmds;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 500, 0) == -5 && command(PGH_CMD_STOP, 0, 0, 0) == -5 && m.ch_cmds == n);
    CHECK(pgh_core_poll(&core) == 0 && m_logged(&m, "[haptics] FAULT write rc=-5"));
    /* the 13T's boot of four: the arbiter drops the VMAX write of the init (status 0x9): default the driver stays off until reboot; with
     * spmi_retry the write goes again and the init ends as ever, with the same registers */
    m_init(&m);
    m.fail_writes_after = 14;
    m.fail_status = 0x9u;
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == -5 && core.state == PGH_ST_FAULT);
    CHECK(m_logged(&m, "[haptics] FAULT write rc=-5 periph=0 reg=0x48 status=0x9"));
    m_init(&m);
    m.fail_writes_after = 14;
    m.fail_status = 0x9u;
    PghConfig retry = m_config(PGH_MODE_ON);
    retry.spmi_retry = 1;
    CHECK(init_with(&m, retry, &io) == 0 && core.state == PGH_ST_IDLE && core.stage == 3 && core.spmi.dropped == 1);
    CHECK(core.vmax_code == 25 && m.reg[PGH_CFG][PGH_CFG_VMAX] == 25 && m_logged(&m, "[haptics] ready fifo=8192") && m_logged(&m, " dropped=1"));
    /* without the option the ready line is what it was */
    m_init(&m);
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0 && !m_logged(&m, "dropped="));
    /* a timeout while streaming: bounded, stopped */
    m_init(&m);
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    CHECK(command(PGH_CMD_STREAM_BEGIN, 8000, 2000, pgh_ring_head(&core.ring)) == 0);
    int8_t data[2000] = {0};
    CHECK(pgh_ring_push(&core.ring, data, sizeof data) == sizeof data);
    CHECK(pgh_core_poll(&core) > 0 && core.state == PGH_ST_STREAM && m.playing);
    const uint64_t t0 = m.now_us;
    m.never_done = 1;
    CHECK(pgh_core_poll(&core) == 0 && core.state == PGH_ST_FAULT);
    CHECK(m.now_us - t0 < 20000);   /* 1000 x 1 us per transaction, then the emergency stop's 1000 */
    m.never_done = 0;
    /* a short circuit during a play */
    m_init(&m);
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    CHECK(command(PGH_CMD_CONSTANT, 1000, 1000, 0) == 0);
    m_advance(&m, 30000);
    m.reg[PGH_CFG][PGH_CFG_FAULT_STATUS] = PGH_FAULT_SC;
    m.sc_persistent = 1;
    m_run(&m, &core, 2000000);
    CHECK(core.state == PGH_ST_FAULT && !m.playing && m_logged(&m, "FAULT short circuit"));
    /* an auto-resonance fault stops this play and turns auto resonance off for the next ones */
    m_init(&m);
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    CHECK(command(PGH_CMD_CONSTANT, 1000, 1000, 0) == 0);
    m.reg[PGH_CFG][PGH_CFG_FAULT_STATUS] = PGH_FAULT_AUTO_RES;
    m_run(&m, &core, 2000000);
    CHECK(core.state == PGH_ST_IDLE && !m.playing && !core.autores_ok);
    CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 0) == 0 && (m.reg[PGH_CFG][PGH_CFG_AUTORES_CFG] & PGH_AUTORES_EN) == 0);
    m_run(&m, &core, 2000000);
    /* the owner changes after the init: the next write is refused, the emergency stop too (not ours), nothing reaches the channel */
    m_init(&m);
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    m.owner[A_CFG] = 3;
    const unsigned c0 = m.ch_cmds;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 0) == -1 && core.state == PGH_ST_FAULT && m.ch_cmds == c0);
    /* HPWR does not settle after a VMAX change: the module is toggled, the play refused, no fault */
    m_init(&m);
    CHECK(init_with(&m, m_config(PGH_MODE_ON), &io) == 0);
    m.hpwr_stuck = 1;
    const unsigned mark = m.log_n;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 3000) == -16 && core.state == PGH_ST_IDLE && !m.playing);
    const int off = m_find(&m, mark, 0, PGH_CFG_EN_CTL);
    CHECK(off >= 0 && m.log[off].data[0] == 0 && m.log[off + 1].reg == PGH_CFG_EN_CTL && m.log[off + 1].data[0] == 0x80);
    CHECK(m_find(&m, mark, 0, PGH_CFG_SPMI_PLAY) < 0 && core.hpwr_timeouts == 1 && m.reg[PGH_CFG][PGH_CFG_TRIG_PRIORITY] == 0);
    m.hpwr_stuck = 0;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 3000) == 0);
    m_run(&m, &core, 1000000);
    /* the boost enabled in open loop by someone else: refused, nothing written */
    m.reg[PGH_HBST][PGH_HBST_VREG_EN] = PGH_HBST_VREG_ON;
    m.reg[PGH_HBST][PGH_HBST_HW_CTRL_FOLLOW] = 0;
    const unsigned c1 = m.ch_cmds;
    CHECK(command(PGH_CMD_CONSTANT, 1000, 100, 0) == -16 && m.ch_cmds == c1 && core.busy == 1 && core.state == PGH_ST_IDLE);
    /* every wait the core made was bounded: the model's clock never jumped by more than the longest single wait */
    CHECK(m.bad_ch == 0);
}

int main(void)
{
    dry_run();
    refusals();
    init_sequence();
    direct_play();
    caps();
    effects();
    streams();
    faults();
    int32_t w[16];
    pgh_core_export(&core, w);
    CHECK(w[0] == 3 && w[1] == PGH_ST_IDLE);
    printf("PASS core: %u checks\n", checks);
    return 0;
}
