/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of BatteryUsb.c against a model of what the boot loaders may have left. The model is the point: a read of a block without
 * power, clocks or reset released fails the test at once, as it would take the phone down, and what is "powered" is worked out here
 * from the raw GCC values with the SM8750 offsets written out independently of the source, so a wrong offset (the SM8550 ones) reads
 * as a register that does not exist and a dropped check reads a dead block. The rest: the order of the steps, the wait between the line
 * announcing a step and its first read, the SMMU table, the verdicts, and that every line fits one U record.
 *
 *   usb         the tests
 *   usb lines   prints the lines of a run of the usual state, for scripts/decode-usb-probe.py's check */
#include <string.h>
#include "check.h"
#include "../BatterySpmi.h"
#include "../BatteryUsb.h"

enum {kMaxLines = 40};

typedef struct {
    /* GCC, with their SM8750 offsets */
    uint32_t bcr_12000, bcr_39000, gdsc_39004, cfg_39008, master_39018, utmi_3902c, sleep_39028, noc_3908c, aggre_90, rcg_39030, urcg_39048, phygdsc_50018;
    uint32_t tcsr;                  /* 0xf206008 */
    uint32_t dwc[0x1000 / 4];
    uint32_t xhci[0x1000 / 4];
    uint32_t qscratch[0x1000 / 4];
    uint32_t phy[0x1000 / 4];
    /* the SMMU */
    uint32_t idr0, idr1, smr[256], s2cr[256], cbar[128], cba2r[128], sctlr[128], ttbr_lo[128], ttbr_hi[128], tcr[128], fsr[128];
    /* the PMIC */
    uint8_t reg[0x1000][256];
    unsigned fail_ppid;             /* reads of this peripheral fail */
    int fail_offset;                /* ... only this offset (-1: all) */
    /* the run */
    uint64_t now, line_ms, quiet_ms;
    unsigned quiet, holds;          /* the UFS log's writer is held, since quiet_ms; how many times */
    unsigned reads[PbuWinCount], pmic_reads;
    unsigned first_order[PbuWinCount], touched;
    char lines[kMaxLines][256];
    unsigned nlines, cost, done_seen, bank_lines;
} Hw;
static Hw hw;

/* Power as the hardware has it. */
static int clock_runs(uint32_t v) {return (v & 1u) && !(v & 0x80000000u);}
static int noc_clock_runs(uint32_t v) {return (v & 1u) && (!(v & 0x80000000u) || (v & 2u));}   /* hardware clock gating wakes on access */
static int dwc_powered(void)
{
    return !(hw.bcr_39000 & 1u) && (hw.gdsc_39004 & 0x80000000u) && !(hw.gdsc_39004 & 1u) && (hw.cfg_39008 & 0x10000u) && !(hw.cfg_39008 & 0x8000u) &&
           !(hw.rcg_39030 & 0x80000000u) && !(hw.urcg_39048 & 0x80000000u) && clock_runs(hw.master_39018) && clock_runs(hw.utmi_3902c) &&
           clock_runs(hw.sleep_39028) && noc_clock_runs(hw.noc_3908c) && noc_clock_runs(hw.aggre_90);
}
static int phy_powered(void)
{
    return dwc_powered() && !(hw.bcr_12000 & 1u) && (hw.tcsr & 1u) && (hw.reg[0x3c2][8] & 0x80u) && (hw.reg[0x6c3][8] & 0x80u) &&
           (hw.reg[0x5c1][8] & 0x80u);
}

static void touch(unsigned window)
{
    if (!hw.reads[window]++) hw.first_order[hw.touched++] = window;
}

/* A read that can hang: with no UFS write on the wire, so the writer is held and a round under way has ended (PanelProbe.h's rule). */
static void no_write_on_the_wire(void)
{
    CHECK(hw.quiet && hw.now - hw.quiet_ms >= 1000);
}

static uint32_t gcc(uint32_t at)
{
    switch (at) {
    case 0x12000: return hw.bcr_12000;
    case 0x39000: return hw.bcr_39000;
    case 0x39004: return hw.gdsc_39004;
    case 0x39008: return hw.cfg_39008;
    case 0x39018: return hw.master_39018;
    case 0x39028: return hw.sleep_39028;
    case 0x3902c: return hw.utmi_3902c;
    case 0x39030: return hw.rcg_39030;
    case 0x39048: return hw.urcg_39048;
    case 0x3908c: return hw.noc_3908c;
    case 0x39090: return hw.aggre_90;
    case 0x50018: return hw.phygdsc_50018;
    default: break;
    }
    fprintf(stderr, "GCC register 0x%x is not an SM8750 USB register\n", at);
    CHECK(0);
    return 0;
}

static uint32_t smmu(uint32_t at)
{
    const uint32_t pages = 1u << (((hw.idr1 >> 28) & 7u) + 1u);
    if (at == 0x20) return hw.idr0;
    if (at == 0x24) return hw.idr1;
    if (at >= 0x800 && at < 0xc00) return hw.smr[(at - 0x800) / 4];
    if (at >= 0xc00 && at < 0x1000) return hw.s2cr[(at - 0xc00) / 4];
    if (at >= 0x1000 && at < 0x1400) return hw.cbar[(at - 0x1000) / 4];
    if (at >= 0x1800 && at < 0x1c00) return hw.cba2r[(at - 0x1800) / 4];
    CHECK(at >= pages * 4096u);
    const uint32_t bank = at / 4096u - pages, in = at & 0xfffu;
    CHECK(bank < (hw.idr1 & 255u));
    /* a bank comes last, on the wait of the verdict */
    CHECK(hw.done_seen && hw.now - hw.line_ms >= 3500 - (hw.bank_lines ? 3500 : 0));
    no_write_on_the_wire();
    switch (in) {
    case 0x00: return hw.sctlr[bank];
    case 0x20: return hw.ttbr_lo[bank];
    case 0x24: return hw.ttbr_hi[bank];
    case 0x30: return hw.tcr[bank];
    case 0x58: return hw.fsr[bank];
    default: CHECK(0); return 0;
    }
}

static uint32_t hw_read(void *ctx, unsigned window, uint32_t at)
{
    (void)ctx;
    CHECK(window < PbuWinCount && !(at & 3) && at + 4 <= pagani_usb_windows[window].bytes);
    /* the first read of a block that can take the phone down: the line before it has had time to reach the UFS log */
    if ((window == PbuWinDwc || window == PbuWinQscratch || window == PbuWinTcsr || window == PbuWinPhy) && !hw.reads[window]) CHECK(hw.now - hw.line_ms >= 3500);
    if (window == PbuWinDwc || window == PbuWinQscratch || window == PbuWinTcsr || window == PbuWinPhy || window == PbuWinXhci) no_write_on_the_wire();
    touch(window);
    switch (window) {
    case PbuWinGcc: return gcc(at);
    case PbuWinTcsr: CHECK(at == 8 && dwc_powered() && hw.reads[PbuWinQscratch]); return hw.tcsr;
    case PbuWinSmmu: return smmu(at);
    case PbuWinDwc: CHECK(dwc_powered()); return hw.dwc[at / 4];
    case PbuWinXhci: CHECK(dwc_powered()); CHECK(((hw.dwc[0x110 / 4] >> 12) & 3u) == 1u); return hw.xhci[at / 4];
    case PbuWinQscratch: CHECK(dwc_powered()); CHECK(at >= 0x800 && at < 0xc00); return hw.qscratch[at / 4];
    case PbuWinPhy:
        CHECK(phy_powered());
        CHECK(at < 0x29c);
        return hw.phy[at / 4];
    default: CHECK(0); return 0;
    }
}

static int hw_pmic(void *ctx, unsigned ppid, uint8_t offset, uint8_t *value)
{
    (void)ctx;
    hw.pmic_reads++;
    /* whatever is asked must be in the list the kernel side can reach */
    CHECK(pagani_spmi_index(ppid) >= 0);
    CHECK((offset >= 0x04 && offset <= 0x10) || (offset >= 0x40 && offset <= 0x7f));
    if (ppid == hw.fail_ppid && (hw.fail_offset < 0 || hw.fail_offset == offset)) return -110;
    *value = hw.reg[ppid][offset];
    return 0;
}

static void hw_trace(void *ctx, const char *line)
{
    (void)ctx;
    const size_t n = strlen(line);
    CHECK(!strncmp(line, "[usbp] ", 7) && n < PBU_LINE);
    hw.cost += (unsigned)n + 34;     /* what a record costs in the events ring */
    hw.line_ms = hw.now;
    if (!strncmp(line, "[usbp] done", 11)) hw.done_seen = 1;
    if (!strncmp(line, "[usbp] smmu bank", 16)) hw.bank_lines++;
    CHECK(hw.nlines < kMaxLines);
    strcpy(hw.lines[hw.nlines++], line);
}

static void hw_quiet(void *ctx, unsigned on)
{
    (void)ctx;
    CHECK(on == !hw.quiet);         /* each hold is let go, and once */
    hw.quiet = on;
    hw.quiet_ms = hw.now;
    hw.holds += on;
}

static const PbuIo io = {0, hw_read, hw_pmic, hw_trace, hw_quiet};

/* The usual guess at what the boot loaders leave: USB up in device mode, stopped, as UEFI's fastboot left it. */
static void hw_reset(void)
{
    memset(&hw, 0, sizeof hw);
    hw.gdsc_39004 = 0x80222000u;
    hw.cfg_39008 = 0x10000u;
    hw.master_39018 = 0x00000001u;
    hw.utmi_3902c = 0x00000001u;
    hw.sleep_39028 = 0x00000001u;
    hw.noc_3908c = 0x80000003u;      /* idle in hardware clock gating */
    hw.aggre_90 = 0x80000003u;
    hw.rcg_39030 = 0x00000000u;
    hw.urcg_39048 = 0x00000000u;
    hw.phygdsc_50018 = 0x80222000u;
    hw.tcsr = 1;
    hw.dwc[0x120 / 4] = 0x3331190au;
    hw.dwc[0x1a0 / 4] = 0x3230302au;
    hw.dwc[0x1a4 / 4] = 0x65613031u;
    hw.dwc[0x110 / 4] = 0x00002000u; /* PRTCAPDIR 2: device */
    hw.dwc[0x704 / 4] = 0x00000000u; /* stopped */
    hw.dwc[0x70c / 4] = 0x00400000u; /* halted */
    hw.reg[0x3c2][8] = hw.reg[0x6c3][8] = hw.reg[0x5c1][8] = hw.reg[0x1cf][8] = hw.reg[0x1c5][8] = 0x80;
    hw.reg[0x3c2][0x46] = hw.reg[0x6c3][0x46] = hw.reg[0x5c1][0x46] = 0x80;
    hw.reg[0x729][0x10] = 0x11;
    hw.reg[0x729][0x52] = 0x28;
    hw.reg[0x726][0x06] = 0x83;
    hw.reg[0x7fd][0x08] = 0x80;
    hw.reg[0x7fd][0x46] = 0x80;
    hw.reg[0x72c][0x51] = 0x02;
    hw.reg[0x72c][0x53] = 0x96;
    /* the SMMU: 128 groups, 64 banks, 128 pages of global space, SID 0x40 translated by bank 5 */
    hw.idr0 = 1u << 30 | 128;
    hw.idr1 = 6u << 28 | 64;
    hw.smr[7] = 0x80000040u;
    hw.s2cr[7] = 5;
    hw.cbar[5] = 0x10000u;
    hw.cba2r[5] = 1;
    hw.sctlr[5] = 0x1085u;
    hw.ttbr_lo[5] = 0xfeed000u;
    hw.ttbr_hi[5] = 8;
    hw.smr[3] = 0x80000060u;         /* the storage stream */
    hw.s2cr[3] = 127;
    hw.fail_ppid = 0xfff;
    hw.fail_offset = -1;
    hw.now = 100000;
    hw.phy[0x54 / 4] = 0x01;         /* enabled, not in SIDDQ */
}

/* The run to its end on the simulated clock. */
static PbuRun r;
static uint32_t *moving;            /* a GCC value that changes between the driver's start and the run */
static uint32_t moved_to;
static const char *run(void)
{
    pagani_usb_init(&r, &io);
    pagani_usb_entry(&r);
    /* the driver's start reads the GCC alone */
    for (unsigned w = 0; w < PbuWinCount; ++w) CHECK(!hw.reads[w] || w == PbuWinGcc);
    CHECK(!hw.pmic_reads && hw.nlines == 1 && !strncmp(hw.lines[0], "[usbp] gcc0 ", 12));
    hw.now += PBU_DELAY_MS;
    if (moving) *moving = moved_to;
    unsigned steps = 0;
    for (;;) {
        const uint32_t wait = pagani_usb_step(&r);
        CHECK(++steps < 16);
        if (!wait) break;
        /* the wait after a step's lines is the log's: the writer takes them, and the announcement of the next step, with nothing holding it */
        CHECK(wait <= PBU_QUIET_MS || !hw.quiet);
        hw.now += wait;
    }
    CHECK(!hw.quiet);                   /* the run leaves the writer free */
    CHECK(pagani_usb_step(&r) == 0);    /* over stays over */
    return pagani_usb_route(&r);
}

static const char *find(const char *start)
{
    for (unsigned i = 0; i < hw.nlines; ++i) if (!strncmp(hw.lines[i], start, strlen(start))) return hw.lines[i];
    return 0;
}

static void closed(const char *why)
{
    char want[64];
    CHECK(!strcmp(run(), "cold") && !hw.reads[PbuWinDwc] && !hw.reads[PbuWinQscratch] && !hw.reads[PbuWinPhy] && !hw.reads[PbuWinXhci]);
    snprintf(want, sizeof want, "closed=%s", why);
    CHECK(find("[usbp] gate") && strstr(find("[usbp] gate"), want));
    CHECK(!hw.reads[PbuWinTcsr]);
    snprintf(want, sizeof want, "route=cold why=%s", why);
    CHECK(find("[usbp] done") && strstr(find("[usbp] done"), want));
}

/* The GCC of the driver's start against the later one. */
static void moved(uint32_t *field, uint32_t later, const char *word)
{
    moving = field;
    moved_to = later;
    (void)run();
    moving = 0;
    CHECK(strstr(find("[usbp] done"), word));
}
static void test_entry(void)
{
    hw_reset();
    moved(&hw.gdsc_39004, 0x80222000u, "gcc=same");
    hw_reset();
    moved(&hw.master_39018, 0x80000001u, "gcc=changed");
    CHECK(strstr(find("[usbp] done"), "route=cold why=master"));
    hw_reset();
    moved(&hw.gdsc_39004, 0x00222000u, "gcc=changed");
    /* the gated branches' status comes and goes: not a change */
    hw_reset();
    moved(&hw.noc_3908c, 0x00000003u, "gcc=same");
    hw_reset();
    moved(&hw.aggre_90, 0x00000003u, "gcc=same");
    /* their enables are */
    hw_reset();
    moved(&hw.noc_3908c, 0x80000001u, "gcc=changed");
    hw_reset();
    moved(&hw.aggre_90, 0x80000000u, "gcc=changed");
}

/* Everything up: the whole way, in order, with the wait before every step that can fault. */
static void test_usual(void)
{
    hw_reset();
    CHECK(!strcmp(run(), "inherit"));
    static const unsigned order[] = {PbuWinGcc, PbuWinSmmu, PbuWinDwc, PbuWinQscratch, PbuWinTcsr, PbuWinPhy};
    CHECK(hw.touched == sizeof order / sizeof order[0] && hw.pmic_reads > 500 && !hw.reads[PbuWinXhci]);
    CHECK(hw.holds == 5);               /* dwc, qscratch, tcsr, phy, banks */
    for (unsigned i = 0; i < hw.touched; ++i) CHECK(hw.first_order[i] == order[i]);
    /* the lines, in their order, and what they say */
    static const char *const starts[] = {"gcc0", "rail", "p7fd", "p726", "p727", "p728", "p729", "p72b", "p72c", "smmu idr0", "gcc ", "gate", "dwc",
                                         "dev", "hw", "qs", "ref", "phy", "done", "smmu bank"};
    CHECK(hw.nlines == sizeof starts / sizeof starts[0]);
    for (unsigned i = 0; i < hw.nlines; ++i) CHECK(!strncmp(hw.lines[i] + 7, starts[i], strlen(starts[i])));
    CHECK(strstr(find("[usbp] rail"), "l2d=0x8080") && strstr(find("[usbp] rail"), "l15b=0x8000") && strstr(find("[usbp] rail"), "phy=1"));
    CHECK(strstr(find("[usbp] smmu idr0"), "group=7 smr=0x80000040 s2cr=0x5 found=1"));
    CHECK(strstr(find("[usbp] smmu bank"), "n=5 cbar=0x10000 cba2r=0x1 sctlr=0x1085 ttbr0=0x80feed000"));
    CHECK(strstr(find("[usbp] gcc "), "gdsc=0x80222000") && strstr(find("[usbp] gcc "), "noc=0x80000003"));
    CHECK(strstr(find("[usbp] gate"), "phybcr=0x0 phygdsc=0x80222000 next=dwc") && strstr(find("[usbp] ref"), "tcsr=0x1 next=phy"));
    CHECK(strstr(find("[usbp] dwc"), "id=0x3331190a ver=0x3230302a type=0x65613031 gctl=0x2000"));
    CHECK(strstr(find("[usbp] hw"), "next=qs") && strstr(find("[usbp] qs"), "next=tcsr"));
    CHECK(strstr(find("[usbp] done"), "route=inherit why=- gcc=same id=0x3331190a role=2 run=0 halt=1 speed=0 link=0 siddq=0 por=0 pfail=0"));
    /* a whole run costs under 5 KiB of the 32 KiB events ring */
    CHECK(hw.cost < 5120);
    /* the repeater's window has what was asked for: 08 and 46, 51-57 */
    CHECK(!strncmp(find("[usbp] p7fd"), "[usbp] p7fd 04=", 15));
    const char *p7fd = find("[usbp] p7fd");
    CHECK(!strncmp(p7fd + 15 + 2 * (8 - 4), "80", 2));
    const char *hi = strstr(p7fd, " 40=");
    CHECK(hi && !strncmp(hi + 4 + 2 * 6, "80", 2) && strlen(hi + 4) == 2 * 64);
}

/* Each condition of the gate alone keeps every later block unread. */
static void test_gate(void)
{
#define CLOSE(field, value, why) do {hw_reset(); hw.field = (value); closed(why);} while (0)
    CLOSE(bcr_39000, 1, "reset");
    CLOSE(gdsc_39004, 0x00222000u, "gdsc");
    CLOSE(gdsc_39004, 0x80222001u, "collapse");
    CLOSE(cfg_39008, 0x00000u, "gdsc-cfg");
    CLOSE(cfg_39008, 0x18000u, "gdsc-cfg");
    CLOSE(rcg_39030, 0x80000000u, "root");
    CLOSE(urcg_39048, 0x80000000u, "utmi-root");
    CLOSE(master_39018, 0x80000001u, "master");
    CLOSE(master_39018, 0x80000003u, "master");      /* hardware clock gating does not excuse the other three */
    CLOSE(master_39018, 0, "master");
    CLOSE(aggre_90, 0x80000000u, "aggre");
    CLOSE(aggre_90, 0x80000002u, "aggre");           /* gating without the enable: nothing wakes it */
    CLOSE(noc_3908c, 0x80000000u, "noc");
    CLOSE(noc_3908c, 0x80000002u, "noc");
    CLOSE(utmi_3902c, 0x80000001u, "utmi");
    CLOSE(utmi_3902c, 0x80000003u, "utmi");
    CLOSE(sleep_39028, 0x80000001u, "sleep");
    CLOSE(sleep_39028, 0x80000003u, "sleep");
    /* all off, as a cold port */
    hw_reset();
    hw.gdsc_39004 = 0x00222001u; hw.cfg_39008 = 0x8000u;
    hw.master_39018 = hw.utmi_3902c = hw.sleep_39028 = hw.noc_3908c = hw.aggre_90 = 0x80000000u;
    hw.rcg_39030 = hw.urcg_39048 = 0x80000000u;
    closed("gdsc");
    /* the two NOC branches running outright open it too */
    hw_reset();
    hw.noc_3908c = hw.aggre_90 = 1;
    CHECK(!strcmp(run(), "inherit"));
}

static void test_dwc(void)
{
    /* not a Synopsys id: nothing further of the block, and the glue and the PHY stay unread */
    static const uint32_t ids[] = {0, 0xffffffffu, 0xdeadbeefu, 0x12345678u};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; ++i) {
        hw_reset();
        hw.dwc[0x120 / 4] = ids[i];
        CHECK(!strcmp(run(), "noid") && hw.reads[PbuWinDwc] == 1 && !hw.reads[PbuWinQscratch] && !hw.reads[PbuWinPhy]);
        CHECK(strstr(find("[usbp] done"), "route=noid why=id"));
    }
    /* DWC_usb3: no version registers of DWC_usb31 */
    hw_reset();
    hw.dwc[0x120 / 4] = 0x5533330au;
    CHECK(!strcmp(run(), "inherit") && !strstr(find("[usbp] dwc"), "ver="));
    hw_reset();
    hw.dwc[0x120 / 4] = 0x3332190au;
    CHECK(!strcmp(run(), "inherit") && strstr(find("[usbp] dwc"), "ver="));
    /* host mode: the xHCI command and status are read, the route says so */
    hw_reset();
    hw.dwc[0x110 / 4] = 0x00001000u;
    hw.xhci[0] = 0x01200020u;
    hw.xhci[0x20 / 4] = 1;
    hw.xhci[0x24 / 4] = 0x18u;
    CHECK(!strcmp(run(), "host") && strstr(find("[usbp] xhci"), "cap=0x1200020 cmd=0x1 sts=0x18"));
    /* ... a capability length that makes no sense: only that word */
    hw_reset();
    hw.dwc[0x110 / 4] = 0x00001000u;
    hw.xhci[0] = 0x01200023u;
    CHECK(!strcmp(run(), "host") && hw.reads[PbuWinXhci] == 1);
    /* OTG role, or the PHY powered down: not the inheritance */
    hw_reset();
    hw.dwc[0x110 / 4] = 0x00003000u;
    CHECK(!strcmp(run(), "mixed"));
    hw_reset();
    hw.phy[0x54 / 4] = 0x05;
    CHECK(!strcmp(run(), "mixed") && strstr(find("[usbp] done"), "siddq=1"));
    hw_reset();
    hw.phy[0x50 / 4] = 0x02;
    CHECK(!strcmp(run(), "mixed") && strstr(find("[usbp] done"), "por=1"));
}

/* The PHY: every condition of its own gate; the rest of the run is not hurt. */
static void test_phy(void)
{
    static const unsigned rails[] = {0x3c2, 0x6c3, 0x5c1};
    for (unsigned i = 0; i < 3; ++i) {
        hw_reset();
        hw.reg[rails[i]][8] = 0;
        CHECK(!strcmp(run(), "mixed") && !hw.reads[PbuWinPhy] && hw.reads[PbuWinQscratch]);
        CHECK(strstr(find("[usbp] qs"), "phy=rail") && strstr(find("[usbp] done"), "why=rail") && strstr(find("[usbp] rail"), "phy=0"));
        /* a rail that cannot be read is not a rail that is on */
        hw_reset();
        hw.fail_ppid = rails[i];
        hw.fail_offset = 8;
        CHECK(!strcmp(run(), "mixed") && !hw.reads[PbuWinPhy] && strstr(find("[usbp] rail"), "=-110"));
        hw_reset();
        hw.fail_ppid = rails[i];
        hw.fail_offset = 0x46;
        CHECK(!strcmp(run(), "mixed") && !hw.reads[PbuWinPhy]);
    }
    /* the repeater's LDOs are not the PHY's */
    hw_reset();
    hw.reg[0x1cf][8] = 0;
    CHECK(!strcmp(run(), "inherit"));
    hw_reset();
    hw.tcsr = 0;
    CHECK(!strcmp(run(), "mixed") && !hw.reads[PbuWinPhy] && strstr(find("[usbp] done"), "why=ref") && strstr(find("[usbp] ref"), "phy=ref"));
    hw_reset();
    hw.bcr_12000 = 1;
    CHECK(!strcmp(run(), "mixed") && !hw.reads[PbuWinPhy] && !hw.reads[PbuWinTcsr] && strstr(find("[usbp] done"), "why=reset"));
    /* the rails closed: the block never read on XNU stays unread too */
    hw_reset();
    hw.reg[0x3c2][8] = 0;
    CHECK(!strcmp(run(), "mixed") && !hw.reads[PbuWinTcsr]);
}

static void test_smmu(void)
{
    /* no entry for the stream */
    hw_reset();
    hw.smr[7] = 0x80000041u;
    CHECK(!strcmp(run(), "inherit") && strstr(find("[usbp] smmu idr0"), "found=0") && !find("[usbp] smmu bank"));
    /* an invalid entry does not count */
    hw_reset();
    hw.smr[7] = 0x00000040u;
    CHECK(strstr((run(), find("[usbp] smmu idr0")), "found=0"));
    /* a mask that covers the stream counts; one that does not, not */
    hw_reset();
    hw.smr[7] = 0x80030041u;
    CHECK(strstr((run(), find("[usbp] smmu idr0")), "found=1"));
    hw_reset();
    hw.smr[7] = 0x80010042u;
    CHECK(strstr((run(), find("[usbp] smmu idr0")), "found=0"));
    /* bypass and fault entries have no bank */
    hw_reset();
    hw.s2cr[7] = 1u << 16;
    CHECK(strstr((run(), find("[usbp] smmu idr0")), "s2cr=0x10000 found=1") && !find("[usbp] smmu bank"));
    hw_reset();
    hw.s2cr[7] = 2u << 16 | 5;
    (void)run();
    CHECK(!find("[usbp] smmu bank"));
    /* a bank past the end is not read */
    hw_reset();
    hw.s2cr[7] = 64;
    (void)run();
    CHECK(strstr(find("[usbp] smmu idr0"), "found=1") && !find("[usbp] smmu bank"));
    /* the bank even when the run ended early: a closed gate does not keep the table's answer back; after the verdict all the same */
    hw_reset();
    hw.gdsc_39004 = 0;
    CHECK(!strcmp(run(), "cold") && find("[usbp] smmu bank") && find("[usbp] smmu bank") > find("[usbp] done"));
    /* three entries: two shown, all counted */
    hw_reset();
    hw.smr[9] = 0x80000040u; hw.s2cr[9] = 6;
    hw.smr[20] = 0x80000040u; hw.s2cr[20] = 7;
    (void)run();
    CHECK(strstr(find("[usbp] smmu idr0"), "group=7") && strstr(find("[usbp] smmu idr0"), "group=9") && !strstr(find("[usbp] smmu idr0"), "group=20"));
    CHECK(strstr(find("[usbp] smmu idr0"), "found=3"));
    /* more groups than the 128 that work: the table stops there */
    hw_reset();
    hw.idr0 = 1u << 30 | 200;
    hw.smr[150] = 0x80000040u;
    hw.smr[7] = 0;
    (void)run();
    CHECK(strstr(find("[usbp] smmu idr0"), "found=0"));
    /* a geometry the mapping does not cover: nothing of the tables */
    static const uint32_t bad[][2] = {{1u << 30 | 128, 1u << 31 | 64}, {1u << 30, 6u << 28 | 64}, {1u << 30 | 128, 7u << 28 | 64}, {1u << 30 | 128, 6u << 28 | 1},
                                      {1u << 30 | 128, 6u << 28 | 200}, {1u << 30 | 128, 7u << 28 | 128}};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        hw_reset();
        hw.idr0 = bad[i][0];
        hw.idr1 = bad[i][1];
        (void)run();
        CHECK(strstr(find("[usbp] smmu idr0"), "refused") && hw.reads[PbuWinSmmu] == 2);
    }
}

static void test_pmic(void)
{
    /* a peripheral without a channel: a short line, and the run goes on */
    hw_reset();
    hw.fail_ppid = 0x72c;
    CHECK(!strcmp(run(), "inherit") && strstr(find("[usbp] p72c"), "none=-110") && strlen(find("[usbp] p72c")) < 40);
    CHECK(strstr(find("[usbp] done"), "pfail=1"));
    /* one register that cannot be read shows as such, the others stay */
    hw_reset();
    hw.fail_ppid = 0x726;
    hw.fail_offset = 0x45;
    CHECK(!strcmp(run(), "inherit") && strstr(find("[usbp] p726"), "--") && strstr(find("[usbp] done"), "pfail=1"));
}

/* The longest values everywhere: every line still fits one record, none is cut. */
static void test_lengths(void)
{
    hw_reset();
    memset(hw.dwc, 0xff, sizeof hw.dwc);
    memset(hw.phy, 0xff, sizeof hw.phy);
    memset(hw.qscratch, 0xff, sizeof hw.qscratch);
    memset(hw.xhci, 0xff, sizeof hw.xhci);
    memset(hw.reg, 0xff, sizeof hw.reg);
    hw.dwc[0x120 / 4] = 0x3332ffffu;
    hw.dwc[0x110 / 4] = 0xffffdfffu;   /* host, for the xHCI line */
    hw.xhci[0] = 0xffffff20u;
    hw.master_39018 = hw.utmi_3902c = hw.sleep_39028 = 0x7fffffffu;
    hw.noc_3908c = hw.aggre_90 = 0xffffffffu;
    hw.rcg_39030 = hw.urcg_39048 = 0x7fffffffu;
    hw.bcr_39000 = hw.bcr_12000 = 0xfffffffeu;
    hw.gdsc_39004 = 0xfffffffeu;
    hw.cfg_39008 = 0xffff7fffu;
    hw.tcsr = 0xffffffffu;
    hw.phygdsc_50018 = 0xffffffffu;
    hw.smr[7] = hw.smr[9] = 0x80000040u;
    hw.s2cr[7] = 0xfffc003fu;
    hw.s2cr[9] = 0xfffc003fu;
    hw.smr[120] = 0x80000040u;
    hw.cbar[63] = hw.cba2r[63] = hw.sctlr[63] = hw.ttbr_lo[63] = hw.ttbr_hi[63] = hw.tcr[63] = hw.fsr[63] = 0xffffffffu;
    (void)run();
    size_t longest = 0;
    for (unsigned i = 0; i < hw.nlines; ++i) {
        const size_t n = strlen(hw.lines[i]);
        if (n > longest) longest = n;
    }
    CHECK(longest < PBU_LINE - 1);     /* one short of the buffer: nothing was dropped */
    CHECK(hw.cost < 6144);
    printf("longest line %zu characters, a worst run %u bytes of the events ring, %u lines\n", longest, hw.cost, hw.nlines);
}

static void test_windows(void)
{
    /* the windows are what the block facts say; none reaches the EUD's 0x88e0000..0x88e2fff or 0xc278000 */
    static const uint64_t bases[PbuWinCount] = {0x100000, 0xf206000, 0xa600000, 0xa60c000, 0xa6f8000, 0x88e3000, 0x15000000};
    for (unsigned i = 0; i < PbuWinCount; ++i) {
        const uint64_t start = pagani_usb_windows[i].base, end = start + pagani_usb_windows[i].bytes;
        CHECK(start == bases[i] && !(start & 0xfff) && !(pagani_usb_windows[i].bytes & 0xfff));
        CHECK(end <= 0x88e0000 || start >= 0x88e3000);
        CHECK(end <= 0xc278000 || start >= 0xc279000);
    }
    CHECK(PBU_LINE == 200 && PBU_SETTLE_MS > 3000 && PBU_DELAY_MS >= 60000);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "lines")) {
        hw_reset();
        (void)run();
        for (unsigned i = 0; i < hw.nlines; ++i) puts(hw.lines[i]);
        return 0;
    }
    test_usual();
    test_entry();
    test_gate();
    test_dwc();
    test_phy();
    test_smmu();
    test_pmic();
    test_lengths();
    test_windows();
    printf("PASS usb: %u checks\n", checks);
    return 0;
}
