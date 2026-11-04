/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_RING_H
#define NATIVE_ADRENO_RING_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* Gen8 (A830) command processor ring and timestamp fence, not a scheduler (ARCHITECTURE 4.9, 4.10, 4.13).
 * Register indices are GPU-space dword offsets; the platform adds the MMIO base and multiplies by four. The ring object only
 * owns the ring memory, the memptrs page and the write pointer: A830Cp::startRing programs CP_RB_BASE/CNTL/RPTR_ADDR(_BV)
 * (KGSL gen8_rb_start order) and A830Cp::startCp starts the SQE. attach() binds and clears the memory and never touches a
 * register; the hardware RPTR/WPTR found at start are only recorded by A830Cp (ARCHITECTURE 10 #1).
 * Ring and memptrs CPU addresses must be write-combined (or otherwise non-cacheable) views of the same IOVAs mapped in the GPU
 * context (ARCHITECTURE 4.14.1): publication is a barrier before the WPTR write, never a cache clean of the ring. The memptrs
 * page is struct msm_rbmemptrs: +0 BR RPTR shadow, +4 completed fence, +8 BV RPTR shadow, +12 BV fence (ARCHITECTURE 4.13.1).
 * Every WPTR write is the KGSL fenced write (gen8_fenced_write, ARCHITECTURE 4.13.2): write, barrier, read the GMU AHB fence
 * status, and while WRITEDROPPED0 is set wait 10 us and write again, at most 200 times; the 200th failure is an error that
 * leaves the ring poisoned for the recovery (never a silent success).
 * Completion waits are A830Fence's (ARCHITECTURE 4.13.3): with a waiter installed, wait() sleeps in it; without one (host
 * tests, early bring-up) it polls the memptrs fence with the caller's clock. Nothing on the submit path logs, cleans caches or
 * reads registers other than through the fenced WPTR write. */
struct NativeAdrenoRingIo {
    void *cookie;
    /* Return zero only on a completed access. */
    int (*write32)(void *,uint32_t reg,uint32_t value);
    int (*read32)(void *,uint32_t reg,uint32_t *value);
    int (*barrier)(void *);   /* dsb sy */
    /* Optional: the GMU AHB fence status (GMUAO_AHB_FENCE_STATUS, bit 0 WRITEDROPPED0: a write to a fenced GPU register was
     * dropped while the GX domain was not ready); clear != 0 also clears it. Without it, WPTR writes are not checked. */
    void *fence_cookie;
    int (*fence_status)(void *,int clear,uint32_t *status);
    /* Optional: busy-wait between fenced WPTR retries (IODelay); without it the retries follow each other directly. */
    void (*delay_us)(void *,uint32_t us);
};
struct NativeAdrenoRingCache {
    void *cookie;
    /* Both optional (write-combined memory needs neither). publish cleans exactly the ring words a submission wrote (two calls
     * when it wraps) before the device reads them; acquire invalidates the memptrs page before the CPU reads it. */
    int (*publish)(void *,const volatile uint32_t *words,size_t bytes);
    int (*acquire)(void *,volatile uint32_t *words,size_t bytes);
};
struct NativeAdrenoRingClock {
    void *cookie;
    uint64_t (*now_us)(void *);
    /* Optional; may sleep. Never used before a successful submit. */
    int (*idle)(void *);
};
/* The completion wait of A830Fence (ARCHITECTURE 4.13.3): sleep until the memptrs fence reaches fence or timeout_us passes.
 * Returns kIOReturnSuccess (passed), kIOReturnTimeout (not yet; also when the sleep was interrupted) or any other error for a
 * lost device. observed receives the last completed fence seen. Called without any ring lock held.
 * stamp (optional; A830Fence::ringStamp, the queue meter of 4.13.5): called with the ring lock held for every kernel and framed
 * submission, after its words are written and published and right before the barrier and the WPTR write that hand fence to
 * the CP, so the stamp is in memory before the CP can complete the work. flags: whose work it is (NativeAdrenoRingSource*: the
 * kernel's for submit(), the source submitFramed() was given) and NativeAdrenoRingStampContext when it starts with the context
 * entry. It must not block. */
enum : uint32_t {
    NativeAdrenoRingSourceKernel=0,      /* submit(): the driver's own work */
    NativeAdrenoRingSourceRender=1,      /* submitFramed() for the render server (backboardd) */
    NativeAdrenoRingSourceExternal=2,    /* submitFramed() for any other process */
    NativeAdrenoRingSourceMask=3,
    NativeAdrenoRingStampContext=1u<<2   /* the submission carries the context entry (a client switch, ARCHITECTURE 4.10) */
};
struct NativeAdrenoRingWaiter {
    void *cookie;
    IOReturn (*wait)(void *,uint32_t fence,uint32_t timeout_us,uint32_t *observed);
    void (*stamp)(void *,uint32_t fence,uint32_t flags);
};
/* The ring lock (ARCHITECTURE 5.4, M10): serializes submissions, kick and recover between user clients and the recovery
 * thread; may sleep. Without one the callers serialize every method. Waits never take it. */
struct NativeAdrenoRingLock {
    void *cookie;
    void (*lock)(void *);
    void (*unlock)(void *);
};
enum NativeAdrenoRingFence {
    NativeAdrenoRingFenceComplete = 0,
    NativeAdrenoRingFenceTimeout,
    NativeAdrenoRingFencePoisoned,
    NativeAdrenoRingFenceArgument,
    NativeAdrenoRingFenceLost        /* the waiter reported a lost device (A830Fence: kIOReturnNotResponding) */
};
enum : uint32_t {NativeAdrenoRingStallWords = 22};   /* NativeAdrenoRing::stallState */
/* The registers the ring emits into packets or reads, as GPU-space dword offsets (Gen8 values in NativeAdrenoRingGen8Regs). */
struct NativeAdrenoRingRegs {
    uint32_t base,cntl,rptr_addr,bv_rptr_addr;           /* CP_RB_BASE 0x800, CP_RB_CNTL 0x802, CP_RB_RPTR_ADDR 0x804,
                                                            CP_RB_RPTR_ADDR_BV 0x808 (A830Cp::startRing; never SQE_CNTL) */
    uint32_t rptr,wptr,bv_rptr;                          /* CP_RB_RPTR 0x806, CP_RB_WPTR 0x807, CP_RB_RPTR_BV 0x80A */
    uint32_t scratch;                                    /* CP_SCRATCH_GLOBAL[2] 0x82C: each fence's number (A7XX 0x885) */
    uint32_t sram_init,slice_sram_init,sram_init_status; /* 0x449, 0x5E8 (0: none), 0x19F (A7XX 0x50E, -, 0x50F) */
    /* stallState(): global registers only (no CP_APERTURE_CNTL_HOST), in this order (Gen8):
     *  0 RBBM_STATUS 0x12, 1 RBBM_STATUS1 0x13, 2 RBBM_GFX_STATUS 0x15, 3 RBBM_GFX_STATUS1 0x16, 4 RBBM_INT_0_STATUS 0x6A,
     *  5 CP_INTERRUPT_STATUS_GLOBAL 0x8B5, 6 CP_CP2GMU_STATUS 0x822, 7 CP_RL_ERROR_DETAILS_0 0x840, 8 CP_RB_RPTR 0x806,
     *  9 CP_RB_WPTR 0x807, 10 RBBM_GFX_BR_STATUS 0x1A, 11 RBBM_GFX_BV_STATUS 0x1C, 12 RBBM_MISC_STATUS 0x1E,
     *  13 CP_RL_ERROR_DETAILS_1 0x841, 14 RBBM_INT_0_MASK 0x62, 15 CP_SQE_CNTL 0x815, 16 CP_RB_RPTR_BV 0x80A,
     *  17 CP_SCRATCH_GLOBAL[2] 0x82C, 18/19 CP_ALWAYS_ON_COUNTER 0x8E7/0x8E8, 20 RBBM_LPAC_STATUS 0x18,
     *  21 RBBM_SW_FUSE_INT_STATUS 0x71. Index 8 and 16 are the BR and BV read pointers ibBefore() takes. The per-pipe
     *  registers (IB1/IB2, fault status, SQE PC) need the aperture and are read by A830Cp::dumpStall/readProgress. */
    uint32_t stall[NativeAdrenoRingStallWords];
};
extern const NativeAdrenoRingRegs NativeAdrenoRingGen8Regs;
struct NativeAdrenoRingWptrStats {
    uint32_t writes;     /* fenced WPTR writes */
    uint32_t dropped;    /* writes the fence dropped at least once */
    uint32_t retries;    /* repeated writes in total */
    uint32_t slow;       /* writes that needed more than NativeAdrenoRingWptrSlowAfter retries (recorded, ARCHITECTURE 4.13.2) */
    uint32_t failures;   /* writes still dropped after the last retry: the ring is poisoned, the device goes to recovery */
};
enum : uint32_t {
    NativeAdrenoRingPrologueWords = 11,      /* framed prologue without the context entry */
    NativeAdrenoRingResetWords = 28,         /* a6xx_set_pagetable()'s A8XX context entry (two SRAM init writes) */
    NativeAdrenoRingFenceWords = 7,          /* kernel submissions: CP_SCRATCH_GLOBAL[2] + CACHE_CLEAN|IRQ timestamp */
    NativeAdrenoRingFramedFenceWords = 31,   /* user submissions: scratch, BV timestamp, BR wait, ring timestamp */
    NativeAdrenoRingGapWords = 8,            /* never filled: a full ring is never mistaken for an empty one */
    NativeAdrenoRingWptrRetries = 200,       /* KGSL GMU_CORE_LONG_WAKEUP_RETRY_LIMIT */
    NativeAdrenoRingWptrSlowAfter = 100,     /* KGSL GMU_CORE_SHORT_WAKEUP_RETRY_LIMIT: recorded */
    NativeAdrenoRingWptrDelayUs = 10,        /* KGSL GMU_CORE_WAKEUP_DELAY_US */
    NativeAdrenoRingFenceBvOffset = 12       /* msm_rbmemptrs.bv_fence */
};
class NativeAdrenoRing final {
    NativeAdrenoRingIo io_{};
    NativeAdrenoRingCache cache_{};
    NativeAdrenoRingWaiter waiter_{};
    NativeAdrenoRingLock lock_{};
    NativeAdrenoRingRegs regs_{};
    NativeAdrenoRingWptrStats wptr_{};
    volatile uint32_t *ring_=nullptr,*fence_=nullptr;
    size_t ring_bytes_=0,fence_bytes_=0;
    uint64_t ring_iova_=0,fence_iova_=0;
    uint32_t next_=0,rptr_shadow_=0,seqno_=0,completed_=0,submissions_=0;
    bool attached_=false,poisoned_=false,reset_pending_=false;
    uint32_t context_=0;
    IOReturn poison();
    void enter() const;
    void leave() const;
    IOReturn writeWptr(uint32_t value);
    uint32_t readShadow();
    IOReturn emitPrologue(uint32_t at,bool reset,uint32_t *end);
    IOReturn emitFence(uint32_t at,uint32_t seqno,uint32_t *end);
    IOReturn emitFramedFence(uint32_t at,uint32_t seqno,uint32_t *end);
    IOReturn publish(uint32_t from,uint32_t to);
    uint32_t extraWords(bool framed,bool reset) const;
    IOReturn submitWords(const uint32_t *words,size_t word_count,uint32_t *fence,int kind,uint32_t source);
    NativeAdrenoRingFence poll(uint32_t fence,const NativeAdrenoRingClock *,uint32_t timeout_us,uint32_t *observed,bool retire);
    void noteCompleted(uint32_t value);
public:
    NativeAdrenoRing()=default;
    ~NativeAdrenoRing()=default;
    NativeAdrenoRing(const NativeAdrenoRing &)=delete;
    NativeAdrenoRing &operator=(const NativeAdrenoRing &)=delete;
    /* ring_bytes is a power of two in [8KiB,1MiB]; ring_iova must be aligned to ring_bytes. fence_bytes >=64 and fence_iova
     * 8-aligned. regs null means NativeAdrenoRingGen8Regs. All inputs are validated before the ring words and the memptrs
     * page are cleared; no register is written (A830Cp::startRing programs the ring). Software pointers start at 0 and the
     * first framed submission carries the context entry (Linux emits it for the first context after hw_init). */
    IOReturn attach(const NativeAdrenoRingIo *,const NativeAdrenoRingCache *,
        void *ring,size_t ring_bytes,uint64_t ring_iova,
        void *fence,size_t fence_bytes,uint64_t fence_iova,const NativeAdrenoRingRegs *regs=nullptr);
    /* Programs nothing. Refuses while submissions are unobserved or poisoned, because only the caller can establish real GPU
     * quiescence and unmap. */
    IOReturn detach();
    /* Optional hooks (copied; null clears). */
    void setWaiter(const NativeAdrenoRingWaiter *);
    void setLock(const NativeAdrenoRingLock *);
    /* Kernel command stream followed by the 7-word timestamp fence (CP_SCRATCH_GLOBAL[2], CP_EVENT_WRITE7 CACHE_CLEAN with
     * IRQ to memptrs+4), committed with one fenced WPTR update. Returns a nonzero fence on success. The words are copied.
     * kIOReturnBusy: the ring is full now (retry after a completion, see waitForSpace); kIOReturnNoResources: the stream can
     * never fit this ring. */
    IOReturn submit(const uint32_t *words,size_t word_count,uint32_t *fence);
    /* User command buffers: Linux a7xx_submit() framing (BR/BV threads, CCU invalidates, BV timestamp, BR wait) and, on a
     * context change, the A8XX context entry of a6xx_set_pagetable() with both SRAM init writes and the 0x19F wait. source
     * (NativeAdrenoRingSourceRender or External) changes no word: it only goes to the stamp, for the queue meter's attribution. */
    IOReturn submitFramed(const uint32_t *words,size_t word_count,uint32_t *fence,uint32_t source=NativeAdrenoRingSourceExternal);
    /* Packets without a timestamp fence (CP_ME_INIT, CP_SET_SECURE_MODE: KGSL/Linux spin on RPTR == WPTR instead). wptr
     * receives the new write index, which the CP has consumed everything at when CP_RB_RPTR equals it (A830Cp::waitIdle). */
    IOReturn submitRaw(const uint32_t *words,size_t word_count,uint32_t *wptr);
    /* The next framed submission starts with the context entry (a new user client, as Linux does per context). */
    void requestContextReset() {reset_pending_=true;}
    /* The client whose submission comes next; a change of client is a context switch (a6xx_set_pagetable()). */
    void useContext(uint32_t context) {if(context!=context_) {context_=context;reset_pending_=true;}}
    /* Completion of an issued fence: the waiter's sleep when installed, else polling the memptrs fence with the clock (each
     * round calls clock->idle). Never writes registers and never advances the ring. Timeout is not poison. Retires: the
     * completed fence advances. */
    NativeAdrenoRingFence wait(uint32_t fence,const NativeAdrenoRingClock *,uint32_t timeout_us,uint32_t *observed);
    /* The same check without retiring anything (a later submission may wait on an earlier batch). Only fence values this
     * ring issued are accepted. */
    NativeAdrenoRingFence observeFence(uint32_t fence,const NativeAdrenoRingClock *,uint32_t timeout_us,uint32_t *observed);
    /* Until word_count more words (framed or not) fit: sleeps on the next completion through the waiter, or polls with the
     * clock. kIOReturnTimeout when they still do not fit; kIOReturnNoResources when they never can. */
    IOReturn waitForSpace(size_t word_count,bool framed,const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Reads regs.stall into out[NativeAdrenoRingStallWords]. Never writes a register; a failed read is poison. */
    IOReturn stallState(uint32_t *out);
    /* Writes CP_RB_WPTR again (fenced) with the value already published: diagnostics and the recovery only, never a normal
     * wait (ARCHITECTURE 4.13.4). */
    IOReturn kick();
    /* After the recovery re-initialized the GPU (ARCHITECTURE 4.13.4, M10): ring words and pointers back to 0, memptrs RPTR
     * shadows 0, the memptrs fences set to the last fence issued so every waiter wakes, the poison lifted, and the next
     * framed submission carries the context entry. The fence numbering continues. last receives the last fence issued: every
     * fence in (completed before, last] belongs to a job the recovery failed. Call before A830Cp::startRing/startCp. */
    IOReturn recover(uint32_t *last);
    /* The CP_INDIRECT_BUFFER packet that ends just before ring index rptr (what a thread whose read pointer is rptr executes):
     * its GPU address and size in dwords. kIOReturnUnsupported when the words there are not that packet. */
    IOReturn ibBefore(uint32_t rptr,uint64_t *iova,uint32_t *dwords) const;
    bool attached() const {return attached_;}
    bool poisoned() const {return poisoned_;}
    uint32_t outstanding() const {return seqno_-__atomic_load_n(&completed_,__ATOMIC_ACQUIRE);}
    uint32_t lastSubmitted() const {return __atomic_load_n(&seqno_,__ATOMIC_ACQUIRE);}   /* the newest fence handed out */
    uint32_t completedFence() const {return __atomic_load_n(&completed_,__ATOMIC_ACQUIRE);}
    uint32_t submissions() const {return submissions_;}
    uint32_t readPointer() const {return rptr_shadow_;}       /* the BR RPTR shadow last read */
    uint32_t writeIndex() const {return next_;}               /* the WPTR last published */
    size_t ringBytes() const {return ring_bytes_;}
    uint64_t ringIova() const {return ring_iova_;}
    uint64_t memptrsIova() const {return fence_iova_;}
    /* The memptrs page (A830Fence reads +4 with acquire semantics; A830Cp::waitIdle reads +0). Null before attach. */
    const volatile uint32_t *memptrs() const {return fence_;}
    const NativeAdrenoRingWptrStats &wptrStats() const {return wptr_;}
    const NativeAdrenoRingRegs &regs() const {return regs_;}
};
#endif
