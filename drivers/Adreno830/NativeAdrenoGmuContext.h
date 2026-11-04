/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GMU_CONTEXT_H
#define NATIVE_ADRENO_GMU_CONTEXT_H
#include "NativeAdrenoVm.h"

/* The GMU's own SMMU context (ARCHITECTURE 4.5.4 first half, 4.7.2): a native4K lower-region (TTBR0) table pool for the GMU's
 * 0..2 GiB space, one context bank programmed and routed through the board's programmer (A830Smmu on the 13T: CB2, ASID 3,
 * slot 1, SID 5), the VM attached, then the GMU memory mapped by its owner (NativeAdrenoGmuMemory, from the firmware's PREALLOC
 * metadata) through the map hook. Bank, ASID, slot and SID are board inputs and never guessed. Every resource is retained on
 * every outcome; there is no teardown. The caller keeps CX powered and the SMMU clocks running and the GMU halted: the GMU
 * fetches its code from DDR through this bank, so it is built before the firmware is loaded (4.7.1). */
enum : uint64_t {GmuContextApertureBytes=0x80000000};
struct NativeAdrenoGmuContextReport {
    uint32_t step;                  /* last step started, NativeAdrenoGmuContext::Step (NativeAdrenoGpuContext::Step) */
    IOReturn status;                /* of that step */
    uint32_t output_bits;           /* the allocation ceiling used: the programmer's IDR2.OAS, capped at 40 */
    uint32_t table_pages;           /* table pool pages */
    uint32_t spans;                 /* VM spans after the map step */
    uint32_t streams;               /* streams routed */
    NativeAdrenoSmmuProgramReport program;   /* the programmer's report of the program step */
};
struct NativeAdrenoGmuContextConfig {
    const NativeAdrenoSmmuProgrammer *programmer;
    unsigned bank,asid,slot;        /* CB2, 3, 1 (board cb_gmu, cb_gmu + 1, the surveyed GMU slot) */
    uint16_t sid;                   /* 5 */
    unsigned output_bits;           /* 0: from the programmer's IDR2 */
    /* Maps the GMU memory into the context's VM (privileged, R/W, the CACHE ranges also executable); null: the caller maps it
     * after bringUp through vm(). */
    IOReturn (*map)(void *cookie,NativeAdrenoVm *vm);
    void *map_cookie;
};
class NativeAdrenoGmuContext final {
    NativeAdrenoMemory tables_;
    NativeAdrenoSmmuContext smmu_;
    NativeAdrenoVm vm_;
    unsigned output_bits_=0;
    bool ready_=false,used_=false;
public:
    enum Step : uint32_t {Allocate=1,Prepare,Program,Attach,VmAttach,Map,Ready};
    /* Lower region: 1 L1 + 2 L2 + 1024 L3 pages for 2 GiB with a 32-bit input. */
    enum : size_t {TablePages=1027};
    NativeAdrenoGmuContext()=default;
    NativeAdrenoGmuContext(const NativeAdrenoGmuContext &)=delete;
    NativeAdrenoGmuContext &operator=(const NativeAdrenoGmuContext &)=delete;
    /* One attempt per object. progress (may be null) is called before each step. */
    IOReturn bringUp(const NativeAdrenoGmuContextConfig &config,void (*progress)(uint32_t step),NativeAdrenoGmuContextReport *report);
    bool ready() const {return ready_;}
    /* The VM of the attached context (after VmAttach; the GMU memory maps through it). */
    NativeAdrenoVm *vm() {return used_ && vm_.attached()?&vm_:nullptr;}
    NativeAdrenoSmmuContext &smmu() {return smmu_;}
    const NativeAdrenoMemory &tables() const {return tables_;}
    unsigned outputBits() const {return output_bits_;}
};
/* The output size of an SMMU context from IDR2.OAS ({32,36,40,42,44,48}), capped at the driver's 40-bit allocation ceiling;
 * 0 for an encoding the kit cannot use. */
unsigned NativeAdrenoSmmuOutputBits(uint32_t idr2);
#endif
