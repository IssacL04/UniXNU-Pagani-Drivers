// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host test of the endpoint's stream in the apps SMMU as PaganiWifi.cpp sets it up (smmuStream): the planner and the writes of the
// Sm8750Platform kit's SmmuTakeover.h (the real header, the one NativeUfs uses for the UFS stream 0x60), here for stream 0x1401 with
// the 13T's DMA window above 4 GiB, against a model of the SMMUv2 global and context bank registers with the UFS stream already on the
// top bank: the Wi-Fi stream gets the next bank down, a free stream-match group (the planner takes the last free one), a VA64 bypass (SCTLR 0, CBAR S1 translate/S2 bypass)
// and S2CR before SMR; a second run reuses it without a write; a stream already mapped to a translating bank is refused with no write;
// the UFS entry is never touched.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define NATIVE_UFS_SM8750 1
#include "SmmuTakeover.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1);} ++checks;} while (0)

enum : unsigned {kGroups = 128, kBanks = 32, kPages = 128};
static uint32_t regs[kUfsSmmuMapBytes / 4];
struct Write {unsigned off; uint32_t value;};
static std::vector<Write> writes;
static uint32_t &at(unsigned off)
{
    CHECK(off < kUfsSmmuMapBytes && !(off & 3));
    return regs[off / 4];
}
static unsigned cb(unsigned bank) {return (kPages + bank) * 4096;}

static void reset()
{
    std::memset(regs, 0, sizeof regs);
    writes.clear();
    at(0x20) = 1u << 30 | kGroups;                       // IDR0: stage 1, 128 stream-match groups
    at(0x24) = 6u << 28 | kBanks;                        // IDR1: 2^(6+1) = 128 pages of global space, 32 context banks
    at(0x28) = 1u << 12 | 5u | 5u << 4 | 5u << 8;        // IDR2: VA64 supported, 48-bit IAS/OAS/UBS
    // the UFS stream as NativeUfs left it: group 3 -> the top bank, a VA64 bypass
    at(0x800 + 4 * 3) = 0x80000000u | 0x60;
    at(0xc00 + 4 * 3) = kBanks - 1;
    at(0x1000 + 4 * (kBanks - 1)) = 0x10000;
    at(0x1800 + 4 * (kBanks - 1)) = 1;
    // another firmware stream elsewhere, on its own bank (translating)
    at(0x800 + 4 * 0) = 0x80000000u | 0x0880;
    at(0xc00 + 4 * 0) = 5;
    at(cb(5)) = 1;
}
static uint32_t rd(unsigned off) {return at(off);}
static void wr(unsigned off, uint32_t value) {at(off) = value; writes.push_back({off, value});}

int main()
{
    const uint16_t stream = 0x1401;
    const uint64_t dma = 0x8c1200000ull, bytes = 48ull << 20;

    reset();
    UfsSmmuPlan plan = planUfsSmmu(stream, dma, bytes, rd);
    CHECK(!plan.error && plan.bank == kBanks - 2 && plan.slot == kGroups - 1 && !plan.reuse && plan.pages == kPages);   // the planner takes the last free group
    CHECK(writes.empty());
    const char *error = configureUfsSmmu(plan, stream, rd, wr);
    CHECK(!error);
    // SCTLR off, VA64, S1 translate / S2 bypass, then S2CR, then SMR (the stream becomes visible last)
    size_t s2cr = SIZE_MAX, smr = SIZE_MAX;
    for (size_t i = 0; i < writes.size(); ++i) {
        if (writes[i].off == 0xc00 + 4 * plan.slot) s2cr = i;
        if (writes[i].off == 0x800 + 4 * plan.slot) smr = i;
    }
    CHECK(s2cr < smr && smr == writes.size() - 1);
    CHECK(at(cb(plan.bank)) == 0 && at(0x1800 + 4 * plan.bank) == 1 && at(0x1000 + 4 * plan.bank) == 0x10000);
    CHECK(at(0x800 + 4 * plan.slot) == (0x80000000u | stream) && at(0xc00 + 4 * plan.slot) == plan.bank);
    // the UFS entry and the other stream are untouched
    CHECK(at(0x800 + 4 * 3) == (0x80000000u | 0x60) && at(0xc00 + 4 * 3) == kBanks - 1 && at(0x800) == (0x80000000u | 0x0880));
    // the TLB sync PaganiWifi runs afterwards: TLBIALLNSNH then sTLBGSYNC, done once GSACTIVE reads clear
    uint32_t status = 1;
    writes.clear();
    CHECK(syncUfsSmmu(rd, wr, [] {return false;}, status) && writes.size() == 2 && writes[0].off == 0x68 && writes[1].off == 0x70);

    // a second start (a warm reboot keeps the SMMU as it was): reused, nothing written
    writes.clear();
    plan = planUfsSmmu(stream, dma, bytes, rd);
    CHECK(!plan.error && plan.reuse && plan.bank == kBanks - 2 && plan.slot == kGroups - 1);
    CHECK(!configureUfsSmmu(plan, stream, rd, wr) && writes.empty());

    // the stream already mapped to a translating bank by someone else: refused, nothing written
    reset();
    at(0x800 + 4 * 7) = 0x80000000u | stream;
    at(0xc00 + 4 * 7) = 9;
    at(cb(9)) = 1;
    plan = planUfsSmmu(stream, dma, bytes, rd);
    CHECK(plan.error != nullptr && writes.empty());
    CHECK(configureUfsSmmu(plan, stream, rd, wr) == plan.error && writes.empty());

    // a DMA window beyond the SMMU's address width: refused
    reset();
    at(0x28) = 1u << 12 | 1u | 1u << 4 | 1u << 8;        // 36-bit widths
    CHECK(planUfsSmmu(stream, 0xfff000000ull, bytes, rd).error != nullptr);
    CHECK(planUfsSmmu(stream, dma, bytes, rd).error == nullptr);   // 0x8c1200000 + 48 MiB fits 36 bits
    std::printf("PASS SMMU stream: %u checks\n", checks);
    return 0;
}
