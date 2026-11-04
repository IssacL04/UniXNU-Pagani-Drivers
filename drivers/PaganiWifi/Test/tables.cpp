// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Consistency checks of PaganiWifiTables.h (the facts of build-userspace/a830-inputs/wifi/facts.md as tables), taken over from the
// facts step's facts-check/tables-selftest.cpp; the boot argument names of the driver are checked as well.
#include <string.h>
#include "../PaganiWifiTables.h"

namespace {
using namespace pagani_wifi;

constexpr uint8_t crc8(uint16_t bdf)
{
    uint8_t crc = 0;
    const uint8_t bytes[2] = {uint8_t(bdf >> 8), uint8_t(bdf)};
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? uint8_t((crc << 1) ^ kBdfHashPoly) : uint8_t(crc << 1);
    }
    return crc;
}
static_assert(crc8(kEndpointRid) == 0x15, "endpoint 01:00.0 hashes to BDF_TO_SID entry 0x15");
static_assert(crc8(kRootRid) == 0x00, "root port hashes to entry 0 (offset 0: the zeroed word)");

constexpr bool phyOffsetsValid()
{
    for (unsigned i = 0; i < phy::kSequenceCount; ++i) {
        const uint32_t o = phy::kSequence[i].offset;
        if (o >= 0x2000 || (o & 3)) return false;
    }
    return true;
}
static_assert(phyOffsetsValid(), "every PHY write lies in the 8 KiB PHY window, 32-bit aligned");

constexpr unsigned count(uint32_t from, uint32_t to)
{
    unsigned n = 0;
    for (unsigned i = 0; i < phy::kSequenceCount; ++i) n += phy::kSequence[i].offset >= from && phy::kSequence[i].offset < to;
    return n;
}
static_assert(count(0x0, 0x400) == 38, "38 serdes writes (the same as sheng's)");
static_assert(count(0x1200, 0x1800) == 29 && count(0x1a00, 0x2000) == 29, "29 RX writes per lane");
static_assert(count(0x1000, 0x1200) == 7 && count(0x1800, 0x1a00) == 8, "7 TX writes per lane, plus 0x1800 = 0");

constexpr bool branchesValid()
{
    for (const Branch &b : kBranches) {
        if (b.voteReg != 0x52000 && b.voteReg != 0x52008) return false;
        if (b.haltReg >= kGccMapBytes || (b.haltReg & 3) || b.voteBit > 31) return false;
        const uint32_t mask = b.voteReg == 0x52008 ? kVote52008Bits : kVote52000Bits;
        if (!(mask & (1u << b.voteBit))) return false;
    }
    return true;
}
static_assert(branchesValid(), "every branch vote bit is in the vote masks and every halt register in the GCC map");
static_assert(kGdscPcie0 < kGccMapBytes && kGdscPcie0Phy < kGccMapBytes && kGdscCollapseVote < kGccMapBytes && kPipeMux < kGccMapBytes,
              "GCC registers fit the mapped window");

constexpr bool pinsValid()
{
    for (const Pin &p : kPins) if (p.gpio >= 216 || p.func > 11) return false;
    return true;
}
static_assert(pinsValid(), "216 TLMM pins, 12 functions");

constexpr bool railsValid()
{
    unsigned seen = 0;
    for (const Rail &r : kRails) {
        if (r.chosenMv < r.minMv || r.chosenMv > r.maxMv || r.order < 1 || r.order > 7) return false;
        seen |= 1u << r.order;
    }
    return seen == 0xfe;
}
static_assert(railsValid(), "every rail's chosen voltage is inside its cnss range, power-up order 1..7 once each");

static_assert(chip::kWindowReg < chip::kWindowStart, "the remap window register is directly addressable");
static_assert((chip::kTcsrSocHwVersion >> chip::kWindowShift) <= chip::kWindowValueMask, "chip addresses fit the 6-bit window field");
static_assert(mhi::kSblBytes == mhi::kSegBytes && mhi::kSblBytes == 0x80000, "SBL and BHIe segments are 512 KiB");
static_assert(kMemBase + kMemBytes <= 0x44000000, "the MEM window stays inside the PCIe0 address space");
static_assert(sizeof kFirmware / sizeof kFirmware[0] == 11, "firmware table");
static_assert(kSidEndpoint - kSidRootPort == 1 && kSpiMsi0 == 141 && chip::kWindowReg == 0x3278, "stream, MSI line, window register");
static_assert(sizeof kBusBw / sizeof kBusBw[0] == 9 && kBusBw[4].avg0 == 100000 && kBusBw[4].peak0 == 800000, "the bus vote level");
}  // namespace

extern "C" uint32_t pagani_wifi_tables_digest(void);
extern "C" uint32_t pagani_wifi_tables_digest(void)
{
    // FNV-1a over the PHY sequence and the register constants a driver programs: the host run prints it, the kernel object
    // carries the same computation (the tables are used, not dead).
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {for (int i = 0; i < 4; ++i) h = (h ^ ((v >> (8 * i)) & 0xff)) * 16777619u;};
    for (unsigned i = 0; i < phy::kSequenceCount; ++i) mix(uint32_t(phy::kSequence[i].offset) << 8 | phy::kSequence[i].value);
    for (const Branch &b : kBranches) mix(b.voteReg ^ b.haltReg ^ b.voteBit);
    for (const Rail &r : kRails) mix(uint32_t(r.ppid) << 16 | r.chosenMv);
    mix(uint32_t(kDbi >> 16)); mix(chip::kWindowReg); mix(kSpiMsi0); mix(kSidEndpoint);
    return h;
}

#if !defined(KERNEL)
#include <stdio.h>
int main()
{
    // A few cross-checks a constexpr cannot express with string data.
    int failures = 0;
    for (const FwFile &f : kFirmware) if (!f.ath12kName[0] || !f.androidName[0]) ++failures;
    if (strcmp(kRails[6].resource, "smpi7") != 0 || kRails[6].chosenMv != 1312) ++failures;
    if (strcmp(kPdcInit[0], "{class: wlan_pdc, ss: rf, res: s5f.m, enable: 1}") != 0) ++failures;
    // every boot argument of the driver is "pagani-wifi" or starts with "pagani-wifi-", and none twice
    const char *const args[] = {bootarg::kStage, bootarg::kMaxGen, bootarg::kFwPartition, bootarg::kFwDir, bootarg::kBdf, bootarg::kM3,
                                bootarg::kChipReset, bootarg::kPdc, bootarg::kMsi, bootarg::kCbc, bootarg::kTme, bootarg::kQdss,
                                bootarg::kAutoJoin, bootarg::kDmaMiB, bootarg::kDmaPa, bootarg::kSleepClk, bootarg::kStopAfter, bootarg::kLog};
    for (unsigned i = 0; i < sizeof args / sizeof args[0]; ++i) {
        if (strncmp(args[i], "pagani-wifi", 11) || (args[i][11] && args[i][11] != '-') || strlen(args[i]) >= 32) ++failures;
        for (unsigned k = 0; k < i; ++k) if (!strcmp(args[i], args[k])) ++failures;
    }
    printf("PaganiWifiTables: %u PHY writes, %zu branches, %zu rails, %zu firmware entries, digest %08x, %d failures\n",
           phy::kSequenceCount, sizeof kBranches / sizeof kBranches[0], sizeof kRails / sizeof kRails[0],
           sizeof kFirmware / sizeof kFirmware[0], pagani_wifi_tables_digest(), failures);
    if (!failures) printf("PASS tables\n");
    return failures ? 1 : 0;
}
#endif
