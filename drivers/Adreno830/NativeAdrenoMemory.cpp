/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoMeter.h"
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
/* pagani-gpu-kopt and the call meter's figures (NativeAdrenoKopt.h, NativeAdrenoMeter.h): here because every file that reads or writes
 * them links this one. */
extern "C" {
volatile uint32_t native_adreno_kopt=NativeAdrenoKoptAll;
volatile uint32_t native_adreno_meter_calls[Adreno830CallKinds],native_adreno_meter_waits[Adreno830WaitKinds];
volatile uint32_t native_adreno_meter_zero[2],native_adreno_meter_contig[3],native_adreno_meter_fence;
}
void NativeAdrenoMemory::setKopt(uint32_t bits) {native_adreno_kopt=bits;}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <string.h>

static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_pool_mib=0,native_adreno_pool_status=0;volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
/* The pool (see reservePool): a free list of page extents over one contiguous block; best fit, neighbours merged on release. */
extern "C" {volatile uint32_t native_adreno_pool_stats[8];}
/* The pool's write-combine (writeCombinePool): bit 31 once tried, the low bits its status. */
extern "C" {volatile uint32_t native_adreno_pool_wc;}   /* [0] pool MiB, [1] BOs from the pool, [2] fell back, [3] MiB in use, [4] peak MiB, [5] scattered BOs, [6] their spans, [7] import spans */
/* The pool's shape: [0] free extents now, [1] their peak, [2] pooled BOs alive, [3] allocations sent past the pool because
 * PoolExtentCapacity - 1 pooled BOs were alive, [4] releases whose extent found no room (never: see Pool), [5] the MiB they lost. */
extern "C" {volatile uint32_t native_adreno_pool_shape[6];}
namespace {
/* The free extents are kept sorted by address and fully merged, so every one of them is a gap between two pooled BOs (or an end of
 * the pool): with L pooled BOs alive there are at most L + 1. The table holds PoolExtentCapacity of them and a BO is taken from the
 * pool only while fewer than PoolExtentCapacity - 1 pooled BOs are alive, so a release always finds its slot or merges. The table of
 * 512 before this held too few: about 1000 live BOs of mixed sizes fragment the pool into some 500 gaps, and from then on every
 * release that needed a slot, a merge with a neighbour included, was dropped, so the pool drained for the rest of the boot while
 * its "used" figure went down, and every later BO fell back to contiguous or scattered pages (a contiguous search of the VM, page
 * attributes changed one page at a time under the Memory lock, wired memory of the system, one GPU span per page). Extents are in
 * pages: a pool is at most 2 GiB (pagani-gpu-pool <= 2048). */
enum : unsigned {PoolExtentCapacity=16384};
struct PoolExtent {uint32_t page,pages;};
struct Pool {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    unsigned char *virt=nullptr;uint64_t physical=0;size_t bytes=0,used=0,peak=0;
    PoolExtent free[PoolExtentCapacity];unsigned count=0,live=0;
    bool write_combined=false;
} pool;
void poolShape()
{
    native_adreno_pool_shape[0]=pool.count;
    if(pool.count>native_adreno_pool_shape[1])native_adreno_pool_shape[1]=pool.count;
    native_adreno_pool_shape[2]=pool.live;
}
void poolRemove(unsigned at)
{
    memmove(&pool.free[at],&pool.free[at+1],(pool.count-at-1)*sizeof(PoolExtent));
    --pool.count;
}
bool poolTake(size_t bytes,size_t *offset) {
    if(pool.live>=PoolExtentCapacity-1) {native_adreno_pool_shape[3]=native_adreno_pool_shape[3]+1;return false;}
    const uint32_t pages=uint32_t(bytes>>12);
    int best=-1;
    for(unsigned i=0;i<pool.count;++i) {
        if(pool.free[i].pages<pages || (best>=0 && pool.free[i].pages>=pool.free[best].pages))continue;
        best=(int)i;
        if(pool.free[i].pages==pages)break;   /* an exact fit is the best fit */
    }
    if(best<0)return false;
    *offset=size_t(pool.free[best].page)<<12;
    pool.free[best].page+=pages;pool.free[best].pages-=pages;
    if(!pool.free[best].pages)poolRemove(unsigned(best));
    ++pool.live;
    pool.used+=bytes;if(pool.used>pool.peak)pool.peak=pool.used;
    native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);native_adreno_pool_stats[4]=(uint32_t)(pool.peak>>20);
    poolShape();
    return true;
}
void poolGive(size_t offset,size_t bytes) {
    const uint32_t page=uint32_t(offset>>12),pages=uint32_t(bytes>>12);
    /* The first extent at or after page (the extents are sorted by address and never overlap a live BO). */
    unsigned low=0,high=pool.count;
    while(low<high) {const unsigned mid=low+(high-low)/2;if(pool.free[mid].page<page)low=mid+1;else high=mid;}
    const unsigned at=low;
    const bool left=at && pool.free[at-1].page+pool.free[at-1].pages==page;
    const bool right=at<pool.count && page+pages==pool.free[at].page;
    if(left && right) {pool.free[at-1].pages+=pages+pool.free[at].pages;poolRemove(at);}
    else if(left)pool.free[at-1].pages+=pages;
    else if(right) {pool.free[at].page=page;pool.free[at].pages+=pages;}
    else if(pool.count<PoolExtentCapacity) {
        memmove(&pool.free[at+1],&pool.free[at],(pool.count-at)*sizeof(PoolExtent));
        pool.free[at]={page,pages};++pool.count;
    } else {
        /* Cannot happen (at most live + 1 gaps, and live stays below the capacity); counted, the extent stays out of use. */
        native_adreno_pool_shape[4]=native_adreno_pool_shape[4]+1;
        native_adreno_pool_shape[5]=native_adreno_pool_shape[5]+uint32_t((bytes+(1u<<20)-1)>>20);
    }
    if(pool.live)--pool.live;
    pool.used-=bytes;native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);
    poolShape();
}
}
size_t NativeAdrenoMemory::poolBytes() {return pool.bytes;}
size_t NativeAdrenoMemory::poolFreeBytes() {return pool.descriptor && pool.bytes>pool.used?pool.bytes-pool.used:0;}
uint64_t NativeAdrenoMemory::poolPhysical() {return pool.descriptor?pool.physical:0;}
/* Imports alive (importTask and importDevice objects not yet released), for the MEM row. */
static uint32_t liveImports;
void NativeAdrenoMemory::usage(uint32_t *pool_mib,uint32_t *used_mib,uint32_t *imports)
{
    if(pool_mib)*pool_mib=pool.descriptor?uint32_t(pool.bytes>>20):0;
    if(used_mib)*used_mib=pool.descriptor?uint32_t(pool.used>>20):0;
    if(imports)*imports=__atomic_load_n(&liveImports,__ATOMIC_RELAXED);
}
/* int-152: a user BO's pages changed attribute one pmap_set_cache_attributes call at a time (each walks the page's mappings, flushes the
 * TLB and cleans the page), on allocation and again on release, under the connection lock: 3 ms a BO on average and 90-130 ms for a
 * surface-sized one, while every other GPU call of every client waited. The pool is the BOs' own block and nothing else maps it, so
 * its pages take the attribute once here and keep it while the pool lives. */
IOReturn NativeAdrenoMemory::writeCombinePool()
{
    if(!pool.descriptor || !pool.virt || !pool.bytes)return kIOReturnNotReady;
    if(pool.write_combined)return kIOReturnSuccess;
    IOReturn status=NativeAdrenoMaintainOwnedPages(pool.virt,pool.bytes,NativeAdrenoCleanInvalidate);
    if(status)return status;
    const NativeAdrenoPhysicalSpan span={0,pool.physical,pool.bytes};
    status=NativeAdrenoSetPagesCacheMode(&span,1,true);
    if(status)return status;
    pool.write_combined=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::reservePool(unsigned bits,size_t bytes)
{
    if(pool.descriptor)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || (bytes>>12)>UINT32_MAX || !outputWidth(bits))return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous,
        bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor)return kIOReturnNoMemory;
    status=descriptor->prepare();
    if(status!=kIOReturnSuccess) {descriptor->release();return status;}
    IOByteCount length=0,tail=0;
    const addr64_t first=descriptor->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    const addr64_t last=descriptor->getPhysicalSegment(bytes-4096,&tail,kIOMemoryMapperNone);
    unsigned char *virt=static_cast<unsigned char *>(descriptor->getBytesNoCopy());
    if(!first || (first&4095) || !virt || ((uintptr_t)virt&4095) || last!=first+bytes-4096 || first>=limit || bytes>limit-first ||
        descriptor->getLength()!=bytes) {
        (void)descriptor->complete();descriptor->release();return kIOReturnUnsupported;
    }
    pool.descriptor=descriptor;pool.virt=virt;pool.physical=first;pool.bytes=bytes;pool.used=pool.peak=0;
    pool.free[0]={0,uint32_t(bytes>>12)};pool.count=1;pool.live=0;
    poolShape();
    native_adreno_pool_stats[0]=(uint32_t)(bytes>>20);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::reservePoolBestEffort(unsigned bits,uint32_t mib,uint32_t min_mib,bool write_combined,
    uint32_t *reserved_mib)
{
    if(!reserved_mib)return kIOReturnBadArgument;
    *reserved_mib=0;
    if(!outputWidth(bits))return kIOReturnBadArgument;
    if(!pool.descriptor) {
        if(!min_mib)min_mib=16;
        /* Halved until a reservation succeeds (a fragmented boot may not have 512 MiB contiguous); none is not an error: every
         * BO is then scattered and changes its own pages' attribute. */
        for(uint32_t size=mib;size && size>=min_mib;size>>=1) {
            const IOReturn status=reservePool(bits,(size_t)size<<20);
            native_adreno_pool_status=(uint32_t)status;
            if(status==kIOReturnSuccess)break;
        }
    }
    if(!pool.descriptor)return kIOReturnSuccess;
    *reserved_mib=uint32_t(pool.bytes>>20);
    native_adreno_pool_stats[0]=*reserved_mib;
    if(write_combined && !pool.write_combined)native_adreno_pool_wc=(uint32_t)writeCombinePool()|1u<<31;
    return kIOReturnSuccess;
}
/* A BO from the pool: kIOReturnNoResources when the pool cannot take it (none, too small, no room: counted as fell back when the pool
 * could have held it), else what the pooled allocation returned. The pool's free list and figures: under the Memory lock. */
IOReturn NativeAdrenoMemory::takePooled(size_t bytes,uint64_t limit)
{
    if(!pool.descriptor || bytes>pool.bytes)return kIOReturnNoResources;
    size_t offset=0;
    if(poolTake(bytes,&offset)) {
        const uint64_t physical=pool.physical+offset;
        if(physical>=limit || bytes>limit-physical) {poolGive(offset,bytes);}
        else {
            spans_=IONewData(NativeAdrenoPhysicalSpan,1);
            if(!spans_) {poolGive(offset,bytes);return kIOReturnNoMemory;}
            span_capacity_=1;span_count_=1;spans_[0]={0,physical,bytes};
            mapping_=pool.virt+offset;bytes_=bytes;pooled_=true;pool_offset_=offset;prepared_=true;unzeroed_=true;
            native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;
            return kIOReturnSuccess;
        }
    }
    native_adreno_pool_stats[2]=native_adreno_pool_stats[2]+1;
    return kIOReturnNoResources;
}
IOReturn NativeAdrenoMemory::finishPooled()
{
    if(!unzeroed_)return kIOReturnSuccess;
    bzero(mapping_,bytes_);
    /* Through a write-combined pool's own mapping the zeros go to DRAM; the barrier completes them. */
    if(pool.write_combined)__asm__ volatile("dsb sy":::"memory");
    else {
        const IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoClean);
        if(status)return status;
    }
    unzeroed_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::allocatePooled(size_t bytes,unsigned bits)
{
    if(descriptor_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || bytes>256u*1024u*1024u)return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    const IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    return takePooled(bytes,UINT64_C(1)<<bits);
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget,bool pooled)
{
    if(descriptor_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget || budget>256u*1024u*1024u)
        return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    uint64_t limit=UINT64_C(1)<<bits;
    if(pooled) {
        status=takePooled(bytes,limit);
        if(status==kIOReturnSuccess) {
            status=finishPooled();
            if(status)(void)release();   /* its pages go back to the pool; the failure is the answer, as for every failed allocation */
            return status;
        }
        if(status!=kIOReturnNoResources)return status;
    }
    /* From here on nothing the pool or another BO shares: the caller needs no Memory lock (allocatePooled); the figures are atomic. */
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(contiguous)options|=kIOMemoryPhysicallyContiguous;
    descriptor_=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor_)return kIOReturnNoMemory;
    status=descriptor_->prepare();
    if(status!=kIOReturnSuccess) {descriptor_->release();descriptor_=nullptr;return status;}
    prepared_=true;bytes_=bytes;mapping_=descriptor_->getBytesNoCopy();
    status=kIOReturnUnsupported;
    if(!mapping_ || ((uintptr_t)mapping_&4095) || bytes>UINTPTR_MAX-(uintptr_t)mapping_ ||
        descriptor_->getLength()!=bytes)goto failed;
    span_capacity_=bytes/4096;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {status=kIOReturnNoMemory;goto failed;}
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        addr64_t physical=descriptor_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)
            {status=kIOReturnUnsupported;goto failed;}
        if(contiguous && offset && physical!=spans_[0].physical+offset)
            {status=kIOReturnUnsupported;goto failed;}
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)
            spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={offset,physical,4096};
    }
    bzero(mapping_,bytes);
    status=NativeAdrenoMaintainOwnedPages(mapping_,bytes,NativeAdrenoClean);
    if(status!=kIOReturnSuccess)goto failed;
    __atomic_fetch_add(&native_adreno_pool_stats[5],1u,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_pool_stats[6],(uint32_t)span_count_,__ATOMIC_RELAXED);
    return kIOReturnSuccess;
failed:
    /* release() deliberately retains descriptor/metadata if complete fails.
     * Preserve the allocation's original failure instead of reporting cleanup
     * as success. No object has been exposed to a GPU or userspace yet. */
    (void)release();return status;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {native_adreno_import_persistent[0]=native_adreno_import_persistent[0]+1;return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)native_adreno_import_persistent[1]=native_adreno_import_persistent[1]+1;
    return range;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    if(descriptor_ || imported_ || spans_ || quarantined_)return kIOReturnBusy;
    native_adreno_import_stage=11;
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget || budget>256u*1024u*1024u)return kIOReturnBadArgument;
    native_adreno_import_stage=12;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    native_adreno_import_stage=13;
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    imported_=importRange(address,bytes,kIODirectionInOut,task);
    native_adreno_import_stage=14;
    if(!imported_)return kIOReturnNoMemory;
    native_adreno_import_stage=15;
    status=imported_->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        /* Read-only memory of the owner (a mapped file, constant data): cannot be pinned for writing. Pin it for reading only; the GPU then maps it
         * read-only. Metal's no-copy buffers over such memory are only ever read by shaders. */
        imported_->release();imported_=nullptr;
        imported_=importRange(address,bytes,kIODirectionOut,task);
        if(!imported_)return kIOReturnNoMemory;
        status=imported_->prepare();
        if(status==kIOReturnSuccess)read_only_=true;
    }
    if(status!=kIOReturnSuccess) {imported_->release();imported_=nullptr;return status;}
    prepared_=true;bytes_=bytes;
    status=kIOReturnUnsupported;
    native_adreno_import_stage=16;
    if(imported_->getLength()!=bytes)goto failed;
    native_adreno_import_stage=17;
    span_capacity_=bytes/4096;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {status=kIOReturnNoMemory;goto failed;}
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        addr64_t physical=imported_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)
            {native_adreno_import_stage=18;native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
             status=kIOReturnUnsupported;goto failed;}
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)
            spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={offset,physical,4096};
    }
    native_adreno_import_stage=19;
    native_adreno_pool_stats[7]=native_adreno_pool_stats[7]+(uint32_t)span_count_;
    __atomic_fetch_add(&liveImports,1u,__ATOMIC_RELAXED);live_import_=true;
    return kIOReturnSuccess;
failed:
    (void)release();return status;
}
IOReturn NativeAdrenoMemory::importDevice(IOMemoryDescriptor *scanout,uint64_t offset,size_t bytes)
{
    if(descriptor_ || imported_ || spans_ || device_ || pooled_ || quarantined_)return kIOReturnBusy;
    if(!scanout || (offset&4095) || !bytes || (bytes&4095) || offset>UINT64_MAX-bytes || bytes>256u*1024u*1024u ||
        offset+bytes>(uint64_t)scanout->getLength())return kIOReturnBadArgument;
    span_capacity_=bytes/4096;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {span_capacity_=0;return kIOReturnNoMemory;}
    for(size_t at=0;at<bytes;at+=4096) {
        IOByteCount length=0;
        const addr64_t physical=scanout->getPhysicalSegment(offset+at,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>UINT64_MAX-4096) {
            IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
            spans_=nullptr;span_count_=span_capacity_=0;
            return kIOReturnUnsupported;
        }
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={at,physical,4096};
    }
    /* Device memory: no wiring, no kernel mapping, no cache maintenance; the GPU writes it non-cacheable. */
    device_=true;prepared_=true;bytes_=bytes;mapping_=nullptr;read_only_=false;
    __atomic_fetch_add(&liveImports,1u,__ATOMIC_RELAXED);live_import_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::makeWriteCombined()
{
    if(!(descriptor_ || pooled_) || imported_ || !prepared_ || quarantined_ || device_references_ || write_combined_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    if(pooled_ && pool.write_combined) {write_combined_=true;return kIOReturnSuccess;}
    IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoCleanInvalidate);
    if(status)return status;
    status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,true);
    if(status)return status;
    write_combined_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::release()
{
    native_adreno_trace_stage=30;
    if(device_references_ || quarantined_)return kIOReturnBusy;
    if(write_combined_ && pooled_ && pool.write_combined)write_combined_=false;   /* the pool's pages keep the attribute */
    if(write_combined_) {
        /* The pages go back to the VM cacheable, as they came. */
        const IOReturn status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,false);
        if(status) {quarantined_=true;return status;}
        write_combined_=false;
    }
    if(imported_) {
        if(prepared_) {
            IOReturn status=imported_->complete();
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        imported_->release();imported_=nullptr;
    }
    /* The presenter's descriptor of a device import was never retained or wired: only the spans go. */
    if(device_) {device_=false;prepared_=false;}
    if(live_import_) {live_import_=false;__atomic_fetch_sub(&liveImports,1u,__ATOMIC_RELAXED);}
    if(descriptor_) {
        if(prepared_) {
            native_adreno_trace_stage=31;
            IOReturn status=descriptor_->complete();
            native_adreno_trace_stage=32;
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        native_adreno_trace_stage=33;
        descriptor_->release();descriptor_=nullptr;
        native_adreno_trace_stage=34;
    }
    if(pooled_) {poolGive(pool_offset_,bytes_);pooled_=false;prepared_=false;pool_offset_=0;unzeroed_=false;}
    if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
    mapping_=nullptr;spans_=nullptr;bytes_=span_count_=span_capacity_=0;read_only_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !prepared_ || !span_count_ || quarantined_ || unzeroed_)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(quarantined_)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes,bool writes,bool include_write_combined)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(device_)return kIOReturnSuccess;   /* device memory (the scanout): no CPU cache holds it */
    if(imported_)return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
    if(write_combined_)   /* no CPU mapping should cache these pages; if one does, reach its lines through the physical pages */
        return include_write_combined?maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean):kIOReturnSuccess;
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,
        writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(device_)return kIOReturnSuccess;
    /* Imported pages may hold the owner's unrelated dirty lines next to the GPU's writes: clean as well as invalidate. */
    if(imported_)return maintainImported(offset,bytes,NativeAdrenoCleanInvalidate);
    if(write_combined_)return kIOReturnSuccess;
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,NativeAdrenoInvalidate);
}
/* Imported pages have no kernel mapping here (and write-combined ones only a non-cacheable one); maintain each span's part of
 * [offset, offset+bytes) by physical address. */
IOReturn NativeAdrenoMemory::maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation op)
{
    for(size_t i=0;i<span_count_;++i) {
        const NativeAdrenoPhysicalSpan &span=spans_[i];
        const size_t begin=offset>span.offset?offset:span.offset;
        const size_t end=offset+bytes<span.offset+span.bytes?offset+bytes:span.offset+span.bytes;
        if(begin>=end)continue;
        const IOReturn status=NativeAdrenoMaintainPhysicalPages(span.physical+(begin-span.offset),end-begin,op);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
