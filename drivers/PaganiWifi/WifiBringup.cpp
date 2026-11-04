// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Ported from UniXNU-sheng package/WifiProbe.cpp (commit 08cde20e): wifiThread, bringUp, endpointProbe; WifiBringup.h says what
// changed. Register facts: PaganiWifiTables.h (each with its source); no code of a GPL or vendor driver is used.
#include "WifiBringup.h"
#include "WifiUtil.h"

namespace pagani_wifi {
namespace {

constexpr unsigned kPinCount = sizeof kPins / sizeof kPins[0];
constexpr unsigned kRailCount = sizeof kRails / sizeof kRails[0];
constexpr unsigned kBranchCount = sizeof kBranches / sizeof kBranches[0];

uint32_t rd(Bringup &b, unsigned win, uint32_t off) {return b.hw->read(b.hw->ctx, win, off);}
void wr(Bringup &b, unsigned win, uint32_t off, uint32_t value) {b.hw->write(b.hw->ctx, win, off, value);}
void setBits(Bringup &b, unsigned win, uint32_t off, uint32_t bits) {wr(b, win, off, rd(b, win, off) | bits);}
void clearBits(Bringup &b, unsigned win, uint32_t off, uint32_t bits) {wr(b, win, off, rd(b, win, off) & ~bits);}

void line(Bringup &b, const char *text)
{
    if (b.hw->log) b.hw->log(b.hw->ctx, text);
}

// Records a step; a step the options say to stop after ends the stage there.
int done(Bringup &b, int step)
{
    b.step = step;
    if (b.opt.stopAfter != step) return 0;
    letGo(b);
    return kErrStopped;
}
int failed(Bringup &b, int step, int error, const char *what)
{
    b.step = -step;
    b.error = error;
    char text[160];
    PwText t;
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] FAIL");
    pw_text_kd(&t, "step", step);
    pw_text_kd(&t, "err", error);
    pw_text_char(&t, ' ');
    pw_text_str(&t, what);
    line(b, text);
    letGo(b);
    return error;
}

const Pin &pin(const char *name)
{
    for (const Pin &p : kPins) if (pw_streq(p.name, name)) return p;
    return kPins[0];
}
uint32_t pinCtl(const Pin &p) {return p.gpio * kTlmmPinStride;}
uint32_t pinIo(const Pin &p) {return p.gpio * kTlmmPinStride + 4;}
// Output level first, then the configuration, then the level again (sheng's setPin): the pad never drives the old level once enabled.
void setPin(Bringup &b, const Pin &p, uint32_t config, int level)
{
    if (level >= 0) wr(b, kWinTlmm, pinIo(p), level ? 2u : 0u);
    wr(b, kWinTlmm, pinCtl(p), config);
    if (level >= 0) wr(b, kWinTlmm, pinIo(p), level ? 2u : 0u);
}

uint32_t readHalts(Bringup &b)
{
    uint32_t off = 0;
    for (unsigned i = 0; i < kBranchCount; ++i) if (rd(b, kWinGcc, kBranches[i].haltReg) & kBranchOff) off |= 1u << i;
    return off;
}
unsigned branchIndex(const char *name)
{
    for (unsigned i = 0; i < kBranchCount; ++i) if (pw_streq(kBranches[i].name, name)) return i;
    return 0;
}
// The branches the controller's registers need running before a read of PARF or the DBI (a read of an unclocked block stalls the
// bus): sheng's kNeeded, by name.
uint32_t neededMask()
{
    return 1u << branchIndex("pcie_0_slv_axi") | 1u << branchIndex("pcie_0_mstr_axi") | 1u << branchIndex("pcie_0_cfg_ahb") |
           1u << branchIndex("pcie_0_aux");
}

bool waitClear(Bringup &b, unsigned win, uint32_t off, uint32_t mask, unsigned polls, unsigned stepUs, uint32_t *used = nullptr)
{
    unsigned i = 0;
    for (; i < polls && (rd(b, win, off) & mask); ++i) b.hw->udelay(b.hw->ctx, stepUs);
    if (used) *used = i;
    return !(rd(b, win, off) & mask);
}

void logRail(Bringup &b, const char *name, const char *when, uint16_t ppid, const RailResult &r)
{
    char text[160];
    PwText t;
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] rail ");
    pw_text_str(&t, name);
    pw_text_char(&t, ' ');
    pw_text_str(&t, when);
    pw_text_kx(&t, "ppid", ppid);
    if (r.read) {
        pw_text_kx(&t, "status", r.status);
        pw_text_kx(&t, "en", r.enable);
        pw_text_kx(&t, "mode", r.mode);
        pw_text_kd(&t, "mv", r.millivolts);
    } else {
        pw_text_str(&t, " unread");
    }
    if (r.voted) pw_text_kd(&t, "voted", r.voted);
    if (r.skipped) pw_text_str(&t, " kept");
    if (r.address) pw_text_kx(&t, "addr", r.address);
    if (r.rc) pw_text_kd(&t, "rc", r.rc);
    line(b, text);
}
void railLine(Bringup &b, const char *when, unsigned i) {logRail(b, kRails[i].resource, when, kRails[i].ppid, b.rails[i]);}

void readPmic(Bringup &b, uint16_t ppid, RailResult &r)
{
    r.read = 0;
    if (!b.hw->pmic || !ppid) return;
    uint8_t status = 0, enable = 0, mode = 0;
    uint16_t mv = 0;
    if (b.hw->pmic(b.hw->ctx, ppid, &status, &enable, &mode, &mv)) return;
    r.read = 1;
    r.status = status;
    r.enable = enable;
    r.mode = mode;
    r.millivolts = mv;
}
void readRail(Bringup &b, unsigned i) {readPmic(b, kRails[i].ppid, b.rails[i]);}

// S7I: the UFS vccq parent (qcom,vccq-parent-supply), and in pagani-linux's description the input of L3B and L11B.
bool isSharedWithUfs(const Rail &rail) {return pw_streq(rail.resource, "smpi7");}
unsigned railOfUfs()
{
    for (unsigned i = 0; i < kRailCount; ++i) if (isSharedWithUfs(kRails[i])) return i;
    return 0;
}
// Sets the bits of a branch vote register, under the lock word: nothing is written when they all are set, and the read and the write
// stay as close as they can (0x52000 also carries the GPU's GPLL0 votes, and Sm8750GpuPower does not take the lock word for them).
void setVote(Bringup &b, uint32_t off, uint32_t bits)
{
    const uint32_t now = rd(b, kWinGcc, off);
    if ((now & bits) != bits) wr(b, kWinGcc, off, now | bits);
}

}  // namespace

void letGo(Bringup &b)
{
    if (!b.held) return;
    b.held = false;
    b.hw->quiet(b.hw->ctx, 0);
}

void guard(Bringup &b, const char *what)
{
    if (!b.hw->quiet) return;
    letGo(b);
    char text[80];
    PwText t;
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] guard ");
    pw_text_str(&t, what);
    line(b, text);
    if (!b.opt.crumbMs) return;
    b.hw->msleep(b.hw->ctx, b.opt.crumbMs);
    b.held = true;
    b.hw->quiet(b.hw->ctx, 1);
    b.hw->msleep(b.hw->ctx, kQuietMs);
}

uint8_t bdfHash(uint16_t bdf)
{
    uint8_t crc = 0;
    const uint8_t bytes[2] = {uint8_t(bdf >> 8), uint8_t(bdf)};
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? uint8_t((crc << 1) ^ kBdfHashPoly) : uint8_t(crc << 1);
    }
    return crc;
}

uint32_t pinConfig(int pull, unsigned function, unsigned driveMa, bool output)
{
    const uint32_t pullBits = pull < 0 ? 1u : pull > 0 ? 3u : 0u;
    const uint32_t drive = driveMa >= 2 && driveMa <= 16 ? driveMa / 2 - 1 : 0u;
    return pullBits | (function & 0xfu) << 2 | (drive & 7u) << 6 | (output ? 1u << 9 : 0u);
}

uint32_t bcmVote(uint32_t kbps, uint32_t width, uint32_t buswidth, uint32_t unit)
{
    if (!kbps || !unit || !buswidth) return 0;
    const uint64_t scaled = (uint64_t(kbps) * width + buswidth - 1) / buswidth;
    const uint64_t vote = (scaled * 1000 + unit - 1) / unit;
    return vote < 1 ? 1 : vote > 0x3fff ? 0x3fff : uint32_t(vote);
}

// ---- stage 1: what the boot loaders left (reads only) ----
int survey(Bringup &b)
{
    if (!b.hw || !b.hw->read || !b.hw->write || !b.hw->udelay || !b.hw->msleep || !b.hw->now_ms) return kErrArgument;
    b.vote52008 = rd(b, kWinGcc, 0x52008);
    b.vote52000 = rd(b, kWinGcc, 0x52000);
    b.gdsc = rd(b, kWinGcc, kGdscPcie0);
    b.phyGdsc = rd(b, kWinGcc, kGdscPcie0Phy);
    b.collapse = rd(b, kWinGcc, kGdscCollapseVote);
    b.halts = readHalts(b);
    char text[200];
    PwText t;
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] survey");
    pw_text_kx(&t, "v52008", b.vote52008);
    pw_text_kx(&t, "v52000", b.vote52000);
    pw_text_kx(&t, "gdsc", b.gdsc);
    pw_text_kx(&t, "phygdsc", b.phyGdsc);
    pw_text_kx(&t, "collapse", b.collapse);
    pw_text_kx(&t, "halts", b.halts);
    line(b, text);
    int rc = done(b, kStepSurveyClocks);
    if (rc) return rc;

    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] pins");
    for (unsigned i = 0; i < kPinCount; ++i) {
        b.pinCtl[i] = rd(b, kWinTlmm, pinCtl(kPins[i]));
        b.pinIo[i] = rd(b, kWinTlmm, pinIo(kPins[i]));
        pw_text_char(&t, ' ');
        pw_text_str(&t, kPins[i].name);
        pw_text_char(&t, '=');
        pw_text_hex(&t, b.pinCtl[i] & 0x3fff);
        pw_text_char(&t, '/');
        pw_text_udec(&t, b.pinIo[i] & 3);
    }
    line(b, text);
    if ((rc = done(b, kStepSurveyPins))) return rc;

    for (unsigned i = 0; i < kRailCount; ++i) {
        readRail(b, i);
        railLine(b, "now", i);
    }
    // The PCIe PHY's LDOs are the UFS path's: never voted here, only looked at.
    for (unsigned i = 0; i < sizeof kPhyRails / sizeof kPhyRails[0]; ++i) {
        readPmic(b, kPhyRails[i].ppid, b.phyRails[i]);
        logRail(b, kPhyRails[i].resource, "phy", kPhyRails[i].ppid, b.phyRails[i]);
    }
    b.sleepClock = b.hw->sleep_clock ? b.hw->sleep_clock(b.hw->ctx, 0) : -1;
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] sleep clock pin");
    pw_text_kd(&t, "on", b.sleepClock);
    line(b, text);
    if ((rc = done(b, kStepSurveyRails))) return rc;

    if ((b.gdsc & kGdscPwrOn) && !(b.halts & neededMask())) {
        guard(b, "controller");
        b.parfLtssm = rd(b, kWinParf, parf::kLtssm);
        b.parfPhyCtrl = rd(b, kWinParf, parf::kPhyCtrl);
        b.rootId = rd(b, kWinDbi, 0);
        b.rootLink = rd(b, kWinDbi, dbi::kLinkControlStatus);
        pw_text_start(&t, text, sizeof text);
        pw_text_str(&t, "[wifi] controller");
        pw_text_kx(&t, "ltssm", b.parfLtssm);
        pw_text_kx(&t, "phyctrl", b.parfPhyCtrl);
        pw_text_kx(&t, "root", b.rootId);
        pw_text_kx(&t, "link", b.rootLink);
        line(b, text);
        letGo(b);
    } else {
        line(b, "[wifi] controller not read: GDSC off or its clocks stopped");
    }
    return done(b, kStepSurveyController);
}

// ---- stage 2: power ----
int powerOn(Bringup &b)
{
    int rc = survey(b);
    if (rc) return rc;
    char text[160];
    PwText t;

    // From the sleep clock pin's first write through the votes: S7I carries the UFS rail.
    guard(b, "rails");

    // The 32 kHz sleep clock the PMU needs (PMK8550 gpio3, func1): Android's PMIC pin control sets it; sheng found it unset.
    // Not switched (another EE owns the pin, or it is not the GPIO kind expected) is only reported: the boot loader may have left
    // it running, and the link step says whether the PMU came up.
    if (b.opt.sleepClock && b.hw->sleep_clock) {
        const int on = b.hw->sleep_clock(b.hw->ctx, 1);
        b.sleepClock = on;
        pw_text_start(&t, text, sizeof text);
        pw_text_str(&t, on == 2 ? "[wifi] sleep clock pin switched on" : on == 1 ? "[wifi] sleep clock pin already on" :
                                                                         "[wifi] WARN sleep clock pin not switched on");
        pw_text_kd(&t, "rc", on);
        line(b, text);
    }
    if ((rc = done(b, kStepSleepClock))) return rc;

    // The seven rails, in cnss2's order, each with three requests as sheng learned (mode, voltage, enable). No rail ever gets a lower
    // voltage than its PMIC reports, none a disable. A set point above the cnss range is left alone: it is not ours to lower and, read
    // wrong, not a value to send. S7I feeds UFS: nothing at all is sent, on any rail, when its state cannot be read (unless
    // pagani-wifi-s7i=1), so that the chip's load never lands on a rail nobody has looked at. Once read, it gets its voltage only when
    // below what peach needs and its enable only when off, but always the mode: the devicetree leaves S7I in its low mode (qcom,init-mode
    // 1, HPM from 200 mA, which only UFS's own 210 mA load vote reaches; cnss2 votes no load), this kernel votes no load at all, and
    // peach's RF and PCIe supplies on a rail left in a low mode would sag the UFS rail with them. The mode only ever goes up.
    if (!b.hw->vrm) return failed(b, kStepRails, kErrRail, "no RPMh");
    const unsigned ufs = railOfUfs();
    readRail(b, ufs);
    const RailResult ufsBefore = b.rails[ufs];
    if (!ufsBefore.read && !b.opt.s7iVote) {
        railLine(b, "unread", ufs);
        return failed(b, kStepRails, kErrRailRead, "S7I not read: no rail voted");
    }
    for (unsigned order = 1; order <= kRailCount; ++order) {
        unsigned i = 0;
        while (i < kRailCount && kRails[i].order != order) ++i;
        if (i == kRailCount) continue;
        const Rail &rail = kRails[i];
        RailResult &r = b.rails[i];
        readRail(b, i);
        uint32_t mv = rail.chosenMv;
        bool setVoltage = true, setEnable = true;
        if (r.read && r.millivolts > mv) {
            if (r.millivolts <= rail.maxMv) mv = r.millivolts;
            else setVoltage = false;
        }
        if (isSharedWithUfs(rail) && r.read) {
            if (r.millivolts >= rail.chosenMv) setVoltage = false;
            if (r.enable & 0x80) setEnable = false;
        }
        uint32_t address = 0;
        rc = b.hw->vrm(b.hw->ctx, rail.resource, 8, 7, &address);
        if (!rc && setVoltage) rc = b.hw->vrm(b.hw->ctx, rail.resource, 0, mv, &address);
        if (!rc && setEnable) rc = b.hw->vrm(b.hw->ctx, rail.resource, 4, 1, &address);
        r.address = address;
        r.rc = rc;
        r.voted = setVoltage ? uint16_t(mv) : 0;
        r.skipped = !setVoltage;
        if (rc) {
            railLine(b, "vote failed", i);
            return failed(b, kStepRails, kErrRail, rail.resource);
        }
    }
    b.hw->msleep(b.hw->ctx, 5);
    for (unsigned i = 0; i < kRailCount; ++i) {
        readRail(b, i);
        railLine(b, "after", i);
    }
    // Nothing goes on with S7I lower or off than it was: WLAN_EN stays low and the chip draws nothing from it.
    const RailResult &ufsAfter = b.rails[ufs];
    if (ufsBefore.read && ufsAfter.read && (ufsAfter.millivolts < ufsBefore.millivolts || ((ufsBefore.enable & 0x80) && !(ufsAfter.enable & 0x80))))
        return failed(b, kStepRails, kErrRail, "S7I dropped");
    if (ufsAfter.read && ufsAfter.millivolts < kRails[ufs].chosenMv) line(b, "[wifi] WARN S7I below what peach needs");
    if ((rc = done(b, kStepRails))) return rc;

    // The endpoint's DMA crosses the PCIe NoC: its bus clock manager (SN4) at the cnss "high" level (avg/peak of path 0).
    if (b.hw->bcm) {
        const BwLevel &level = kBusBw[4];
        rc = b.hw->bcm(b.hw->ctx, kPcieNocBcm, level.avg0, level.peak0, 8, &b.busVote);
        pw_text_start(&t, text, sizeof text);
        pw_text_str(&t, "[wifi] bus vote ");
        pw_text_str(&t, kPcieNocBcm);
        pw_text_kx(&t, "data", b.busVote);
        pw_text_kd(&t, "rc", rc);
        line(b, text);
        if (rc) return failed(b, kStepBus, kErrBus, "PCIe NoC vote");
    }
    if ((rc = done(b, kStepBus))) return rc;

    if (b.opt.pdc && b.hw->pdc) {
        for (const char *message : kPdcInit) {
            rc = b.hw->pdc(b.hw->ctx, message);
            if (rc) return failed(b, kStepPdc, kErrPdc, message);
            ++b.pdcSent;
        }
        if (b.opt.pdc >= 2)
            for (const char *message : kPdcOlCprPagani) {
                rc = b.hw->pdc(b.hw->ctx, message);
                if (rc) return failed(b, kStepPdc, kErrPdc, message);
                ++b.pdcSent;
            }
    }
    if ((rc = done(b, kStepPdc))) return rc;

    // The chip's load lands on S7I with WLAN_EN; the GDSCs, the TCSR page (read for the first time) and the clocks come with it.
    guard(b, "wlan_en");

    // PERST# held low, CLKREQ# to its PCIe function, WLAN_EN low long enough for whatever an earlier start left to stop, then high
    // and the PMU's power-up time (pwrseq 50 ms; sheng waited 150 ms low and 60 ms high).
    const Pin &perst = pin("perst"), &clkreq = pin("clkreq"), &wlanEn = pin("wlan_en");
    setPin(b, perst, pinConfig(perst.pull, 0, perst.driveMa, true), 0);
    b.perstAssertedMs = b.hw->now_ms(b.hw->ctx);
    setPin(b, clkreq, pinConfig(clkreq.pull, clkreq.func, clkreq.driveMa, false), -1);
    setPin(b, wlanEn, pinConfig(-1, 0, 2, true), 0);
    b.hw->msleep(b.hw->ctx, 150);
    setPin(b, wlanEn, pinConfig(wlanEn.pull, 0, wlanEn.driveMa, true), 1);
    b.hw->msleep(b.hw->ctx, kPmuPowerUpDelayMs + 10);
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] wlan_en");
    pw_text_kx(&t, "ctl", rd(b, kWinTlmm, pinCtl(wlanEn)));
    pw_text_kx(&t, "io", rd(b, kWinTlmm, pinIo(wlanEn)));
    pw_text_kx(&t, "perst", rd(b, kWinTlmm, pinCtl(perst)));
    pw_text_kx(&t, "clkreq", rd(b, kWinTlmm, pinCtl(clkreq)));
    line(b, text);
    if ((rc = done(b, kStepWlanEn))) return rc;

    // Both GDSCs are votable: a set bit in the collapse vote register keeps one collapsed (PCIe0 bit 0, its PHY bit 2).
    clearBits(b, kWinGcc, kGdscCollapseVote, kCollapsePcie0 | kCollapsePcie0Phy);
    unsigned tries = 0;
    while (tries++ < 100 && (!(rd(b, kWinGcc, kGdscPcie0) & kGdscPwrOn) || !(rd(b, kWinGcc, kGdscPcie0Phy) & kGdscPwrOn)))
        b.hw->udelay(b.hw->ctx, 10);
    b.gdsc = rd(b, kWinGcc, kGdscPcie0);
    b.phyGdsc = rd(b, kWinGcc, kGdscPcie0Phy);
    if (!(b.gdsc & kGdscPwrOn) || !(b.phyGdsc & kGdscPwrOn)) return failed(b, kStepGdsc, kErrGdsc, "GDSC not on");
    if ((rc = done(b, kStepGdsc))) return rc;

    // Sources first (an RCG takes a new CFG on CMD bit 0), the pipe mux parked on TCXO until the PHY runs, then the branch votes
    // (under the shared lock word), then the PHY reference clock from TCSR.
    wr(b, kWinGcc, kAuxRcg + 4, kAuxCfg19p2MHz);
    setBits(b, kWinGcc, kAuxRcg, 1u);
    wr(b, kWinGcc, kRchngRcg + 4, kRchngCfg100MHz);
    setBits(b, kWinGcc, kRchngRcg, 1u);
    // The roots are off here (no branch votes them yet): sheng never looked at the result of these two updates, and Linux's ops for these
    // RCGs do not even ask for one while the clock is off. An update still pending now is judged after the branch votes.
    (void)waitClear(b, kWinGcc, kAuxRcg, 1u, 100, 10);
    (void)waitClear(b, kWinGcc, kRchngRcg, 1u, 100, 10);
    wr(b, kWinGcc, kPipeMux, (rd(b, kWinGcc, kPipeMux) & ~3u) | kPipeMuxXo);
    if (b.hw->vote_lock && b.hw->vote_lock(b.hw->ctx, 1)) return failed(b, kStepClocks, kErrLock, "GCC vote lock busy");
    setVote(b, 0x52008, kVote52008Bits);
    setVote(b, 0x52000, kVote52000Bits);
    if (b.hw->vote_lock) b.hw->vote_lock(b.hw->ctx, 0);
    setBits(b, kWinTcsr, uint32_t(kTcsrPcie0ClkrefEn & 0xfff), kTcsrClkrefEnBit);
    b.hw->udelay(b.hw->ctx, 200);
    b.halts = readHalts(b);
    b.vote52008 = rd(b, kWinGcc, 0x52008);
    b.vote52000 = rd(b, kWinGcc, 0x52000);
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] clocks");
    pw_text_kx(&t, "v52008", b.vote52008);
    pw_text_kx(&t, "v52000", b.vote52000);
    pw_text_kx(&t, "halts", b.halts);
    line(b, text);
    // A branch with hardware gating may read CLK_OFF while idle; the others must run (the pipe clock waits for the PHY).
    uint32_t mustRun = 0;
    for (unsigned i = 0; i < kBranchCount; ++i)
        if (!kBranches[i].hwGated && !pw_streq(kBranches[i].name, "pcie_0_pipe")) mustRun |= 1u << i;
    if ((b.vote52008 & kVote52008Bits) != kVote52008Bits || (b.vote52000 & kVote52000Bits) != kVote52000Bits)
        return failed(b, kStepClocks, kErrBranch, "a branch vote did not stick");
    if (!waitClear(b, kWinGcc, kAuxRcg, 1u, 100, 10) || !waitClear(b, kWinGcc, kRchngRcg, 1u, 100, 10))
        return failed(b, kStepClocks, kErrRcg, "RCG update did not finish");
    if (b.halts & mustRun) return failed(b, kStepClocks, kErrBranch, "a branch did not start");
    if ((rc = done(b, kStepClocks))) return rc;

    // The controller's and the PHY's block resets.
    const uint32_t resets[2] = {kBcrPcie0, kBcrPcie0Phy};
    for (uint32_t bcr : resets) {
        setBits(b, kWinGcc, bcr, 1u);
        b.hw->udelay(b.hw->ctx, 1000);
        clearBits(b, kWinGcc, bcr, 1u);
        b.hw->udelay(b.hw->ctx, 1000);
    }
    rc = done(b, kStepResets);
    letGo(b);
    return rc;
}

// ---- stage 3: the link ----
int linkUp(Bringup &b)
{
    int rc = powerOn(b);
    if (rc) return rc;
    char text[160];
    PwText t;

    // The first access to the wrapper and the PHY since the boot loaders (a read of a block that is not clocked stalls the bus).
    guard(b, "parf");

    // The SM8550 model of the wrapper (mainline pcie-qcom ops 1_9_0, the same offsets in pci-msm): root complex, the PHY out of its
    // test power-down, where the DBI and the iATU are, the slave address space, then the power-management bits.
    wr(b, kWinParf, parf::kDeviceType, parf::kDeviceTypeRc);
    clearBits(b, kWinParf, parf::kPhyCtrl, parf::kPhyTestPwrDown);
    wr(b, kWinParf, parf::kDbiBase, uint32_t(kDbi));
    wr(b, kWinParf, parf::kDbiBaseHi, uint32_t(kDbi >> 32));
    wr(b, kWinParf, parf::kAtuBase, uint32_t(kAtu));
    wr(b, kWinParf, parf::kAtuBaseHi, uint32_t(kAtu >> 32));
    wr(b, kWinParf, parf::kSlvAddrSpace, 0);
    wr(b, kWinParf, parf::kSlvAddrSpaceHi, 0x80000000u);
    clearBits(b, kWinParf, parf::kSysCtrl, 1u << 29);
    setBits(b, kWinParf, parf::kMhiClockResetCtrl, 1u << 4);
    clearBits(b, kWinParf, parf::kPmCtrl, 1u << 5);
    setBits(b, kWinParf, parf::kAxiMstrWrAddrHalt, 1u << 31);
    if ((rc = done(b, kStepParf))) return rc;

    // The running devicetree's qcom,phy-sequence: everything up to the reset release, the pipe clock from the PHY, then the release
    // and the start (the last two writes), then PHYSTATUS low.
    for (unsigned i = 0; i + 2 < phy::kSequenceCount; ++i) wr(b, kWinPhy, phy::kSequence[i].offset, phy::kSequence[i].value);
    wr(b, kWinGcc, kPipeMux, (rd(b, kWinGcc, kPipeMux) & ~3u) | kPipeMuxPhy);
    for (unsigned i = phy::kSequenceCount - 2; i < phy::kSequenceCount; ++i)
        wr(b, kWinPhy, phy::kSequence[i].offset, phy::kSequence[i].value);
    b.hw->udelay(b.hw->ctx, 1000);
    const bool ready = waitClear(b, kWinPhy, phy::kPcsStatus1, phy::kPhyStatus, 100, 100, &b.phyPolls);
    b.pcsStatus = rd(b, kWinPhy, phy::kPcsStatus1);
    const uint32_t pipe = rd(b, kWinGcc, kBranches[branchIndex("pcie_0_pipe")].haltReg);
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] phy");
    pw_text_kx(&t, "pcs_status1", b.pcsStatus);
    pw_text_kd(&t, "polls", b.phyPolls);
    pw_text_kx(&t, "pipe_cbcr", pipe);
    line(b, text);
    if (!ready) return failed(b, kStepPhy, kErrPhy, "PHY not ready");
    if ((rc = done(b, kStepPhy))) return rc;

    // Which stream the endpoint's requests carry: the hash table of requester IDs (BDF << 16 | stream offset << 8 | next), used once
    // the bypass bit is off. The root port (requester 0) hashes to entry 0 with offset 0, which the zeroed table already says.
    for (unsigned i = 0; i < 256; ++i) wr(b, kWinParf, parf::kBdfToSidTable + 4 * i, 0);
    clearBits(b, kWinParf, parf::kBdfToSidCfg, parf::kBdfToSidBypass);
    b.sidHash = bdfHash(kEndpointRid);
    wr(b, kWinParf, parf::kBdfToSidTable + 4 * b.sidHash, uint32_t(kEndpointRid) << 16 | uint32_t(kSidEndpoint - kSidRootPort) << 8);
    b.sidCfg = rd(b, kWinParf, parf::kBdfToSidCfg);
    if ((rc = done(b, kStepSid))) return rc;

    // The first access to the controller's own registers, then the link: nothing sleeps between PERST#, the LTSSM and L0.
    guard(b, "dbi");

    // The root port: bus numbers 0/1/1, memory space and bus master, the target link speed (pagani-wifi-gen; Gen3 needs cx/mx at
    // NOM, which nothing here votes).
    wr(b, kWinDbi, dbi::kBusNumbers, 0x00010100u);
    setBits(b, kWinDbi, dbi::kCommand, 0x6u);
    const unsigned gen = b.opt.maxGen >= 1 && b.opt.maxGen <= kRcMaxGen ? b.opt.maxGen : 2;
    wr(b, kWinDbi, dbi::kLinkControl2, (rd(b, kWinDbi, dbi::kLinkControl2) & ~0xfu) | gen);
    b.rootId = rd(b, kWinDbi, 0);
    if ((rc = done(b, kStepRootPort))) return rc;

    // PERST# low for at least 100 ms in all, then high, then the LTSSM.
    const uint64_t lowFor = b.hw->now_ms(b.hw->ctx) - b.perstAssertedMs;
    if (lowFor < 100) b.hw->msleep(b.hw->ctx, unsigned(100 - lowFor));
    b.perstLowMs = uint32_t(b.hw->now_ms(b.hw->ctx) - b.perstAssertedMs);
    const Pin &perst = pin("perst");
    setPin(b, perst, pinConfig(perst.pull, 0, perst.driveMa, true), 1);
    b.hw->udelay(b.hw->ctx, 1000);
    setBits(b, kWinParf, parf::kLtssm, parf::kLtssmEnable);
    if ((rc = done(b, kStepPerst))) return rc;

    const uint64_t start = b.hw->now_ms(b.hw->ctx);
    unsigned ms = 0;
    for (; ms < 1000; ++ms) {
        b.portDebug0 = rd(b, kWinDbi, dbi::kPortDebug0);
        b.portDebug1 = rd(b, kWinDbi, dbi::kPortDebug1);
        if ((b.portDebug1 & dbi::kPortDebug1LinkUp) && (b.portDebug0 & dbi::kLtssmMask) == dbi::kLtssmL0) break;
        b.hw->msleep(b.hw->ctx, 1);
    }
    b.linkMs = uint32_t(b.hw->now_ms(b.hw->ctx) - start);
    b.linkStatus = rd(b, kWinDbi, dbi::kLinkControlStatus);
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] link");
    pw_text_kx(&t, "debug0", b.portDebug0);
    pw_text_kx(&t, "debug1", b.portDebug1);
    pw_text_kd(&t, "gen", (b.linkStatus >> 16) & 0xf);
    pw_text_kd(&t, "width", (b.linkStatus >> 20) & 0x3f);
    pw_text_kd(&t, "ms", b.linkMs);
    pw_text_kd(&t, "perst_low_ms", b.perstLowMs);
    line(b, text);
    if (ms == 1000) return failed(b, kStepLinkUp, kErrLink, "no L0 within 1 s");
    if ((rc = done(b, kStepLinkUp))) return rc;

    // The first request that leaves the SoC.
    guard(b, "config");

    // Outbound iATU region 0 (unrolled, at the iATU base) turns the 1 MiB configuration window into type 0 accesses to bus 1.
    wr(b, kWinAtu, atu::kLowerBase, uint32_t(kConfig));
    wr(b, kWinAtu, atu::kUpperBase, uint32_t(kConfig >> 32));
    wr(b, kWinAtu, atu::kLimit, uint32_t(kConfig + 0xfffff));
    wr(b, kWinAtu, atu::kUpperLimit, uint32_t((kConfig + 0xfffff) >> 32));
    wr(b, kWinAtu, atu::kLowerTarget, 1u << 24);
    wr(b, kWinAtu, atu::kUpperTarget, 0);
    wr(b, kWinAtu, atu::kCtrl1, atu::kTypeCfg0);
    wr(b, kWinAtu, atu::kCtrl2, atu::kEnable);
    for (unsigned i = 0; i < 5 && !(rd(b, kWinAtu, atu::kCtrl2) & atu::kEnable); ++i) b.hw->udelay(b.hw->ctx, 10);
    if (!(rd(b, kWinAtu, atu::kCtrl2) & atu::kEnable)) return failed(b, kStepEndpointId, kErrAtu, "iATU region 0 not enabled");
    // The endpoint answers some time after L0: all ones, or the retry status's vendor 0x0001, until then (the vendor driver gives it 1 s).
    for (unsigned i = 0; i < 100; ++i) {
        b.endpointId = rd(b, kWinCfg, 0);
        if (b.endpointId != 0xffffffffu && (b.endpointId & 0xffffu) != 0x0001u) break;
        b.hw->msleep(b.hw->ctx, 10);
    }
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] endpoint");
    pw_text_kx(&t, "id", b.endpointId);
    pw_text_kx(&t, "class", rd(b, kWinCfg, 8));
    pw_text_kx(&t, "subsys", rd(b, kWinCfg, 0x2c));
    line(b, text);
    if (b.endpointId != kEndpointVendorDevice) return failed(b, kStepEndpointId, kErrEndpoint, "not 17cb:110e");
    rc = done(b, kStepEndpointId);
    letGo(b);
    return rc;
}

// ---- stage 4: the MEM window and BAR0 ----
int endpoint(Bringup &b)
{
    if (!b.hw) return kErrArgument;
    // region 1: memory, the CPU window to the same PCI addresses; the root port forwards the window to bus 1
    const uint32_t r1 = atu::kRegionStride;
    wr(b, kWinAtu, r1 + atu::kLowerBase, uint32_t(kMemBase));
    wr(b, kWinAtu, r1 + atu::kUpperBase, uint32_t(kMemBase >> 32));
    wr(b, kWinAtu, r1 + atu::kLimit, uint32_t(kMemBase + kMemBytes - 1));
    wr(b, kWinAtu, r1 + atu::kUpperLimit, uint32_t((kMemBase + kMemBytes - 1) >> 32));
    wr(b, kWinAtu, r1 + atu::kLowerTarget, uint32_t(kMemBase));
    wr(b, kWinAtu, r1 + atu::kUpperTarget, uint32_t(kMemBase >> 32));
    wr(b, kWinAtu, r1 + atu::kCtrl1, atu::kTypeMem);
    wr(b, kWinAtu, r1 + atu::kCtrl2, atu::kEnable);
    for (unsigned i = 0; i < 5 && !(rd(b, kWinAtu, r1 + atu::kCtrl2) & atu::kEnable); ++i) b.hw->udelay(b.hw->ctx, 10);
    if (!(rd(b, kWinAtu, r1 + atu::kCtrl2) & atu::kEnable)) return failed(b, kStepMemWindow, kErrAtu, "iATU region 1 not enabled");
    // type 1 header memory base (bits 15:4 = address bits 31:20) and limit (31:20)
    wr(b, kWinDbi, dbi::kMemBaseLimit,
       uint32_t(((kMemBase + kMemBytes - 1) & 0xfff00000u) | ((kMemBase >> 16) & 0xfff0u)));
    int rc = done(b, kStepMemWindow);
    if (rc) return rc;

    // BAR0: size it, put it at the first address of the window aligned to its size, memory decoding and bus mastering on.
    const uint32_t saved = rd(b, kWinCfg, 0x10);
    wr(b, kWinCfg, 0x10, 0xffffffffu);
    b.barMask = rd(b, kWinCfg, 0x10);
    wr(b, kWinCfg, 0x10, saved);
    const uint32_t size = ~(b.barMask & ~0xfu) + 1;
    if (!(b.barMask & ~0xfu) || !size || size > kMemBytes || (b.barMask & 1u))
        return failed(b, kStepBar, kErrBar, "BAR0 not a memory BAR of a size the window holds");
    const uint64_t address = (kMemBase + size - 1) & ~uint64_t(size - 1);
    if (address + size > kMemBase + kMemBytes) return failed(b, kStepBar, kErrBar, "BAR0 does not fit the window");
    wr(b, kWinCfg, 0x10, uint32_t(address) | (b.barMask & 0xfu));
    if ((b.barMask & 6u) == 4u) wr(b, kWinCfg, 0x14, 0);
    wr(b, kWinCfg, 0x04, (rd(b, kWinCfg, 0x04) & 0xffffu) | 0x6u);
    b.barValue = rd(b, kWinCfg, 0x10);
    b.command = rd(b, kWinCfg, 0x04);
    b.barAddress = address;
    b.barBytes = size;
    char text[160];
    PwText t;
    pw_text_start(&t, text, sizeof text);
    pw_text_str(&t, "[wifi] bar0");
    pw_text_kx(&t, "mask", b.barMask);
    pw_text_kx(&t, "value", b.barValue);
    pw_text_kx(&t, "bytes", size);
    pw_text_kx(&t, "command", b.command);
    line(b, text);
    if ((b.barValue & ~0xfu) != uint32_t(address)) return failed(b, kStepBar, kErrBar, "BAR0 did not take its address");
    return done(b, kStepBar);
}

}  // namespace pagani_wifi
