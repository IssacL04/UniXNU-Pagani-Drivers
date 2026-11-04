/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GMU_MEMORY_H
#define NATIVE_ADRENO_GMU_MEMORY_H
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoVm.h"

/* Backing store of the A830 GMU's own address space (context bank 2 through SID 5, ARCHITECTURE 4.7.2), driven by the
 * PREALLOC metadata of gen80000_gmu.bin (4.7.1) the way KGSL gen8_gmu_process_prealloc reserves it, instead of the
 * a6xx-era 16 MiB physically contiguous instruction cache:
 *  - every PREALLOC request inside the CACHE range [0x4000, 0x1000000) becomes one region rounded out to whole pages
 *    (PKX110: 0x4000-0x1A000, 0x100000-0x11C000, 0x200000-0x21F000, 0x300000-0x325000, 0x76000 bytes in all); a
 *    request inside an earlier region needs nothing (KGSL find_gmu_memdesc), a partial overlap is refused;
 *  - requests inside the TCMs need nothing: the TCMs are written through registers (A830GmuFirmware::load);
 *  - PREALLOC_PERSIST requests must lie inside the persistent region at 0x60000000 (16 KiB), always allocated;
 *  - the log (0x60004000, 16 KiB, GENERAL_8 = 0x60004003) and the HFI shared memory (0x60008000, 16 KiB) are added.
 * Every region is its own object, physically contiguous when the VM can give it (scattered pages otherwise): nothing has
 * to be contiguous across regions. Regions are zeroed once, at allocation, and their CPU view is then made write-combined
 * (the GMU maps them Normal non-cacheable, MAIR index 0), so firmware copies and HFI queue updates reach DRAM with a
 * barrier and no cache maintenance. A native4K table pool that translates the space can be kept here as before (it has no
 * IOVA); a plan with table_bytes 0 has none, for a GMU SMMU context that owns its own pool (NativeAdrenoGmuContext, which
 * then maps these regions through its map hook with mapInto). allocate() only assigns addresses; mapInto() maps the regions
 * privileged into the GMU VM: CACHE read, write and
 * execute (the GMU fetches its code from 0x4000 through SID 5), the others read and write, execute-never. Any failure
 * releases what was allocated and leaves no layout. Callers serialize every call (the start sequence owns the object). */
enum : uint64_t {GmuApertureBytes=0x80000000};   /* lower (TTBR0) region of the GMU context: the whole 2 GiB GMU space */
enum : uint32_t {
    NativeAdrenoGmuItcmIova=0x00000000,NativeAdrenoGmuItcmBytes=0x4000,
    NativeAdrenoGmuCacheIova=0x00004000,NativeAdrenoGmuCacheEnd=0x01000000,
    NativeAdrenoGmuDtcmIova=0x10004000,NativeAdrenoGmuDtcmBytes=0x4000,
    NativeAdrenoGmuUncachedIova=0x60000000,NativeAdrenoGmuUncachedEnd=0x80000000,
    NativeAdrenoGmuPersistIova=0x60000000,NativeAdrenoGmuPersistBytes=0x4000,
    NativeAdrenoGmuLogIova=0x60004000,NativeAdrenoGmuLogBytes=0x4000,
    NativeAdrenoGmuHfiIova=0x60008000,NativeAdrenoGmuHfiBytes=0x4000,
    NativeAdrenoGmuMaxRegions=16,                 /* CACHE regions + persist + log + HFI */
    NativeAdrenoGmuMaxCacheRegions=NativeAdrenoGmuMaxRegions-3,
    NativeAdrenoGmuPageBytes=0x1000,
};
/* Block types of the metadata (size 0) blocks that ask for backing memory (KGSL GMU_BLK_TYPE_PREALLOC_*). */
enum NativeAdrenoGmuRequestType : uint32_t {NativeAdrenoGmuRequestPrealloc=1,NativeAdrenoGmuRequestPersist=7};
/* One PREALLOC metadata block: the GMU wants `bytes` of backing store at GMU address `iova`. */
struct NativeAdrenoGmuRequest {uint32_t iova,bytes,type;};
enum NativeAdrenoGmuRegionKind : uint32_t {
    NativeAdrenoGmuRegionCache,NativeAdrenoGmuRegionPersist,NativeAdrenoGmuRegionLog,NativeAdrenoGmuRegionHfi,
    NativeAdrenoGmuRegionKinds
};
/* ArmSmmu4K permission bits of each kind: CACHE R|W|X|Priv (15), the others R|W|Priv (11). */
struct NativeAdrenoGmuRegion {uint32_t iova,bytes,kind,permissions;};
struct NativeAdrenoGmuPlan {
    NativeAdrenoGmuRegion region[NativeAdrenoGmuMaxRegions];
    uint32_t count;              /* CACHE regions in address order, then persist, log, HFI */
    uint32_t cache_bytes;        /* sum of the CACHE regions (PKX110: 0x76000) */
    uint32_t tcm_requests;       /* requests inside ITCM/DTCM: register-loaded, nothing to allocate */
    uint32_t persist_requests;   /* PREALLOC_PERSIST requests, all inside the persistent region */
    uint32_t covered;            /* requests inside an earlier region */
    size_t table_bytes;          /* native4K table pool, a 4 KiB multiple; 0 = no pool here */
};
/* Builds the plan from the PREALLOC requests of one firmware image. Refused (kIOReturnBadArgument): a zero-byte request, a
 * request that leaves its range or wraps, a CACHE request that overlaps an earlier region without being inside it, an
 * uncached request outside the persistent region, a request in no GMU range, more than NativeAdrenoGmuMaxCacheRegions
 * CACHE regions, or a table pool that is not a 4 KiB multiple. */
IOReturn NativeAdrenoGmuPlanRequests(const NativeAdrenoGmuRequest *requests,size_t count,size_t table_bytes,
    NativeAdrenoGmuPlan *plan);
struct NativeAdrenoGmuMemoryLayout {
    uint64_t persist_iova,log_iova,hfi_iova;
    uint64_t tables_iova;         /* 0: the table pool has no IOVA */
    size_t tables_bytes;          /* 0: no pool here */
    uint64_t end_iova;            /* first address after the highest region */
    uint32_t cache_bytes,regions;
};
class NativeAdrenoGmuMemory final {
    NativeAdrenoMemory region_[NativeAdrenoGmuMaxRegions],tables_;
    NativeAdrenoGmuPlan plan_{};
    NativeAdrenoGmuMemoryLayout layout_{};
    uint64_t handles_[NativeAdrenoGmuMaxRegions]{};
    uint32_t scattered_=0;
    bool ready_=false;
    IOReturn releaseAll();
    int indexOf(uint32_t kind) const;
public:
    NativeAdrenoGmuMemory()=default;
    ~NativeAdrenoGmuMemory()=default;
    NativeAdrenoGmuMemory(const NativeAdrenoGmuMemory &)=delete;
    NativeAdrenoGmuMemory &operator=(const NativeAdrenoGmuMemory &)=delete;
    /* Allocates every region of the plan below 2^address_bits (the SMMU output size, ID2.OAS) and the table pool when the plan
     * has one (physically contiguous). A region is tried physically contiguous first and then as scattered pages; scatteredRegions() counts the
     * second kind (each scattered page is one more VM span). kIOReturnBusy when already allocated. */
    IOReturn allocate(const NativeAdrenoGmuPlan &plan,unsigned address_bits=40);
    /* Maps every region into the VM's lower region at its GMU address (the VM must already be attached with a lower arena
     * covering the regions), GMU-side non-cacheable (writeback 0, MAIR index 0), outer shareable, privileged. Handles are
     * retained for unmapFrom. Defined here, so code that never maps (the firmware loader, the boot sequence) does not link
     * the VM. */
    IOReturn mapInto(NativeAdrenoVm *vm)
    {
        if(!ready_ || !vm)return kIOReturnBadArgument;
        for(uint64_t mapped:handles_)if(mapped)return kIOReturnBusy;
        /* The table pool is never mapped: its pages are the translation storage and the VM refuses such an overlap. */
        for(uint32_t i=0;i<plan_.count;++i) {
            const NativeAdrenoGmuRegion &r=plan_.region[i];
            const IOReturn status=vm->map(&region_[i],0,r.iova,r.permissions,0,2,&handles_[i]);
            if(status!=kIOReturnSuccess) {
                for(uint32_t previous=i;previous--;)
                    if(handles_[previous] && vm->unmap(handles_[previous])==kIOReturnSuccess)handles_[previous]=0;
                handles_[i]=0;
                return status;
            }
        }
        return kIOReturnSuccess;
    }
    /* Reverse order unmap; refuses while a mapping has outstanding jobs. */
    IOReturn unmapFrom(NativeAdrenoVm *vm)
    {
        if(!vm)return kIOReturnBadArgument;
        IOReturn status=kIOReturnSuccess;
        for(size_t index=NativeAdrenoGmuMaxRegions;index--;)if(handles_[index]) {
            const IOReturn unmapped=vm->unmap(handles_[index]);
            if(unmapped)status=unmapped;else handles_[index]=0;
        }
        return status;
    }
    /* Only when every object has been released; on any refusal nothing is destroyed and the caller keeps the quarantined
     * objects alive. */
    IOReturn release();
    bool ready() const {return ready_;}
    const NativeAdrenoGmuMemoryLayout &layout() const {return layout_;}
    const NativeAdrenoGmuPlan &plan() const {return plan_;}
    size_t regionCount() const {return ready_?plan_.count:0;}
    uint32_t scatteredRegions() const {return scattered_;}
    /* Region `index` (plan order) and its backing object; null past the end or before allocation. */
    const NativeAdrenoGmuRegion *region(size_t index) const {return index<regionCount()?&plan_.region[index]:nullptr;}
    NativeAdrenoMemory *memory(size_t index) {return index<regionCount()?&region_[index]:nullptr;}
    const NativeAdrenoMemory *memory(size_t index) const {return index<regionCount()?&region_[index]:nullptr;}
    /* VM handle of region `index` while mapped by mapInto, else 0. */
    uint64_t handle(size_t index) const {return index<NativeAdrenoGmuMaxRegions?handles_[index]:0;}
    /* Index of the region holding all of [iova, iova + bytes) (bytes > 0), or -1. */
    int find(uint64_t iova,uint64_t bytes) const;
    /* Write-combined CPU address of GMU address `iova` when [iova, iova + bytes) lies inside one region, else null. */
    void *cpu(uint64_t iova,uint64_t bytes) const;
    /* The fixed regions; null before allocation. */
    NativeAdrenoMemory *persist() {const int i=indexOf(NativeAdrenoGmuRegionPersist);return i<0?nullptr:&region_[i];}
    NativeAdrenoMemory *log() {const int i=indexOf(NativeAdrenoGmuRegionLog);return i<0?nullptr:&region_[i];}
    NativeAdrenoMemory *hfi() {const int i=indexOf(NativeAdrenoGmuRegionHfi);return i<0?nullptr:&region_[i];}
    NativeAdrenoMemory &tables() {return tables_;}
    const NativeAdrenoMemory &tables() const {return tables_;}
    /* Zeroes every region whose kind bit (1u << NativeAdrenoGmuRegionKind) is set, then completes the stores (dsb sy).
     * Allocation already zeroes everything once; the persistent region is never cleared again by the driver. */
    IOReturn clear(uint32_t kinds);
};
/* Former name of the class, kept for the GMU SMMU context (NativeAdrenoGmuContext). */
using NativeAdrenoGmuAllocator=NativeAdrenoGmuMemory;
#endif
