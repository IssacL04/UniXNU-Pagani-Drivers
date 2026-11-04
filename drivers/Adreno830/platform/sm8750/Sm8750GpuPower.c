/* SPDX-License-Identifier: MIT */
#include "Sm8750GpuPower.h"
#include <stddef.h>

/* GCC */
enum {
    GccGpll0Mode = 0x0, GccGpll0L = 0x10, GccGpll0Alpha = 0x14,
    GccClockVote = 0x52000,                 /* gcc_gpu_gpll0_clk_src BIT(15), _div_ BIT(16): HALT_DELAY */
    GccGpuCfgAhb = 0x71004,                 /* critical, set only */
    GccGpuGemnocGfx = 0x71010,              /* BRANCH_HALT_VOTED, hw clock gated */
    GccDdrssGpuAxi = 0x71150,               /* BRANCH_HALT_SKIP, hw clock gated */
};
/* GPU_CC */
enum {
    Pll0Mode = 0x0, Pll0Opmode = 0x4, Pll0L = 0x10, Pll0Alpha = 0x14, Pll0UserCtl = 0x18, Pll0UserCtlU = 0x1c,
    Pll0ConfigCtl = 0x20, Pll0ConfigCtlU = 0x24, Pll0ConfigCtlU1 = 0x28,
    CxBcr = 0x907c, CxGdscr = 0x9080, CxCfgGdscr = 0x9084, CxGdsHwCtrl = 0x9094, GmuBcr = 0x9314,
    GmuRcg = 0x9318, HubRcg = 0x93ec, AhbCbcr = 0x90bc,
};

enum {
    CbcrEnable = 1u << 0, CbcrHwCtl = 1u << 1, CbcrNocFsm = 7u << 28, CbcrNocFsmOn = 2u << 28, CbcrClkOff = 1u << 31,
    PllOutctrl = 1u << 0, PllResetN = 1u << 2, PllLockDet = 1u << 31, PllStandby = 0, PllRun = 1, PllOutputs = 7,
    GdscSwCollapse = 1u << 0, GdscRetainFf = 1u << 11, GdscPwrOn = 1u << 31, GdscPowerDownComplete = 1u << 15,
    RcgUpdate = 1u << 0, RcgRootEn = 1u << 1, RcgRootOff = 1u << 31,
    RcgCfgMask = 0x10371f,                  /* SRC_DIV, SRC_SEL, MODE, HW_CLK_CTRL */
    Gpll0Votes = 0x18000,
};

/* Linux gpucc-sm8750.c: gpu_cc_pll0_config (Taycan ELU), cal_l 0x44 in L[23:16]. */
enum {
    Pll0LVal = 0x00440034, Pll0AlphaVal = 0x1555, Pll0ConfigVal = 0x19660387, Pll0ConfigUVal = 0x098060a0,
    Pll0ConfigU1Val = 0xb416cb20, Pll0UserCtlVal = 0x00000400, Pll0UserCtlUVal = 0x00000002,
};

/* clk_branch_wait: what a branch toggle waits for. */
typedef enum {HaltSkip, HaltDelay, HaltWait} Halt;

typedef struct {
    const Sm8750GpuPowerIo *io;
    Sm8750GpuPowerTrace *trace;
} Ctx;

static Sm8750GpuPowerStatus rd(Ctx *c, unsigned block, uint32_t offset, uint32_t *value)
{
    c->trace->block = block;
    c->trace->offset = offset;
    c->trace->reads++;
    if (c->io->read32(c->io->cookie, block, offset, value)) {
        c->trace->value = ~0u;
        return Sm8750GpuPowerIoError;
    }
    c->trace->value = *value;
    return Sm8750GpuPowerOk;
}

static Sm8750GpuPowerStatus wr(Ctx *c, unsigned block, uint32_t offset, uint32_t value)
{
    c->trace->block = block;
    c->trace->offset = offset;
    c->trace->value = value;
    c->trace->writes++;
    return c->io->write32(c->io->cookie, block, offset, value) ? Sm8750GpuPowerIoError : Sm8750GpuPowerOk;
}

static Sm8750GpuPowerStatus wait_us(Ctx *c, unsigned microseconds)
{
    return c->io->delay_us(c->io->cookie, microseconds) ? Sm8750GpuPowerIoError : Sm8750GpuPowerOk;
}

/* Read-modify-write without requiring the write to read back (regmap_update_bits). */
static Sm8750GpuPowerStatus update(Ctx *c, unsigned block, uint32_t offset, uint32_t mask, uint32_t value)
{
    uint32_t old;
    Sm8750GpuPowerStatus s = rd(c, block, offset, &old);
    if (s || (old & mask) == value)
        return s;
    return wr(c, block, offset, (old & ~mask) | value);
}

/* GCC_APCS_CLOCK_BRANCH_ENA_VOTE 0x52000 also holds PCIe0's branch votes (PaganiWifi): the two read-modify-writes must not interleave.
 * The weak lock word of SOC kits/Sm8750Platform/PeripheralIO.h, the protocol its 0x52008 users follow: 0 free, else the holder's tag,
 * tried every 10 us for at most 50 ms; a holder that never gives it back does not stop the power sequence, the update then goes ahead. */
__attribute__((weak)) volatile uint32_t sm8750_gcc_vote_owner;
enum {GccVoteOwner = 0x41383330, GccVoteSteps = 5000};

static Sm8750GpuPowerStatus gcc_gpll0_vote(Ctx *c, uint32_t value)
{
    int locked = 0;
    for (unsigned step = 0; step < GccVoteSteps && !locked; step++) {
        uint32_t expected = 0;
        locked = __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, GccVoteOwner, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
        if (!locked) {
            const Sm8750GpuPowerStatus s = wait_us(c, 10);
            if (s)
                return s;
        }
    }
    const Sm8750GpuPowerStatus s = update(c, Sm8750GpuGcc, GccClockVote, Gpll0Votes, value);
    if (locked) {
        uint32_t expected = GccVoteOwner;
        __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    return s;
}

/* Read and compare: on a mismatch the trace already holds this register and its value. */
static Sm8750GpuPowerStatus expect(Ctx *c, unsigned block, uint32_t offset, uint32_t mask, uint32_t want)
{
    uint32_t value;
    Sm8750GpuPowerStatus s = rd(c, block, offset, &value);
    return s ? s : (value & mask) == want ? Sm8750GpuPowerOk : Sm8750GpuPowerUnexpectedState;
}

static Sm8750GpuPowerStatus poll(Ctx *c, unsigned block, uint32_t offset, uint32_t mask, uint32_t want,
    unsigned budget_us)
{
    for (unsigned spent = 0;; spent++) {
        uint32_t value;
        Sm8750GpuPowerStatus s = rd(c, block, offset, &value);
        if (s)
            return s;
        if ((value & mask) == want)
            return Sm8750GpuPowerOk;
        if (spent == budget_us)
            return Sm8750GpuPowerTimeout;
        if ((s = wait_us(c, 1)))
            return s;
    }
}

/* Linux clk_branch2_check_halt: a branch is on when CLK_OFF clears or its NOC FSM reports ON,
 * and off when CLK_OFF is set. A branch under hardware clock gating (HW_CTL) is not waited for. */
static int branch_settled(uint32_t value, int on)
{
    if (value & CbcrHwCtl)
        return 1;
    return on ? !(value & CbcrClkOff) || (value & CbcrNocFsm) == CbcrNocFsmOn : !!(value & CbcrClkOff);
}

/* clk_branch2 enable/disable. HaltWait polls (500 us on, 200 us off); HaltDelay, and voted
 * branches when turned off, get 10 us; HaltSkip nothing. */
static Sm8750GpuPowerStatus branch(Ctx *c, unsigned block, uint32_t cbcr, int on, Halt halt)
{
    Sm8750GpuPowerStatus s = update(c, block, cbcr, CbcrEnable, on ? CbcrEnable : 0);
    if (s || halt == HaltSkip)
        return s;
    if (halt == HaltDelay)
        return wait_us(c, 10);
    for (unsigned spent = 0, budget = on ? 500 : 200;; spent++) {
        uint32_t value;
        if ((s = rd(c, block, cbcr, &value)))
            return s;
        if (branch_settled(value, on))
            return Sm8750GpuPowerOk;
        if (spent == budget)
            return Sm8750GpuPowerTimeout;
        if ((s = wait_us(c, 1)))
            return s;
    }
}

/* Critical CBCRs: Linux qcom_branch_set_clk_en, set with no status check. */
static Sm8750GpuPowerStatus critical(Ctx *c, unsigned block, uint32_t cbcr)
{
    return update(c, block, cbcr, CbcrEnable, CbcrEnable);
}

static Sm8750GpuPowerStatus deassert(Ctx *c, uint32_t bcr)
{
    uint32_t value;
    Sm8750GpuPowerStatus s = rd(c, Sm8750GpuGpuCc, bcr, &value);
    if (s || !(value & 1))
        return s;
    if ((s = wr(c, Sm8750GpuGpuCc, bcr, value & ~1u)))
        return s;
    return wait_us(c, 1);
}

/* clk_rcg2_shared_ops: force the root on, switch, latch with UPDATE, release. */
static Sm8750GpuPowerStatus rcg(Ctx *c, uint32_t cmd, uint32_t cfg)
{
    Sm8750GpuPowerStatus s = update(c, Sm8750GpuGpuCc, cmd, RcgRootEn, RcgRootEn);
    if (!s)
        s = poll(c, Sm8750GpuGpuCc, cmd, RcgRootOff, 0, 500);
    if (!s)
        s = update(c, Sm8750GpuGpuCc, cmd + 4, RcgCfgMask, cfg);
    if (!s)
        s = update(c, Sm8750GpuGpuCc, cmd, RcgUpdate, RcgUpdate);
    if (!s)
        s = poll(c, Sm8750GpuGpuCc, cmd, RcgUpdate, 0, 500);
    if (!s)
        s = update(c, Sm8750GpuGpuCc, cmd, RcgRootEn, 0);
    return s;
}

/* clk_rcg2_shared_disable: under a forced root, latch the safe source (XO, CFG 0), release,
 * then put the rate's configuration back in CFG unlatched for the next rcg(). This must happen
 * while the old source still runs, so before PLL0 or the GPLL0 votes go. A timeout is a warning
 * (Linux ignores the force-enable result and only warns on UPDATE). */
static Sm8750GpuPowerStatus park(Ctx *c, uint32_t cmd, uint32_t warning)
{
    uint32_t cfg;
    Sm8750GpuPowerStatus s;
    if ((s = rd(c, Sm8750GpuGpuCc, cmd + 4, &cfg)) || (s = update(c, Sm8750GpuGpuCc, cmd, RcgRootEn, RcgRootEn)))
        return s;
    if ((s = poll(c, Sm8750GpuGpuCc, cmd, RcgRootOff, 0, 500)) == Sm8750GpuPowerTimeout)
        s = Sm8750GpuPowerOk;
    if (s || (s = wr(c, Sm8750GpuGpuCc, cmd + 4, 0)) || (s = update(c, Sm8750GpuGpuCc, cmd, RcgUpdate, RcgUpdate)))
        return s;
    if ((s = poll(c, Sm8750GpuGpuCc, cmd, RcgUpdate, 0, 500)) == Sm8750GpuPowerTimeout) {
        c->trace->warnings |= warning;
        s = Sm8750GpuPowerOk;
    }
    if (s || (s = update(c, Sm8750GpuGpuCc, cmd, RcgRootEn, 0)))
        return s;
    return wr(c, Sm8750GpuGpuCc, cmd + 4, cfg);
}

/* GPU_CC_PLL0 at 1000 MHz (L 0x34, alpha 0x1555); out_even (/2) feeds the GMU.
 * clk_lucid_evo_pll_configure + alpha_pll_lucid_evo_enable, stopping the PLL before it is
 * reprogrammed. */
static Sm8750GpuPowerStatus pll0(Ctx *c)
{
    uint32_t mode, opmode;
    Sm8750GpuPowerStatus s;
    if ((s = rd(c, Sm8750GpuGpuCc, Pll0Mode, &mode)) || (s = rd(c, Sm8750GpuGpuCc, Pll0Opmode, &opmode)))
        return s;
    if ((mode & PllOutctrl) && (opmode & PllRun)) {
        /* Enabled (trion_pll_is_enabled). Linux takes any running PLL; accept only our rate,
         * locked, with all outputs on, since anything else means another owner. */
        if ((s = expect(c, Sm8750GpuGpuCc, Pll0L, 0xffff, Pll0LVal & 0xffff)) ||
            (s = expect(c, Sm8750GpuGpuCc, Pll0Alpha, 0xffff, Pll0AlphaVal)) ||
            (s = expect(c, Sm8750GpuGpuCc, Pll0UserCtl, 0x2003c07, 0x407)) ||
            (s = expect(c, Sm8750GpuGpuCc, Pll0Opmode, ~0u, PllRun)))
            return s;
        return expect(c, Sm8750GpuGpuCc, Pll0Mode, PllLockDet | PllResetN | PllOutctrl,
            PllLockDet | PllResetN | PllOutctrl);
    }
    /* Not enabled, including RUN without outputs after an earlier lock timeout: outputs off and
     * STANDBY first (the tail of clk_lucid_evo_pll_configure, moved ahead of the writes). */
    if ((s = update(c, Sm8750GpuGpuCc, Pll0Mode, PllOutctrl, 0)) ||
        (opmode != PllStandby && (s = wr(c, Sm8750GpuGpuCc, Pll0Opmode, PllStandby))))
        return s;
    if ((s = wr(c, Sm8750GpuGpuCc, Pll0L, Pll0LVal)) || (s = wr(c, Sm8750GpuGpuCc, Pll0Alpha, Pll0AlphaVal)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0ConfigCtl, Pll0ConfigVal)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0ConfigCtlU, Pll0ConfigUVal)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0ConfigCtlU1, Pll0ConfigU1Val)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0UserCtl, Pll0UserCtlVal)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0UserCtlU, Pll0UserCtlUVal)))
        return s;
    if ((s = update(c, Sm8750GpuGpuCc, Pll0Mode, PllResetN, PllResetN)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0Opmode, PllRun)) ||
        (s = poll(c, Sm8750GpuGpuCc, Pll0Mode, PllLockDet, PllLockDet, 1500)) ||
        (s = update(c, Sm8750GpuGpuCc, Pll0UserCtl, PllOutputs, PllOutputs)))
        return s;
    return update(c, Sm8750GpuGpuCc, Pll0Mode, PllOutctrl, PllOutctrl);
}

static const uint32_t critical_cbcrs[] = {0x9004, 0x9008, 0x9064, 0x90cc, 0x93a4, 0x93a8};
/* cx_gmu, cxo, hub_cx_int: DT GMU clocks; hlos1_vote_gpu_smmu: kgsl_smmu; demet, hub_aon: not in
 * the DT, kept from the author's tested sequence. All but cxo (BRANCH_HALT) are HALT_VOTED. */
static const uint32_t waited_cbcrs[] = {0x9010, 0x90d4, 0x90e4, 0x93e8, 0x90e8, 0x7000};
enum {CxoCbcr = 0x90e4, WaitedCbcrs = sizeof(waited_cbcrs) / sizeof(waited_cbcrs[0])};

static int valid(const Sm8750GpuPowerIo *io, Sm8750GpuPowerTrace *trace)
{
    if (!io || !trace || !io->read32 || !io->write32 || !io->delay_us)
        return 0;
    *trace = (Sm8750GpuPowerTrace){0};
    return 1;
}

Sm8750GpuPowerStatus Sm8750GpuPowerOn(const Sm8750GpuPowerIo *io, Sm8750GpuPowerTrace *trace)
{
    uint32_t value;
    Sm8750GpuPowerStatus s;
    if (!valid(io, trace))
        return Sm8750GpuPowerArgument;
    Ctx ctx = {io, trace}, *c = &ctx;

    /* gcc_gpu_cfg_ahb_clk: set only if the boot chain left it off; then read it back on GCC and
     * give the branch 10 us, as the tested sequence did, before the first GPU_CC access. */
    trace->step = Sm8750GpuStepGccAhb;
    if ((s = rd(c, Sm8750GpuGcc, GccGpuCfgAhb, &value)))
        return s;
    if (!(value & CbcrEnable) && ((s = wr(c, Sm8750GpuGcc, GccGpuCfgAhb, value | CbcrEnable)) ||
        (s = rd(c, Sm8750GpuGcc, GccGpuCfgAhb, &value)) || (s = wait_us(c, 10))))
        return s;

    trace->step = Sm8750GpuStepResets;
    if ((s = deassert(c, CxBcr)) || (s = deassert(c, GmuBcr)))
        return s;

    /* gpu_cc_cx_gdsc: wait values 2/2/8, SW_COLLAPSE off, PWR_ON in gds_hw_ctrl, RETAIN_FF_ENABLE. */
    trace->step = Sm8750GpuStepCxGdsc;
    if ((s = update(c, Sm8750GpuGpuCc, CxGdscr, 0xfff007, 0x228000)) || (s = wait_us(c, 1)) ||
        (s = poll(c, Sm8750GpuGpuCc, CxGdsHwCtrl, GdscPwrOn, GdscPwrOn, 2000)) || (s = wait_us(c, 1)) ||
        (s = update(c, Sm8750GpuGpuCc, CxGdscr, GdscRetainFf, GdscRetainFf)))
        return s;

    /* GPLL0 (600 MHz, owned by boot firmware) feeds the hub RCG: route it and check its rate. */
    trace->step = Sm8750GpuStepGpll0;
    if ((s = gcc_gpll0_vote(c, Gpll0Votes)) || (s = wait_us(c, 10)) ||
        (s = expect(c, Sm8750GpuGcc, GccGpll0Mode, PllLockDet, PllLockDet)) ||
        (s = expect(c, Sm8750GpuGcc, GccGpll0L, 0xffff, 0x1f)) ||
        (s = expect(c, Sm8750GpuGcc, GccGpll0Alpha, 0xffff, 0x4000)))
        return s;

    trace->step = Sm8750GpuStepPll0;
    if ((s = pll0(c)))
        return s;

    /* gmu_clk_src = PLL0_OUT_EVEN / 1 (500 MHz); hub_clk_src = GPLL0 / 3 (200 MHz). */
    trace->step = Sm8750GpuStepRcg;
    if ((s = rcg(c, GmuRcg, 0x201)) || (s = rcg(c, HubRcg, 0x505)))
        return s;

    trace->step = Sm8750GpuStepBranches;
    for (size_t i = 0; i < sizeof(critical_cbcrs) / sizeof(critical_cbcrs[0]); i++)
        if ((s = critical(c, Sm8750GpuGpuCc, critical_cbcrs[i])))
            return s;
    if ((s = branch(c, Sm8750GpuGpuCc, AhbCbcr, 1, HaltDelay)))
        return s;
    for (size_t i = 0; i < WaitedCbcrs; i++)
        if ((s = branch(c, Sm8750GpuGpuCc, waited_cbcrs[i], 1, HaltWait)))
            return s;
    if ((s = branch(c, Sm8750GpuGcc, GccDdrssGpuAxi, 1, HaltSkip)) ||
        (s = branch(c, Sm8750GpuGcc, GccGpuGemnocGfx, 1, HaltWait)))
        return s;

    trace->step = Sm8750GpuStepDone;
    return Sm8750GpuPowerOk;
}

Sm8750GpuPowerStatus Sm8750GpuPowerOff(const Sm8750GpuPowerIo *io, Sm8750GpuPowerTrace *trace)
{
    uint32_t value;
    Sm8750GpuPowerStatus s;
    if (!valid(io, trace))
        return Sm8750GpuPowerArgument;
    Ctx ctx = {io, trace}, *c = &ctx;

    /* D1: branches off in reverse order; critical CBCRs and gcc_gpu_cfg_ahb stay on. */
    trace->step = Sm8750GpuStepOffBranches;
    if ((s = branch(c, Sm8750GpuGcc, GccGpuGemnocGfx, 0, HaltDelay)) ||
        (s = branch(c, Sm8750GpuGcc, GccDdrssGpuAxi, 0, HaltSkip)))
        return s;
    for (size_t i = WaitedCbcrs; i-- > 0;) {
        s = branch(c, Sm8750GpuGpuCc, waited_cbcrs[i], 0, waited_cbcrs[i] == CxoCbcr ? HaltWait : HaltDelay);
        if (s == Sm8750GpuPowerTimeout) {
            trace->warnings |= Sm8750GpuOffCxoStillOn;
            s = Sm8750GpuPowerOk;
        }
        if (s)
            return s;
    }
    if ((s = branch(c, Sm8750GpuGpuCc, AhbCbcr, 0, HaltDelay)))
        return s;

    trace->step = Sm8750GpuStepOffRcg;
    if ((s = park(c, HubRcg, Sm8750GpuOffHubRcgNotParked)) || (s = park(c, GmuRcg, Sm8750GpuOffGmuRcgNotParked)))
        return s;

    /* D2: _alpha_pll_lucid_evo_disable without reset: RESET_N and the configuration stay. */
    trace->step = Sm8750GpuStepOffPll0;
    if ((s = update(c, Sm8750GpuGpuCc, Pll0Mode, PllOutctrl, 0)) ||
        (s = update(c, Sm8750GpuGpuCc, Pll0UserCtl, PllOutputs, 0)) ||
        (s = wr(c, Sm8750GpuGpuCc, Pll0Opmode, PllStandby)))
        return s;

    trace->step = Sm8750GpuStepOffGpll0;
    if ((s = gcc_gpll0_vote(c, 0)) || (s = wait_us(c, 10)))
        return s;

    /* D4: gpu_cc_cx_gdsc is VOTABLE: request the collapse and wait 500 us without polling; the GMU
     * may still hold CX, so POWER_DOWN_COMPLETE is only reported. */
    trace->step = Sm8750GpuStepOffGdsc;
    if ((s = update(c, Sm8750GpuGpuCc, CxGdscr, GdscSwCollapse, GdscSwCollapse)) || (s = wait_us(c, 500)) ||
        (s = rd(c, Sm8750GpuGpuCc, CxCfgGdscr, &value)))
        return s;
    if (!(value & GdscPowerDownComplete))
        trace->warnings |= Sm8750GpuOffCxStillOn;

    trace->step = Sm8750GpuStepOffDone;
    return Sm8750GpuPowerOk;
}
