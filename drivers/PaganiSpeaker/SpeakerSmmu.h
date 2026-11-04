/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/SmmuBypass.h (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e), the sequence sheng's speakers
 * play through; 13T: the bank is the one the storage stream already runs on, found in the stream-match table, and nothing of it is
 * written. */
/* The audio streams in pass-through in the firmware-managed apps SMMU. Linux's qcom_smmu bypass quirk, as sheng and NativeUfs use it:
 * every bypass stream shares one context bank (SCTLR 0, CBAR.TYPE S1_TRANS_S2_BYPASS, CBA2R.VA64) and maps to it with S2CR TYPE=TRANSLATE,
 * S2CR written before SMR (a TYPE=BYPASS S2CR reads back as FAULT on this firmware).
 *
 * Why not the kit's planner (SmmuTakeover.h), which gives each stream a context bank of its own: that writes a bank nobody has shown to be
 * free for these streams, and the hypervisor holds on to the audio contexts: programming a translating context for them reset the whole
 * board every time, whatever bank or table (sheng i496..i502). This plan writes only one S2CR and one SMR of a free group, pointing at the
 * bank the running storage path already proves in pass-through (stream 0x60: NativeUfs set it up long before the DSP could start, whose
 * firmware it read through that path); no bank register, no TLB maintenance (a bypass stream has no translations), no global register.
 * Anything unexpected in the tables refuses without a write. Pure planning and configuration over register callbacks; Test/smmu.c. */
#ifndef PAGANI_SPEAKER_SMMU_H
#define PAGANI_SPEAKER_SMMU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SPEAKER_SMMU_STORAGE_SID 0x60u     /* NativeUfs's stream, in pass-through since before the root mounted */
#define SPEAKER_SMMU_GROUPS 128u           /* Linux qcom_smmu_cfg_probe: groups above 128 behave differently */
enum {
    SpeakerSmmuOk = 0,
    SpeakerSmmuTaken = -16,                /* a mapping of the stream that is not ours, or no free group */
    SpeakerSmmuNoBank = -1,                /* no storage stream in pass-through to share a bank with */
    SpeakerSmmuReadback = -5,              /* a register did not read back */
    SpeakerSmmuGeometry = -19,             /* an SMMU this plan was not made for, or an address it cannot pass */
};

typedef struct {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);    /* completes with a barrier */
} SpeakerSmmuIo;

typedef struct {
    uint32_t slot, bank;
    int reuse;                             /* the stream is already mapped to the bank */
    int error;                             /* SpeakerSmmu* */
    uint32_t smr, s2cr;                    /* the entry that refused it */
} SpeakerSmmuPlan;

static inline unsigned speaker_smmu_bits(unsigned code)
{
    switch (code) {
    case 0: return 32; case 1: return 36; case 2: return 40; case 3: return 42; case 4: return 44; case 5: return 48;
    default: return 0;
    }
}

/* Reads only. dma/bytes: the physical range the stream will reach; refused when an SMMU width cannot carry it. */
static inline SpeakerSmmuPlan speaker_smmu_plan(const SpeakerSmmuIo *io, uint16_t stream, uint64_t dma, uint64_t bytes, uint32_t map_bytes)
{
    SpeakerSmmuPlan p = {0xffffffffu, 0, 0, SpeakerSmmuOk, 0, 0};
    const uint32_t idr0 = io->read(io->context, 0x20), idr1 = io->read(io->context, 0x24), idr2 = io->read(io->context, 0x28);
    if (!(idr0 & (1u << 30)) || !(idr2 & (1u << 12))) {p.error = SpeakerSmmuGeometry; return p;}
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        const unsigned bits = speaker_smmu_bits((idr2 >> shift) & 15u);
        if (!bits || !bytes || dma >= 1ull << bits || bytes > (1ull << bits) - dma) {p.error = SpeakerSmmuGeometry; return p;}
    }
    uint32_t count = idr0 & 255u;
    const uint32_t banks = idr1 & 255u, pages = 1u << (((idr1 >> 28) & 7u) + 1u);
    if ((idr1 >> 31) || !count || pages > 128 || banks < 2 || banks > 128 || (uint64_t)(pages + banks) * 4096u > map_bytes) {
        p.error = SpeakerSmmuGeometry;
        return p;
    }
    if (count > SPEAKER_SMMU_GROUPS) count = SPEAKER_SMMU_GROUPS;
    /* the bank: the one exact, translating entry of the storage stream, and that bank really in bypass */
    unsigned storage = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t smr = io->read(io->context, 0x800 + 4 * i);
        if (!(smr & 0x80000000u) || ((smr ^ SPEAKER_SMMU_STORAGE_SID) & 0xffffu & ~((smr >> 16) & 0x7fffu))) continue;
        const uint32_t s2cr = io->read(io->context, 0xc00 + 4 * i);
        p.smr = smr;
        p.s2cr = s2cr;
        if (++storage > 1 || (smr & 0x7fff0000u) || ((s2cr >> 16) & 3u) || (s2cr & 255u) >= banks) {p.error = SpeakerSmmuNoBank; return p;}
        p.bank = s2cr & 255u;
    }
    if (!storage) {p.error = SpeakerSmmuNoBank; return p;}
    if (io->read(io->context, (pages + p.bank) * 4096u) != 0 || io->read(io->context, 0x1000 + 4 * p.bank) != 0x10000u ||
        io->read(io->context, 0x1800 + 4 * p.bank) != 1u) {
        p.error = SpeakerSmmuNoBank;
        return p;
    }
    /* the stream: not mapped at all, or exactly as this plan would */
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t smr = io->read(io->context, 0x800 + 4 * i);
        if (!(smr & 0x80000000u)) {
            if (p.slot == 0xffffffffu) p.slot = i;
            continue;
        }
        if ((smr ^ stream) & 0xffffu & ~((smr >> 16) & 0x7fffu)) continue;
        const uint32_t s2cr = io->read(io->context, 0xc00 + 4 * i);
        if (p.reuse || (smr & 0x7fff0000u) || ((s2cr >> 16) & 3u) || (s2cr & 255u) != p.bank) {
            p.slot = i;
            p.smr = smr;
            p.s2cr = s2cr;
            p.error = SpeakerSmmuTaken;
            return p;
        }
        p.reuse = 1;
        p.slot = i;
    }
    if (p.reuse) return p;
    if (p.slot == 0xffffffffu) p.error = SpeakerSmmuTaken;
    return p;
}

/* The plan's two writes, S2CR before SMR, each read back. SpeakerSmmuOk, or the plan's error, or SpeakerSmmuReadback. */
static inline int speaker_smmu_configure(const SpeakerSmmuIo *io, const SpeakerSmmuPlan *p, uint16_t stream)
{
    if (p->error) return p->error;
    const uint32_t smr = 0x80000000u | stream;
    if (!p->reuse) {
        io->write(io->context, 0xc00 + 4 * p->slot, p->bank);
        if (io->read(io->context, 0xc00 + 4 * p->slot) != p->bank) return SpeakerSmmuReadback;
        io->write(io->context, 0x800 + 4 * p->slot, smr);
    }
    if (io->read(io->context, 0x800 + 4 * p->slot) != smr || io->read(io->context, 0xc00 + 4 * p->slot) != p->bank)
        return SpeakerSmmuReadback;
    return SpeakerSmmuOk;
}

#ifdef __cplusplus
}
#endif
#endif
