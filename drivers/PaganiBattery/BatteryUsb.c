/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The USB probe (BatteryUsb.h). */
#include "BatteryUsb.h"
#include "BatteryText.h"

const PbuWindow pagani_usb_windows[PbuWinCount] = {
    {0x100000ull, 0x60000u},        /* GCC up to the USB PHY GDSC */
    {0xf206000ull, 0x1000u},        /* the page of TCSR_USB2_CLKREF_EN */
    {0xa600000ull, 0x1000u},        /* xHCI capability and operational registers */
    {0xa60c000ull, 0x1000u},        /* the DWC3 globals and device registers */
    {0xa6f8000ull, 0x1000u},        /* the page of the glue */
    {0x88e3000ull, 0x1000u},        /* the M31 PHY, 0x29c bytes */
    {0x15000000ull, 0x100000u},     /* the apps SMMU */
};

enum {
    kGccPhyBcr = 0x12000,           /* GCC_QUSB2PHY_PRIM_BCR */
    kGccBcr = 0x39000,
    kGccGdsc = 0x39004, kGccGdscCfg = 0x39008,
    kGccMaster = 0x39018, kGccAggre = 0x39090, kGccNoc = 0x3908c, kGccUtmi = 0x3902c, kGccSleep = 0x39028,
    kGccMasterRcg = 0x39030, kGccUtmiRcg = 0x39048,
    kGccPhyGdsc = 0x50018,
    kTcsrUsb2Ref = 0x008,
    kBcrAsserted = 1,
    kGdscCollapse = 1, kCfgDownDone = 1 << 15, kCfgUpDone = 1 << 16,
    kClkEnable = 1, kClkHwCtl = 2,
    kXhciCap = 0x000,
    kQsConfig = 0x808, kQsHsPhy = 0x810, kQsClockGate = 0x828, kQsSsPhy = 0x830, kQsPowerEvent = 0x858, kQsPowerMask = 0x85c,
    kPhyCtrl5 = 0x50, kPhyCommon0 = 0x54,
    kPhyPor = 1 << 1, kPhySiddq = 1 << 2,
    kStatusInRegulation = 0x80,
    kRoleHost = 1, kRoleDevice = 2,
    kSmmuGroups = 128,              /* Linux qcom_smmu_cfg_probe: groups above 128 behave differently */
    kSid = 0x40,
    kNext = 1,                      /* no reason to wait: the milliseconds to the next step */
};
#define kGdscPwrOn 0x80000000u
#define kClkOff 0x80000000u
#define kRootOff 0x80000000u

enum {StageRails, StagePmic, StageSmmu, StageGcc, StageDwc, StageQscratch, StageRef, StagePhy, StageDone, StageBanks, StageOver};

static uint32_t rd(const PbuRun *r, unsigned window, uint32_t offset) {return r->io->read(r->io->ctx, window, offset);}

static void start(PbuRun *r, PbatText *t, const char *what)
{
    pbat_text_start(t, r->line, sizeof r->line);
    pbat_text_str(t, "[usbp] ");
    pbat_text_str(t, what);
}
static void finish(PbuRun *r)
{
    r->lines++;
    r->io->trace(r->io->ctx, r->line);
}

/* A branch that runs, and one that runs or sleeps in hardware clock gating. */
static int branch_on(uint32_t v) {return (v & kClkEnable) && !(v & kClkOff);}
static int gated_on(uint32_t v) {return (v & kClkEnable) && (!(v & kClkOff) || (v & kClkHwCtl));}

/* ---- step 1 ---- */
static void step_rails(PbuRun *r)
{
    static const struct {uint16_t ppid; uint8_t phy; const char *name;} rails[] = {
        {0x3c2, 1, "l2d"}, {0x6c3, 1, "l3g"}, {0x5c1, 1, "l1f"}, {0x1cf, 0, "l15b"}, {0x1c5, 0, "l5b"},
    };
    PbatText t;
    int ok = 1;
    start(r, &t, "rail");
    for (unsigned i = 0; i < sizeof rails / sizeof rails[0]; ++i) {
        uint8_t status = 0, enable = 0;
        int rc = r->io->pmic(r->io->ctx, rails[i].ppid, 0x08, &status);
        if (!rc) rc = r->io->pmic(r->io->ctx, rails[i].ppid, 0x46, &enable);
        if (rc) {
            r->pmic_failed++;
            pbat_text_kd(&t, rails[i].name, rc);
        } else {
            pbat_text_kx(&t, rails[i].name, (uint32_t)status << 8 | enable);
        }
        if (rails[i].phy && (rc || !(status & kStatusInRegulation))) ok = 0;
    }
    r->rails_ok = (uint8_t)ok;
    pbat_text_kd(&t, "phy", ok);
    finish(r);
}

/* ---- step 2 ---- */
static void hex2(PbatText *t, unsigned v)
{
    static const char hex[] = "0123456789abcdef";
    pbat_text_char(t, hex[(v >> 4) & 15]);
    pbat_text_char(t, hex[v & 15]);
}
static void bytes(PbuRun *r, PbatText *t, unsigned ppid, unsigned first, unsigned last)
{
    pbat_text_char(t, ' ');
    hex2(t, first);
    pbat_text_char(t, '=');
    for (unsigned at = first; at <= last; ++at) {
        uint8_t value = 0;
        if (r->io->pmic(r->io->ctx, ppid, (uint8_t)at, &value)) {
            r->pmic_failed++;
            pbat_text_str(t, "--");
        } else {
            hex2(t, value);
        }
    }
}
static void step_pmic(PbuRun *r)
{
    static const uint16_t pages[] = {0x7fd, 0x726, 0x727, 0x728, 0x729, 0x72b, 0x72c};
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; ++i) {
        PbatText t;
        uint8_t value = 0;
        start(r, &t, "p");
        pbat_text_char(&t, (char)('0' + (pages[i] >> 8)));
        hex2(&t, pages[i] & 0xffu);
        /* a peripheral without a channel costs one read and one short line */
        const int rc = r->io->pmic(r->io->ctx, pages[i], 0x04, &value);
        if (rc) {
            r->pmic_failed++;
            pbat_text_kd(&t, "none", rc);
        } else {
            bytes(r, &t, pages[i], 0x04, 0x10);
            bytes(r, &t, pages[i], 0x40, 0x7f);
        }
        finish(r);
    }
}

/* ---- step 3 ---- */
static void smmu_bank(PbuRun *r, uint32_t bank)
{
    const uint32_t cb = (r->smmu_pages + bank) * 4096u;
    PbatText t;
    start(r, &t, "smmu bank");
    pbat_text_kd(&t, "n", bank);
    pbat_text_kx(&t, "cbar", rd(r, PbuWinSmmu, 0x1000u + 4u * bank));
    pbat_text_kx(&t, "cba2r", rd(r, PbuWinSmmu, 0x1800u + 4u * bank));
    pbat_text_kx(&t, "sctlr", rd(r, PbuWinSmmu, cb));
    pbat_text_kx(&t, "ttbr0", (uint64_t)rd(r, PbuWinSmmu, cb + 0x24u) << 32 | rd(r, PbuWinSmmu, cb + 0x20u));
    pbat_text_kx(&t, "tcr", rd(r, PbuWinSmmu, cb + 0x30u));
    pbat_text_kx(&t, "fsr", rd(r, PbuWinSmmu, cb + 0x58u));
    finish(r);
}
static void step_smmu(PbuRun *r)
{
    const uint32_t idr0 = rd(r, PbuWinSmmu, 0x20), idr1 = rd(r, PbuWinSmmu, 0x24);
    const uint32_t banks = idr1 & 255u, pages = 1u << (((idr1 >> 28) & 7u) + 1u);
    uint32_t count = idr0 & 255u, found = 0, shown = 0;
    PbatText t;
    start(r, &t, "smmu");
    pbat_text_kx(&t, "idr0", idr0);
    pbat_text_kx(&t, "idr1", idr1);
    /* not the geometry NativeUfs knows: no table is read (these limits are what keeps every read inside the 1 MiB window) */
    if ((idr1 >> 31) || !count || pages > 128 || banks < 2 || banks > 128) {
        pbat_text_str(&t, " refused");
        finish(r);
        return;
    }
    if (count > kSmmuGroups) count = kSmmuGroups;
    r->smmu_pages = pages;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t smr = rd(r, PbuWinSmmu, 0x800u + 4u * i);
        if (!(smr & 0x80000000u) || ((smr ^ kSid) & 0xffffu & ~((smr >> 16) & 0x7fffu))) continue;
        found++;
        if (shown == PBU_SMMU_SHOWN) continue;
        shown++;
        const uint32_t s2cr = rd(r, PbuWinSmmu, 0xc00u + 4u * i);
        pbat_text_kd(&t, "group", i);
        pbat_text_kx(&t, "smr", smr);
        pbat_text_kx(&t, "s2cr", s2cr);
        if (!((s2cr >> 16) & 3u) && (s2cr & 255u) < banks) r->smmu_bank[r->smmu_banks++] = s2cr & 255u;   /* TYPE translate */
    }
    pbat_text_kd(&t, "found", found);
    finish(r);
}

/* ---- step 4 ---- */
static const char *gate(const PbuGcc *g)
{
    if (g->bcr & kBcrAsserted) return "reset";
    if (!(g->gdsc & kGdscPwrOn)) return "gdsc";
    if (g->gdsc & kGdscCollapse) return "collapse";
    if (!(g->cfg & kCfgUpDone) || (g->cfg & kCfgDownDone)) return "gdsc-cfg";
    if (g->master_rcg & kRootOff) return "root";
    if (g->utmi_rcg & kRootOff) return "utmi-root";
    if (!branch_on(g->master)) return "master";
    if (!gated_on(g->aggre)) return "aggre";
    if (!gated_on(g->noc)) return "noc";
    if (!branch_on(g->utmi)) return "utmi";
    if (!branch_on(g->sleep)) return "sleep";
    return 0;
}
static void gcc_read(PbuRun *r, const char *name, PbuGcc *g)
{
    g->bcr = rd(r, PbuWinGcc, kGccBcr);
    g->gdsc = rd(r, PbuWinGcc, kGccGdsc);
    g->cfg = rd(r, PbuWinGcc, kGccGdscCfg);
    g->master = rd(r, PbuWinGcc, kGccMaster);
    g->aggre = rd(r, PbuWinGcc, kGccAggre);
    g->noc = rd(r, PbuWinGcc, kGccNoc);
    g->utmi = rd(r, PbuWinGcc, kGccUtmi);
    g->sleep = rd(r, PbuWinGcc, kGccSleep);
    g->master_rcg = rd(r, PbuWinGcc, kGccMasterRcg);
    g->utmi_rcg = rd(r, PbuWinGcc, kGccUtmiRcg);
    PbatText t;
    start(r, &t, name);
    pbat_text_kx(&t, "bcr", g->bcr);
    pbat_text_kx(&t, "gdsc", g->gdsc);
    pbat_text_kx(&t, "cfg", g->cfg);
    pbat_text_kx(&t, "mst", g->master);
    pbat_text_kx(&t, "agg", g->aggre);
    pbat_text_kx(&t, "noc", g->noc);
    pbat_text_kx(&t, "utmi", g->utmi);
    pbat_text_kx(&t, "slp", g->sleep);
    pbat_text_kx(&t, "rcg", g->master_rcg);
    pbat_text_kx(&t, "urcg", g->utmi_rcg);
    finish(r);
}
void pagani_usb_entry(PbuRun *r)
{
    gcc_read(r, "gcc0", &r->entry);
    r->entry_read = 1;
}
static int gcc_same(const PbuGcc *a, const PbuGcc *b)
{
    /* the status bits of the two gated branches come and go with the bus: their enables are what counts */
    return a->bcr == b->bcr && a->gdsc == b->gdsc && a->cfg == b->cfg && a->master == b->master && a->utmi == b->utmi && a->sleep == b->sleep &&
           !((a->aggre ^ b->aggre) & ~kClkOff) && !((a->noc ^ b->noc) & ~kClkOff) && a->master_rcg == b->master_rcg && a->utmi_rcg == b->utmi_rcg;
}
static uint32_t step_gcc(PbuRun *r)
{
    PbuGcc g;
    gcc_read(r, "gcc", &g);
    r->gcc_same = r->entry_read && gcc_same(&r->entry, &g);
    r->phy_bcr = rd(r, PbuWinGcc, kGccPhyBcr);
    const char *closed = gate(&g);
    r->core_open = !closed;
    PbatText t;
    start(r, &t, "gate");
    pbat_text_kx(&t, "phybcr", r->phy_bcr);
    pbat_text_kx(&t, "phygdsc", rd(r, PbuWinGcc, kGccPhyGdsc));
    if (closed) {
        r->why = closed;
        pbat_text_ks(&t, "closed", closed);
        finish(r);
        r->stage = StageDone;
        return kNext;
    }
    pbat_text_str(&t, " next=dwc");
    finish(r);
    r->stage = StageDwc;
    return PBU_SETTLE_MS;
}

/* ---- step 5 ---- */
static uint32_t step_dwc(PbuRun *r)
{
    PbatText t;
    r->gsnpsid = rd(r, PbuWinDwc, 0x120);
    const uint32_t product = r->gsnpsid >> 16;
    r->id_ok = product == 0x5533 || product == 0x3331 || product == 0x3332;
    if (!r->id_ok) {
        r->why = "id";
        start(r, &t, "dwc");
        pbat_text_kx(&t, "id", r->gsnpsid);
        pbat_text_str(&t, " not a Synopsys id");
        finish(r);
        r->stage = StageDone;
        return kNext;
    }
    r->gctl = rd(r, PbuWinDwc, 0x110);
    start(r, &t, "dwc");
    pbat_text_kx(&t, "id", r->gsnpsid);
    if (product != 0x5533) {   /* DWC_usb31 and 32 keep their version here */
        pbat_text_kx(&t, "ver", rd(r, PbuWinDwc, 0x1a0));
        pbat_text_kx(&t, "type", rd(r, PbuWinDwc, 0x1a4));
    }
    pbat_text_kx(&t, "gctl", r->gctl);
    pbat_text_kx(&t, "gsts", rd(r, PbuWinDwc, 0x118));
    pbat_text_kx(&t, "guctl", rd(r, PbuWinDwc, 0x12c));
    pbat_text_kx(&t, "guctl1", rd(r, PbuWinDwc, 0x11c));
    pbat_text_kx(&t, "guid", rd(r, PbuWinDwc, 0x128));
    pbat_text_kx(&t, "u2", rd(r, PbuWinDwc, 0x200));
    pbat_text_kx(&t, "pipe", rd(r, PbuWinDwc, 0x2c0));
    finish(r);
    r->dctl = rd(r, PbuWinDwc, 0x704);
    r->dsts = rd(r, PbuWinDwc, 0x70c);
    start(r, &t, "dev");
    pbat_text_kx(&t, "dcfg", rd(r, PbuWinDwc, 0x700));
    pbat_text_kx(&t, "dctl", r->dctl);
    pbat_text_kx(&t, "dsts", r->dsts);
    pbat_text_kx(&t, "devten", rd(r, PbuWinDwc, 0x708));
    pbat_text_kx(&t, "alep", rd(r, PbuWinDwc, 0x720));
    pbat_text_kx(&t, "evt", (uint64_t)rd(r, PbuWinDwc, 0x404) << 32 | rd(r, PbuWinDwc, 0x400));
    pbat_text_kx(&t, "evsz", rd(r, PbuWinDwc, 0x408));
    pbat_text_kx(&t, "evcnt", rd(r, PbuWinDwc, 0x40c));
    finish(r);
    if (((r->gctl >> 12) & 3u) == kRoleHost) {
        const uint32_t cap = rd(r, PbuWinXhci, kXhciCap), length = cap & 255u;
        start(r, &t, "xhci");
        pbat_text_kx(&t, "cap", cap);
        if (length >= 0x10u && length <= 0x80u && !(length & 3u)) {
            pbat_text_kx(&t, "cmd", rd(r, PbuWinXhci, length));
            pbat_text_kx(&t, "sts", rd(r, PbuWinXhci, length + 4u));
        }
        finish(r);
    }
    start(r, &t, "hw");
    for (uint32_t i = 0; i < 8; ++i) {
        pbat_text_char(&t, ' ');
        pbat_text_hex(&t, rd(r, PbuWinDwc, 0x140u + 4u * i));
    }
    pbat_text_char(&t, ' ');
    pbat_text_hex(&t, rd(r, PbuWinDwc, 0x600));
    pbat_text_str(&t, " next=qs");
    finish(r);
    r->stage = StageQscratch;
    return PBU_SETTLE_MS;
}

/* ---- step 6 ---- */
static uint32_t step_qscratch(PbuRun *r)
{
    PbatText t;
    start(r, &t, "qs");
    pbat_text_kx(&t, "cfg", rd(r, PbuWinQscratch, kQsConfig));
    pbat_text_kx(&t, "hs", rd(r, PbuWinQscratch, kQsHsPhy));
    pbat_text_kx(&t, "cg", rd(r, PbuWinQscratch, kQsClockGate));
    pbat_text_kx(&t, "ss", rd(r, PbuWinQscratch, kQsSsPhy));
    pbat_text_kx(&t, "pwr", rd(r, PbuWinQscratch, kQsPowerEvent));
    pbat_text_kx(&t, "pmask", rd(r, PbuWinQscratch, kQsPowerMask));
    const char *closed = !r->rails_ok ? "rail" : (r->phy_bcr & kBcrAsserted) ? "reset" : 0;
    if (closed) {
        r->why = closed;
        pbat_text_ks(&t, "phy", closed);
        finish(r);
        r->stage = StageDone;
        return kNext;
    }
    pbat_text_str(&t, " next=tcsr");
    finish(r);
    r->stage = StageRef;
    return PBU_SETTLE_MS;
}

/* ---- step 7: the first read of this block on XNU, which is why it waits until the controller's answers are in the log ---- */
static uint32_t step_ref(PbuRun *r)
{
    const uint32_t ref = rd(r, PbuWinTcsr, kTcsrUsb2Ref);
    PbatText t;
    start(r, &t, "ref");
    pbat_text_kx(&t, "tcsr", ref);
    if (!(ref & 1u)) {
        r->why = "ref";
        pbat_text_ks(&t, "phy", "ref");
        finish(r);
        r->stage = StageDone;
        return kNext;
    }
    pbat_text_str(&t, " next=phy");
    finish(r);
    r->stage = StagePhy;
    return PBU_SETTLE_MS;
}

/* ---- step 8 ---- */
static void step_phy(PbuRun *r)
{
    static const uint16_t offsets[] = {0x3c, kPhyCtrl5, kPhyCommon0, 0x64, 0x94, 0xb8, 0x154, 0x15c, 0x168, 0x16c, 0x17c};
    PbatText t;
    start(r, &t, "phy");
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        const uint32_t value = rd(r, PbuWinPhy, offsets[i]);
        if (offsets[i] == kPhyCtrl5) r->phy_ctrl5 = value;
        if (offsets[i] == kPhyCommon0) r->phy_common0 = value;
        pbat_text_char(&t, ' ');
        hex2(&t, offsets[i] >> 8);
        hex2(&t, offsets[i] & 0xffu);
        pbat_text_char(&t, '=');
        pbat_text_hex(&t, value);
    }
    r->phy_read = 1;
    finish(r);
}

/* ---- step 9 ---- */
const char *pagani_usb_route(const PbuRun *r)
{
    if (!r->core_open) return "cold";
    if (!r->id_ok) return "noid";
    const uint32_t role = (r->gctl >> 12) & 3u;
    if (role == kRoleHost) return "host";
    if (role == kRoleDevice && r->phy_read && !(r->phy_common0 & kPhySiddq) && !(r->phy_ctrl5 & kPhyPor)) return "inherit";
    return "mixed";
}
static void step_done(PbuRun *r)
{
    PbatText t;
    start(r, &t, "done");
    pbat_text_ks(&t, "route", pagani_usb_route(r));
    pbat_text_ks(&t, "why", r->why);
    pbat_text_ks(&t, "gcc", !r->entry_read ? "?" : r->gcc_same ? "same" : "changed");
    if (r->id_ok) {
        pbat_text_kx(&t, "id", r->gsnpsid);
        pbat_text_kd(&t, "role", (r->gctl >> 12) & 3u);
        pbat_text_kd(&t, "run", r->dctl >> 31);
        pbat_text_kd(&t, "halt", (r->dsts >> 22) & 1u);
        pbat_text_kd(&t, "speed", r->dsts & 7u);
        pbat_text_kd(&t, "link", (r->dsts >> 18) & 15u);
    }
    if (r->phy_read) {
        pbat_text_kd(&t, "siddq", (r->phy_common0 & kPhySiddq) != 0);
        pbat_text_kd(&t, "por", (r->phy_ctrl5 & kPhyPor) != 0);
    }
    pbat_text_kd(&t, "pfail", r->pmic_failed);
    finish(r);
}

void pagani_usb_init(PbuRun *r, const PbuIo *io)
{
    pbat_zero(r, sizeof *r);
    r->io = io;
    r->why = "-";
}

static uint32_t step(PbuRun *r)
{
    switch (r->stage) {
    case StageRails:
        step_rails(r);
        r->stage = StagePmic;
        return kNext;
    case StagePmic:
        step_pmic(r);
        r->stage = StageSmmu;
        return kNext;
    case StageSmmu:
        step_smmu(r);
        r->stage = StageGcc;
        return kNext;
    case StageGcc:
        return step_gcc(r);
    case StageDwc:
        return step_dwc(r);
    case StageQscratch:
        return step_qscratch(r);
    case StageRef:
        return step_ref(r);
    case StagePhy:
        step_phy(r);
        r->stage = StageDone;
        return kNext;
    case StageDone:
        step_done(r);
        r->stage = r->smmu_banks ? StageBanks : StageOver;
        return r->smmu_banks ? PBU_SETTLE_MS : 0;
    case StageBanks:
        for (uint32_t i = 0; i < r->smmu_banks; ++i) smmu_bank(r, r->smmu_bank[i]);
        r->stage = StageOver;
        return 0;
    default:
        return 0;
    }
}

/* The steps whose reads can take the phone down: they run with the UFS log's writer held. */
static int can_hang(uint32_t stage)
{
    return stage == StageDwc || stage == StageQscratch || stage == StageRef || stage == StagePhy || stage == StageBanks;
}

uint32_t pagani_usb_step(PbuRun *r)
{
    if (can_hang(r->stage) && !r->held) {
        r->held = 1;
        r->io->quiet(r->io->ctx, 1);
        return PBU_QUIET_MS;
    }
    const uint32_t wait = step(r);
    if (r->held) {      /* a hang never gets here and keeps it held: the reset's last snapshot still goes out (UfsLog.h) */
        r->held = 0;
        r->io->quiet(r->io->ctx, 0);
    }
    return wait;
}
