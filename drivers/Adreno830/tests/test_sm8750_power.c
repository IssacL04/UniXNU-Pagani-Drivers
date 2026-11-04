/* SPDX-License-Identifier: MIT */
/* Host model of the SM8750 GPU clock registers as observed on PKX110, with the dependencies the
 * sequence must respect: source -> RCG -> branch, CX GDSC -> CX branches, gcc_gpu_cfg_ahb_clk ->
 * any GPU_CC access, and no source stopped while an RCG has it latched. The GDSC dependency of the
 * branches is the ordering of design 4.2.2, not measured hardware behaviour. Every read, write and
 * delay can be made to fail. tests/mutate_sm8750_power.py checks that this test kills known
 * regressions of the module. */
#include "../platform/sm8750/Sm8750GpuPower.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {GdscDelay = 5, PllDelay = 40, RcgMask = 0x10371f};

typedef struct {
    uint32_t gcc[0x80000 / 4], gpucc[0xa000 / 4];
    uint32_t latched[2];                    /* RCG configuration in effect: GMU, hub */
    unsigned now, gdsc_on_at, gdsc_off_at, pll_lock_at, ahb_ready_at;
    int pll_never_locks, gdsc_never_on, cx_other_voter;
    unsigned fail_read_at, fail_write_at, fail_delay_at;
    uint32_t stuck_cbcr, stuck_on_cbcr, fsm_cbcr, fsm_field, frozen_rcg;
    unsigned reads, writes, delays, seq, seq_user_out, seq_outctrl, pll_cfg_writes, cxo_aon_writes, ahb_writes;
    int ahb_readback;
    const char *violation;
} Model;

static Model m;

static uint32_t *reg(Model *x, unsigned block, uint32_t offset)
{
    return block == Sm8750GpuGcc ? &x->gcc[offset / 4] : &x->gpucc[offset / 4];
}

static void violate(Model *x, const char *what)
{
    if (!x->violation)
        x->violation = what;
}

static int gdsc_on(const Model *x)
{
    if (x->gdsc_never_on)
        return 0;
    if (x->gpucc[0x9080 / 4] & 1)                               /* collapse requested: drops after a while */
        return x->cx_other_voter || x->now < x->gdsc_off_at;
    return x->now >= x->gdsc_on_at;
}

static int pll_locked(const Model *x)
{
    return !x->pll_never_locks && (x->gpucc[0] & 4) && x->gpucc[1] == 1 && x->now >= x->pll_lock_at;
}

static int src_live(const Model *x, uint32_t cfg)
{
    switch ((cfg >> 8) & 7) {
    case 0: return 1;                                                               /* XO */
    case 2: return pll_locked(x) && (x->gpucc[0] & 1) && (x->gpucc[0x18 / 4] & 2);  /* PLL0_OUT_EVEN */
    case 5: return (x->gcc[0x52000 / 4] & (1u << 15)) && (x->gcc[0] & 0x80000000u); /* GPLL0 by GCC vote */
    default: return 0;
    }
}

static int rcg_index(uint32_t cmd)
{
    return cmd == 0x9318 ? 0 : 1;
}

/* A root runs when forced (ROOT_EN) or requested by an enabled child, from a live source. */
static int rcg_requested(const Model *x, uint32_t cmd)
{
    static const uint32_t gmu_children[] = {0x90d4, 0}, hub_children[] = {0x93e8, 0x90e8, 0x90bc, 0};
    if (x->gpucc[cmd / 4] & 2)
        return 1;
    for (const uint32_t *k = cmd == 0x9318 ? gmu_children : hub_children; *k; k++)
        if (x->gpucc[*k / 4] & 1)
            return 1;
    return 0;
}

static int rcg_on(const Model *x, uint32_t cmd)
{
    return rcg_requested(x, cmd) && src_live(x, x->latched[rcg_index(cmd)]);
}

static uint32_t parent_rcg(uint32_t cbcr)
{
    return cbcr == 0x90d4 ? 0x9318 : (cbcr == 0x93e8 || cbcr == 0x90e8 || cbcr == 0x90bc) ? 0x93ec : 0;
}

static int modelled_branch(unsigned block, uint32_t o)
{
    return block == Sm8750GpuGpuCc && (o == 0x9010 || o == 0x90d4 || o == 0x90e4 || o == 0x93e8 || o == 0x90e8 ||
        o == 0x7000 || o == 0x90bc);
}

static void touch(Model *x, unsigned block)
{
    if (block != Sm8750GpuGpuCc)
        return;
    if (!(x->gcc[0x71004 / 4] & 1))
        violate(x, "GPU_CC accessed before gcc_gpu_cfg_ahb_clk");
    else if (x->ahb_writes && (!x->ahb_readback || x->now < x->ahb_ready_at))
        violate(x, "GPU_CC accessed without the cfg_ahb read-back and 10 us");
}

static void source_stopped(Model *x, int rcg, unsigned src, const char *what)
{
    if (((x->latched[rcg] >> 8) & 7) == src)
        violate(x, what);
}

static int model_read(void *cookie, unsigned block, uint32_t offset, uint32_t *value)
{
    Model *x = cookie;
    touch(x, block);
    if (++x->reads == x->fail_read_at)
        return -1;
    uint32_t v = *reg(x, block, offset);
    if (block == Sm8750GpuGcc) {
        if (offset == 0x71004 && x->ahb_writes)
            x->ahb_readback = 1;
    } else if (offset == 0x9094) {
        v = gdsc_on(x) ? 0x80000000u : 0;
    } else if (offset == 0x9084) {
        v = gdsc_on(x) ? 1u << 16 : 1u << 15;                   /* POWER_UP / POWER_DOWN_COMPLETE */
    } else if (offset == 0) {
        v = (v & ~0x80000000u) | (pll_locked(x) ? 0x80000000u : 0);
    } else if (offset == 0x9318 || offset == 0x93ec) {
        uint32_t cfg = x->gpucc[(offset + 4) / 4];
        int i = rcg_index(offset);
        if ((v & 1) && offset != x->frozen_rcg && rcg_requested(x, offset) && src_live(x, cfg) &&
            src_live(x, x->latched[i])) {
            x->latched[i] = cfg;                                /* glitch-free switch: both sources run */
            v &= ~1u;
            *reg(x, block, offset) = v;
        }
        v = (v & ~0x80000000u) | (rcg_on(x, offset) ? 0 : 0x80000000u);
    } else if (modelled_branch(block, offset)) {
        uint32_t p = parent_rcg(offset);
        int on = (v & 1) && gdsc_on(x) && (!p || rcg_on(x, p)) && offset != x->stuck_cbcr;
        if (offset == x->stuck_on_cbcr && !(v & 1))
            on = 1;
        v = (v & ~0x80000000u) | (on ? 0 : 0x80000000u);
        if (offset == x->fsm_cbcr && (v & 1))
            v = (v & ~0x70000000u) | 0x80000000u | x->fsm_field << 28;     /* CLK_OFF with this NOC FSM state */
    }
    *value = v;
    return 0;
}

static int model_write(void *cookie, unsigned block, uint32_t offset, uint32_t value)
{
    Model *x = cookie;
    touch(x, block);
    if (++x->writes == x->fail_write_at)
        return -1;
    x->seq++;
    uint32_t *r = reg(x, block, offset), old = *r;
    if (block == Sm8750GpuGcc) {
        if (offset == 0x71004) {
            x->ahb_writes++;
            x->ahb_readback = 0;
            x->ahb_ready_at = x->now + 10;
        }
        if (offset == 0x71010) {                                /* gemnoc gfx: hw gated, CLK_OFF while idle */
            *r = (value & 3) | 0x80000000u;
            return 0;
        }
        if (offset == 0x52000 && (old & (1u << 15)) && !(value & (1u << 15)))
            source_stopped(x, 1, 5, "GPLL0 vote dropped under the hub RCG");
        *r = value;
        return 0;
    }
    switch (offset) {
    case 0x9008:                                                /* GPU_CC_CXO_AON_CLK: enable never sticks */
        x->cxo_aon_writes++;
        *r = 0x80000000u;
        return 0;
    case 0x9080:
        if ((old & 1) && !(value & 1))
            x->gdsc_on_at = x->now + GdscDelay;
        if (!(old & 1) && (value & 1))
            x->gdsc_off_at = x->now + GdscDelay;
        break;
    case 0x0:
        if ((value & 1) && !(old & 1))
            x->seq_outctrl = x->seq;
        if ((old & 1) && !(value & 1))
            source_stopped(x, 0, 2, "PLL0 output stopped under the GMU RCG");
        break;
    case 0x4:
        if (value == 1 && old != 1)
            x->pll_lock_at = x->now + PllDelay;
        if (old == 1 && value != 1)
            source_stopped(x, 0, 2, "PLL0 stopped under the GMU RCG");
        break;
    case 0x18:
        if ((value & 7) == 7 && (old & 7) != 7)
            x->seq_user_out = x->seq;
        if ((old & 2) && !(value & 2))
            source_stopped(x, 0, 2, "PLL0 even output stopped under the GMU RCG");
        break;
    case 0x9318:
    case 0x93ec:
        value &= ~0x80000000u;                                  /* ROOT_OFF is read-only */
        break;
    }
    if (offset >= 0x10 && offset <= 0x28 && (offset != 0x18 || ((old ^ value) & ~7u))) {
        x->pll_cfg_writes++;
        if (x->gpucc[1] == 1)
            violate(x, "PLL0 reprogrammed while running");
    }
    *r = value;
    return 0;
}

static int model_delay(void *cookie, unsigned us)
{
    Model *x = cookie;
    if (++x->delays == x->fail_delay_at)
        return -1;
    x->now += us;
    return 0;
}

static void reset(Model *x)
{
    memset(x, 0, sizeof(*x));
    x->gcc[0] = 0x80000001u;                                    /* GPLL0 locked, out enabled */
    x->gcc[0x10 / 4] = 0x1f;
    x->gcc[0x14 / 4] = 0x4000;
    x->gcc[0x71010 / 4] = 0x80000003u;                          /* as read on hardware */
    for (uint32_t o = 0x9004; o < 0x9400; o += 4)
        x->gpucc[o / 4] = 0x80000000u;                          /* CBCRs off, RCG roots off */
    x->gpucc[0x907c / 4] = x->gpucc[0x9314 / 4] = 0;            /* BCRs released */
    x->gpucc[0x9080 / 4] = 1;                                   /* CX GDSC collapsed */
    x->gpucc[0x9084 / 4] = x->gpucc[0x9094 / 4] = 0;
    x->gpucc[0x931c / 4] = x->gpucc[0x93f0 / 4] = 0;            /* RCGs on XO */
    x->gpucc[0x7000 / 4] = 0x80000000u;
}

static Sm8750GpuPowerStatus on(Sm8750GpuPowerTrace *t)
{
    Sm8750GpuPowerIo io = {&m, model_read, model_write, model_delay};
    return Sm8750GpuPowerOn(&io, t);
}

static Sm8750GpuPowerStatus off(Sm8750GpuPowerTrace *t)
{
    Sm8750GpuPowerIo io = {&m, model_read, model_write, model_delay};
    return Sm8750GpuPowerOff(&io, t);
}

static unsigned checks;
#define CHECK(c) (assert(c), checks++)

static void check_trace(Sm8750GpuPowerStatus s, Sm8750GpuPowerStatus want, const Sm8750GpuPowerTrace *t,
    uint32_t step, unsigned block, uint32_t offset, uint32_t value)
{
    if (s != want || t->step != step || t->block != block || t->offset != offset || t->value != value)
        printf("status %d step %u block %u offset %#x value %#x\n", s, t->step, t->block, t->offset, t->value);
    CHECK(s == want && t->step == step && t->block == block && t->offset == offset && t->value == value);
}

static const uint32_t waited[] = {0x9010, 0x90d4, 0x90e4, 0x93e8, 0x90e8, 0x7000, 0x90bc};
static const uint32_t critical[] = {0x9004, 0x9064, 0x90cc, 0x93a4, 0x93a8};

static void check_on_state(void)
{
    CHECK(!m.violation);
    CHECK(m.gcc[0x71004 / 4] & 1);
    CHECK((m.gpucc[0x9080 / 4] & 0xfff007) == 0x228000 && (m.gpucc[0x9080 / 4] & 0x800));  /* GDSC on, retain FF */
    CHECK((m.gpucc[0x931c / 4] & RcgMask) == 0x201 && (m.gpucc[0x93f0 / 4] & RcgMask) == 0x505);
    CHECK(m.latched[0] == m.gpucc[0x931c / 4] && m.latched[1] == m.gpucc[0x93f0 / 4]);
    CHECK((m.gpucc[0x9318 / 4] & 3) == 0 && (m.gpucc[0x93ec / 4] & 3) == 0);               /* released, latched */
    CHECK(m.gpucc[0x10 / 4] == 0x00440034 && (m.gpucc[0x18 / 4] & 7) == 7 && (m.gpucc[0] & 5) == 5 &&
        m.gpucc[1] == 1);
    CHECK(m.seq_user_out && m.seq_user_out < m.seq_outctrl);                                /* outputs, then OUTCTRL */
    CHECK((m.gcc[0x52000 / 4] & 0x18000) == 0x18000);
    for (size_t i = 0; i < sizeof(waited) / sizeof(waited[0]); i++)
        CHECK(m.gpucc[waited[i] / 4] & 1);
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); i++)
        CHECK(m.gpucc[critical[i] / 4] & 1);
    CHECK((m.gcc[0x71010 / 4] & 1) && (m.gcc[0x71150 / 4] & 1));
}

int main(void)
{
    Sm8750GpuPowerTrace t;
    Sm8750GpuPowerStatus s;

    /* Cold start with gcc_gpu_cfg_ahb_clk left off by the boot chain. */
    reset(&m);
    s = on(&t);
    printf("power on: status %d step %u last block %u offset %#x value %#x reads %u writes %u\n",
        s, t.step, t.block, t.offset, t.value, t.reads, t.writes);
    CHECK(s == Sm8750GpuPowerOk && t.step == Sm8750GpuStepDone && t.warnings == 0);
    CHECK(m.cxo_aon_writes == 1 && m.ahb_writes == 1);          /* critical clocks set, never polled */
    check_on_state();
    const unsigned on_reads = m.reads, on_writes = m.writes, on_delays = m.delays;

    /* Warm re-entry: PLL0 running with our configuration is kept, nothing is reprogrammed. */
    m.pll_cfg_writes = 0;
    CHECK(on(&t) == Sm8750GpuPowerOk && m.pll_cfg_writes == 0);
    check_on_state();
    /* Running at another rate belongs to someone else; the trace names the register. */
    m.gpucc[0x10 / 4] = 0x00440043;
    check_trace(on(&t), Sm8750GpuPowerUnexpectedState, &t, Sm8750GpuStepPll0, Sm8750GpuGpuCc, 0x10, 0x00440043);
    m.gpucc[0x10 / 4] = 0x00440034;
    m.gpucc[0x18 / 4] &= ~4u;                                   /* odd output off */
    check_trace(on(&t), Sm8750GpuPowerUnexpectedState, &t, Sm8750GpuStepPll0, Sm8750GpuGpuCc, 0x18, 0x403);

    /* The AHB clock already on: no write, no settle delay. */
    reset(&m);
    m.gcc[0x71004 / 4] = 1;
    CHECK(on(&t) == Sm8750GpuPowerOk && m.ahb_writes == 0);
    check_on_state();

    /* GPLL0 must be locked at 600 MHz; the trace names the first wrong register. */
    reset(&m);
    m.gcc[0] = 1;
    check_trace(on(&t), Sm8750GpuPowerUnexpectedState, &t, Sm8750GpuStepGpll0, Sm8750GpuGcc, 0x0, 1);
    reset(&m);
    m.gcc[0x10 / 4] = 0x20;
    check_trace(on(&t), Sm8750GpuPowerUnexpectedState, &t, Sm8750GpuStepGpll0, Sm8750GpuGcc, 0x10, 0x20);
    reset(&m);
    m.gcc[0x14 / 4] = 0x4001;
    check_trace(on(&t), Sm8750GpuPowerUnexpectedState, &t, Sm8750GpuStepGpll0, Sm8750GpuGcc, 0x14, 0x4001);

    /* Timeouts point at the register that never settled. */
    reset(&m);
    m.gdsc_never_on = 1;
    check_trace(on(&t), Sm8750GpuPowerTimeout, &t, Sm8750GpuStepCxGdsc, Sm8750GpuGpuCc, 0x9094, 0);
    reset(&m);
    m.stuck_cbcr = 0x90d4;
    s = on(&t);
    CHECK(s == Sm8750GpuPowerTimeout && t.step == Sm8750GpuStepBranches && t.offset == 0x90d4);

    /* A lock timeout leaves RUN without outputs; the next call stops and reprograms the PLL. */
    reset(&m);
    m.pll_never_locks = 1;
    s = on(&t);
    CHECK(s == Sm8750GpuPowerTimeout && t.step == Sm8750GpuStepPll0 && t.offset == 0x0);
    CHECK(m.gpucc[1] == 1 && !(m.gpucc[0] & 1));
    m.pll_never_locks = 0;
    CHECK(on(&t) == Sm8750GpuPowerOk);
    check_on_state();
    /* OUTCTRL left set in STANDBY is not "enabled": it is reprogrammed too. */
    reset(&m);
    m.gpucc[0] = 1;
    CHECK(on(&t) == Sm8750GpuPowerOk && m.pll_cfg_writes);
    check_on_state();

    /* Linux clk_branch2_check_halt: CLK_OFF with NOC_FSM_STATUS == ON (2) counts as on; 1 and 3 do not. */
    reset(&m);
    m.fsm_cbcr = 0x90e8;
    m.fsm_field = 2;
    CHECK(on(&t) == Sm8750GpuPowerOk);
    for (uint32_t field = 1; field <= 3; field += 2) {
        reset(&m);
        m.fsm_cbcr = 0x90e8;
        m.fsm_field = field;
        check_trace(on(&t), Sm8750GpuPowerTimeout, &t, Sm8750GpuStepBranches, Sm8750GpuGpuCc, 0x90e8,
            0x80000001u | field << 28);
    }

    /* Block resets left asserted are released. */
    reset(&m);
    m.gpucc[0x907c / 4] = m.gpucc[0x9314 / 4] = 1;
    CHECK(on(&t) == Sm8750GpuPowerOk && !(m.gpucc[0x907c / 4] & 1) && !(m.gpucc[0x9314 / 4] & 1));

    /* Every failing access or delay stops the sequence with IoError; a failed read shows 0xffffffff. */
    for (unsigned n = 1; n <= on_reads; n++) {
        reset(&m);
        m.fail_read_at = n;
        s = on(&t);
        CHECK(s == Sm8750GpuPowerIoError && t.reads == n && t.value == ~0u);
    }
    for (unsigned n = 1; n <= on_writes; n++) {
        reset(&m);
        m.fail_write_at = n;
        CHECK(on(&t) == Sm8750GpuPowerIoError && t.writes == n);
    }
    for (unsigned n = 1; n <= on_delays; n++) {
        reset(&m);
        m.fail_delay_at = n;
        CHECK(on(&t) == Sm8750GpuPowerIoError);
    }

    Sm8750GpuPowerIo io = {&m, model_read, model_write, model_delay}, bad = io;
    CHECK(Sm8750GpuPowerOn(NULL, &t) == Sm8750GpuPowerArgument && Sm8750GpuPowerOn(&io, NULL) == Sm8750GpuPowerArgument);
    CHECK(Sm8750GpuPowerOff(NULL, &t) == Sm8750GpuPowerArgument && Sm8750GpuPowerOff(&io, NULL) == Sm8750GpuPowerArgument);
    bad.delay_us = NULL;
    CHECK(Sm8750GpuPowerOn(&bad, &t) == Sm8750GpuPowerArgument && Sm8750GpuPowerOff(&bad, &t) == Sm8750GpuPowerArgument);

    /* Power off (design 4.2.3): branches off, RCGs parked on XO before their sources stop, PLL0 in
     * standby with its configuration, GPLL0 votes dropped, CX collapse requested; critical clocks stay. */
    reset(&m);
    CHECK(on(&t) == Sm8750GpuPowerOk);
    const unsigned base_reads = m.reads, base_writes = m.writes, base_delays = m.delays;
    s = off(&t);
    CHECK(s == Sm8750GpuPowerOk && t.step == Sm8750GpuStepOffDone && t.warnings == 0 && !m.violation);
    for (size_t i = 0; i < sizeof(waited) / sizeof(waited[0]); i++)
        CHECK(!(m.gpucc[waited[i] / 4] & 1));
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); i++)
        CHECK(m.gpucc[critical[i] / 4] & 1);
    CHECK(!(m.gcc[0x71010 / 4] & 1) && !(m.gcc[0x71150 / 4] & 1) && (m.gcc[0x71004 / 4] & 1));
    CHECK(m.latched[0] == 0 && m.latched[1] == 0);
    CHECK((m.gpucc[0x931c / 4] & RcgMask) == 0x201 && (m.gpucc[0x93f0 / 4] & RcgMask) == 0x505);
    CHECK((m.gpucc[0x9318 / 4] & 3) == 0 && (m.gpucc[0x93ec / 4] & 3) == 0);
    CHECK((m.gpucc[0] & 5) == 4 && (m.gpucc[0x18 / 4] & 7) == 0 && m.gpucc[1] == 0 && m.gpucc[0x10 / 4] == 0x00440034);
    CHECK((m.gcc[0x52000 / 4] & 0x18000) == 0 && (m.gpucc[0x9080 / 4] & 1));
    const unsigned off_reads = m.reads - base_reads, off_writes = m.writes - base_writes,
        off_delays = m.delays - base_delays;
    /* ... and power on again from there. */
    CHECK(on(&t) == Sm8750GpuPowerOk && t.step == Sm8750GpuStepDone);
    check_on_state();

    /* Power-off findings are warnings, not failures. */
    reset(&m);
    CHECK(on(&t) == Sm8750GpuPowerOk);
    m.stuck_on_cbcr = 0x90e4;
    CHECK(off(&t) == Sm8750GpuPowerOk && t.warnings == Sm8750GpuOffCxoStillOn && t.step == Sm8750GpuStepOffDone);
    reset(&m);
    CHECK(on(&t) == Sm8750GpuPowerOk);
    m.cx_other_voter = 1;
    CHECK(off(&t) == Sm8750GpuPowerOk && t.warnings == Sm8750GpuOffCxStillOn);
    reset(&m);
    CHECK(on(&t) == Sm8750GpuPowerOk);
    m.frozen_rcg = 0x93ec;
    CHECK(off(&t) == Sm8750GpuPowerOk && (t.warnings & Sm8750GpuOffHubRcgNotParked));
    reset(&m);
    CHECK(on(&t) == Sm8750GpuPowerOk);
    m.frozen_rcg = 0x9318;
    CHECK(off(&t) == Sm8750GpuPowerOk && t.warnings == Sm8750GpuOffGmuRcgNotParked);

    for (unsigned n = 1; n <= off_reads; n++) {
        reset(&m);
        CHECK(on(&t) == Sm8750GpuPowerOk);
        m.fail_read_at = m.reads + n;
        s = off(&t);
        CHECK(s == Sm8750GpuPowerIoError && t.value == ~0u);
    }
    for (unsigned n = 1; n <= off_writes; n++) {
        reset(&m);
        CHECK(on(&t) == Sm8750GpuPowerOk);
        m.fail_write_at = m.writes + n;
        CHECK(off(&t) == Sm8750GpuPowerIoError);
    }
    for (unsigned n = 1; n <= off_delays; n++) {
        reset(&m);
        CHECK(on(&t) == Sm8750GpuPowerOk);
        m.fail_delay_at = m.delays + n;
        CHECK(off(&t) == Sm8750GpuPowerIoError);
    }

    printf("%u checks: power on %u reads %u writes %u delays, power off %u reads %u writes %u delays\n", checks,
        on_reads, on_writes, on_delays, off_reads, off_writes, off_delays);
    puts("PASS: SM8750 GPU power-up follows the Linux clock semantics observed on PKX110");
    return 0;
}
