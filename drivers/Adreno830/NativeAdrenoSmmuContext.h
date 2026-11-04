/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SMMU_CONTEXT_H
#define NATIVE_ADRENO_SMMU_CONTEXT_H
#include "NativeAdrenoSmmuMemory.h"
#include "ArmSmmu4KStreams.h"

/* The board's SMMU register programmer, injected into a context (ARCHITECTURE 2.2): the context keeps the qualified table pool
 * preparation of the kit (ArmSmmu4KLivePrepare through NativeAdrenoPrepareSmmuMemory) and the kit's image encoder, and asks the
 * programmer to put that image into a context bank and to route streams to it under the board's own rules. On the 13T this is
 * A830Smmu (ARCHITECTURE 4.5.4-4.5.7): FAULT-first stream claims, the tolerance rules of a hypervisor-emulated global space, a
 * TTBR1 guard table, record-only fault status. It never calls ArmSmmu4KProgramDisabledContext or ArmSmmu4KAttachStreams, whose
 * exact-value preconditions (S2CR 0x00020000, sGFSR 0, exact ACTLR) the hardware does not meet.
 * Every callback returns kIOReturnSuccess or the step's error; a failed callback leaves its resources retained. */
struct NativeAdrenoSmmuProgramReport {
    ArmSmmu4KContextImage want;      /* what the programmer wrote (the kit's image with the board's TTBR1/TCR changes) */
    ArmSmmu4KContextImage got;       /* the readback after the enable (ARCHITECTURE 4.5.4 step 14) */
    uint32_t actlr;                  /* written, and as read back (record only) */
    uint32_t actlr_read;
    uint32_t fsr;                    /* FSR after the enable (record only, step 16) */
    uint32_t mismatch;               /* fields beyond the tolerance rules: bit 0 CBAR, 1 CBA2R, 2 TCR, 3 TCR2, 4 MAIR0, 5 MAIR1,
                                        6 TTBR0, 7 TTBR1, 8 SCTLR */
    uint32_t step;                   /* last step started (ARCHITECTURE 8.3 numbering of the programmer) */
    uint32_t writes;                 /* register writes issued */
    uint64_t context_offset;         /* the bank's byte offset in the SMMU window */
};
struct NativeAdrenoSmmuProgrammer {
    void *cookie;
    /* The I/O the context's VM gives the kit's table transactions: only TLB maintenance of this bank goes through it after the
     * VM's attach (TLBIVA* as one 64-bit store, TLBIASID/TLBIALL/TLBSYNC as 32-bit stores, TLBSTATUS polled under the board's
     * time budget), plus the read-only fault registers of NativeAdrenoVm::faultState. */
    const ArmSmmu4KIo *io;
    uint32_t id0,id1,id2;            /* the SMMU's IDR0/IDR1/IDR2 as read by the programmer, for the kit's encoder */
    /* ARCHITECTURE 4.5.4 steps 1-2: the stream's S2CR to FAULT, then its exact SMR. */
    IOReturn (*claim)(void *cookie,unsigned bank,const ArmSmmu4KStream *stream);
    /* Steps 3-14: program and enable bank from image (TTBR1 replaced by ttbr1: 0, or ASID<<48 | a zeroed guard table, which
     * also sets the TTBR1 walk fields with EPD1 kept set). report is filled on every outcome. */
    IOReturn (*program)(void *cookie,unsigned bank,const ArmSmmu4KContextImage *image,uint64_t ttbr1,
                        NativeAdrenoSmmuProgramReport *report);
    /* Steps 15-16: the claimed stream's S2CR to translate to bank. */
    IOReturn (*route)(void *cookie,unsigned bank,const ArmSmmu4KStream *stream);
    /* Read-only: every stream routes to bank, the bank is enabled with this programmer's image (tolerance rules applied). */
    IOReturn (*verify)(void *cookie,unsigned bank,const ArmSmmu4KStream *streams,size_t count);
    /* The streams' S2CR back to FAULT, their SMRs kept valid so the SIDs stay blocked (ARCHITECTURE 4.5.7). */
    IOReturn (*fault)(void *cookie,unsigned bank,const ArmSmmu4KStream *streams,size_t count);
    /* SCTLR = 0, TLBIALL and TLBSYNC; only after fault. Tables may be freed only when this succeeded. */
    IOReturn (*disable)(void *cookie,unsigned bank);
    /* true: the VM follows every successful map with one invalidation of the bank's ASID and its TLBSYNC through io
     * (ArmSmmu4KSyncContextTlb, all = 0). A830Smmu sets it in the annibale profile, whose table I/O sends that as TLBIASID +
     * TLBSYNC = 1: the K90 port invalidated after a map too (kernel-sequences 7.5, section 14 row 12). false: a map issues
     * no TLB maintenance (ARCHITECTURE 4.6.4: an SMMU caches no invalid translation and the tables above the leaves are
     * fixed). */
    bool sync_after_map;
};
/* What a NativeAdrenoVm needs to update the tables of a programmed, routed context (NativeAdrenoSmmuContext::binding). */
struct NativeAdrenoSmmuBinding {
    const NativeAdrenoSmmuProgrammer *programmer;
    ArmSmmu4KContextConfig *context;   /* the kit configuration the regions were prepared with */
    unsigned bank;
    uint64_t context_offset;           /* from the programmer's report */
    const ArmSmmu4KStream *streams;    /* every stream routed to the bank */
    size_t stream_count;
};

/* One single-region GPU SMMU context (lower/TTBR0, as the GMU's 0..2 GiB space or the GPU's 0..4 GiB space, or upper/TTBR1),
 * composed from the qualified kits and an injected programmer: prepare an inactive native4K table pool, claim the streams and
 * program the bank through the programmer, then route the streams. Every step is separate and can only run in order; a failure
 * leaves the earlier state retained and marks this object unusable for further programming. The bank number, ASID and stream
 * slots are required inputs and are never guessed. No power, clock, reset or global SMMU setup is performed here. */
class NativeAdrenoSmmuContext final {
    NativeAdrenoMemory *tables_=nullptr;
    ArmSmmu4KLiveArena arena_{};
    ArmSmmu4KLiveState state_{};
    ArmSmmu4KLiveVisibility visibility_{};
    ArmSmmu4KContextConfig config_{};
    NativeAdrenoSmmuProgrammer programmer_{};
    NativeAdrenoSmmuProgramReport report_{};
    ArmSmmu4KStream streams_[8]{};
    size_t stream_count_=0,routed_=0;
    uint64_t upper_guard_=0;
    unsigned bank_=0,poll_budget_=0;
    bool prepared_=false,programmed_=false,attached_=false,poisoned_=false,disabled_=false;
public:
    NativeAdrenoSmmuContext()=default;
    ~NativeAdrenoSmmuContext()=default;
    NativeAdrenoSmmuContext(const NativeAdrenoSmmuContext &)=delete;
    NativeAdrenoSmmuContext &operator=(const NativeAdrenoSmmuContext &)=delete;
    /* A 16 GiB lower aperture (the GPU's by default, ARCHITECTURE 4.6.1) is 4194304 leaves, the 4 GiB one (pagani-gpu-va=4) 1048576;
     * the GMU's 2 GiB is 524288. */
    enum : size_t {MaxLeaves=size_t(1)<<22};
    enum : size_t {MaxStreams=8};
    /* iova/bytes describe the aperture in the chosen region; the pool must be contiguous.
     * output_bits: the context's output size, at most the SMMU's ID2.OAS (Linux uses OAS for stage 1). input_bits: the context's
     * input size (32..48, at most ID2.UBS, the stage-1 input bound the kit checks; T0SZ = 64 - input_bits), the aperture lies
     * below 2^input_bits. */
    IOReturn prepare(NativeAdrenoMemory *tables,unsigned upper,uint64_t iova,size_t bytes,
        size_t leaf_budget,unsigned bank,unsigned asid,unsigned poll_budget,unsigned output_bits=40,unsigned input_bits=32);
    /* The TTBR1 guard of a lower context (CB0, ARCHITECTURE 4.5.4 step 9): the physical address of one zeroed, published,
     * 4 KiB-aligned table page below 2^output_bits. Only after prepare and before program; 0 keeps TTBR1 = 0 (CB2). */
    IOReturn setUpperGuard(uint64_t physical);
    /* Claims each stream (steps 1-2), then programs and enables the bank (steps 3-14) through programmer. */
    IOReturn program(const NativeAdrenoSmmuProgrammer *programmer,const ArmSmmu4KStream *streams,size_t count);
    /* Routes the claimed streams to the enabled bank (steps 15-16). */
    IOReturn attach();
    /* One more stream on the enabled, attached bank: claim then route (the LPAC stream, ARCHITECTURE 4.5.5). */
    IOReturn addStream(const ArmSmmu4KStream &stream);
    /* Read-only observation of the owned routes and enabled context through the programmer. */
    IOReturn observe() const;
    /* Only after attach: the streams FAULT (SMRs kept), then the bank is disabled with a TLB invalidation and sync. The tables
     * stay owned by the caller until this succeeded. */
    IOReturn disable();
    bool prepared() const {return prepared_;}
    bool programmed() const {return programmed_;}
    bool attached() const {return attached_;}
    bool poisoned() const {return poisoned_;}
    unsigned bank() const {return bank_;}
    unsigned asid() const {return config_.asid;}
    const ArmSmmu4KContextConfig *configuration() const {return &config_;}
    const ArmSmmu4KLiveArena *arena() const {return &arena_;}
    /* Handles for a NativeAdrenoVm that will map buffers through this bank. */
    ArmSmmu4KLiveArena *arenaHandle() {return &arena_;}
    /* Scratch the kit's physical-alias checks sort in (ArmSmmu4KLiveArena::alias_scratch), owned by the caller for this context's
     * lifetime; without it those checks compare pairwise. Only after prepare. */
    void setAliasScratch(uint32_t *entries,size_t count) {arena_.alias_scratch=entries;arena_.alias_scratch_entries=entries?count:0;}
    ArmSmmu4KLiveState *stateHandle() {return &state_;}
    ArmSmmu4KContextConfig *configHandle() {return &config_;}
    /* Only after attach: the VM binding (programmer, kit configuration, bank, its register offset, the routed streams). */
    NativeAdrenoSmmuBinding binding();
    /* The programmer's report of the last program step (zero before it ran). */
    const NativeAdrenoSmmuProgramReport *programReport() const {return &report_;}
    const ArmSmmu4KStream *streams() const {return streams_;}
    size_t streamCount() const {return stream_count_;}
    uint64_t upperGuard() const {return upper_guard_;}
};
#endif
