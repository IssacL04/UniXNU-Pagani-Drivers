/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_MEMORY_H
#define NATIVE_ADRENO_MEMORY_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
class IOBufferMemoryDescriptor;
class IOMemoryDescriptor;
struct NativeAdrenoPhysicalSpan {uint64_t offset,physical,bytes;};
struct NativeAdrenoCpuState {
    uint64_t tcr,ttbr0,ttbr1,sctlr;
    uint64_t kernel_page_bytes,kernel_map_page_bytes;
};
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *);
enum NativeAdrenoCacheOperation {NativeAdrenoClean,NativeAdrenoCleanInvalidate,NativeAdrenoInvalidate};
IOReturn NativeAdrenoMaintainOwnedPages(void *,size_t,NativeAdrenoCacheOperation);
/* dsb ishst: orders stores to cacheable page-table memory before a coherent table walker reads them (ARCHITECTURE 4.6.1, M10:
 * once V4 proved the walk coherent, a changed table page takes this instead of a clean to PoC). */
static inline void NativeAdrenoStoreBarrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb ishst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}
/* The same over whole physical pages of DRAM, through the kernel's physical aperture: data cache lines are physically tagged, so this
 * reaches lines another task's mapping left (an imported IOSurface). Apple's sys_dcache_flush is only a DSB, so user space cannot. */
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation);
/* NativeAdrenoMaintainOwnedPages' loop (pagani-gpu-kopt bit 3; upstream Adreno740 2c3df1e): whole pages in runs of at most run_bytes (a
 * multiple of 4096), each with the CPU pinned, its own line size and a barrier on either side, the closing one on the core that
 * issued the run completing it. Between two runs the thread may be preempted or migrate: the next run is whole on its own core.
 * Ops: pin(), unpin(), lineBytes(), barrier(), maintain(address). A line size above a page is refused with the CPU unpinned. */
enum : size_t {NativeAdrenoMaintainRunBytes=size_t(64)*4096};
template<class Ops>
static inline IOReturn NativeAdrenoMaintainRuns(uintptr_t address,size_t bytes,size_t run_bytes,Ops &ops)
{
    const uintptr_t end=address+bytes;
    while(address<end) {
        ops.pin();
        const size_t line=ops.lineBytes();
        if(line>4096) {ops.unpin();return kIOReturnUnsupported;}
        const uintptr_t stop=size_t(end-address)>run_bytes?address+run_bytes:end;
        ops.barrier();
        for(;address<stop;address+=line)ops.maintain(address);
        ops.barrier();
        ops.unpin();
    }
    return kIOReturnSuccess;
}
/* Every CPU mapping of the spans' pages becomes write-combined (true) or takes the default cacheable attribute again (false). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *,size_t count,bool write_combined);
/* int-100 found a write-combined BO's page back at the default (cacheable) attribute with dirty IB lines: XNU's view machinery sets a page's
 * attribute when it populates or tears down a device-pager view. Every page of the spans no longer write-combined is made so again (the pmap
 * then rewrites every mapping it tracks and writes the page's cache lines back); *reset counts them. */
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *,size_t count,uint32_t *reset);

/* Native XNU backing store, not a Metal device or user ABI. No IOMMU mapper,
 * Linux fd/ioctl, hardware address, guessed PA or implicit destructor cleanup.
 * Driver owns this object until release() succeeds; do not destroy/copy it
 * with outstanding memory. Serialize every call under the queue/resource gate.
 * Every published mapping/job must hold a device reference BEFORE publication,
 * and retire it only after actual DMA quiescence and completed native TLB unmap.
 * References are lifetime accounting, never fences or proof of quiescence. */
class NativeAdrenoMemory final {
    IOBufferMemoryDescriptor *descriptor_=nullptr;
    IOMemoryDescriptor *imported_=nullptr;
    bool read_only_=false,device_=false,live_import_=false;
    void *mapping_=nullptr;
    NativeAdrenoPhysicalSpan *spans_=nullptr;
    size_t bytes_=0,span_count_=0,span_capacity_=0;
    uint32_t device_references_=0;
    bool prepared_=false,quarantined_=false,write_combined_=false,pooled_=false;
    bool unzeroed_=false;   /* from the pool, still holding what its last owner left: no device reference until finishPooled succeeded */
    size_t pool_offset_=0;
    IOReturn maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation);
    IOReturn takePooled(size_t bytes,uint64_t limit);
public:
    NativeAdrenoMemory()=default;
    ~NativeAdrenoMemory()=default;
    NativeAdrenoMemory(const NativeAdrenoMemory &)=delete;
    NativeAdrenoMemory &operator=(const NativeAdrenoMemory &)=delete;
    /* Explicit driver budget; bounded to 256MiB per backing object for now.
     * Error may retain quarantined allocation state if complete() fails.
     * Caller always keeps this object and checks quarantined(), even on error.
     * CPU native4K state and kernel page ABI are checked before allocation.
     * prepare(), raw unmapped physical segments, span/PA bounds and optional
     * physical contiguity are verified before zeroing/PoC publication. */
    /* `pooled`: take the bytes from the BO pool when one is reserved and has room. Only user BOs do: the pool is write-combined for
     * the CPU (writeCombinePool), and the kernel's own objects (SMMU tables, ring, fence and firmware pages) are walked or read by the
     * CPU through their cacheable mapping (int-153: page tables from the write-combined pool made each map's table update 2.6x slower). */
    IOReturn allocate(size_t bytes,unsigned output_bits,bool contiguous,size_t byte_budget,bool pooled=false);
    /* allocate()'s pool step alone: the bytes from the BO pool, or kIOReturnNoResources without a pool or room in it (nothing else
     * is tried). The pool's free list and figures are shared: the caller holds the Memory lock (NativeAdrenoSubmitLockMemory). A BO
     * outside the pool (allocate with pooled false: the contiguous attempt, the scattered pages, makeWriteCombined, release) touches
     * nothing shared but atomic figures, so the user-client cores allocate and release it without that lock (int-152: 3 ms a BO,
     * 90-130 ms a surface-sized one, of page attribute changes). The BO is not zeroed yet: finishPooled does that. */
    IOReturn allocatePooled(size_t bytes,unsigned output_bits);
    /* allocatePooled's zero fill (allocate(..., pooled=true) calls it itself): the pages are this object's alone, so it needs no lock
     * and the caller drops the Memory lock first (pagani-gpu-kopt bit 0): every process's BoAlloc, BoImport and BoFree waited for the
     * write-combined zeros of the largest BO anyone asked for. Until it succeeded holdForDevice refuses, so a BO whose fill was forgotten
     * or failed cannot reach the GPU. That gate is the device's alone: a client's CPU view does not look at it, a client is kept out
     * by the user client publishing the BO only after this succeeded. On a failure the caller releases the object (under the Memory
     * lock, as ever for a pooled BO). Nothing to do for any other object, or twice. */
    IOReturn finishPooled();
    /* One physically contiguous block taken from the VM early (while memory is not yet fragmented) and cut into BOs afterwards: a BO from it is
     * one span of the GPU's table instead of one per scattered page (int-137: 59474 spans for 332 MiB, the 65536 span capacity ran out,
     * BoMap failed, the translator answered out of device memory and the compositor fell back to the CPU; each map also walked every span, 167 ms).
     * allocate(..., pooled=true) uses the pool when one is reserved and has room, else the scattered path. Only the board calls this.
     * Best fit over free extents kept sorted and merged; the extent table never overflows (a BO comes from the pool only while fewer
     * than 16383 pooled BOs are alive, and those leave at most one gap more than their number), so every release returns its pages:
     * native_adreno_pool_shape counts the extents, the pooled BOs alive and the allocations the bound sent elsewhere. */
    static IOReturn reservePool(unsigned output_bits,size_t bytes);
    /* ARCHITECTURE 3.4 S3: the pool of pagani-gpu-pool (512 MiB) while memory is still unfragmented, halved until a reservation
     * succeeds, never below min_mib; *reserved_mib is what was reserved (0: none, not an error). With write_combined the pool's
     * pages become write-combined once (writeCombinePool). A pool already reserved is kept and reported. */
    static IOReturn reservePoolBestEffort(unsigned output_bits,uint32_t mib,uint32_t min_mib,bool write_combined,
        uint32_t *reserved_mib);
    static size_t poolBytes();
    /* Pool bytes not handed out (0 without a pool). Read under the Memory lock by an allocation that must leave part of the pool to the
     * render server (NativeAdrenoUserClient::setReserve). */
    static size_t poolFreeBytes();
    /* The pool's first physical byte (0 without a pool): its pages belong to pooled BOs alone. */
    static uint64_t poolPhysical();
    /* The status page's MEM row (ARCHITECTURE 8.4): pool MiB, pool MiB in use, live imports (task and device). */
    static void usage(uint32_t *pool_mib,uint32_t *used_mib,uint32_t *imports);
    /* The whole pool write-combined once, for good: a pooled BO then takes no page-attribute change on allocation or release.
     * Board only, after reservePool, when user BOs are write-combined. */
    static IOReturn writeCombinePool();
    /* pagani-gpu-kopt (NativeAdrenoKopt.h): S0 stores the accepted bits, before anything reads them. */
    static void setKopt(uint32_t bits);
    /* Pins bytes at address in task (a user task's own pages) instead of allocating; prepare() wires them until release().
     * There is no kernel mapping: the owner keeps its CPU mapping and does its own cache maintenance around GPU use, so
     * publishForDevice/acquireDeviceWrites have nothing to do for this object. Spans and PA bounds are checked as for allocate. */
    IOReturn importTask(void *task,uint64_t address,size_t bytes,unsigned output_bits,size_t byte_budget);
    /* M9 (ARCHITECTURE 4.14.1, IMPLEMENTATION_PLAN M9): the scanout buffer of the display hand-off as a BO, from the physical
     * segments of the presenter's IODeviceMemory at [offset, offset + bytes) (page aligned). Device memory is not wired and has no
     * kernel mapping here; the descriptor is not retained (the presenter's scanout memory lives for the boot) and must outlive the
     * BO. The GPU maps it non-cacheable for writing (NativeAdrenoVm::attributes); no CPU cache maintenance applies to it. */
    IOReturn importDevice(IOMemoryDescriptor *scanout,uint64_t offset,size_t bytes);
    bool imported() const {return imported_!=nullptr;}
    bool scanout() const {return device_;}
    bool pooled() const {return pooled_;}
    /* An import of memory the owner maps read-only (the kernel could pin it for reading only): the GPU may read it, never write it. */
    bool readOnly() const {return read_only_;}
    /* After allocate(), before any device or client mapping: cleans and invalidates the pages and makes every CPU mapping of them
     * write-combined, so publishForDevice/acquireDeviceWrites have nothing left to do. release() restores the default attribute. */
    IOReturn makeWriteCombined();
    bool writeCombined() const {return write_combined_;}
    /* NativeAdrenoReassertWriteCombined over this object's pages; nothing for an object that is not write-combined, nor for one from the
     * BO pool: XNU set scattered BOs' pages back to the default attribute (int-160: 123847 pages), never a pool page (int-161: none in
     * 1280 submissions, while walking the pool's pages took 0.65 ms of each). The submissions' line audit still covers every object. */
    IOReturn reassertWriteCombined(uint32_t *reset) const
    {return write_combined_ && span_count_ && !pooled_?NativeAdrenoReassertWriteCombined(spans_,span_count_,reset):kIOReturnSuccess;}
    bool backed() const {return descriptor_ || imported_ || pooled_ || device_;}
    /* Refuses device references/quarantine; only clears ownership after real
     * descriptor complete() succeeds. No automatic reset or resource recycling. */
    IOReturn release();
    IOReturn holdForDevice();
    IOReturn retireAfterQuiescence();
    void quarantine() {quarantined_=true;}
    /* Requires real CPU/device exclusion and retained ownership. Before GPU
     * writes, clean+invalidate; after genuine completion, invalidate only.
     * CPU must not dirty these pages while the device owns access. */
    /* A write-combined object is skipped unless include_write_combined: then its pages are maintained through the physical aperture,
     * for a client whose view turned out to be cacheable after all (NativeAdrenoVm::distrustWriteCombined). */
    IOReturn publishForDevice(size_t offset,size_t bytes,bool device_writes,bool include_write_combined=false);
    IOReturn acquireDeviceWrites(size_t offset,size_t bytes);
    bool quarantined() const {return quarantined_;}
    uint32_t deviceReferences() const {return device_references_;}
    size_t bytes() const {return bytes_;}
    size_t spanCount() const {return span_count_;}
    const NativeAdrenoPhysicalSpan *spans() const {return spans_;}
    void *mapping() const {return mapping_;}
    /* Borrowed descriptor, never release independently or expose table pools
     * through clientMemoryForType. Sharing requires a separate owner/task ABI. */
    IOBufferMemoryDescriptor *descriptor() const {return descriptor_;}
};
#endif
