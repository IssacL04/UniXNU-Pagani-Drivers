/* SPDX-License-Identifier: MIT */
#ifndef A830_SELF_TEST_H
#define A830_SELF_TEST_H
#include "A830Fence.h"
#include "A830Irq.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* The S8 checks (ARCHITECTURE 3.4 S8, 4.14.2, 8.3, 8.4; PLAN M6, M10), called by the orchestrator before anything is
 * published:
 *   1. attach the GPU interrupt 332 (A830Irq::attach; refused -> reason 19);
 *   2. CP_MEM_WRITE of 0x08750830 to the scratch page with the ring's IRQ fence: the scratch reads it back, the fence
 *      interrupt count grows, the waiter is woken by it (not by the 16 ms safety slice), all within 100 ms;
 *   3. CP_ALWAYS_ON_COUNTER against the GMU AO counter (record only);
 *   4. V4 (coherent page-table walk) and V9a (CPU write -> GPU read through a WB+IS mapping), record only;
 *   5. with pagani-gpu-cohtest=1 only, at the end: V9b (GPU write -> CPU read) and V9c (a 2D-engine pass writes a WB+IS target,
 *      then CACHE_CLEAN; the A740 hang), 100 ms each. A timeout caps the coherence level at 1 and stops S8 with reason 18,
 *      or, once the minimal recovery exists (recover hook, M10), recovers and continues with level <= 1.
 * The results go to the status block (S8 page aux words, coh_limit) and to the VM coherence policy (vm.policy):
 * V4 pass -> cacheable page-table pages (M10), level 1 (V9a) -> GPU-read-only imports mapped WB+IS (io_coherent), level 2
 * (V9a + V9b + V9c in this boot) -> NativeAdrenoDeviceCachedCoherent may be offered. pagani-gpu-coherent, when given, can
 * only lower the level the tests allow. */

/* S8 step numbers (ARCHITECTURE 8.3: V9b 0x29, V9c 0x2A; the others follow the same scheme). */
enum : uint16_t {
    A830S8StepAttach=0x01,      /* armcpu_spi_attach(332): reg = INTID, value = IOReturn */
    A830S8StepMemWrite=0x02,    /* the self-test fence: reg = scratch IOVA, value = completed fence, expect = its seqno */
    A830S8StepReadback=0x03,    /* reg = scratch IOVA, value = scratch word, expect = 0x08750830 */
    A830S8StepIrq=0x04,         /* fence interrupts: value = count after, expect = count before + 1 */
    A830S8StepWake=0x05,        /* the waiter was not woken by the interrupt: value = slices, expect = 0 */
    A830S8StepSubmit=0x06,      /* the ring refused a self-test stream: value = IOReturn */
    A830S8StepV4=0x24,
    A830S8StepV9a=0x28,
    A830S8StepV9b=0x29,
    A830S8StepV9c=0x2A,
};
enum : uint32_t {
    A830SelfTestScratchValue=0x08750830u,
    /* The scratch page (A830_GPU_VA_SCRATCH, Normal-NC on both sides) as the checks use it. */
    A830SelfTestScratchMemWrite=0x000,   /* 1 dword */
    A830SelfTestScratchV4=0x100,         /* 16 dwords */
    A830SelfTestScratchV9a=0x200,        /* 64 dwords, one per line of the probe page */
    A830SelfTestScratchBytes=0x300,
    A830SelfTestV4Words=16,A830SelfTestV9Lines=64,A830SelfTestV9aRounds=8,A830SelfTestLineBytes=64,
};

/* One probe page: ordinary cacheable (write-back) kernel memory, 4 KiB, never made write-combined. */
struct A830SelfTestPage {
    volatile uint32_t *cpu;     /* the kernel's cacheable view */
    uint64_t physical;
    uint64_t iova;              /* set by map */
    uint64_t handle;            /* the mapper's handle */
    void *owner;                /* the allocator's object */
};
enum A830SelfTestMap : unsigned {
    A830SelfTestMapCachedTable,   /* V4: NC data; the new leaf written through the table page's cacheable view, dsb ishst only */
    A830SelfTestMapCoherentRo,    /* WB+IS read-only (A830_PTE_COHERENT_RO): V9a */
    A830SelfTestMapCoherentRw,    /* WB+IS read-write (A830_PTE_COHERENT_RW): V9b, V9c */
};
enum A830SelfTestCache : unsigned {A830SelfTestClean,A830SelfTestInvalidate,A830SelfTestCleanInvalidate};
enum : uint8_t {A830CoherenceOff,A830CoherenceReadOnly,A830CoherenceFull};   /* levels 0/1/2 (pagani-gpu-coherent) */

struct A830CoherenceResult {
    uint8_t v4,v9a,v9b,v9c;       /* A830CohNotRun / A830CohPass / A830CohFail (Adreno830Status.h) */
    uint8_t limit;                /* what the tests allow: 0, 1 (V9a), 2 (V9a + V9b + V9c) */
    uint8_t level;                /* in effect: limit, lowered by an explicit pagani-gpu-coherent */
    bool tables_cacheable;        /* V4 passed */
    bool timed_out;               /* a V9b/V9c fence did not complete within the limit */
    uint32_t v4_mismatch,v9a_mismatch,v9b_mismatch,v9c_mismatch;   /* words that differed */
    uint32_t v4_fsr;              /* CB0 fault status after the V4 copy (0 = none) */
    uint64_t v4_far;
    uint32_t v9a_rounds;          /* V9a rounds that matched */
};

/* The GPU address-space side (NativeAdrenoGpuContext / NativeAdrenoVm / A830Smmu), wired by the orchestrator. */
struct A830SelfTestVm {
    void *cookie;
    IOReturn (*allocate)(void *cookie,A830SelfTestPage *page);   /* one cacheable 4 KiB page */
    void (*release)(void *cookie,A830SelfTestPage *page);
    IOReturn (*map)(void *cookie,A830SelfTestPage *page,unsigned mode);   /* A830SelfTestMap; sets page->iova and handle */
    IOReturn (*unmap)(void *cookie,A830SelfTestPage *page);
    void (*maintain)(void *cookie,A830SelfTestPage *page,unsigned op);   /* A830SelfTestCache over the whole page */
    uint32_t (*faults)(void *cookie,uint64_t *far);   /* CB0 FSR fault bits since the previous call (and clears them); 0 = none */
    void (*policy)(void *cookie,const A830CoherenceResult *result);   /* the VM's coherence policy */
};

struct A830SelfTestEnv {
    A830Irq *irq;
    A830Fence *fence;
    void *ring;
    IOReturn (*submit)(void *ring,const uint32_t *words,size_t count,uint32_t *fence);   /* NativeAdrenoRing::submit */
    volatile uint32_t *scratch;   /* CPU view of the scratch page (write-combined) */
    uint64_t scratch_iova;        /* A830_GPU_VA_SCRATCH */
    A830SelfTestVm vm;            /* all members null: V4 and V9 are not run (shown "-") */
    void *timer;
    uint64_t (*ao_counter)(void *timer);    /* GMU AO counter, 19.2 MHz (NativeAdrenoTimer); null: 0 */
    void *recovery;
    IOReturn (*recover)(void *recovery,uint16_t reason);   /* M10 minimal recovery; null: a V9b/V9c timeout stops S8 (r18) */
    uint32_t coherent;            /* pagani-gpu-coherent */
    bool coherent_given;          /* ... was on the command line */
    bool cohtest;                 /* pagani-gpu-cohtest */
    uint32_t timeout_us;          /* 0: A830_T_SELFTEST_US (100 ms) */
};

struct A830SelfTestReport {
    uint32_t attach_result;       /* IOReturn of armcpu_spi_attach */
    uint32_t readback;            /* GIC readback (rb A003 expected) */
    uint64_t router;
    uint64_t irq_count;           /* 332 entries */
    uint64_t fence_irqs;          /* fence interrupts after the CP_MEM_WRITE check */
    uint32_t wake_us;             /* fence interrupt -> waiter running */
    uint32_t scratch;             /* CP_MEM_WRITE readback */
    uint32_t fence;               /* the CP_MEM_WRITE fence */
    uint64_t cp_counter,ao_counter;
    A830CoherenceResult coherence;
    bool recovered;               /* a V9b/V9c timeout was recovered */
};
struct A830SelfTestFailure {uint16_t step,reason;uint64_t reg,value,expect;};
extern "C" A830SelfTestReport a830_selftest_last;   /* the latest S8 run (host tools) */

class A830SelfTest final {
    A830SelfTestEnv env_{};
    uint32_t timeoutUs() const;
    IOReturn fenced(const uint32_t *words,size_t count,uint32_t *seq,A830WaitStats *stats,A830SelfTestFailure *failure,
                    uint16_t step);
    bool mapped(A830SelfTestPage *page,unsigned mode);
    void drop(A830SelfTestPage *page,bool mapped_page);
    uint8_t checkV4(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung);
    uint8_t checkV9a(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung);
    uint8_t checkV9b(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung);
    uint8_t checkV9c(A830CoherenceResult *result,A830SelfTestFailure *failure,bool *hung);
public:
    explicit A830SelfTest(const A830SelfTestEnv &env);
    /* Step 2: CP_MEM_WRITE value to iova (whose CPU view is cpu) with the IRQ fence; checks readback, interrupt count and the
     * wakeup. Fills the IRQ part of report. */
    IOReturn memWrite(uint64_t iova,volatile uint32_t *cpu,uint32_t value,A830SelfTestReport *report,A830SelfTestFailure *failure);
    /* Step 4 and (cohtest) step 5. Returns an error only when a probe hung the GPU and it could not be recovered (reason 18). */
    IOReturn coherence(bool cohtest,A830CoherenceResult *result,A830SelfTestFailure *failure,bool *recovered=nullptr);
    /* Step 3: CP_ALWAYS_ON_COUNTER (hi-lo-hi) and the GMU AO counter. */
    void timestamps(uint64_t *cp,uint64_t *ao);
    /* The whole S8 sequence; on success the S8 page and coh_limit are written and vm.policy called. */
    IOReturn run(uint32_t gpu_intid,A830SelfTestReport *report,A830SelfTestFailure *failure);
    /* The coherence level the results allow, and the one in effect for a pagani-gpu-coherent setting. */
    static uint8_t limitOf(const A830CoherenceResult &result);
    static uint8_t levelOf(const A830CoherenceResult &result,uint32_t coherent,bool coherent_given);
    /* S8 page (Adreno830Status.h): aux[0] readback | wake us << 32, aux[1] 332 count, aux[2] V4/V9a/V9b/V9c (2 bits each),
     * aux[3] CP counter, aux[4] AO counter, aux[5] scratch | fence << 32; coh_limit. Store only. */
    static void record(const A830SelfTestReport &report);
    /* The words of each probe stream (exposed for the host tests). */
    static size_t memWriteWords(uint64_t iova,uint32_t value,uint32_t *out,size_t capacity);
    static size_t copyWords(const uint64_t *src,const uint64_t *dst,size_t count,bool invalidate,uint32_t *out,size_t capacity);
    static size_t fillWords(const uint64_t *dst,const uint32_t *values,size_t count,uint32_t *out,size_t capacity);
    static size_t blitWords(uint64_t iova,uint32_t bytes,uint32_t value,uint32_t *out,size_t capacity);
};
#endif
