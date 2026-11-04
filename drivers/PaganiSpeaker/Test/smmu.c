/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host test of the audio streams' pass-through plan (SpeakerSmmu.h) against a model of the apps SMMU's registers: the geometry of the
 * 13T (PKX110: firmware hand-off state plus NativeUfs on the top bank and the touch on the next), every refusal without a write, and on
 * success exactly two writes, S2CR before SMR, and none to a context bank, a TLB or a global register: the board reset sheng saw came
 * from programming contexts for these streams. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SpeakerSmmu.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

#define MAP 0x100000u
enum {Groups = 128, Banks = 64, Pages = 64};
static uint32_t regs[MAP / 4];
static struct {uint32_t offset, value;} writes[16];
static unsigned nwrites;
static int ignore_s2cr, ignore_smr;
static uint32_t rd(void *c, uint32_t at)
{
    (void)c;
    CHECK(at < MAP && !(at & 3));
    return regs[at / 4];
}
static void wr(void *c, uint32_t at, uint32_t value)
{
    (void)c;
    CHECK(at < MAP && !(at & 3) && nwrites < 16);
    writes[nwrites].offset = at;
    writes[nwrites++].value = value;
    if ((ignore_s2cr && at >= 0xc00 && at < 0x1000) || (ignore_smr && at >= 0x800 && at < 0xc00)) return;
    regs[at / 4] = value;
}
static const SpeakerSmmuIo io = {0, rd, wr};

static void bank(unsigned n, uint32_t sctlr, uint32_t cbar, uint32_t cba2r)
{
    regs[(Pages + n) * 4096u / 4] = sctlr;
    regs[(0x1000 + 4 * n) / 4] = cbar;
    regs[(0x1800 + 4 * n) / 4] = cba2r;
}
static void group(unsigned n, uint32_t smr, uint32_t s2cr)
{
    regs[(0x800 + 4 * n) / 4] = smr;
    regs[(0xc00 + 4 * n) / 4] = s2cr;
}
/* What NativeUfs and the touch leave: UFS 0x60 on the top bank, the touch on the one below, firmware's own streams on low banks. */
static void reset(void)
{
    memset(regs, 0, sizeof regs);
    nwrites = 0;
    ignore_s2cr = ignore_smr = 0;
    regs[0x20 / 4] = 1u << 30 | 200u;                    /* S1 AArch64 supported, more groups than the architected 128 */
    regs[0x24 / 4] = (uint32_t)(5u << 28 | Banks);       /* 64 pages of global space */
    regs[0x28 / 4] = 1u << 12 | 2u << 8 | 2u << 4 | 2u;  /* AArch64 4K, 40-bit widths */
    group(0, 0x80000000u | 0x800, 3);                    /* the display, a translating bank of the firmware's */
    bank(3, 1, 0, 0);
    group(5, 0x80000000u | 0x60, Banks - 1u);            /* NativeUfs */
    bank(Banks - 1u, 0, 0x10000, 1);
    group(6, 0x80000000u | 0x584, Banks - 2u);           /* the touch */
    bank(Banks - 2u, 0, 0x10000, 1);
}
static void no_writes(void) {CHECK(nwrites == 0);}

int main(void)
{
    /* a stream comes to the storage bank: one S2CR, one SMR, in that order, in the first free group */
    reset();
    SpeakerSmmuPlan p = speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP);
    no_writes();
    CHECK(!p.error && p.slot == 1 && p.bank == Banks - 1u && !p.reuse);
    CHECK(speaker_smmu_configure(&io, &p, 0x1001) == SpeakerSmmuOk);
    CHECK(nwrites == 2 && writes[0].offset == 0xc04 && writes[0].value == Banks - 1u);
    CHECK(writes[1].offset == 0x804 && writes[1].value == (0x80000000u | 0x1001));
    /* every other audio stream shares it */
    static const uint16_t sids[] = {0x1081, 0x1041, 0x1061};
    for (unsigned i = 0; i < 3; ++i) {
        nwrites = 0;
        p = speaker_smmu_plan(&io, sids[i], 0xd0000000ull, 0x10000, MAP);
        CHECK(!p.error && p.bank == Banks - 1u && p.slot == 2u + i);
        CHECK(speaker_smmu_configure(&io, &p, sids[i]) == SpeakerSmmuOk && nwrites == 2);
    }
    /* run again (the mapping stands): nothing written */
    nwrites = 0;
    p = speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP);
    CHECK(!p.error && p.reuse && p.slot == 1);
    CHECK(speaker_smmu_configure(&io, &p, 0x1001) == SpeakerSmmuOk);
    no_writes();

    /* no storage stream, a storage stream on the firmware's FAULT or BYPASS type, on a masked entry, twice, or a bank that is not a
     * bypass bank: nothing is written */
    reset();
    group(5, 0, 0);
    p = speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP);
    CHECK(p.error == SpeakerSmmuNoBank && speaker_smmu_configure(&io, &p, 0x1001) == SpeakerSmmuNoBank);
    no_writes();
    reset();
    group(5, 0x80000000u | 0x60, 2u << 16);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuNoBank);
    reset();
    group(5, 0x80000000u | 0x10u << 16 | 0x60, Banks - 1u);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuNoBank);
    reset();
    group(9, 0x80000000u | 0x60, Banks - 1u);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuNoBank);
    reset();
    group(5, 0x80000000u | 0x60, 200);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuNoBank);
    static const uint32_t bad[3][3] = {{1, 0x10000, 1}, {0, 0x20000, 1}, {0, 0x10000, 0}};
    for (unsigned i = 0; i < 3; ++i) {
        reset();
        bank(Banks - 1u, bad[i][0], bad[i][1], bad[i][2]);
        CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuNoBank);
    }
    no_writes();

    /* the stream mapped by someone else, on a masked entry of its family, or twice: refused */
    reset();
    group(7, 0x80000000u | 0x1001, 3);
    p = speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP);
    CHECK(p.error == SpeakerSmmuTaken && p.slot == 7 && p.s2cr == 3);
    reset();
    group(7, 0x80000000u | 0x80u << 16 | 0x1001, Banks - 1u);
    CHECK(speaker_smmu_plan(&io, 0x1081, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuTaken);
    reset();
    group(7, 0x80000000u | 0x1001, Banks - 1u);
    group(8, 0x80000000u | 0x1001, Banks - 1u);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuTaken);
    /* no free group among the first 128 (the ones above behave differently) */
    reset();
    for (unsigned i = 0; i < Groups; ++i) if (i != 5) group(i, 0x80000000u | (0x2000 + i), 3);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuTaken);
    no_writes();

    /* an SMMU this was not made for */
    reset();
    regs[0x20 / 4] &= ~(1u << 30);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuGeometry);
    reset();
    regs[0x28 / 4] &= ~(1u << 12);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuGeometry);
    reset();
    regs[0x28 / 4] = 1u << 12;                            /* 32-bit widths */
    CHECK(speaker_smmu_plan(&io, 0x1001, 0x100000000ull, 0x10000, MAP).error == SpeakerSmmuGeometry);
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0, MAP).error == SpeakerSmmuGeometry);
    reset();
    regs[0x24 / 4] |= 1u << 31;
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP).error == SpeakerSmmuGeometry);
    reset();
    CHECK(speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, 0x10000).error == SpeakerSmmuGeometry);
    no_writes();

    /* a register that does not read back (the firmware ignores it): the SMR is not written after a lost S2CR */
    reset();
    ignore_s2cr = 1;
    p = speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP);
    CHECK(speaker_smmu_configure(&io, &p, 0x1001) == SpeakerSmmuReadback && nwrites == 1 && writes[0].offset == 0xc04);
    reset();
    ignore_smr = 1;
    p = speaker_smmu_plan(&io, 0x1001, 0xd0000000ull, 0x10000, MAP);
    CHECK(speaker_smmu_configure(&io, &p, 0x1001) == SpeakerSmmuReadback && nwrites == 2);

    printf("PASS smmu: %u checks\n", checks);
    return 0;
}
