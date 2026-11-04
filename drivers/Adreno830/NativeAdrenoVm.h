/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_VM_H
#define NATIVE_ADRENO_VM_H
#include "NativeAdrenoSmmuContext.h"
#include "NativeAdrenoHandleIndex.h"
struct NativeAdrenoVmRegion {
    NativeAdrenoMemory *tables;
    ArmSmmu4KLiveArena *arena;
    ArmSmmu4KLiveState *state;
};
struct NativeAdrenoVmResult {
    ArmSmmu4KStatus operation,quarantine;
    size_t completed_spans;
    bool complete;
};
/* Kernel-owned BO mapping transactions of one context bank, not a userspace ABI or GPU scheduler.
 * Attach under the actual bank/ASID/streams of a programmed, routed context (NativeAdrenoSmmuContext::binding) with empty
 * native4K arenas. Attach proves the whole of both regions once (the kit's table audit) and the routes and context through the
 * board's programmer (A830Smmu::verifyRoutes); after it the table transactions read no SMMU register: a map writes leaves and
 * publishes their pages (a clean to PoC, or only dsb ishst once the coherent table walk was proved, ARCHITECTURE 4.6.1/4.14.2),
 * an unmap clears leaves and invalidates their pages (<= 512 by TLBIVAL, else TLBIASID) with one TLBSYNC (4.6.4). A programmer
 * with sync_after_map (A830Smmu's annibale profile) has every map end with one TLBIASID of the bank and its TLBSYNC too. No fault
 * status, sGFSR or route is polled per operation: a client's page fault never poisons the shared VM (ARCHITECTURE 4.12.3).
 * A table transaction that cannot be proved poisons the VM, keeps every old/new BO reference and FAULTs the owned streams.
 * The real table allocations, context/streams and BO objects remain retained by the owning driver. No implicit destructor
 * cleanup; detach must succeed. Every method serializes on the VM's own lock (ARCHITECTURE 5.4, the M10 lock split); callers
 * may hold their own locks around it but never call back into the VM from a hook. A buffer is unmapped only once the fence of
 * its last use completed (noteUse/setFenceSource) and no job holds it: the GPU is not idled globally. */
/* One table for every client's mappings: 1024 filled up once the render server primed its shaders (int-237: BoMap refused with
 * kIOReturnNoResources at objects=1024, Turnip wrote through a NULL command stream and backboardd crashed), and 4096 after a few app
 * switches (i567: both backboardd crashes came at objects=4096, after it held about 4000 for minutes). 32768 since every app may be a
 * Metal client (2026-10-30): 2.5 MiB of records and a 1 MiB index, allocated once at attach. */
enum : size_t {NativeAdrenoVmMaxObjects=32768,NativeAdrenoVmMaxWindows=4};
/* What a mapping of any client but the render server must leave free in the shared tables (mapClient's keep arguments, 2026-10-30):
 * a refusal there costs one app a VK_ERROR_OUT_OF_DEVICE_MEMORY, the render server's own refusal the whole interface (int-237).
 * backboardd held about 4000 objects for minutes (i567); all clients together held 52609 table ranges once (i568). */
enum : size_t {NativeAdrenoVmRenderReserveObjects=8192,NativeAdrenoVmRenderReserveSpans=32768};
/* Mappings refused for the render server's reserve: [0] for objects, [1] for table ranges. */
extern "C" volatile uint32_t native_adreno_vm_reserve_refused[2];
/* Coherence level (ARCHITECTURE 4.14.1, 3.6 pagani-gpu-coherent): 0 every GPU mapping non-cacheable; 1 GPU-read-only imports
 * write-back inner-shareable (needs V9a); 2 also GPU-written imports and own BOs a client maps write-back (CachedCoherent, needs
 * V9b and V9c). The effective level is native_adreno_io_coherent; the board sets it once S8 decided (NativeAdrenoVm::setCoherence). */
enum NativeAdrenoVmCoherence : unsigned {NativeAdrenoCoherenceOff=0,NativeAdrenoCoherenceReadImports=1,NativeAdrenoCoherenceCached=2};
extern "C" volatile uint32_t native_adreno_io_coherent;
/* Board hook for sampleWord(): reads one aligned word of physical memory through a fresh uncached mapping; 0 on success. */
extern "C" int (*native_adreno_dram_read32)(uint64_t physical,uint32_t *value);
/* The board's reader through the kernel's cacheable physical aperture (null on the host: the BO's own mapping is read). */
extern "C" int (*native_adreno_cached_read32)(uint64_t physical,uint32_t *value);
/* Board hook for auditRange(): [physical, physical+bytes), whole 64-byte lines inside one page, as the cacheable physical aperture and
 * DRAM (a fresh uncached mapping) read it. A line that differs is cleaned and invalidated and DRAM read again: when DRAM then holds the
 * cached copy the line was dirty (*dirty), when DRAM is unchanged it was a stale clean copy (*stale). 0 on success. */
extern "C" int (*native_adreno_line_audit)(uint64_t physical,size_t bytes,uint32_t *dirty,uint32_t *stale);
class NativeAdrenoVm final {
    struct Mapping {
        NativeAdrenoMemory *memory=nullptr;
        uint64_t handle=0,iova=0,last_use=0;
        size_t spans=0,mapped_spans=0;
        unsigned permissions=0,writeback=0,shareability=0,jobs=0;
        uint32_t prev=UINT32_MAX,next=UINT32_MAX;   /* the maintained list (GPU-read-only imports mapped non-cacheable) */
        bool maintained=false;
    };
    Mapping *mappings_=nullptr;
    NativeAdrenoHandleIndex<NativeAdrenoVmMaxObjects> index_;   /* live handle -> its mappings_ slot */
    size_t free_hint_=0;   /* no free mappings_ slot below it */
    size_t high_=0;   /* no live mappings_ slot at or above it: the walks over every mapping stop there */
    size_t class_spans_[16]{};   /* ranges_ per alias class, so a map counts the other classes without a walk */
    size_t import_spans_[16]{};   /* the imported mappings' ranges per alias class */
    uint32_t maintained_head_=UINT32_MAX;
    size_t maintained_count_=0;
    NativeAdrenoVmRegion regions_[2]{};
    ArmSmmu4KLiveVisibility visibility_[2]{};
    ArmSmmu4KIo io_{};
    ArmSmmu4KContextConfig *context_=nullptr;
    /* The routes and the programmer: a board programmer (verify, fault), or the kit's stream state of the legacy attach. */
    NativeAdrenoSmmuProgrammer programmer_{};
    bool has_programmer_=false;
    bool sync_after_map_=false;   /* the programmer's sync_after_map: each map ends with the bank's ASID invalidation */
    ArmSmmu4KStreamState *kit_streams_=nullptr;
    ArmSmmu4KStream streams_[NativeAdrenoSmmuContext::MaxStreams]{};
    size_t stream_count_=0;
    unsigned bank_=0;
    uint64_t context_offset_=0;
    struct Window {uint64_t iova,bytes;unsigned upper;} windows_[NativeAdrenoVmMaxWindows]{};
    size_t window_count_=0;
    const volatile uint32_t *fence_=nullptr;
    void *lock_=nullptr;
    ArmSmmu4KRange *ranges_=nullptr;
    size_t capacity_=0,lower_count_=0,upper_count_=0,objects_=0;
    uint64_t bytes_=0;   /* bytes of every live mapping (own, pooled, imported, scanout): the MEM row's vm */
    unsigned budget_=0;
    uint64_t next_handle_=1;
    bool attached_=false,poisoned_=false,distrust_write_combined_=false,coherent_walk_=false;
    NativeAdrenoVmResult result_{};
    ArmSmmu4KLiveRegion region(unsigned) const;
    IOReturn fail(ArmSmmu4KStatus);
    Mapping *find(uint64_t);
    const Mapping *find(uint64_t) const;
    size_t rangeAt(uint64_t iova) const;
    void insert(size_t,unsigned);
    void erase(size_t,size_t,unsigned);
    IOReturn preflight(const NativeAdrenoMemory *,unsigned,uint64_t,unsigned,unsigned,unsigned,bool exclusive) const;
    bool exclusivePages(const NativeAdrenoMemory *,unsigned cls) const;
    bool forbidden(uint64_t iova,uint64_t bytes) const;
    bool completed(uint64_t fence) const;
    void linkMaintained(Mapping &);
    void unlinkMaintained(Mapping &);
    IOReturn attachCommon(const ArmSmmu4KIo *,ArmSmmu4KContextConfig *,const NativeAdrenoVmRegion *,const NativeAdrenoVmRegion *,
        size_t,unsigned);
    IOReturn verifyAndSeed();
    IOReturn mapLocked(NativeAdrenoMemory *,unsigned,uint64_t,unsigned,unsigned,unsigned,uint64_t *);
    void lock() const;
    void unlock() const;
public:
    NativeAdrenoVm()=default;
    ~NativeAdrenoVm()=default;
    NativeAdrenoVm(const NativeAdrenoVm &)=delete;
    NativeAdrenoVm &operator=(const NativeAdrenoVm &)=delete;
    /* The board's path: the binding of a programmed, routed NativeAdrenoSmmuContext. Proves both regions (the kit's table audit)
     * and the routes and context (the programmer's verify), then records each region as proved for the kit's later transactions. */
    IOReturn attach(const NativeAdrenoSmmuBinding &smmu,const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
        size_t max_spans,unsigned poll_budget);
    /* The kit's own route state (contexts the kit programmed through ArmSmmu4KAttachStreams, as the software peers of the older
     * tests): the routes and context are proved once with ArmSmmu4KObserveStreams, a failure FAULTs them with ArmSmmu4KFaultStreams. */
    IOReturn attach(const ArmSmmu4KIo *,ArmSmmu4KContextConfig *,ArmSmmu4KStreamState *,
        const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
        size_t max_spans,unsigned poll_budget);
    /* Frees CPU metadata only, with no BOs remaining. Never releases roots,
     * disables a bank or returns the platform lease; board lifecycle owns it. */
    IOReturn detach();
    /* A GPU address window no mapping may touch, before any map (the null guard 0..0x100000 and the 12 MiB GMEM window at
     * 0x4000000 of ARCHITECTURE 4.6.2). findAddress skips them. */
    IOReturn reserveWindow(uint64_t iova,uint64_t bytes);
    IOReturn findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *iova) const;
    /* A mapping with exactly these attributes (kernel buffers: privileged ring/memptrs, firmware, GMU memory). */
    IOReturn map(NativeAdrenoMemory *,unsigned upper,uint64_t iova,
        unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle);
    /* A client mapping (BoMap): the attributes of ARCHITECTURE 4.14.1 for this memory at the current coherence level replace the
     * requested ones (attributes()); a GPU-read-only import mapped non-cacheable joins the maintained list. keep_objects and keep_spans
     * (0 for the render server) are mapping records and table ranges this mapping must leave free, checked under the VM's lock: else
     * kIOReturnNoResources and native_adreno_vm_reserve_refused counts it. */
    IOReturn mapClient(NativeAdrenoMemory *,uint64_t iova,unsigned permissions,unsigned writeback,unsigned shareability,
        uint64_t *handle,size_t keep_objects=0,size_t keep_spans=0);
    /* Live mapping records and table ranges this VM can still take (no lock: a figure for the status rows and tests). */
    size_t freeObjects() const {return NativeAdrenoVmMaxObjects>objects_?NativeAdrenoVmMaxObjects-objects_:0;}
    size_t freeSpans() const {return capacity_>spanCount()?capacity_-spanCount():0;}
    /* ARCHITECTURE 4.14.1: own BOs (pool or scattered) non-cacheable, unless the level is 2 and the client asked write-back;
     * GPU-read-only imports write-back inner-shareable from level 1; GPU-written imports write-back from level 2; the scanout
     * import always non-cacheable. Non-cacheable mappings are outer shareable. */
    static void attributes(const NativeAdrenoMemory &memory,unsigned permissions,unsigned requested_writeback,
        unsigned requested_shareability,unsigned *writeback,unsigned *shareability);
    /* The effective coherence level (min of pagani-gpu-coherent or the S8 default and the proved ceiling), for new mappings. */
    static void setCoherence(unsigned level);
    static unsigned coherence();
    /* Refused (kIOReturnBusy) while a job holds the mapping or the fence of its last use has not completed. */
    IOReturn unmap(uint64_t handle);
    IOReturn holdJob(uint64_t handle);
    /* Only a scheduler's actual successful GPU completion may call this.
     * A timeout, interrupt bit alone or CPU-written timestamp is insufficient. */
    IOReturn retireJobAfterCompletion(uint64_t handle);
    /* The fence the CP writes when a submission completed (memptrs + 4, the BR fence; 32 bits wrap) for unmap and idle. Null:
     * a mapping that was ever used counts as busy. */
    void setFenceSource(const volatile uint32_t *completed);
    /* The submission with this fence reads or writes the mapping (Submit, per buffer, before it is queued). */
    IOReturn noteUse(uint64_t handle,uint64_t fence);
    /* The fence of the last use (0: never used) and whether it completed. */
    uint64_t lastUse(uint64_t handle) const;
    bool idle(uint64_t handle) const;
    /* Before a submission: cleans the CPU caches over the maintained list only (GPU-read-only imports mapped non-cacheable; empty
     * from coherence level 1). Own write-combined BOs and GPU-written imports take no maintenance per submission (4.14.1). */
    IOReturn cleanForSubmission();
    size_t maintainedCount() const;
    /* The previous names of the submission and completion maintenance: publishMapped() is cleanForSubmission(); acquireImported()
     * does nothing (GPU-written imports were cleaned and invalidated once when mapped, ARCHITECTURE 4.14.1). */
    IOReturn publishMapped();
    IOReturn acquireImported();
    /* M10 (ARCHITECTURE 4.6.1): once V4 proved the table walk coherent, a changed table page takes only dsb ishst (true); false
     * goes back to cleaning it to PoC. Applies to the following transactions. */
    IOReturn setTableWalkCoherent(bool coherent);
    bool tableWalkCoherent() const {return coherent_walk_;}
    /* TLBIALL for the context bank plus TLBSYNC: drops every cached translation of the bank whatever its ASID (recovery). */
    IOReturn invalidateTlb();
    /* The submit path saw a CPU-cached word that DRAM did not hold yet in a client's BO: from now on write-combined BOs are maintained
     * like any other (sticky; the safe behaviour of int-070..097). */
    void distrustWriteCombined() {distrust_write_combined_=true;}
    bool writeCombinedTrusted() const {return !distrust_write_combined_;}
    /* Diagnostic, never a proof of coherence: the 32-bit word at a mapped GPU address as the kernel's cacheable alias reads it
     * and, when the board installed native_adreno_dram_read32, as DRAM reads it through an uncached mapping (else ~0). */
    IOReturn sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const;
    /* Before a submission while write-combined BOs are trusted: every page of every mapped write-combined BO that XNU set back to the
     * default attribute is write-combined again (and its lines written back); *reset counts the pages. */
    IOReturn reassertWriteCombined(uint32_t *reset) const;
    /* Every line of [iova, iova+bytes) that lies in a write-combined BO, through native_adreno_line_audit: *dirty counts lines a CPU
     * cache held newer than DRAM, *stale clean copies, *lines the lines compared. kIOReturnUnsupported without a board hook. */
    IOReturn auditRange(uint64_t iova,size_t bytes,uint32_t *lines,uint32_t *dirty,uint32_t *stale) const;
    /* Read-only description of a live mapping; never pins or extends it. */
    IOReturn describe(uint64_t handle,uint64_t *iova,size_t *bytes,unsigned *permissions) const;
    /* Up to `count` dwords of a mapped kernel-owned BO at a GPU address (through the kernel's own mapping), for the board log. */
    IOReturn peekWords(uint64_t iova,uint32_t *out,size_t count) const;
    /* The same after cleaning and invalidating the CPU cache lines over the words. */
    IOReturn peekWordsUncached(uint64_t iova,uint32_t *out,size_t count) const;
    /* The context bank's fault record for the board log: FSR, FAR (lo, hi) and FSYNR0. Reads only; never clears a fault. */
    IOReturn faultState(uint32_t out[4]) const;
    /* Map handles count up and are never reused, so they order mappings in time: the next one, and the one over a GPU address (0: none). */
    uint64_t nextHandle() const {return next_handle_;}
    uint64_t handleAt(uint64_t iova) const;
    bool poisoned() const {return poisoned_;}
    bool attached() const {return attached_;}
    size_t objectCount() const {return objects_;}
    uint64_t mappedBytes() const {return __atomic_load_n(&bytes_,__ATOMIC_RELAXED);}   /* read without the VM lock (status row) */
    size_t spanCount() const {return lower_count_+upper_count_;}
    NativeAdrenoVmResult lastResult() const {return result_;}
    unsigned bank() const {return bank_;}
};
#endif
