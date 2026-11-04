// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Model test of WifiBringup.cpp (stages 1 to 4) against a simulated SM8750 PCIe0 corner: GCC (collapse votes, GDSCs, RCGs, branch votes
// and halts, block resets), TLMM pins, TCSR, PARF, the QMP PHY, the DBI/iATU, the endpoint's configuration space, the RPMh VRM/BCM
// layer, the PMIC readback and the sleep clock pin. The model answers like the hardware where the sequence depends on it (a GDSC turns
// on when its collapse vote clears, PHYSTATUS falls once the whole sequence ran with the pipe clock on the PHY, the link trains once
// PERST# is high and the LTSSM enabled, the configuration window answers once iATU region 0 is a CFG0 window, BAR0 sizes) and records
// every access, so the test checks the 13T's values and the order: the rails in cnss2's order with mode, voltage, enable each; S7I
// never lowered, never disabled, always in the high power mode, and no rail voted when it cannot be read; the PCIe PHY's LDOs read and
// never voted; PERST# low for at least 100 ms; WLAN_EN low 150 ms then high for the PMU
// time; the collapse votes; the shared vote register's other bits kept under the lock word; the 128 PHY writes in the running
// devicetree's order with the pipe mux switched between the 126th and the 127th; the BDF-to-SID entry; the windows; BAR0 at
// 0x40400000. Then each failure path stops at its step with nothing done after it, and a survey writes nothing at all.
// The UFS log's writer: no rail moves and no block is touched for the first time unless the writer is held, after the line naming the stretch had
// its time to reach the log and a round under way its time to end; a stretch that hangs (the model's hang knob) keeps it held with that line last;
// every other way out of a stretch lets go. The register sequence is the same with and without the holds.
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../WifiBringup.h"
#include "../WifiUtil.h"

using namespace pagani_wifi;

static unsigned checks;
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1);} ++checks;} while (0)

struct Event {unsigned win; uint32_t off, value; uint64_t ms; bool lock;};
struct Vote {std::string resource; uint32_t reg, value;};
struct Model {
    std::vector<uint32_t> regs[kWinCount];
    std::vector<Event> writes;
    std::vector<Vote> votes;
    std::vector<std::string> pdc, log;
    uint64_t us = 0;
    bool lockHeld = false, lockBusy = false;
    unsigned lockTakes = 0;
    // knobs
    bool gdscStuck = false, rcgStuck = false, auxStuck = false, phyStuck = false, linkFails = false, rpmhFail = false;
    unsigned cfgLate = 0;                          // configuration reads of the ID that answer nothing yet
    bool rcgNeedsRoot = false, s7iFalls = false, s7iOff = false;   // an RCG update that waits for its root; a vote that takes S7I down
    std::string rpmhFailOn;
    uint32_t endpointId = kEndpointVendorDevice, barBytes = 0x200000;
    bool bar64 = true;
    // PMIC rails by PPID: status, enable, mV; unreadable PPIDs
    struct Rail {uint16_t ppid; uint8_t status, enable; uint16_t mv; bool readable; uint8_t mode;};
    std::vector<Rail> rails;
    int sleepClock = 0, sleepClockWrites = 0;
    // derived state
    unsigned phyWrites = 0;
    bool phyStarted = false, pipeOnPhy = false, perstHigh = false, ltssm = false, linkUp = false, cfgWindow = false;
    uint32_t barValue = 0, barHi = 0, command = 0;
    uint64_t perstLowAtUs = 0, perstHighAtUs = 0, ltssmAtUs = 0;
    int pipeSwitchedAfterPhyWrite = -1;
    // the UFS log's writer
    bool quiet = false;
    unsigned holds = 0, lets = 0;
    uint64_t quietAtUs = 0, lastLineUs = 0;
    bool touched[kWinCount] = {}, voted = false;
    std::vector<std::string> firstTouches;         // what was touched first, in order, with the writer held long enough
    int hangWin = -1;                              // a read or write of this window never returns
    uint32_t hangOff = 0;
    bool hangAnyOff = false;
    bool hangOnVote = false;
    bool relaxed = false;                          // no writer in this run: the rules on its hold do not apply
};
static Model M;
static const unsigned kCrumb = 3500;
static jmp_buf gHang;

static uint32_t &reg(unsigned win, uint32_t off)
{
    CHECK(win < kWinCount && !(off & 3) && off + 4 <= kWinBytes[win]);
    return M.regs[win][off / 4];
}
static const Pin &pinNamed(const char *n)
{
    for (const Pin &p : kPins) if (!std::strcmp(p.name, n)) return p;
    std::abort();
}
static Model::Rail *railOf(uint16_t ppid)
{
    for (Model::Rail &r : M.rails) if (r.ppid == ppid) return &r;
    return nullptr;
}

// The blocks a hang can come from, with the rule of their first access: the writer has been held for a round's end.
static void touch(unsigned win, uint32_t off)
{
    if (int(win) == M.hangWin && (M.hangAnyOff || off == M.hangOff)) std::longjmp(gHang, 1);
    if (win != kWinParf && win != kWinPhy && win != kWinDbi && win != kWinAtu && win != kWinCfg && win != kWinTcsr) return;
    if (M.touched[win]) return;
    M.touched[win] = true;
    M.firstTouches.push_back(std::to_string(win));
    CHECK(M.relaxed || (M.quiet && M.us - M.quietAtUs >= kQuietMs * 1000ull));
}
static uint32_t mRead(void *, unsigned win, uint32_t off)
{
    touch(win, off);
    uint32_t &r = reg(win, off);
    if (win == kWinCfg && !M.cfgWindow) return 0xffffffffu;
    if (win == kWinCfg && off == 0 && M.cfgLate) {--M.cfgLate; return M.cfgLate & 1 ? 0xffffffffu : 0xffff0001u;}
    if (win == kWinCfg && off == 0) return M.endpointId;
    if (win == kWinCfg && off == 0x10) return M.barValue;
    if (win == kWinCfg && off == 0x14) return M.barHi;
    if (win == kWinCfg && off == 0x04) return M.command;
    return r;
}
static void mWrite(void *, unsigned win, uint32_t off, uint32_t value)
{
    touch(win, off);
    reg(win, off) = value;
    M.writes.push_back({win, off, value, M.us / 1000, M.lockHeld});
    if (win == kWinGcc) {
        if (off == kGdscCollapseVote && !M.gdscStuck) {
            if (!(value & kCollapsePcie0)) reg(kWinGcc, kGdscPcie0) |= kGdscPwrOn;
            if (!(value & kCollapsePcie0Phy)) reg(kWinGcc, kGdscPcie0Phy) |= kGdscPwrOn;
        }
        if ((off == kAuxRcg || off == kRchngRcg) && !M.rcgStuck && !M.rcgNeedsRoot) reg(kWinGcc, off) &= ~1u;
        if (off == 0x52008 && M.rcgNeedsRoot) {
            reg(kWinGcc, kAuxRcg) &= ~1u;
            reg(kWinGcc, kRchngRcg) &= ~1u;
        }
        if (off == 0x52008 || off == 0x52000)
            for (const Branch &b : kBranches)
                if (b.voteReg == off && (value >> b.voteBit) & 1 && std::strcmp(b.name, "pcie_0_pipe") &&
                    !(M.auxStuck && !std::strcmp(b.name, "pcie_0_aux")))
                    reg(kWinGcc, b.haltReg) &= ~kBranchOff;
        if (off == kPipeMux && (value & 3) == kPipeMuxPhy) {
            M.pipeOnPhy = true;
            M.pipeSwitchedAfterPhyWrite = int(M.phyWrites);
        }
    } else if (win == kWinPhy) {
        ++M.phyWrites;
        if (off == phy::kStartControl && value == phy::kStartBoth && M.phyWrites == phy::kSequenceCount && M.pipeOnPhy && !M.phyStuck) {
            reg(kWinPhy, phy::kPcsStatus1) &= ~phy::kPhyStatus;
            M.phyStarted = true;
            reg(kWinGcc, kBranches[4].haltReg) &= ~kBranchOff;   // the pipe branch runs
        }
    } else if (win == kWinTlmm) {
        const Pin &perst = pinNamed("perst");
        const Pin &wlanEn = pinNamed("wlan_en");
        if (off == wlanEn.gpio * kTlmmPinStride + 4 && (value & 2)) CHECK(M.relaxed || (M.quiet && M.us - M.quietAtUs >= kQuietMs * 1000ull));   // the chip's load lands
        if (off == perst.gpio * kTlmmPinStride + 4) {
            const bool high = value & 2;
            if (!high && (M.perstHigh || !M.perstLowAtUs)) M.perstLowAtUs = M.us;
            if (high && !M.perstHigh) M.perstHighAtUs = M.us;
            M.perstHigh = high;
        }
    } else if (win == kWinParf) {
        if (off == parf::kLtssm && (value & parf::kLtssmEnable)) {
            M.ltssm = true;
            M.ltssmAtUs = M.us;
            if (M.perstHigh && M.phyStarted && !M.linkFails) {
                reg(kWinDbi, dbi::kPortDebug0) = dbi::kLtssmL0;
                reg(kWinDbi, dbi::kPortDebug1) = dbi::kPortDebug1LinkUp;
                reg(kWinDbi, dbi::kLinkControlStatus) = 2u << 16 | 1u << 20 | 0x40;
                M.linkUp = true;
            }
        }
    } else if (win == kWinAtu) {
        if (off == atu::kCtrl2 && (value & atu::kEnable) && reg(kWinAtu, atu::kCtrl1) == atu::kTypeCfg0 &&
            reg(kWinAtu, atu::kLowerTarget) == 1u << 24 && reg(kWinAtu, atu::kLowerBase) == uint32_t(kConfig) && M.linkUp)
            M.cfgWindow = true;
    } else if (win == kWinCfg && M.cfgWindow) {
        if (off == 0x10) M.barValue = value == 0xffffffffu ? (~(M.barBytes - 1) | (M.bar64 ? 4u : 0u)) : value;
        if (off == 0x14) M.barHi = value;
        if (off == 0x04) M.command = value;
    }
}
static void mUdelay(void *, unsigned us) {M.us += us;}
static void mMsleep(void *, unsigned ms) {M.us += uint64_t(ms) * 1000;}
static uint64_t mNow(void *) {return M.us / 1000;}
static int mLock(void *, int take)
{
    if (!take) {M.lockHeld = false; return 0;}
    if (M.lockBusy) return -16;
    ++M.lockTakes;
    M.lockHeld = true;
    return 0;
}
static int mVrm(void *, const char *resource, uint32_t r, uint32_t value, uint32_t *address)
{
    if (M.hangOnVote) std::longjmp(gHang, 1);
    CHECK(M.relaxed || (M.quiet && M.us - M.quietAtUs >= kQuietMs * 1000ull));   // a rail moves: S7I carries the UFS path, and the first vote is what round A is about
    M.voted = true;
    if (M.rpmhFail || M.rpmhFailOn == resource) return -110;
    M.votes.push_back({resource, r, value});
    *address = 0x40000 + uint32_t(M.votes.size()) * 0x100;
    // the PMIC follows a full vote
    for (const Rail &rail : kRails) {
        if (std::strcmp(rail.resource, resource)) continue;
        Model::Rail *p = railOf(rail.ppid);
        if (!p) break;
        if (r == 0) p->mv = uint16_t(value);
        if (r == 4 && value) {p->enable = 0x80; p->status = 0x80;}
        if (r == 4 && !value) p->enable = 0;
        if (r == 8) p->mode = uint8_t(value);
        if (M.s7iFalls && rail.ppid == 0x8ad) p->mv = 1200;
        if (M.s7iOff && rail.ppid == 0x8ad) p->enable = 0;
    }
    return 0;
}
static std::vector<std::string> bcmNames;
static int mBcm(void *, const char *name, uint32_t avg, uint32_t peak, uint32_t width, uint32_t *vote)
{
    bcmNames.push_back(std::string(name) + " " + std::to_string(avg) + " " + std::to_string(peak) + " " + std::to_string(width));
    *vote = 0x60000000u | 0x10;
    return 0;
}
static int mPmic(void *, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
{
    Model::Rail *r = railOf(ppid);
    if (!r || !r->readable) return -19;
    *status = r->status; *enable = r->enable; *mode = r->mode; *mv = r->mv;
    return 0;
}
static int mSleepClock(void *, int write)
{
    if (M.sleepClock) return 1;
    if (!write) return 0;
    ++M.sleepClockWrites;
    M.sleepClock = 1;
    return 2;
}
static int mPdc(void *, const char *message) {M.pdc.push_back(message); return 0;}
static void mLog(void *, const char *line)
{
    M.log.push_back(line);
    M.lastLineUs = M.us;
    if (std::getenv("PW_VERBOSE")) std::printf("%s\n", line);
}
static void mQuiet(void *, unsigned on)
{
    CHECK(bool(on) != M.quiet);                     // set once, let go once: the word's other bits are other drivers'
    M.quiet = on;
    if (on) {
        ++M.holds;
        M.quietAtUs = M.us;
        CHECK(M.us - M.lastLineUs >= kCrumb * 1000ull);   // the line naming the stretch has reached the log
        CHECK(M.log.back().find("[wifi] guard ") == 0);
    } else {
        ++M.lets;
    }
}
static const Hw kHw = {nullptr, mRead, mWrite, mUdelay, mMsleep, mNow, mLock, mVrm, mBcm, mPmic, mSleepClock, mPdc, mLog, mQuiet};

static void reset()
{
    M = Model();
    bcmNames.clear();
    for (unsigned w = 0; w < kWinCount; ++w) M.regs[w].assign(kWinBytes[w] / 4, 0);
    // after the boot loaders: PCIe0 collapsed (its two votes set, an unrelated bit kept), every branch off, PHYSTATUS high
    reg(kWinGcc, kGdscCollapseVote) = kCollapsePcie0 | kCollapsePcie0Phy | 0x10;
    reg(kWinGcc, 0x52008) = 1u << 7;          // the battery's I2C vote, not ours
    reg(kWinGcc, 0x52000) = 1u << 3;
    for (const Branch &b : kBranches) reg(kWinGcc, b.haltReg) = kBranchOff;
    reg(kWinPhy, phy::kPcsStatus1) = phy::kPhyStatus;
    reg(kWinParf, parf::kBdfToSidCfg) = parf::kBdfToSidBypass;
    reg(kWinTcsr, uint32_t(kTcsrPcie0ClkrefEn & 0xfff)) = 0x100;
    for (const Rail &r : kRails) M.rails.push_back({r.ppid, 0x80, 0x80, uint16_t(r.minMv > 600 ? r.minMv : 600), true, 3});
    // S7I is the UFS vccq parent: up at 1312 mV already (the morning fact M15 decides; the test varies it)
    railOf(0x8ad)->mv = 1312;
    for (Model::Rail &r : M.rails) if (r.ppid != 0x8ad) {r.enable = 0; r.status = 0; r.mv = 0;}
    for (const PhyRail &r : kPhyRails) M.rails.push_back({r.ppid, 0x80, 0x80, 880, true, 7});
}
static Bringup fresh(unsigned stage = 4)
{
    Bringup b;
    b.hw = &kHw;
    b.opt.stage = stage;
    b.opt.crumbMs = kCrumb;
    return b;
}
static size_t firstWrite(unsigned win, uint32_t off, size_t from = 0)
{
    for (size_t i = from; i < M.writes.size(); ++i) if (M.writes[i].win == win && M.writes[i].off == off) return i;
    return SIZE_MAX;
}
static size_t writesTo(unsigned win) {size_t n = 0; for (const Event &e : M.writes) n += e.win == win; return n;}

static void checkSuccess()
{
    reset();
    Bringup b = fresh();
    CHECK(linkUp(b) == 0 && b.step == kStepEndpointId && b.error == 0);
    // rails: cnss2's order, mode 7 then the voltage then enable 1; S7I (already up at 1312 mV) gets the mode alone
    const char *order[] = {"ldof3", "ldof2", "ldob8", "smpd4", "smpf5", "smpg3"};
    const uint32_t mv[] = {1800, 1200, 2800, 876, 876, 1860};
    CHECK(M.votes.size() == 19);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(M.votes[3 * i].resource == order[i] && M.votes[3 * i].reg == 8 && M.votes[3 * i].value == 7);
        CHECK(M.votes[3 * i + 1].resource == order[i] && M.votes[3 * i + 1].reg == 0 && M.votes[3 * i + 1].value == mv[i]);
        CHECK(M.votes[3 * i + 2].resource == order[i] && M.votes[3 * i + 2].reg == 4 && M.votes[3 * i + 2].value == 1);
    }
    CHECK(M.votes[18].resource == "smpi7" && M.votes[18].reg == 8 && M.votes[18].value == 7);
    for (const Vote &v : M.votes) CHECK(!(v.reg == 4 && v.value == 0) && !(v.resource == "smpi7" && v.reg != 8));
    CHECK(b.rails[6].skipped == 1 && railOf(0x8ad)->mode == 7);
    for (const Vote &v : M.votes) for (const PhyRail &r : kPhyRails) CHECK(v.resource != r.resource);   // the PHY's LDOs: never voted
    CHECK(bcmNames.size() == 1 && bcmNames[0] == "SN4 100000 800000 8");
    CHECK(M.pdc.empty() && M.sleepClockWrites == 1);
    // pins: PERST# low first, CLKREQ# to its function, WLAN_EN low 150 ms then high, and 60 ms of PMU time before the GDSCs
    const Pin &perst = pinNamed("perst"), &clkreq = pinNamed("clkreq"), &wlanEn = pinNamed("wlan_en");
    const size_t perstCtl = firstWrite(kWinTlmm, perst.gpio * kTlmmPinStride);
    const size_t enLow = firstWrite(kWinTlmm, wlanEn.gpio * kTlmmPinStride);
    const size_t enHigh = firstWrite(kWinTlmm, wlanEn.gpio * kTlmmPinStride, enLow + 1);
    const size_t collapse = firstWrite(kWinGcc, kGdscCollapseVote);
    CHECK(perstCtl < enLow && enLow < enHigh && enHigh < collapse);
    CHECK(M.writes[perstCtl].value == 0x201 && M.writes[perstCtl - 1].value == 0);                      // 2 mA, pull-down, output, low
    CHECK(reg(kWinTlmm, clkreq.gpio * kTlmmPinStride) == 0x7);                                         // func 1, pull-up
    CHECK(M.writes[enLow].value == 0x201 && M.writes[enHigh].value == 0x3c3);                          // 16 mA, pull-up, output
    CHECK(M.writes[enHigh].ms - M.writes[enLow].ms >= 150 && M.writes[collapse].ms - M.writes[enHigh].ms >= kPmuPowerUpDelayMs);
    CHECK(reg(kWinTlmm, wlanEn.gpio * kTlmmPinStride + 4) == 2);
    // GDSCs: only the two collapse bits cleared; clocks: the RCGs' values, the pipe parked on TCXO, the votes OR'd under the lock
    CHECK(reg(kWinGcc, kGdscCollapseVote) == 0x10);
    CHECK(reg(kWinGcc, kAuxRcg + 4) == kAuxCfg19p2MHz && reg(kWinGcc, kRchngRcg + 4) == ((6u << 8) | 5u));
    CHECK(reg(kWinGcc, 0x52008) == (kVote52008Bits | 1u << 7) && reg(kWinGcc, 0x52000) == (kVote52000Bits | 1u << 3));
    for (const Event &e : M.writes) if (e.win == kWinGcc && (e.off == 0x52008 || e.off == 0x52000)) CHECK(e.lock);
    CHECK(M.lockTakes == 1 && !M.lockHeld);
    CHECK(reg(kWinTcsr, uint32_t(kTcsrPcie0ClkrefEn & 0xfff)) == (0x100 | kTcsrClkrefEnBit));
    const size_t muxXo = firstWrite(kWinGcc, kPipeMux), votes = firstWrite(kWinGcc, 0x52008);
    CHECK(muxXo < votes && M.writes[muxXo].value == kPipeMuxXo);
    // block resets: set then clear, controller then PHY, after the clocks
    const size_t bcr0 = firstWrite(kWinGcc, kBcrPcie0), bcr1 = firstWrite(kWinGcc, kBcrPcie0Phy);
    CHECK(votes < bcr0 && bcr0 < bcr1 && M.writes[bcr0].value == 1 && M.writes[bcr0 + 1].value == 0 && M.writes[bcr1 + 1].value == 0);
    // PARF: root complex, DBI and iATU where the 13T has them
    CHECK(reg(kWinParf, parf::kDeviceType) == 4 && reg(kWinParf, parf::kDbiBase) == 0x40000000 && reg(kWinParf, parf::kAtuBase) ==
          0x40001000 && reg(kWinParf, parf::kSlvAddrSpaceHi) == 0x80000000u && (reg(kWinParf, parf::kAxiMstrWrAddrHalt) >> 31));
    // the PHY: the running devicetree's 128 writes, in order, with the pipe clock switched to the PHY after the 126th
    std::vector<Event> phyWrites;
    for (const Event &e : M.writes) if (e.win == kWinPhy) phyWrites.push_back(e);
    CHECK(phyWrites.size() == phy::kSequenceCount);
    for (unsigned i = 0; i < phy::kSequenceCount; ++i)
        CHECK(phyWrites[i].off == phy::kSequence[i].offset && phyWrites[i].value == phy::kSequence[i].value);
    CHECK(M.pipeSwitchedAfterPhyWrite == int(phy::kSequenceCount) - 2);
    CHECK(firstWrite(kWinParf, parf::kDeviceType) < firstWrite(kWinPhy, phy::kPowerDownControl));
    // BDF-to-SID: the table cleared, the bypass off, the endpoint's entry (hash 0x15: BDF 01:00.0, stream offset 1)
    CHECK(b.sidHash == 0x15 && reg(kWinParf, parf::kBdfToSidTable + 4 * 0x15) == 0x01000100u);
    CHECK(!(reg(kWinParf, parf::kBdfToSidCfg) & 1));
    for (unsigned i = 0; i < 256; ++i) if (i != 0x15) CHECK(reg(kWinParf, parf::kBdfToSidTable + 4 * i) == 0);
    // the root port: bus numbers, memory and bus master, Gen2 target
    CHECK(reg(kWinDbi, dbi::kBusNumbers) == 0x00010100u && (reg(kWinDbi, dbi::kCommand) & 6) == 6 &&
          (reg(kWinDbi, dbi::kLinkControl2) & 0xf) == 2);
    // PERST# low at least 100 ms, released before the LTSSM is enabled
    CHECK(M.perstHighAtUs - M.perstLowAtUs >= 100000 && M.ltssmAtUs > M.perstHighAtUs && b.perstLowMs >= 100);
    CHECK(b.endpointId == 0x110e17cbu && ((b.linkStatus >> 16) & 0xf) == 2);
    // the writer: five stretches in the order of the risks, none left open
    std::vector<std::string> names;
    for (const std::string &line : M.log) if (line.find("[wifi] guard ") == 0) names.push_back(line.substr(13));
    CHECK((names == std::vector<std::string>{"rails", "wlan_en", "parf", "dbi", "config"}));
    CHECK(M.holds == 5 && M.lets == 5 && !M.quiet && !b.held);
    // stage 4: the MEM window and BAR0 (2 MiB, 64-bit) at the first 2 MiB boundary of the window
    CHECK(endpoint(b) == 0 && b.step == kStepBar);
    CHECK(!M.quiet);
    CHECK(reg(kWinAtu, 0x200 + atu::kLowerBase) == 0x40300000 && reg(kWinAtu, 0x200 + atu::kLimit) == 0x43ffffff &&
          reg(kWinAtu, 0x200 + atu::kCtrl1) == atu::kTypeMem && (reg(kWinAtu, 0x200 + atu::kCtrl2) >> 31));
    CHECK(reg(kWinDbi, dbi::kMemBaseLimit) == 0x43f04030u);
    CHECK(b.barAddress == 0x40400000 && b.barBytes == 0x200000 && M.barValue == (0x40400000u | 4u) && M.barHi == 0 &&
          (M.command & 6) == 6);
}

static void checkSurveyWritesNothing()
{
    reset();
    Bringup b = fresh(1);
    CHECK(survey(b) == 0 && b.step == kStepSurveyController && M.writes.empty() && M.votes.empty());
    CHECK((b.halts & 0xfff) == 0xfff && b.collapse == (kCollapsePcie0 | kCollapsePcie0Phy | 0x10));
    CHECK(b.sleepClock == 0 && b.rails[6].read && b.rails[6].millivolts == 1312 && b.rails[6].mode == 3);
    for (const RailResult &r : b.phyRails) CHECK(r.read && r.enable == 0x80 && r.millivolts == 880 && r.mode == 7);
    // with the controller powered and clocked by a boot loader the survey also reads PARF and the DBI (still no write)
    reset();
    reg(kWinGcc, kGdscPcie0) = kGdscPwrOn;
    for (const Branch &br : kBranches) reg(kWinGcc, br.haltReg) = 0;
    reg(kWinParf, parf::kLtssm) = 0x100;
    reg(kWinDbi, 0) = 0x011117cb;
    b = fresh(1);
    CHECK(survey(b) == 0 && M.writes.empty() && b.parfLtssm == 0x100 && b.rootId == 0x011117cb);
}

static void checkRailRules()
{
    // S7I below what peach needs: raised to 1312 (mode, voltage; already enabled, so no enable), never lower
    reset();
    railOf(0x8ad)->mv = 1224;
    Bringup b = fresh(2);
    CHECK(powerOn(b) == 0);
    unsigned s7i = 0;
    for (const Vote &v : M.votes) if (v.resource == "smpi7") {++s7i; CHECK(v.reg != 4); if (v.reg == 0) CHECK(v.value == 1312);}
    CHECK(s7i == 2 && railOf(0x8ad)->mode == 7);
    // S7I off: mode, voltage and enable
    reset();
    railOf(0x8ad)->mv = 1224;
    railOf(0x8ad)->enable = 0;
    b = fresh(2);
    CHECK(powerOn(b) == 0);
    s7i = 0;
    for (const Vote &v : M.votes) if (v.resource == "smpi7") ++s7i;
    CHECK(s7i == 3);
    // S7I at the top of its range: the voltage is not written, whatever it is
    reset();
    railOf(0x8ad)->mv = 1336;
    b = fresh(2);
    CHECK(powerOn(b) == 0);
    for (const Vote &v : M.votes) CHECK(!(v.resource == "smpi7" && v.reg == 0));
    // S7I unreadable: nothing is voted on any rail, WLAN_EN never rises, the chip draws nothing from a rail nobody looked at
    reset();
    railOf(0x8ad)->readable = false;
    b = fresh(2);
    CHECK(powerOn(b) == kErrRailRead && b.step == -kStepRails && b.error == kErrRailRead && M.votes.empty());
    CHECK(firstWrite(kWinTlmm, pinNamed("wlan_en").gpio * kTlmmPinStride) == SIZE_MAX);
    // ... unless pagani-wifi-s7i=1
    reset();
    railOf(0x8ad)->readable = false;
    b = fresh(2);
    b.opt.s7iVote = 1;
    CHECK(powerOn(b) == 0);
    s7i = 0;
    for (const Vote &v : M.votes) if (v.resource == "smpi7") ++s7i;
    CHECK(s7i == 3);
    // S7I reads lower after the votes: stops before WLAN_EN
    reset();
    railOf(0x8ad)->mv = 1224;
    M.s7iFalls = true;
    b = fresh(2);
    CHECK(powerOn(b) == kErrRail && b.step == -kStepRails && firstWrite(kWinTlmm, pinNamed("wlan_en").gpio * kTlmmPinStride) == SIZE_MAX);
    // S7I off after the votes: the same
    reset();
    M.s7iOff = true;
    b = fresh(2);
    CHECK(powerOn(b) == kErrRail && b.step == -kStepRails && firstWrite(kWinTlmm, pinNamed("wlan_en").gpio * kTlmmPinStride) == SIZE_MAX);
    // a rail already above the chosen voltage (inside the cnss range) keeps it
    reset();
    railOf(0x6a1)->mv = 1900;
    b = fresh(2);
    CHECK(powerOn(b) == 0);
    for (const Vote &v : M.votes) if (v.resource == "smpg3" && v.reg == 0) CHECK(v.value == 1900);
    // above the cnss range: never lowered to it, never sent back either (a misread must not become a vote); mode and enable still go
    reset();
    railOf(0x6a1)->mv = 4095;
    b = fresh(2);
    CHECK(powerOn(b) == 0);
    unsigned s3g = 0;
    for (const Vote &v : M.votes) if (v.resource == "smpg3") {++s3g; CHECK(v.reg != 0);}
    CHECK(s3g == 2 && railOf(0x6a1)->mv == 4095 && b.rails[5].voted == 0 && b.rails[5].skipped == 1);
    // an unread ordinary rail is voted the chosen voltage (it can only be the cnss floor)
    reset();
    railOf(0x5a7)->readable = false;
    b = fresh(2);
    CHECK(powerOn(b) == 0);
    for (const Vote &v : M.votes) if (v.resource == "smpf5" && v.reg == 0) CHECK(v.value == 876);
    // vote registers already right are not written at all
    reset();
    reg(kWinGcc, 0x52008) = kVote52008Bits | 1u << 7;
    reg(kWinGcc, 0x52000) = kVote52000Bits | 1u << 3;
    for (const Branch &br : kBranches) reg(kWinGcc, br.haltReg) = 0;
    b = fresh(2);
    CHECK(powerOn(b) == 0 && firstWrite(kWinGcc, 0x52008) == SIZE_MAX && firstWrite(kWinGcc, 0x52000) == SIZE_MAX);
    // RCG updates that stay pending until the branches run their roots (sheng's order): not a failure
    reset();
    M.rcgNeedsRoot = true;
    b = fresh(2);
    CHECK(powerOn(b) == 0 && b.step == kStepResets);
    // the PDC messages: the devicetree's four, then the four OL-CPR values
    reset();
    b = fresh(2);
    b.opt.pdc = 2;
    CHECK(powerOn(b) == 0 && M.pdc.size() == 8 && M.pdc[0] == kPdcInit[0] && M.pdc[7] == kPdcOlCprPagani[3]);
    // sleep clock already running: not written
    reset();
    M.sleepClock = 1;
    b = fresh(2);
    CHECK(powerOn(b) == 0 && M.sleepClockWrites == 0);
}

static void checkFailures()
{
    struct Case {const char *name; void (*arrange)(); int step, error;};
    const Case cases[] = {
        {"rail", [] {M.rpmhFailOn = "smpd4";}, -kStepRails, kErrRail},
        {"gdsc", [] {M.gdscStuck = true;}, -kStepGdsc, kErrGdsc},
        {"rcg", [] {M.rcgStuck = true;}, -kStepClocks, kErrRcg},
        {"lock", [] {M.lockBusy = true;}, -kStepClocks, kErrLock},
        {"branch", [] {M.auxStuck = true;}, -kStepClocks, kErrBranch},
        {"phy", [] {M.phyStuck = true;}, -kStepPhy, kErrPhy},
        {"link", [] {M.linkFails = true;}, -kStepLinkUp, kErrLink},
        {"endpoint", [] {M.endpointId = 0x110717cbu;}, -kStepEndpointId, kErrEndpoint},
    };
    for (const Case &c : cases) {
        reset();
        c.arrange();
        Bringup b = fresh();
        const int rc = linkUp(b);
        if (b.step != c.step || rc != c.error) std::fprintf(stderr, "case %s: step %d rc %d\n", c.name, b.step, rc);
        CHECK(b.step == c.step && rc == c.error && b.error == c.error);
        CHECK(!M.log.empty() && M.log.back().find("[wifi] FAIL") == 0);
        const Pin &wlanEn = pinNamed("wlan_en");
        if (c.step == -kStepRails) CHECK(firstWrite(kWinTlmm, wlanEn.gpio * kTlmmPinStride) == SIZE_MAX);   // nothing powered after it
        if (c.step == -kStepGdsc || c.step == -kStepClocks) CHECK(writesTo(kWinPhy) == 0 && writesTo(kWinParf) == 0);
        if (!std::strcmp(c.name, "lock")) CHECK(firstWrite(kWinGcc, 0x52008) == SIZE_MAX);
        if (c.step == -kStepPhy) CHECK(!M.perstHigh && !M.ltssm);
        CHECK(!M.quiet && !b.held);                 // a failure is an answer, not a hang: its line must get out
    }
    // the endpoint answers configuration reads only a while after L0: waited for, up to 1 s
    {
        reset();
        M.cfgLate = 40;
        Bringup late = fresh();
        CHECK(linkUp(late) == 0 && late.step == kStepEndpointId && late.endpointId == kEndpointVendorDevice);
        reset();
        M.cfgLate = 1000;
        late = fresh();
        CHECK(linkUp(late) == kErrEndpoint && late.step == -kStepEndpointId);
        // another device is refused at once
        reset();
        M.endpointId = 0x110717cbu;
        late = fresh();
        CHECK(linkUp(late) == kErrEndpoint);
    }
    // stopping after a step: stage 2 up to WLAN_EN, no GDSC touched
    reset();
    Bringup b = fresh(2);
    b.opt.stopAfter = kStepWlanEn;
    CHECK(powerOn(b) == kErrStopped && b.step == kStepWlanEn && firstWrite(kWinGcc, kGdscCollapseVote) == SIZE_MAX);
    CHECK(!M.quiet && !b.held);
    // the link speed option: 3 accepted, out of range back to 2
    reset();
    b = fresh();
    b.opt.maxGen = 3;
    CHECK(linkUp(b) == 0 && (reg(kWinDbi, dbi::kLinkControl2) & 0xf) == 3);
    reset();
    b = fresh();
    b.opt.maxGen = 9;
    CHECK(linkUp(b) == 0 && (reg(kWinDbi, dbi::kLinkControl2) & 0xf) == 2);
    // a BAR larger than the window is refused, nothing assigned; a 32-bit 64 KiB BAR sits at the window's start
    reset();
    M.barBytes = 0x8000000;
    b = fresh();
    CHECK(linkUp(b) == 0 && endpoint(b) == kErrBar && b.step == -kStepBar && !(M.command & 6));
    reset();
    M.barBytes = 0x10000;
    M.bar64 = false;
    b = fresh();
    CHECK(linkUp(b) == 0 && endpoint(b) == 0 && b.barAddress == 0x40300000 && M.barValue == 0x40300000u);
}

static std::vector<Event> linkUpWrites()
{
    Bringup b = fresh();
    CHECK(linkUp(b) == 0);
    return M.writes;
}
static bool sameWrites(const std::vector<Event> &a, const std::vector<Event> &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].win != b[i].win || a[i].off != b[i].off || a[i].value != b[i].value) return false;
    return true;
}

// What the writer's hold has to do when it matters, and that it costs nothing else.
static void checkHolds()
{
    // Each stretch that hangs keeps the writer held, and the last line in the log is the one naming it: nothing in the log after it, no write on the wire.
    struct Hang {const char *name; int win; uint32_t off; bool any;};
    const Hang hangs[] = {
        {"wlan_en", kWinTcsr, 0, true},             // the TCSR page's first read
        {"parf", kWinParf, 0, true}, {"parf", kWinPhy, 0, true},
        {"dbi", kWinDbi, 0, true},
        {"config", kWinCfg, 0, true},
    };
    for (const Hang &h : hangs) {
        reset();
        M.hangWin = h.win;
        M.hangOff = h.off;
        M.hangAnyOff = h.any;
        Bringup b = fresh();
        if (!setjmp(gHang)) {
            linkUp(b);
            CHECK(false);
        }
        CHECK(M.quiet && b.held && M.lets == M.holds - 1);
        std::string last;
        for (const std::string &line : M.log) if (line.find("[wifi] guard ") == 0) last = line;
        CHECK(last == std::string("[wifi] guard ") + h.name);
        CHECK(M.log.back() == last || !std::strcmp(h.name, "wlan_en"));   // (the pins' line comes before the TCSR page)
    }
    // a vote that hangs: the same
    reset();
    M.hangOnVote = true;
    {
        Bringup b = fresh(2);
        if (!setjmp(gHang)) {
            powerOn(b);
            CHECK(false);
        }
        CHECK(M.quiet && b.held && M.holds == 1 && M.lets == 0);
    }
    // stage 2 alone ends with the writer free, and the wait covers the writer's look (every 500 ms at the longest) and its round
    CHECK(kQuietMs >= 1000);
    reset();
    {
        Bringup b = fresh(2);
        CHECK(powerOn(b) == 0 && b.step == kStepResets && !M.quiet && !b.held && M.holds == 2 && M.lets == 2);
    }
    // the rails' stop (round A): the votes under the hold, let go at the stop
    reset();
    {
        Bringup b = fresh(2);
        b.opt.stopAfter = kStepPdc;
        CHECK(powerOn(b) == kErrStopped && b.step == kStepPdc && M.voted && !M.quiet && !b.held && M.holds == 1 && M.lets == 1);
        CHECK(firstWrite(kWinTlmm, pinNamed("wlan_en").gpio * kTlmmPinStride) == SIZE_MAX);
    }
    // The survey touches no block when the boot loaders left PCIe0 off, so it holds nothing; a controller they left on is a stretch of its own.
    reset();
    {
        Bringup b = fresh(1);
        CHECK(survey(b) == 0 && M.holds == 0);
    }
    reset();
    reg(kWinGcc, kGdscPcie0) = kGdscPwrOn;
    for (const Branch &br : kBranches) reg(kWinGcc, br.haltReg) = 0;
    {
        Bringup b = fresh(1);
        CHECK(survey(b) == 0 && M.holds == 1 && M.lets == 1 && !M.quiet);
    }
    // stage 4's accesses are the same with the stretches in force and a guard from the kernel side lets go by itself before the next one
    reset();
    {
        Bringup b = fresh();
        CHECK(linkUp(b) == 0 && endpoint(b) == 0);
        const uint64_t before = M.us;
        guard(b, "bar0");
        CHECK(b.held && M.quiet && M.us - before == (kCrumb + kQuietMs) * 1000ull);
        guard(b, "chip");
        CHECK(b.held && M.holds == 7 && M.lets == 6);
        letGo(b);
        letGo(b);                                   // twice is once
        CHECK(!b.held && !M.quiet && M.lets == 7);
    }
    // Without the log (sheng-log not 1) the stretches are named and nothing waits: no hold, no sleep. Same with a board that has no such hook.
    reset();
    std::vector<Event> held = (linkUpWrites());
    const uint64_t heldUs = M.us;
    reset();
    {
        Hw none = kHw;
        none.quiet = nullptr;
        Bringup b = fresh();
        b.hw = &none;
        M.relaxed = true;
        CHECK(linkUp(b) == 0 && M.holds == 0);
        CHECK(M.us + 5 * (kCrumb + kQuietMs) * 1000ull == heldUs);
        CHECK(sameWrites(held, M.writes));
        for (const std::string &line : M.log) CHECK(line.find("[wifi] guard ") != 0);
    }
    reset();
    {
        Bringup b = fresh();
        b.opt.crumbMs = 0;
        M.relaxed = true;
        CHECK(linkUp(b) == 0 && M.holds == 0 && !b.held);
        CHECK(M.us + 5 * (kCrumb + kQuietMs) * 1000ull == heldUs);
        CHECK(sameWrites(held, M.writes));
        unsigned named = 0;
        for (const std::string &line : M.log) named += line.find("[wifi] guard ") == 0;
        CHECK(named == 5);
    }
}

int main()
{
    CHECK(bdfHash(0x100) == 0x15 && bdfHash(0) == 0);
    CHECK(pinConfig(-1, 0, 2, true) == 0x201 && pinConfig(1, 1, 2, false) == 0x7 && pinConfig(1, 0, 16, true) == 0x3c3);
    CHECK(bcmVote(0, 1, 8, 1000) == 0 && bcmVote(800000, 1, 8, 100000) == 1000 && bcmVote(1, 1, 8, 1000000) == 1);
    CHECK(bcmVote(100000000, 16, 8, 1000) == 0x3fff);
    // an offset read from a device that does not answer: all ones neither wraps past the bound nor makes an unaligned access
    CHECK(pw_reg_ok(0, 4) && pw_reg_ok(0x1ffffc, 0x200000) && !pw_reg_ok(0x200000, 0x200000) && !pw_reg_ok(0x27, 0x200000));
    CHECK(!pw_reg_ok(0xffffffffu, 0x200000) && !pw_reg_ok(0xfffffffcu, 0x200000));
    checkSuccess();
    checkSurveyWritesNothing();
    checkRailRules();
    checkFailures();
    checkHolds();
    std::printf("PASS bring-up model: %u checks\n", checks);
    return 0;
}
