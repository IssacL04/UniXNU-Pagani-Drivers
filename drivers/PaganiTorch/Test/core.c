/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of TorchCore.c on the arbiter + flash module model: the configuration of the boot arguments, the dry run, refusals before any
 * write, the init, the exact on and off sequences, the current of every level, the caps, the auto-off, every fault bit, a failure at every
 * write and every read of the sequences, the log's ration. The model's LED tells what the hardware would have done. */
#include "model.h"

static Model m;

static void config(void)
{
    PgtConfig c;
    CHECK(pgt_config_make(&c, 0, 0, M_PPID, 3, 200, 0, 1) == 0 && pgt_config_make(&c, 3, 0, M_PPID, 3, 200, 0, 1) == 0);
    CHECK(pgt_config_make(0, 1, 0, M_PPID, 3, 200, 0, 1) == 0);
    CHECK(pgt_config_make(&c, 1, 0, M_PPID, 3, 200, 0, 1) == 1);
    CHECK(c.max_ma == 50 && c.hard_ma == 200 && c.auto_off_ms == 300000 && c.poll_ms == 100 && c.log == 1 && c.mode == 1 && c.chan_mask == 3);
    CHECK(pgt_config_make(&c, 2, 0, M_PPID, 3, 200, 120, 9) == 1 && c.max_ma == 120 && c.log == 2 && c.mode == 2);
    CHECK(pgt_config_make(&c, 1, 0, M_PPID, 3, 200, 201, 0) == 1 && c.max_ma == 200 && c.log == 0);
    CHECK(pgt_config_make(&c, 1, 0, M_PPID, 3, 200, 500000, 0) == 1 && c.max_ma == 200);
    CHECK(pgt_config_make(&c, 1, 0, M_PPID, 3, 200, 1, 0) == 1 && c.max_ma == 5);
    /* a board header cannot lift the hard cap */
    CHECK(pgt_config_make(&c, 1, 0, M_PPID, 3, 500, 400, 0) == 1 && c.max_ma == 200 && c.hard_ma == 200);
    CHECK(pgt_config_make(&c, 1, 0, M_PPID, 3, 100, 150, 0) == 1 && c.max_ma == 100 && c.hard_ma == 100);
    CHECK(pgt_test_ms(0) == 0 && pgt_test_ms(300) == 300 && pgt_test_ms(1000) == 1000 && pgt_test_ms(1001) == 1000 && pgt_test_ms(~0u) == 1000);
}

static void formulas(void)
{
    /* [L]: (mA x 1000 + 1) / 5000 - 1 */
    CHECK(pgt_ma_code(0) == 0 && pgt_ma_code(5) == 0 && pgt_ma_code(9) == 0 && pgt_ma_code(10) == 1 && pgt_ma_code(50) == 9);
    CHECK(pgt_ma_code(200) == 39 && pgt_ma_code(500) == 99 && pgt_ma_code(640) == 127 && pgt_ma_code(100000) == 127);
    CHECK(pgt_code_ma(9) == 50 && pgt_code_ma(39) == 200 && pgt_code_ma(0) == 5);
    /* every current: a code never drives more than asked */
    for (unsigned ma = 5; ma <= 640; ++ma) CHECK(pgt_code_ma(pgt_ma_code(ma)) <= ma && pgt_code_ma(pgt_ma_code(ma)) + 5 > ma);
    CHECK(pgt_level_ma(0, 50) == 0 && pgt_level_ma(1, 50) == 5 && pgt_level_ma(1000, 50) == 50 && pgt_level_ma(500, 50) == 25);
    CHECK(pgt_level_ma(5000, 50) == 50 && pgt_level_ma(1000, 200) == 200);
    for (unsigned cap = 5; cap <= 200; ++cap)
        for (unsigned level = 1; level <= 1000; ++level) {
            const unsigned ma = pgt_level_ma(level, cap);
            CHECK(ma >= 5 && ma <= cap);
        }
}

static void boot(PgtCore *t, const PgtConfig *c, const PgtOps *ops, const PgtSpmiIo *io)
{
    CHECK(pgt_core_init(t, c, ops, io) == 0 && t->state == PGT_ST_OFF && t->stage == 4);
}

static void dry_run(void)
{
    static PgtCore t;
    m_init(&m);
    m.reg[PGT_FLASH_EN_CTL] = PGT_MODULE_EN;   /* whatever it finds, it stays */
    m.reg[PGT_FLASH_CHAN_EN] = 3;
    m.reg[PGT_FLASH_CHAN_STROBE(0)] = m.reg[PGT_FLASH_CHAN_STROBE(1)] = PGT_STROBE_ACTIVE_HIGH;
    m_settle(&m);
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    const PgtConfig c = m_config(PGT_MODE_DRY, 0);
    CHECK(pgt_core_init(&t, &c, &ops, &io) == 1 && t.state == PGT_ST_DRY && t.stage == 5);
    CHECK(m.ch_cmds == 0 && m.log_n == 0 && t.spmi.writes == 0 && m.lit == 3);
    CHECK(m.map_calls == 1 && m.mapped == A_FLASH);    /* the one channel page, read once */
    CHECK(m_logged(&m, "[torch] spmi") && m_logged(&m, "flash=0x1ee:300:ee0:irq1:x1") && m_logged(&m, "[torch] id flash=18/07:r1.0.2.0"));
    CHECK(m_logged(&m, "verdict=ok") && m_logged(&m, "46=80") && m_logged(&m, "4e=03") && m_logged(&m, "[torch] chan apid=300"));
    CHECK(m_logged(&m, "dry run done: nothing written"));
    CHECK(pgt_core_set(&t, 1000, 0, "x") == -19 && pgt_core_poll(&t) == 0 && m.ch_cmds == 0);
    /* a dry run on the wrong thing reports it and does not touch the channel page */
    m_init(&m);
    m.reg[PGT_REG_SUBTYPE] = 0x04;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == 1 && m_logged(&m, "verdict=not-the-4-channel-module") && m.map_calls == 0);
    int32_t w[16];
    pgt_core_export(&t, w);
    CHECK(w[0] == 5 && w[1] == PGT_ST_DRY && w[11] == A_FLASH && w[12] == 0 && w[13] == 0x1804);
    /* what would refuse the first "on" shows in the dry run too */
    m_init(&m);
    m.reg[PGT_FLASH_EN_CTL] = 0x41;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == 1 && m_logged(&m, "dry run done: nothing written verdict=en-ctl-reserved-bits") && m.ch_cmds == 0);
}

static void refusals(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    const PgtConfig c = m_config(PGT_MODE_ON, 0);
    /* not the v7 arbiter */
    m_init(&m);
    m.version = 0x50000000u;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -19 && t.stage == -2 && t.state == PGT_ST_REFUSED && m.ch_cmds == 0 && m_logged(&m, "refused: arbiter"));
    /* no channel */
    m_init(&m);
    m.map[A_FLASH] = 0;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -19 && t.stage == -2 && m_logged(&m, "refused: no-channel") && m.obs_cmds == 0);
    /* identity */
    m_init(&m);
    m.reg[PGT_REG_TYPE] = 0x15;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -19 && m_logged(&m, "refused: not-a-flash-module") && m.map_calls == 0 && m.ch_cmds == 0);
    m_init(&m);
    m.reg[PGT_REG_SUBTYPE] = 0x03;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -19 && m_logged(&m, "refused: not-the-4-channel-module") && m.ch_cmds == 0);
    /* another EE's */
    m_init(&m);
    m.owner[A_FLASH] = 3;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -19 && m_logged(&m, "refused: channel-not-owned") && m.map_calls == 0 && m.ch_cmds == 0);
    /* bits the gate would refuse at the first "on" */
    m_init(&m);
    m.reg[PGT_FLASH_EN_CTL] = 0x01;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -19 && m_logged(&m, "refused: en-ctl-reserved-bits") && m.map_calls == 0 && m.ch_cmds == 0);
    /* reads fail, each one in turn */
    for (int n = 0; n < 8; ++n) {
        m_init(&m);
        m.fail_reads_after = n;
        CHECK(pgt_core_init(&t, &c, &ops, &io) < 0 && t.state == PGT_ST_REFUSED && m.ch_cmds == 0 && t.stage == -3);
    }
    /* the mapping fails */
    m_init(&m);
    m.chan_map_fail = 1;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -12 && t.stage == -4 && m_logged(&m, "refused: open") && m.ch_cmds == 0);
    /* the first write fails: fault, stopped */
    m_init(&m);
    m.fail_writes_after = 0;
    CHECK(pgt_core_init(&t, &c, &ops, &io) == -5 && t.state == PGT_ST_FAULT && m_logged(&m, "FAULT init-write"));
    CHECK(pgt_core_set(&t, 100, 0, "x") == -5);
    /* configurations */
    m_init(&m);
    PgtConfig bad = c;
    bad.mode = 3;
    CHECK(pgt_core_init(&t, &bad, &ops, &io) == -22 && t.stage == -1 && m.core_reads == 0);
    bad = c;
    bad.chan_mask = 0;
    CHECK(pgt_core_init(&t, &bad, &ops, &io) == -22);
    bad.chan_mask = 0x13;
    CHECK(pgt_core_init(&t, &bad, &ops, &io) == -22);
    bad = c;
    bad.hard_ma = 4;
    CHECK(pgt_core_init(&t, &bad, &ops, &io) == -22);
    CHECK(pgt_core_init(0, &c, &ops, &io) == -22 && pgt_core_init(&t, 0, &ops, &io) == -22 && pgt_core_init(&t, &c, 0, &io) == -22);
    CHECK(pgt_core_init(&t, &c, &ops, 0) == -22);
    CHECK(pgt_core_set(0, 1, 0, "x") == -22 && pgt_core_poll(0) == 0);
}

static void init_stops_what_it_finds(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    const PgtConfig c = m_config(PGT_MODE_ON, 0);
    m_init(&m);
    m.reg[PGT_FLASH_EN_CTL] = PGT_MODULE_EN;
    m.reg[PGT_FLASH_CHAN_EN] = 0xf;   /* left on by whatever ran before, other channels too */
    m.reg[PGT_FLASH_CHAN_STROBE(0)] = PGT_STROBE_ACTIVE_HIGH;
    m_settle(&m);
    CHECK(m.lit);
    boot(&t, &c, &ops, &io);
    CHECK(!m.lit && m.reg[PGT_FLASH_CHAN_EN] == 0 && m.reg[PGT_FLASH_EN_CTL] == 0 && m.log_n == 2);
    CHECK(m.log[0].reg == PGT_FLASH_CHAN_EN && m.log[1].reg == PGT_FLASH_EN_CTL);
    CHECK(t.boot_sts[0] == 0 && t.chan_en == 0xf && t.en_ctl == 0x80 && t.code_max == 9 && t.hard_code == 39 && t.spmi.itarget_max == 39);
}

/* The exact register writes of an "on" from cold and of an "off", the timing rules, the state. */
static void sequences(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    const PgtConfig c = m_config(PGT_MODE_ON, 0);
    m_init(&m);
    m.reg[PGT_FLASH_ITARGET(0)] = 0xff;   /* bit 7 outside the field stays */
    m.reg[PGT_FLASH_CHAN_STROBE(1)] |= 0x80;
    boot(&t, &c, &ops, &io);
    CHECK(m.log_n == 1 && m.log[0].reg == PGT_FLASH_CHAN_EN);    /* nothing to stop: one write proves the path */
    m.log_n = 0;
    CHECK(pgt_core_set(&t, 1000, 0, "test") == 0 && t.state == PGT_ST_ON && t.level == 1000 && t.ma == 50 && t.code == 9);
    static const struct {uint8_t reg, value;} on[] = {
        {PGT_FLASH_IRES, 3}, {PGT_FLASH_ITARGET(0), 0x80 | 9}, {PGT_FLASH_ITARGET(1), 9}, {PGT_FLASH_CHAN_TIMER(0), 0}, {PGT_FLASH_CHAN_TIMER(1), 0},
        {PGT_FLASH_EN_CTL, PGT_MODULE_EN}, {PGT_FLASH_CHAN_STROBE(0), PGT_STROBE_ACTIVE_HIGH}, {PGT_FLASH_CHAN_STROBE(1), 0x80 | PGT_STROBE_ACTIVE_HIGH},
        {PGT_FLASH_CHAN_EN, 3}};
    CHECK(m.log_n == sizeof on / sizeof on[0]);
    for (unsigned i = 0; i < m.log_n; ++i) CHECK(m.log[i].reg == on[i].reg && m.log[i].value == on[i].value);
    CHECK(m.lit == 3 && m.lit_ma_max == 50 && m.lit_mask_all == 3);
    CHECK(m_channel_ma(&m, 0) == 50 && m_channel_ma(&m, 1) == 50);
    /* no channel other than the board's is ever touched */
    for (unsigned i = 0; i < m.log_n; ++i) {
        CHECK(m.log[i].reg != PGT_FLASH_ITARGET(2) && m.log[i].reg != PGT_FLASH_ITARGET(3));
        CHECK(m.log[i].reg != PGT_FLASH_CHAN_STROBE(2) && m.log[i].reg != PGT_FLASH_CHAN_TIMER(3));
    }
    /* the same level again writes nothing */
    m.log_n = 0;
    CHECK(pgt_core_set(&t, 1000, 0, "again") == 0 && m.log_n == 0);
    /* a level with the same code writes nothing either */
    CHECK(pgt_core_set(&t, 999, 0, "same") == 0 && m.log_n == 0 && t.level == 999);
    /* another level: channels off first, then the sequence, channels on last */
    CHECK(pgt_core_set(&t, 500, 0, "half") == 0 && t.ma == 25 && t.code == 4 && m.lit == 3);
    CHECK(m.log[0].reg == PGT_FLASH_CHAN_EN && m.log[0].value == 0 && m.log[m.log_n - 1].reg == PGT_FLASH_CHAN_EN && m.log[m.log_n - 1].value == 3);
    CHECK(m.log[1].reg == PGT_FLASH_EN_CTL && m.log[1].value == 0);
    CHECK(m_channel_ma(&m, 0) == 25 && m.lit_ma_max == 50);
    /* off */
    m.log_n = 0;
    CHECK(pgt_core_set(&t, 0, 0, "off") == 0 && t.state == PGT_ST_OFF && t.level == 0 && t.ma == 0 && t.code == 0 && !m.lit);
    CHECK(m.log_n == 4 && m.log[0].reg == PGT_FLASH_CHAN_EN && m.log[0].value == 0 && m.log[1].reg == PGT_FLASH_EN_CTL && m.log[1].value == 0);
    CHECK(m.log[2].reg == PGT_FLASH_ITARGET(0) && m.log[2].value == 0x80 && m.log[3].reg == PGT_FLASH_ITARGET(1) && m.log[3].value == 0);
    /* off when off: nothing */
    m.log_n = 0;
    CHECK(pgt_core_set(&t, 0, 0, "off") == 0 && m.log_n == 0 && t.offs == 1);
    /* bad level */
    CHECK(pgt_core_set(&t, 1001, 0, "x") == -22 && t.state == PGT_ST_OFF && m.log_n == 0);
    /* on again from off */
    CHECK(pgt_core_set(&t, 200, 0, "low") == 0 && t.ma == 10 && t.code == 1 && m.lit == 3 && t.ons == 2);
    CHECK(pgt_core_poll(&t) == 100);
    CHECK(t.spmi.refused == 0 && t.spmi.failures == 0 && !t.spmi.closed);
    CHECK(m_logged(&m, "[torch] ready max_ma=50 code=9 hard_ma=200 hard_code=39 chans=0x3 off_s=300"));
    CHECK(m_logged(&m, "[torch] on level=1000 ma=50 code=9 by=test off_in_ms=300000"));
    CHECK(m_logged(&m, "[torch] off level=0 ma=0 code=0 by=off"));
    int32_t w[16];
    pgt_core_export(&t, w);
    CHECK(w[0] == 4 && w[1] == PGT_ST_ON && w[2] == 200 && w[3] == 10 && w[4] == 1 && w[5] == 2 && w[6] == 1 && w[7] == 0);
    CHECK(w[9] == (int32_t)t.spmi.writes && w[10] == 0 && w[11] == A_FLASH && w[13] == 0x1807 && w[14] == 0);
}

/* Every level of every cap: the hardware never sees more than the cap. */
static void caps(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    static const unsigned arg[] = {0, 5, 12, 50, 75, 199, 200, 201, 500, 100000};
    for (unsigned a = 0; a < sizeof arg / sizeof arg[0]; ++a) {
        const PgtConfig c = m_config(PGT_MODE_ON, arg[a]);
        const unsigned cap = c.max_ma;
        CHECK(cap >= 5 && cap <= 200);
        m_init(&m);
        boot(&t, &c, &ops, &io);
        for (unsigned level = 1; level <= 1000; level += 17) {
            CHECK(pgt_core_set(&t, level, 0, "cap") == 0 && t.ma <= cap && t.code <= t.code_max);
            CHECK(m.lit == 3 && m_channel_ma(&m, 0) == t.ma && m_channel_ma(&m, 1) == t.ma);
            CHECK(pgt_core_set(&t, 0, 0, "cap") == 0);
            m.log_n = 0;
        }
        CHECK(pgt_core_set(&t, 1000, 0, "cap") == 0 && t.ma == (cap / 5) * 5);
        CHECK(m.lit_ma_max <= cap && m.lit_ma_max <= 200 && t.spmi.refused == 0);
    }
    /* a configuration that bypasses pgt_config_make: the core's caps still hold */
    PgtConfig wild = m_config(PGT_MODE_ON, 0);
    wild.hard_ma = 500;
    wild.max_ma = 500;
    wild.auto_off_ms = 3600000;
    wild.poll_ms = 100000;
    m_init(&m);
    boot(&t, &wild, &ops, &io);
    CHECK(t.cfg.hard_ma == 200 && t.cfg.max_ma == 200 && t.cfg.auto_off_ms == 300000 && t.cfg.poll_ms == 1000 && t.hard_code == 39);
    CHECK(pgt_core_set(&t, 1000, 0, "cap") == 0 && m.lit_ma_max == 200);
    wild.poll_ms = 1;
    wild.auto_off_ms = 0;
    wild.max_ma = 1;
    m_init(&m);
    boot(&t, &wild, &ops, &io);
    CHECK(t.cfg.poll_ms == 10 && t.cfg.auto_off_ms == 300000 && t.cfg.max_ma == 5);
}

static void auto_off(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    const PgtConfig c = m_config(PGT_MODE_ON, 0);
    m_init(&m);
    m.now_us = 1000000;
    boot(&t, &c, &ops, &io);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == 0);
    uint32_t polls = 0;
    const uint64_t start = m.now_us;
    for (;;) {
        const uint32_t ms = pgt_core_poll(&t);
        if (!ms) break;
        CHECK(ms <= 100 && ++polls < 10000);
        m_advance(&m, (uint64_t)ms * 1000u);
    }
    CHECK(t.state == PGT_ST_OFF && !m.lit && t.auto_offs == 1 && t.offs == 1 && m.now_us - start >= 300000000u && m.now_us - start < 300100000u);
    CHECK(m.lit_longest <= 300100000u && m.lit_longest >= 300000000u && m_logged(&m, "by=auto-off"));
    /* the bring-up test: at most its hold, never past the limit */
    m_init(&m);
    boot(&t, &c, &ops, &io);
    CHECK(pgt_core_set(&t, 1000, 300, "test") == 0);
    m_run(&m, &t, 2000000);
    CHECK(t.state == PGT_ST_OFF && m.lit_longest >= 300000 && m.lit_longest <= 301000);
    m_init(&m);
    boot(&t, &c, &ops, &io);
    CHECK(pgt_core_set(&t, 1000, 0x7fffffffu, "test") == 0);
    CHECK(t.deadline_us == m.now_us + 300000000u);
    /* changing the level, or asking again, does not extend an "on" */
    m_init(&m);
    boot(&t, &c, &ops, &io);
    CHECK(pgt_core_set(&t, 500, 0, "t") == 0);
    const uint64_t deadline = t.deadline_us;
    m_advance(&m, 200000000);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == 0 && t.deadline_us == deadline);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == 0 && t.deadline_us == deadline);
    /* a hold can only shorten it */
    CHECK(pgt_core_set(&t, 1000, 50, "t") == 0 && t.deadline_us == m.now_us + 50000u);
    /* off and on again starts anew */
    CHECK(pgt_core_set(&t, 0, 0, "t") == 0 && pgt_core_set(&t, 1000, 0, "t") == 0 && t.deadline_us == m.now_us + 300000000u);
    /* a poll that is late still ends it at once */
    m_advance(&m, 400000000);
    CHECK(pgt_core_poll(&t) == 0 && !m.lit && t.state == PGT_ST_OFF);
}

/* Every fault bit stops the LED and keeps it off. */
static void faults(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    const PgtConfig c = m_config(PGT_MODE_ON, 200);
    static const struct {uint8_t s1, s2, s3; const char *why; int counts;} bit[] = {
        {0x01, 0, 0, "short-circuit", 1}, {0x04, 0, 0, "short-circuit", 1},
        {0x10, 0, 0, 0, 0}, {0x40, 0, 0, 0, 0},                  /* the channels the board does not use */
        {0x02, 0, 0, 0, 0},                                       /* bit 1: not a channel's short-circuit bit */
        {0, 0x01, 0, "vph-low", 1}, {0, 0x02, 0, "over-current", 1}, {0, 0x04, 0, "over-current", 1}, {0, 0x08, 0, "over-current", 1},
        {0, 0x10, 0, "over-temperature", 1}, {0, 0x20, 0, "over-temperature", 1}, {0, 0x40, 0, "over-temperature", 1},
        {0, 0x80, 0, 0, 0},                                       /* bit 7: outside the field */
        {0, 0, 0x01, "safety-timer", 1}, {0, 0, 0x04, "safety-timer", 1}, {0, 0, 0x10, 0, 0},
    };
    for (unsigned i = 0; i < sizeof bit / sizeof bit[0]; ++i) {
        m_init(&m);
        boot(&t, &c, &ops, &io);
        CHECK(pgt_core_set(&t, 1000, 0, "t") == 0 && m.lit == 3);
        m_advance(&m, 500000);
        m.fault_s1 = bit[i].s1;
        m.fault_s2 = bit[i].s2;
        m.fault_s3 = bit[i].s3;
        m.fault_at_us = m.now_us;
        const uint32_t ms = pgt_core_poll(&t);
        if (bit[i].counts) {
            char want[64];
            snprintf(want, sizeof want, "FAULT %s", bit[i].why);
            CHECK(ms == 0 && t.state == PGT_ST_FAULT && !m.lit && t.faults == 1 && m_logged(&m, want));
            CHECK(m.reg[PGT_FLASH_CHAN_EN] == 0 && m.reg[PGT_FLASH_EN_CTL] == 0);
            CHECK(pgt_core_set(&t, 1000, 0, "t") == -5 && pgt_core_set(&t, 0, 0, "t") == -5 && pgt_core_poll(&t) == 0 && !m.lit);
            int32_t w[16];
            pgt_core_export(&t, w);
            CHECK(w[1] == PGT_ST_FAULT && w[2] == 0 && w[3] == 0 && w[7] == 1);
            CHECK(w[8] == (int32_t)((uint32_t)bit[i].s1 << 16 | (uint32_t)bit[i].s2 << 8 | bit[i].s3));
        } else {
            CHECK(ms == 100 && t.state == PGT_ST_ON && m.lit == 3);
        }
    }
    /* a fault already there when the LED goes on: it never stays on */
    m_init(&m);
    boot(&t, &c, &ops, &io);
    m.fault_s2 = 0x20;
    m.fault_at_us = m.now_us ? m.now_us : 1;
    m_advance(&m, 10);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == -5 && t.state == PGT_ST_FAULT && !m.lit);
    /* the channel taken away while on: the gate refuses the off, the emergency stop refuses too; the arbiter would have denied them */
    m_init(&m);
    boot(&t, &c, &ops, &io);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == 0);
    m.owner[A_FLASH] = 3;
    CHECK(pgt_core_set(&t, 0, 0, "t") == -5 && t.state == PGT_ST_FAULT && t.spmi.refused == 1 && m_logged(&m, "FAULT off-write"));
}

/* A failure at every write and every read of the on and off sequences: never a lit LED afterwards, never more than the cap. */
static void sweep(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    const PgtConfig c = m_config(PGT_MODE_ON, 0);
    unsigned failed_on = 0;
    for (int n = 0; n < 16; ++n) {            /* the n-th write of the "on" fails */
        m_init(&m);
        boot(&t, &c, &ops, &io);
        m.fail_writes_after = n;
        const int rc = pgt_core_set(&t, 1000, 0, "t");
        if (m.fail_writes_after == -1) {
            failed_on++;
            CHECK(rc == -5 && t.state == PGT_ST_FAULT && !m.lit && m.lit_ma_max <= 50);
        } else {
            m.fail_writes_after = -1;
            CHECK(rc == 0 && t.state == PGT_ST_ON);
        }
    }
    CHECK(failed_on == 9);
    for (int n = 0; n < 8; ++n) {             /* the n-th write of the "off" fails */
        m_init(&m);
        boot(&t, &c, &ops, &io);
        CHECK(pgt_core_set(&t, 1000, 0, "t") == 0);
        m.fail_writes_after = n;
        const int rc = pgt_core_set(&t, 0, 0, "t");
        CHECK(!m.lit);                        /* the emergency stop covers a failed channel write */
        CHECK(n < 4 ? rc == -5 && t.state == PGT_ST_FAULT : rc == 0);
    }
    unsigned failed_reads = 0;
    for (int n = 0; n < 40; ++n) {            /* the n-th read of on + poll + off fails */
        m_init(&m);
        boot(&t, &c, &ops, &io);
        m.fail_reads_after = n;
        int rc = pgt_core_set(&t, 1000, 0, "t");
        if (!rc) rc = pgt_core_poll(&t) ? 0 : -5;
        if (!rc) rc = pgt_core_set(&t, 0, 0, "t");
        if (m.fail_reads_after == -1) {
            failed_reads++;
            CHECK(rc == -5 && t.state == PGT_ST_FAULT && !m.lit);
        } else {
            m.fail_reads_after = -1;
            CHECK(rc == 0);
        }
    }
    CHECK(failed_reads >= 20);
    /* a timeout on the bus */
    m_init(&m);
    boot(&t, &c, &ops, &io);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == 0);
    m.never_done = 1;
    CHECK(pgt_core_poll(&t) == 0 && t.state == PGT_ST_FAULT);
    /* every register that must not be left wrong is read back before the channels go on */
    static const uint8_t stuck[] = {PGT_FLASH_IRES, PGT_FLASH_ITARGET(0), PGT_FLASH_ITARGET(1), PGT_FLASH_EN_CTL, PGT_FLASH_CHAN_TIMER(0),
                                    PGT_FLASH_CHAN_TIMER(1), PGT_FLASH_CHAN_STROBE(0), PGT_FLASH_CHAN_STROBE(1)};
    for (unsigned i = 0; i < sizeof stuck; ++i) {
        m_init(&m);
        boot(&t, &c, &ops, &io);
        m.stuck_on = 1;
        m.stuck_reg = stuck[i];
        CHECK(pgt_core_set(&t, 1000, 0, "t") == -5 && t.state == PGT_ST_FAULT && !m.lit && m_logged(&m, "FAULT verify"));
        CHECK(m_find(&m, 1, PGT_FLASH_CHAN_EN) < 0 || m.log[m_find(&m, 1, PGT_FLASH_CHAN_EN)].value == 0);
    }
    /* an "off" the hardware ignored is found, and the emergency stop's module write still goes out */
    m_init(&m);
    boot(&t, &c, &ops, &io);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == 0);
    m.stuck_on = 1;
    m.stuck_reg = PGT_FLASH_CHAN_EN;
    CHECK(pgt_core_set(&t, 0, 0, "t") == -5 && t.state == PGT_ST_FAULT && m_logged(&m, "FAULT off-verify") && !m.lit);
    CHECK(m.log[m.log_n - 1].reg == PGT_FLASH_EN_CTL && m.log[m.log_n - 1].value == 0);
}

static void ration(void)
{
    static PgtCore t;
    const PgtOps ops = m_ops(&m);
    const PgtSpmiIo io = m_io(&m);
    PgtConfig c = m_config(PGT_MODE_ON, 0);
    /* log 1: the first 8 lines, then one per 5 minutes, at most 24 */
    c.log = 1;
    m_init(&m);
    boot(&t, &c, &ops, &io);
    const unsigned base = m.line_n;
    for (unsigned i = 0; i < 2000; ++i) {
        CHECK(pgt_core_set(&t, i % 2 ? 0 : 1000, 0, "t") == 0);
        m_advance(&m, 1000);
        m.log_n = 0;
    }
    CHECK(m.line_n - base == 8 && t.dropped == 1992);
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(pgt_core_set(&t, i % 2 ? 0 : 1000, 0, "t") == 0);
        m_advance(&m, 300000000);
    }
    CHECK(m.line_n - base == 24 && t.event_lines == 24);
    /* log 0: failures only */
    c.log = 0;
    m_init(&m);
    boot(&t, &c, &ops, &io);
    const unsigned quiet = m.line_n;
    CHECK(pgt_core_set(&t, 1000, 0, "t") == 0 && pgt_core_set(&t, 0, 0, "t") == 0 && m.line_n == quiet && t.dropped == 2);
    /* a fault line is never rationed away, and at most three are written */
    c.log = 0;
    m_init(&m);
    boot(&t, &c, &ops, &io);
    m.fault_s2 = 0x20;
    m.fault_at_us = 1;
    m_advance(&m, 10);
    CHECK(pgt_core_set(&t, 1000, 0, "t") == -5 && m_logged(&m, "FAULT over-temperature"));
}

int main(void)
{
    config();
    formulas();
    dry_run();
    refusals();
    init_stops_what_it_finds();
    sequences();
    caps();
    auto_off();
    faults();
    sweep();
    ration();
    printf("PASS core: %u checks\n", checks);
    return 0;
}
