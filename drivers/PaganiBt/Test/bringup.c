/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* BtBringup.c 对模型：SM8750 的 GCC 和 TLMM 一角、RPMh、PMIC 读回、BT_EN，SE 接 geni_model.h。检查顺序和数值，重点是安全规则：
 *   S7I 读不到一路都不投；电压只升不降；总投高功率模式；绝不 enable=0；投后读回不对就停；
 *   每条失败路径都停在 BT_EN 之前；BT_EN 低电平够 54 ms；分支没起来绝不碰 SE；勘察不写任何寄存器。 */
#include "../BtBringup.h"
#include "../BtUtil.h"
#include "geni_model.h"
#include "check.h"

#define MAX_VOTES 64
typedef struct {char resource[16]; uint32_t reg, value; uint64_t ms;} Vote;
typedef struct {uint16_t ppid; uint8_t status, enable, mode; uint16_t mv; uint8_t readable;} ModelRail;
static struct {
    uint32_t gcc[0x80000 / 4], tlmm[0x1c000 / 4];
    GeniModel se;
    Vote votes[MAX_VOTES];
    unsigned vote_count;
    ModelRail rails[PB_RAIL_COUNT];
    uint64_t us;
    uint32_t writes_gcc, writes_tlmm;
    int lock_held, lock_busy, lock_takes, vote_written_unlocked;
    int bt_en_level, bt_en_calls, bt_en_writes, bt_en_fail;
    uint64_t bt_en_low_at, bt_en_high_at;
    int sleep_clock, sleep_clock_writes;
    char fail_vote[16];
    int branch_stuck, branch_gated, rcg_stuck, s7i_drop_after, s7i_off_after, s7i_stays_low, se_reads_before_clock;
    int unreadable_after, bad_access;
    int quiet, quiet_calls, loud_rail_writes;   /* loud_rail_writes：写者没持住时的投票、睡眠时钟脚和 BT_EN */
    uint64_t quiet_us, last_line_us, se_first_read_us;
    int se_read_loud;                                  /* 第一次读 SE 时写者没持住，或者上一行还没落盘 */
    char log[64][200];
    unsigned log_count;
} m;

static uint32_t *gcc_reg(uint32_t off) {return &m.gcc[off / 4];}
static void gcc_step(void)
{
    /* 投上了的分支 CLK_OFF 清零，除非测试让它卡住 */
    const uint32_t vote = *gcc_reg(PbGccVote);
    static const struct {uint32_t halt, bit;} branches[5] = {{PbHaltMAhb, PbVoteMAhb}, {PbHaltSAhb, PbVoteSAhb}, {PbHaltCore, PbVoteCore},
                                                              {PbHaltCore2x, PbVoteCore2x}, {PbHaltSe6, PbVoteSe6}};
    for (unsigned i = 0; i < 5; ++i) {
        const int on = (vote & branches[i].bit) && !(m.branch_stuck & (1 << i));
        *gcc_reg(branches[i].halt) = (on ? 0 : PB_CBCR_OFF) | (m.branch_gated & (1 << i) ? PB_CBCR_HW_CTL | PB_CBCR_OFF : 0);
    }
}
static int all_clocks_on(void)
{
    static const uint32_t halts[5] = {PbHaltMAhb, PbHaltSAhb, PbHaltCore, PbHaltCore2x, PbHaltSe6};
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t v = *gcc_reg(halts[i]);
        if ((v & PB_CBCR_OFF) && !(v & PB_CBCR_HW_CTL)) return 0;
    }
    return 1;
}
static uint32_t hw_read(void *c, unsigned win, uint32_t off)
{
    (void)c;
    m.us += 1;
    if (win >= PbWinCount || (off & 3) || off + 4u > kPbWinBytes[win]) {
        m.bad_access++;
        return 0xffffffffu;
    }
    if (win == PbWinGcc) {
        gcc_step();
        return *gcc_reg(off);
    }
    if (win == PbWinTlmm) return m.tlmm[off / 4];
    if (!all_clocks_on()) m.se_reads_before_clock++;
    if (!m.se_first_read_us) {
        m.se_first_read_us = m.us;
        /* 3.5 s 给上一行落盘，1 s 给写者手里的一轮收尾 */
        if (!m.quiet || m.us - m.quiet_us < 1000000 || m.us - m.last_line_us < 4500000) m.se_read_loud = 1;
    }
    m.se.us = m.us;
    const uint32_t v = gm_read(&m.se, off);
    m.us = m.se.us;
    return v;
}
static void hw_write(void *c, unsigned win, uint32_t off, uint32_t value)
{
    (void)c;
    m.us += 1;
    if (win >= PbWinCount || (off & 3) || off + 4u > kPbWinBytes[win]) {
        m.bad_access++;
        return;
    }
    if (win == PbWinGcc) {
        m.writes_gcc++;
        if (off == PbGccVote && !m.lock_held) m.vote_written_unlocked++;
        *gcc_reg(off) = value;
        if (off == PbRcg && (value & PbRcgUpdate) && !m.rcg_stuck) {
            *gcc_reg(off) = value & ~(uint32_t)PbRcgUpdate;
            /* RCG 生效：SE 的输入时钟 */
            const uint32_t cfg = *gcc_reg(PbRcg + PbRcgCfg), m_v = *gcc_reg(PbRcg + PbRcgM), n_raw = *gcc_reg(PbRcg + PbRcgN);
            const uint32_t n = ((~n_raw) & 0xffffu) + m_v;
            const uint32_t src = ((cfg >> 8) & 7u) == 1 ? 600000000u : 300000000u;
            m.se.rcg_hz = (uint32_t)((uint64_t)src * 2 / ((cfg & 0x1fu) + 1) * m_v / n);
        }
    } else if (win == PbWinTlmm) {
        m.writes_tlmm++;
        m.tlmm[off / 4] = value;
    } else {
        if (!all_clocks_on()) m.se_reads_before_clock++;
        m.se.us = m.us;
        gm_write(&m.se, off, value);
        m.us = m.se.us;
    }
}
static void hw_udelay(void *c, unsigned us)
{
    (void)c;
    m.us += us;
    gm_run(&m.se, m.us);
}
static void hw_msleep(void *c, unsigned ms) {hw_udelay(c, ms * 1000u);}
static uint64_t hw_now(void *c) {(void)c; return m.us / 1000;}
static int hw_lock(void *c, int take)
{
    (void)c;
    if (take) {
        if (m.lock_busy) return -16;
        m.lock_held = 1;
        m.lock_takes++;
    } else {
        m.lock_held = 0;
    }
    return 0;
}
static ModelRail *rail_of(const char *resource)
{
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) if (pb_streq(kPbRails[i].resource, resource)) return &m.rails[i];
    return NULL;
}
static int hw_vrm(void *c, const char *resource, uint32_t reg, uint32_t value, uint32_t *address)
{
    (void)c;
    if (!m.quiet) m.loud_rail_writes++;
    CHECK(m.vote_count < MAX_VOTES);
    Vote *v = &m.votes[m.vote_count++];
    strncpy(v->resource, resource, sizeof v->resource - 1);
    v->reg = reg;
    v->value = value;
    v->ms = m.us / 1000;
    if (address) *address = 0x30000;
    if (m.fail_vote[0] && pb_streq(m.fail_vote, resource)) return -110;
    ModelRail *r = rail_of(resource);
    if (r) {
        if (reg == PbVrmEnable) r->enable = value ? 0x80 : 0;
        if (reg == PbVrmVoltage && !(pb_streq(resource, PB_RAIL_S7I) && m.s7i_stays_low)) r->mv = (uint16_t)value;
        if (reg == PbVrmMode) r->mode = (uint8_t)value;
    }
    if (pb_streq(resource, PB_RAIL_S7I)) {
        if (m.s7i_drop_after && reg == PbVrmMode) r->mv = (uint16_t)(r->mv - 50);
        if (m.s7i_off_after && reg == PbVrmMode) r->enable = 0;
        if (m.unreadable_after && reg == PbVrmMode) r->readable = 0;
    }
    return 0;
}
static int hw_pmic(void *c, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
{
    (void)c;
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) {
        const ModelRail *r = &m.rails[i];
        if (r->ppid != ppid) continue;
        if (!r->readable) return -5;
        *status = r->status;
        *enable = r->enable;
        *mode = r->mode;
        *mv = r->mv;
        return 0;
    }
    return -19;
}
static int hw_sleep_clock(void *c, int write)
{
    (void)c;
    if (write) m.sleep_clock_writes++;
    if (write && !m.quiet) m.loud_rail_writes++;
    return m.sleep_clock;
}
static int hw_bt_en(void *c, int level, PbGpioState *s)
{
    (void)c;
    memset(s, 0, sizeof *s);
    m.bt_en_calls++;
    if (level < 0) return 0;
    m.bt_en_writes++;
    if (!m.quiet) m.loud_rail_writes++;
    if (m.bt_en_fail) return -5;
    if (!level) m.bt_en_low_at = m.us;
    else m.bt_en_high_at = m.us;
    m.bt_en_level = level;
    return 0;
}
static void hw_log(void *c, const char *line)
{
    (void)c;
    if (m.log_count < 64) strncpy(m.log[m.log_count++], line, 199);
    CHECK(strlen(line) < 190);                         /* 一条 U 记录放得下 */
    m.last_line_us = m.us;
}
static void hw_quiet(void *c, unsigned on)
{
    (void)c;
    CHECK((int)on == !m.quiet);                        /* 持一次放一次 */
    m.quiet = (int)on;
    m.quiet_us = m.us;
    m.quiet_calls++;
}
static const PbHw kHw = {NULL, hw_read, hw_write, hw_udelay, hw_msleep, hw_now, hw_lock, hw_vrm, hw_pmic, hw_sleep_clock, hw_bt_en, hw_log, hw_quiet};

static PbBringup b;

static void reset_model(void)
{
    memset(&m, 0, sizeof m);
    gm_init(&m.se, 64);
    static const uint16_t mv_now[PB_RAIL_COUNT] = {1800, 1200, 0, 500, 500, 1860, 1224};
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) {
        m.rails[i].ppid = kPbRails[i].ppid;
        m.rails[i].status = 0x80;
        m.rails[i].enable = i < 2 ? 0x80 : 0;
        m.rails[i].enable = i == 6 ? 0x80 : m.rails[i].enable;   /* S7I 带 UFS，开着 */
        m.rails[i].mode = 5;
        m.rails[i].mv = mv_now[i];
        m.rails[i].readable = 1;
    }
    m.sleep_clock = 1;
    *gcc_reg(PbRcg) = 0;
    /* SE 的 FIFO 开在 TLMM 切换之后才有对端，这里只测到 301 */
    memset(&b, 0, sizeof b);
    b.hw = &kHw;
}
static const Vote *find_vote(const char *resource, uint32_t reg)
{
    for (unsigned i = 0; i < m.vote_count; ++i) if (pb_streq(m.votes[i].resource, resource) && m.votes[i].reg == reg) return &m.votes[i];
    return NULL;
}
static int log_has(const char *text)
{
    for (unsigned i = 0; i < m.log_count; ++i) if (strstr(m.log[i], text)) return 1;
    return 0;
}
static unsigned s7i_index(void)
{
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) if (pb_streq(kPbRails[i].resource, PB_RAIL_S7I)) return i;
    return 0;
}

static void survey(void)
{
    reset_model();
    CHECK(pb_survey(&b) == 0);
    CHECK(b.step == PbStepSurvey);
    CHECK(m.writes_gcc == 0 && m.writes_tlmm == 0 && m.se.writes == 0 && m.vote_count == 0);
    CHECK(m.sleep_clock_writes == 0 && m.bt_en_writes == 0);
    CHECK(m.se_reads_before_clock == 0);               /* 勘察不碰 SE */
    CHECK(log_has("[bt] survey") && log_has("[bt] pins") && log_has("rail smpi7 now"));
    /* 缺回调时什么都不做 */
    PbHw broken = kHw;
    broken.pmic = NULL;
    b.hw = &broken;
    CHECK(pb_survey(&b) == PbErrArgument);
}

static void clocks(void)
{
    reset_model();
    *gcc_reg(PbGccVote) = 0x10000;                     /* 别的位 */
    CHECK(pb_clocks(&b) == 0);
    CHECK(b.step == PbStepClocks);
    CHECK(*gcc_reg(PbGccVote) == (0x10000u | PbVoteBits));   /* 别的位留着 */
    CHECK(PbVoteBits == 0x40f);                        /* ClockDxe：core 1、s_ahb 2、m_ahb 4、core_2x 8、s6 0x400 */
    CHECK(m.lock_takes == 1 && !m.lock_held && !m.vote_written_unlocked);
    CHECK(!m.se_reads_before_clock);
    CHECK(b.info.proto == PB_PROTO_UART && b.info.version == 0x66);
    CHECK(!m.se.writes);                               /* 探测只读 */
    CHECK(m.se_first_read_us && !m.se_read_loud);      /* 第一次读 SE：上一行落盘了，写者持住了 */
    CHECK(!m.quiet && m.quiet_calls == 2);             /* 读完放开，这一步的日志才出得去 */
    /* 已经投过：不写 */
    reset_model();
    *gcc_reg(PbGccVote) = PbVoteBits;
    CHECK(pb_clocks(&b) == 0);
    CHECK(m.writes_gcc == 0);
    /* 锁拿不到：不写，不碰 SE */
    reset_model();
    m.lock_busy = 1;
    CHECK(pb_clocks(&b) == PbErrLock && b.step == -PbStepClocks && !m.writes_gcc && !m.se_reads_before_clock);
    CHECK(!m.quiet_calls);
    /* 任何一个分支没起来：绝不读 SE，读没时钟的块会挂总线 */
    for (int i = 0; i < 5; ++i) {
        reset_model();
        m.branch_stuck = 1 << i;
        CHECK(pb_clocks(&b) == PbErrBranch);
        CHECK(!m.se_reads_before_clock && !m.se.writes && !m.quiet_calls);
    }
    /* 硬件门控的分支空闲时可以读到 CLK_OFF，不算没起来 */
    reset_model();
    m.branch_gated = 3;
    CHECK(pb_clocks(&b) == 0);
    /* 不是 UART 固件 */
    reset_model();
    m.se.regs[PB_SE_FW_REVISION / 4] = 3u << 8;
    CHECK(pb_clocks(&b) == PbErrGeni);
    CHECK(!m.quiet);                                   /* 失败也放开 */
}

static void power_ok(void)
{
    reset_model();
    CHECK(pb_power(&b) == 0);
    CHECK(b.step == PbStepPower);
    /* 顺序：cnss2 的 order */
    unsigned previous = 0;
    for (unsigned i = 0; i < m.vote_count; ++i) {
        for (unsigned r = 0; r < PB_RAIL_COUNT; ++r) {
            if (!pb_streq(m.votes[i].resource, kPbRails[r].resource)) continue;
            CHECK(kPbRails[r].order >= previous);
            previous = kPbRails[r].order;
        }
    }
    /* 每路先投模式 7；没有任何一条 enable=0 */
    for (unsigned r = 0; r < PB_RAIL_COUNT; ++r) CHECK(find_vote(kPbRails[r].resource, PbVrmMode) && find_vote(kPbRails[r].resource, PbVrmMode)->value == 7);
    for (unsigned i = 0; i < m.vote_count; ++i) if (m.votes[i].reg == PbVrmEnable) CHECK(m.votes[i].value == 1);
    /* S7I：1224 → 1312，已经开着不再投使能 */
    const Vote *v = find_vote(PB_RAIL_S7I, PbVrmVoltage);
    CHECK(v && v->value == 1312);
    CHECK(!find_vote(PB_RAIL_S7I, PbVrmEnable));
    CHECK(m.rails[s7i_index()].mode == 7);
    /* 关着的轨道投使能 */
    CHECK(find_vote("smpd4", PbVrmEnable) && find_vote("smpd4", PbVrmVoltage)->value == 876);
    /* rfclka1：+4 写 1，在轨道之后 */
    v = find_vote(PB_RF_CLOCK, PbRfClockEnable);
    CHECK(v && v->value == 1);
    CHECK(pb_streq(m.votes[m.vote_count - 1].resource, PB_RF_CLOCK));
    /* 这一步绝不碰 BT_EN */
    CHECK(!m.bt_en_writes);
    CHECK(m.sleep_clock_writes == 1);
    /* S7I 带着 UFS：整段写者持住，走完放开 */
    CHECK(!m.loud_rail_writes && !m.quiet && m.quiet_calls == 2);

    /* 已经比要的高：沿用，绝不往下投 */
    reset_model();
    m.rails[s7i_index()].mv = 1330;
    m.rails[3].mv = 900;
    CHECK(pb_power(&b) == 0);
    CHECK(!find_vote(PB_RAIL_S7I, PbVrmVoltage));
    CHECK(find_vote("smpd4", PbVrmVoltage)->value == 900);
    CHECK(m.rails[s7i_index()].mv == 1330);
    /* 超过范围：不投电压 */
    reset_model();
    m.rails[3].mv = 1100;
    CHECK(pb_power(&b) == 0);
    CHECK(!find_vote("smpd4", PbVrmVoltage));
    /* S7I 高于范围也不投电压，模式照投 */
    reset_model();
    m.rails[s7i_index()].mv = 1400;
    CHECK(pb_power(&b) == 0);
    CHECK(!find_vote(PB_RAIL_S7I, PbVrmVoltage) && find_vote(PB_RAIL_S7I, PbVrmMode));
    /* S7I 没开（不该发生）：只投开，不投关 */
    reset_model();
    m.rails[s7i_index()].enable = 0;
    CHECK(pb_power(&b) == 0);
    CHECK(find_vote(PB_RAIL_S7I, PbVrmEnable)->value == 1);
    /* 其他轨道读不到时仍投，和 Wi-Fi 同一条规则 */
    reset_model();
    m.rails[0].readable = 0;
    CHECK(pb_power(&b) == 0);
    CHECK(find_vote("ldof3", PbVrmVoltage)->value == 1800);
    /* 睡眠时钟脚别的 EE 占着：记录，继续 */
    reset_model();
    m.sleep_clock = PbSpmiNotOwner;
    CHECK(pb_power(&b) == 0);
    CHECK(log_has("WARN sleep clock"));
    /* 没有睡眠时钟回调 */
    reset_model();
    PbHw no_clock = kHw;
    no_clock.sleep_clock = NULL;
    b.hw = &no_clock;
    CHECK(pb_power(&b) == 0);
}

static void power_failures(void)
{
    /* 读不到 S7I：一路都不投 */
    reset_model();
    m.rails[s7i_index()].readable = 0;
    CHECK(pb_power(&b) == PbErrRailRead);
    CHECK(b.step == -PbStepPower && m.vote_count == 0);
    /* 投票失败：停 */
    for (unsigned i = 0; i < PB_RAIL_COUNT; ++i) {
        reset_model();
        strncpy(m.fail_vote, kPbRails[i].resource, sizeof m.fail_vote - 1);
        CHECK(pb_power(&b) == PbErrRail);
        CHECK(!find_vote(PB_RF_CLOCK, PbRfClockEnable));
    }
    reset_model();
    strncpy(m.fail_vote, PB_RF_CLOCK, sizeof m.fail_vote - 1);
    CHECK(pb_power(&b) == PbErrRail);
    /* 投后 S7I 掉了：停 */
    reset_model();
    m.s7i_drop_after = 1;
    m.s7i_stays_low = 1;
    CHECK(pb_power(&b) == PbErrRail);
    CHECK(!find_vote(PB_RF_CLOCK, PbRfClockEnable));
    /* 投前已经高过所需，投后比原来低，哪怕还够：也算掉了 */
    reset_model();
    m.rails[s7i_index()].mv = 1335;
    m.s7i_drop_after = 1;
    CHECK(pb_power(&b) == PbErrRail);
    CHECK(log_has("S7I dropped"));
    /* 投后 S7I 被关了 */
    reset_model();
    m.s7i_off_after = 1;
    CHECK(pb_power(&b) == PbErrRail);
    /* 投后读不回来 */
    reset_model();
    m.unreadable_after = 1;
    CHECK(pb_power(&b) == PbErrRailRead);
    /* 投了电压却没升上去：低于芯片所需，停 */
    reset_model();
    m.s7i_stays_low = 1;
    CHECK(pb_power(&b) == PbErrRail);
    CHECK(log_has("S7I below"));
    /* 没有 RPMh */
    reset_model();
    PbHw no_rpmh = kHw;
    no_rpmh.vrm = NULL;
    b.hw = &no_rpmh;
    CHECK(pb_power(&b) == PbErrRail);
}

/* 上面所有的失败都必须没动 BT_EN：跑到 202 的调用方（内核线程）在 pb_power 失败时不调 pb_bt_enable，这里走一遍主流程确认。 */
static int sequence(void)
{
    int rc = pb_survey(&b);
    if (!rc) rc = pb_clocks(&b);
    if (!rc) rc = pb_power(&b);
    if (!rc) rc = pb_bt_enable(&b);
    if (!rc) rc = pb_uart(&b);
    return rc;
}
static void stops_before_bt_en(void)
{
    reset_model();
    m.rails[s7i_index()].readable = 0;
    CHECK(sequence() == PbErrRailRead && !m.bt_en_writes);
    reset_model();
    m.s7i_stays_low = 1;
    CHECK(sequence() == PbErrRail && !m.bt_en_writes);
    reset_model();
    m.branch_stuck = 1;
    CHECK(sequence() == PbErrBranch && !m.bt_en_writes && !m.vote_count);
    reset_model();
    m.lock_busy = 1;
    CHECK(sequence() == PbErrLock && !m.bt_en_writes);
    reset_model();
    m.se.regs[PB_SE_IF_DISABLE / 4] = 1;
    CHECK(sequence() == PbErrGeni && !m.bt_en_writes && !m.vote_count);
    /* stop 选项 */
    reset_model();
    b.stop = PbStepPower;
    CHECK(sequence() == PbErrStopped && b.step == PbStepPower && !m.bt_en_writes);
    reset_model();
    b.stop = PbStepClocks;
    CHECK(sequence() == PbErrStopped && !m.vote_count);
    reset_model();
    b.stop = PbStepSurvey;
    CHECK(sequence() == PbErrStopped && !m.writes_gcc);
}

static void bt_enable(void)
{
    reset_model();
    CHECK(pb_bt_enable(&b) == 0);
    /* 芯片的负载落到 S7I 上这一段写者持住，走完放开 */
    CHECK(!m.loud_rail_writes && !m.quiet && m.quiet_calls == 2);
    CHECK(b.step == PbStepBtEn);
    CHECK(m.bt_en_writes == 2 && m.bt_en_level == 1);
    CHECK(m.bt_en_high_at - m.bt_en_low_at >= PbBtEnLowMs * 1000u);
    CHECK(m.us - m.bt_en_high_at >= PbBtEnHighMs * 1000u);
    /* 拉低失败：不拉高 */
    reset_model();
    m.bt_en_fail = 1;
    CHECK(pb_bt_enable(&b) == PbErrBtEn);
    CHECK(!m.quiet);                                   /* 失败也放开 */
    CHECK(m.bt_en_writes == 1);
}

static void uart(void)
{
    reset_model();
    b.stop = 0;
    CHECK(sequence() == 0);
    CHECK(b.step == PbStepUart);
    /* RCG：ClockDxe 的 7.3728 MHz，源 6，M 384，N 15625 */
    CHECK(*gcc_reg(PbRcg + PbRcgM) == 384);
    CHECK(*gcc_reg(PbRcg + PbRcgN) == (~(15625u - 384u) & 0xffffu));
    CHECK(*gcc_reg(PbRcg + PbRcgD) == (~15625u & 0xffffu));
    CHECK(*gcc_reg(PbRcg + PbRcgCfg) == (2u << 12 | 6u << 8 | 1u));
    CHECK(m.se.rcg_hz == 7372800 && gm_baud(&m.se) == 115200);
    /* TLMM：功能 1，2 mA，CTS 无上下拉，RTS 下拉，TX 上拉，RX 无 */
    CHECK(m.tlmm[PbPinCts * PbPinStride / 4] == (1u << 2));
    CHECK(m.tlmm[PbPinRts * PbPinStride / 4] == (1u | 1u << 2));
    CHECK(m.tlmm[PbPinTx * PbPinStride / 4] == (3u | 1u << 2));
    CHECK(m.tlmm[PbPinRx * PbPinStride / 4] == (1u << 2));
    /* 流控打开，接收在跑 */
    CHECK(m.se.regs[PB_SE_UART_MANUAL_RFR / 4] == 0 && m.se.regs[PB_SE_UART_TX_TRANS_CFG / 4] == 0);
    CHECK(m.se.s_active);
    CHECK(m.se.regs[PB_SE_UART_LOOPBACK / 4] == 0);
    /* 内部回环过了：只记录 */
    CHECK(b.loopback == 1 && log_has("[bt] loopback") && log_has("ok=1"));
    CHECK(!m.se.errors && !m.bad_access);

    /* RCG 不更新：配置失败，SE 不配 */
    reset_model();
    m.rcg_stuck = 1;
    CHECK(sequence() == PbErrRcg);
    CHECK(!m.se.regs[PB_SE_TX_PACK0 / 4]);

    /* 动态频率表开着：不写 CFG，用 CLK_SEL 选级 */
    reset_model();
    *gcc_reg(PbRcg + PbRcgDfs) = 1;
    for (unsigned i = 0; i < PbRcgLevels; ++i) *gcc_reg(PbRcg + PbRcgPerf + 4 * i) = 0x001;
    *gcc_reg(PbRcg + PbRcgPerf + 4 * 3) = 2u << 12 | 6u << 8 | 1u;
    *gcc_reg(PbRcg + PbRcgPerfM + 4 * 3) = 384;
    *gcc_reg(PbRcg + PbRcgPerfN + 4 * 3) = ~(15625u - 384u) & 0xffffu;
    m.se.rcg_hz = 7372800;
    CHECK(sequence() == 0);
    CHECK(!*gcc_reg(PbRcg + PbRcgCfg) && b.level == 3 && m.se.regs[PB_SE_CLK_SEL / 4] == 3);
    /* 表里没有这个频率：失败，不乱选 */
    reset_model();
    *gcc_reg(PbRcg + PbRcgDfs) = 1;
    CHECK(sequence() == PbErrRcg);

    /* 换速：8 Mbps 走 128 MHz，SE 分频 1 */
    reset_model();
    CHECK(sequence() == 0);
    CHECK(pb_uart_baud(&b, 8000000) == 0);
    CHECK(*gcc_reg(PbRcg + PbRcgCfg) == (2u << 12 | 1u << 8 | 1u));
    CHECK(*gcc_reg(PbRcg + PbRcgM) == 16);
    CHECK(m.se.rcg_hz == 128000000 && gm_baud(&m.se) == 8000000);
    CHECK(m.se.s_cancels >= 1 && m.se.s_active);
    CHECK(pb_uart_baud(&b, 3000000) == 0);
    CHECK(gm_baud(&m.se) == 3000000);
    CHECK(pb_uart_baud(&b, 12345) == PbErrArgument);
}

static void tables(void)
{
    /* 事实本身：测试别的地方都跟着常量走，常量写错只有这里抓得到。来源见 PaganiBtBoard.h */
    CHECK(PbGccVote == 0x52010 && PbRcg == 0x1e77c);                                                /* ClockDxe + 0x10000 */
    CHECK(PbHaltMAhb == 0x23298 && PbHaltSAhb == 0x2329c && PbHaltCore == 0x232a0 && PbHaltCore2x == 0x232b4 && PbHaltSe6 == 0x1e76c);
    CHECK(PbVoteMAhb == 4 && PbVoteSAhb == 2 && PbVoteCore == 1 && PbVoteCore2x == 8 && PbVoteSe6 == 0x400);
    CHECK(PB_SE_BASE == 0x898000 && PB_SE_BYTES == 0x4000);                                         /* 设备树 reg */
    CHECK(PbPinCts == 24 && PbPinRts == 25 && PbPinTx == 26 && PbPinRx == 27 && PbPinFunction == 1 && PbPinDriveMa == 2);
    CHECK(PB_BT_EN_PPID == 0x58a && PbBtEnLowMs == 54 && PbBtEnHighMs == 54 && PbBtEnVin == 1);
    CHECK(PB_SLEEP_CLOCK_PPID == 0x0ba);
    CHECK(!strcmp(PB_RF_CLOCK, "rfclka1") && PbRfClockEnable == 4);
    CHECK(PbVrmVoltage == 0 && PbVrmEnable == 4 && PbVrmMode == 8 && PbVrmModeHigh == 7);
    CHECK(PB_SOC_VERSION == 0x40210200u && PB_PRODUCT_ID == 0x21);
    CHECK(kPbRates[0].code == 0x00 && kPbRates[1].code == 0x0e && kPbRates[2].code == 0x11 && kPbRates[3].code == 0x15);   /* SetBaudRate */
    /* GENI 寄存器偏移：主线 geni-se.h，和 PaganiSpeaker GeniI2c.h 同一份 */
    CHECK(PB_SE_FW_REVISION == 0x68 && PB_SE_M_CMD0 == 0x600 && PB_SE_S_CMD0 == 0x630 && PB_SE_TX_FIFO == 0x700 && PB_SE_RX_FIFO == 0x780);
    CHECK(PB_SE_TX_FIFO_STATUS == 0x800 && PB_SE_RX_FIFO_STATUS == 0x804 && PB_SE_IOS == 0x908 && PB_SE_HW_PARAM_0 == 0xe24);
    /* 速率表：RCG 频率 = 波特率 × 16 × 分频，= 源 × M / N */
    for (unsigned i = 0; i < PB_RATE_COUNT; ++i) {
        const PbRate *r = &kPbRates[i];
        CHECK(r->rcgHz == r->baud * 16u * r->seDiv);
        const uint64_t source = r->source == 1 ? 600000000ull : 300000000ull;
        CHECK(source * r->m / r->n == r->rcgHz);
        CHECK(r->div2x == 2);
        uint32_t mv, nv, dv, cfg;
        pb_rcg_values(r, &mv, &nv, &dv, &cfg);
        CHECK(mv == r->m && ((~nv & 0xffffu) + r->m) == r->n && (~dv & 0xffffu) == r->n);
    }
    CHECK(pb_pin_config(0, 1, 2, 0) == 4);
    CHECK(pb_pin_config(-1, 1, 2, 0) == 5);
    CHECK(pb_pin_config(1, 1, 2, 0) == 7);
    CHECK(pb_pin_config(0, 0, 16, 1) == (7u << 6 | 1u << 9));
}

int main(void)
{
    tables();
    survey();
    clocks();
    power_ok();
    power_failures();
    stops_before_bt_en();
    bt_enable();
    uart();
    printf("PASS bringup: %u checks\n", checks);
    return 0;
}
